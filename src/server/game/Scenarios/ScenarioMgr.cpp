/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScenarioMgr.h"
#include "DB2Stores.h"
#include "DatabaseEnv.h"
#include "InstanceScenario.h"
#include "Log.h"
#include "Map.h"
#include "MapUtils.h"
#include "ScenarioPackets.h"

ScenarioMgr::ScenarioMgr() = default;
ScenarioMgr::~ScenarioMgr() = default;

ScenarioMgr* ScenarioMgr::Instance()
{
    static ScenarioMgr instance;
    return &instance;
}

InstanceScenario* ScenarioMgr::CreateInstanceScenarioForTeam(InstanceMap* map, TeamId team) const
{
    auto dbDataItr = _scenarioDBData.find(std::make_pair(map->GetId(), map->GetDifficultyID()));
    // No scenario registered for this map and difficulty in the database
    if (dbDataItr == _scenarioDBData.end())
        return nullptr;

    uint32 scenarioID = 0;
    switch (team)
    {
        case TEAM_ALLIANCE:
            scenarioID = dbDataItr->second.Scenario_A;
            break;
        case TEAM_HORDE:
            scenarioID = dbDataItr->second.Scenario_H;
            break;
        default:
            break;
    }

    return CreateInstanceScenario(map, scenarioID);
}

InstanceScenario* ScenarioMgr::CreateInstanceScenario(InstanceMap* map, uint32 scenarioID) const
{
    auto itr = _scenarioData.find(scenarioID);
    if (itr == _scenarioData.end())
    {
        TC_LOG_ERROR("scenario", "No scenario data was found related to scenario (Id: {}) for map (Id: {}), difficulty (Id: {}).", scenarioID, map->GetId(), map->GetDifficultyID());
        return nullptr;
    }

    return new InstanceScenario(map, &itr->second);
}

void ScenarioMgr::LoadDBData()
{
    _scenarioDBData.clear();

    uint32 oldMSTime = getMSTime();

    QueryResult result = WorldDatabase.Query("SELECT map, difficulty, scenario_A, scenario_H FROM scenarios");

    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 scenarios. DB table `scenarios` is empty!");
        return;
    }

    do
    {
        Field* fields = result->Fetch();

        uint32 mapId = fields[0].GetUInt32();
        uint8 difficulty = fields[1].GetUInt8();

        uint32 scenarioAllianceId = fields[2].GetUInt32();
        if (scenarioAllianceId > 0 && _scenarioData.find(scenarioAllianceId) == _scenarioData.end())
        {
            TC_LOG_ERROR("sql.sql", "ScenarioMgr::LoadDBData: DB Table `scenarios`, column scenario_A contained an invalid scenario (Id: {})!", scenarioAllianceId);
            continue;
        }

        uint32 scenarioHordeId = fields[3].GetUInt32();
        if (scenarioHordeId > 0 && _scenarioData.find(scenarioHordeId) == _scenarioData.end())
        {
            TC_LOG_ERROR("sql.sql", "ScenarioMgr::LoadDBData: DB Table `scenarios`, column scenario_H contained an invalid scenario (Id: {})!", scenarioHordeId);
            continue;
        }

        if (scenarioHordeId == 0)
            scenarioHordeId = scenarioAllianceId;

        ScenarioDBData& data = _scenarioDBData[std::make_pair(mapId, difficulty)];
        data.MapID = mapId;
        data.DifficultyID = difficulty;
        data.Scenario_A = scenarioAllianceId;
        data.Scenario_H = scenarioHordeId;
    }
    while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} instance scenario entries in {} ms", _scenarioDBData.size(), GetMSTimeDiffToNow(oldMSTime));
}

void ScenarioMgr::LoadDB2Data()
{
    _scenarioData.clear();

    std::unordered_map<uint32, std::map<uint8, ScenarioStepEntry const*>> scenarioSteps;
    uint32 deepestCriteriaTreeSize = 0;

    for (ScenarioStepEntry const* step : sScenarioStepStore)
    {
        scenarioSteps[step->ScenarioID][step->OrderIndex] = step;
        if (CriteriaTree const* tree = sCriteriaMgr->GetCriteriaTree(step->Criteriatreeid))
        {
            uint32 criteriaTreeSize = 0;
            CriteriaMgr::WalkCriteriaTree(tree, [&criteriaTreeSize](CriteriaTree const* /*tree*/)
            {
                ++criteriaTreeSize;
            });
            deepestCriteriaTreeSize = std::max(deepestCriteriaTreeSize, criteriaTreeSize);
        }
    }

    ASSERT(deepestCriteriaTreeSize < MAX_ALLOWED_SCENARIO_POI_QUERY_SIZE, "MAX_ALLOWED_SCENARIO_POI_QUERY_SIZE must be at least %u", deepestCriteriaTreeSize + 1);

    for (ScenarioEntry const* scenario : sScenarioStore)
    {
        ScenarioData& data = _scenarioData[scenario->ID];
        data.Entry = scenario;
        data.Steps = std::move(scenarioSteps[scenario->ID]);
    }
}

void ScenarioMgr::LoadScenarioPOI()
{
    uint32 oldMSTime = getMSTime();

    _scenarioPOIStore.clear(); // need for reload case

    uint32 count = 0;

    //                                                      0            1        2     3       4         5       6          7               8                        9
    QueryResult result = WorldDatabase.Query("SELECT CriteriaTreeID, BlobIndex, Idx1, MapID, UiMapID, Priority, Flags, WorldEffectID, PlayerConditionID, NavigationPlayerConditionID FROM scenario_poi ORDER BY CriteriaTreeID, Idx1");
    if (!result)
    {
        TC_LOG_ERROR("server.loading", ">> Loaded 0 scenario POI definitions. DB table `scenario_poi` is empty.");
        return;
    }

    //                                                       0        1    2  3  4
    QueryResult pointsResult = WorldDatabase.Query("SELECT CriteriaTreeID, Idx1, X, Y, Z FROM scenario_poi_points ORDER BY CriteriaTreeID DESC, Idx1, Idx2");

    std::unordered_map<int32, std::map<int32, std::vector<ScenarioPOIPoint>>> allPoints;

    if (pointsResult)
    {
        Field* fields = pointsResult->Fetch();

        do
        {
            fields = pointsResult->Fetch();

            int32 CriteriaTreeID = fields[0].GetInt32();
            int32 Idx1 = fields[1].GetInt32();
            int32 X = fields[2].GetInt32();
            int32 Y = fields[3].GetInt32();
            int32 Z = fields[4].GetInt32();

            allPoints[CriteriaTreeID][Idx1].emplace_back(X, Y, Z);
        } while (pointsResult->NextRow());
    }

    do
    {
        Field* fields = result->Fetch();

        int32 criteriaTreeID = fields[0].GetInt32();
        int32 blobIndex = fields[1].GetInt32();
        int32 idx1 = fields[2].GetInt32();
        int32 mapID = fields[3].GetInt32();
        int32 uiMapID = fields[4].GetInt32();
        int32 priority = fields[5].GetInt32();
        int32 flags = fields[6].GetInt32();
        int32 worldEffectID = fields[7].GetInt32();
        int32 playerConditionID = fields[8].GetInt32();
        int32 navigationPlayerConditionID = fields[9].GetInt32();

        if (!sCriteriaMgr->GetCriteriaTree(criteriaTreeID))
            TC_LOG_ERROR("sql.sql", "`scenario_poi` CriteriaTreeID ({}) Idx1 ({}) does not correspond to a valid criteria tree", criteriaTreeID, idx1);

        if (std::map<int32, std::vector<ScenarioPOIPoint>>* blobs = Trinity::Containers::MapGetValuePtr(allPoints, criteriaTreeID))
        {
            if (std::vector<ScenarioPOIPoint>* points = Trinity::Containers::MapGetValuePtr(*blobs, idx1))
            {
                _scenarioPOIStore[criteriaTreeID].emplace_back(blobIndex, mapID, uiMapID, priority, flags, worldEffectID, playerConditionID, navigationPlayerConditionID, std::move(*points));
                ++count;
                continue;
            }
        }

        TC_LOG_ERROR("server.loading", "Table scenario_poi references unknown scenario poi points for criteria tree id {} POI id {}", criteriaTreeID, blobIndex);

    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} scenario POI definitions in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

ScenarioPOIVector const* ScenarioMgr::GetScenarioPOIs(int32 criteriaTreeID) const
{
    auto itr = _scenarioPOIStore.find(criteriaTreeID);
    if (itr != _scenarioPOIStore.end())
        return &itr->second;

    return nullptr;
}
