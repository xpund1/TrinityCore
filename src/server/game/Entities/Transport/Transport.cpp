/*
 * Copyright (C) 2008-2014 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
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

#include "Common.h"
#include "Transport.h"
#include "TransportMgr.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "Path.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "DBCStores.h"
#include "World.h"
#include "GameObjectAI.h"
#include "Vehicle.h"
#include "MapReference.h"
#include "Player.h"
#include "Cell.h"
#include "CellImpl.h"

Transport* MapManager::LoadTransportInMap(Map* instance, uint32 goEntry, uint32 period)
{
    const GameObjectTemplate* goInfo = sObjectMgr->GetGameObjectTemplate(goEntry);

    if (!goInfo)
    {
        return NULL;
    }

    if (goInfo->type != GAMEOBJECT_TYPE_MO_TRANSPORT)
    {
        return NULL;
    }

    Transport* t = new Transport(period, goInfo->ScriptId);
    std::set<uint32> mapsUsed;
    if (!t->GenerateWaypoints(goInfo->moTransport.taxiPathId, mapsUsed))
    {
        delete t;
        return NULL;
    }
    uint32 transportLowGuid = sObjectMgr->GenerateLowGuid(HIGHGUID_MO_TRANSPORT);

    if (!t->Create(transportLowGuid, goEntry, t->m_WayPoints[0].mapid, t->m_WayPoints[0].x, t->m_WayPoints[0].y, t->m_WayPoints[0].z-10, 0.0f, 0))
    {
        delete t;
        return NULL;
    }

    m_Transports.insert(t);
    m_TransportsByInstanceIdMap[instance->GetInstanceId()].insert(t);
    t->SetMap(instance);
    t->AddToWorld();

    return t;
}

void MapManager::UnLoadTransportFromMap(Transport* t)
{
    Map* map = t->GetMap();

    for (Transport::CreatureSet::iterator itr = t->m_NPCPassengerSet.begin(); itr != t->m_NPCPassengerSet.end();)
    {
        if (Creature* npc = *itr)
        {
            npc->SetTransport(NULL);
            npc->setActive(false);
            npc->RemoveFromWorld();
        }
        ++itr;
    }

    UpdateData transData;
    t->BuildOutOfRangeUpdateBlock(&transData);
    WorldPacket out_packet;
    transData.BuildPacket(&out_packet);

    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        if (t != itr->GetSource()->GetTransport())
            itr->GetSource()->SendDirectMessage(&out_packet);

    t->m_NPCPassengerSet.clear();
    m_TransportsByInstanceIdMap[t->GetInstanceId()].erase(t);
    m_Transports.erase(t);
    t->m_WayPoints.clear();
    t->RemoveFromWorld();

}

void MapManager::LoadTransportForPlayers(Player* player)
{
    MapManager::TransportMap& tmap = sMapMgr->m_TransportsByInstanceIdMap;
    
    UpdateData transData;

    MapManager::TransportSet& tset = tmap[player->GetInstanceId()];

    for (MapManager::TransportSet::const_iterator i = tset.begin(); i != tset.end(); ++i)
    {
        (*i)->BuildCreateUpdateBlockForPlayer(&transData, player);
    }

    WorldPacket packet;
    transData.BuildPacket(&packet);
    player->SendDirectMessage(&packet);
}

void MapManager::UnLoadTransportForPlayers(Player* player)
{
    MapManager::TransportMap& tmap = sMapMgr->m_TransportsByInstanceIdMap;
    
    UpdateData transData;

    MapManager::TransportSet& tset = tmap[player->GetInstanceId()];

    for (MapManager::TransportSet::const_iterator i = tset.begin(); i != tset.end(); ++i)
    {
        for (Transport::CreatureSet::iterator itr = (*i)->m_NPCPassengerSet.begin(); itr != (*i)->m_NPCPassengerSet.end();)
        {
            if (Creature* npc = *itr)
            {
                npc->SetTransport(NULL);
                npc->setActive(false);
                npc->RemoveFromWorld();
            }
            ++itr;
        }

        (*i)->BuildOutOfRangeUpdateBlock(&transData);
    }

    WorldPacket packet;
    transData.BuildPacket(&packet);
    player->SendDirectMessage(&packet);
}

void MapManager::LoadTransports()
{
    uint32 oldMSTime = getMSTime();

    QueryResult result = WorldDatabase.Query("SELECT guid, entry, name, period, ScriptName FROM transports");

    if (!result)
    {
        TC_LOG_ERROR("entities.transport", ">> Loaded 0 transports. DB table `transports` is empty!");
        return;
    }

    uint32 count = 0;

    do
    {

        Field* fields = result->Fetch();
        uint32 lowguid = fields[0].GetUInt32();
        uint32 entry = fields[1].GetUInt32();
        std::string name = fields[2].GetString();
        uint32 period = fields[3].GetUInt32();
        uint32 scriptId = sObjectMgr->GetScriptId(fields[4].GetCString());

        GameObjectTemplate const* goinfo = sObjectMgr->GetGameObjectTemplate(entry);

        if (!goinfo)
        {
            TC_LOG_ERROR("entities.transport", "Transport ID:%u, Name: %s, will not be loaded, gameobject_template missing", entry, name.c_str());
            continue;
        }

        if (goinfo->type != GAMEOBJECT_TYPE_MO_TRANSPORT)
        {
            TC_LOG_ERROR("entities.transport", "Transport ID:%u, Name: %s, will not be loaded, gameobject_template type wrong", entry, name.c_str());
            continue;
        }

        // sLog->outInfo(LOG_FILTER_SERVER_LOADING, "Loading transport %d between %s, %s", entry, name.c_str(), goinfo->name);

        std::set<uint32> mapsUsed;

        Transport* t = new Transport(period, scriptId);
        if (!t->GenerateWaypoints(goinfo->moTransport.taxiPathId, mapsUsed))
            // skip transports with empty waypoints list
        {
            TC_LOG_ERROR("sql.sql", "Transport (path id %u) path size = 0. Transport ignored, check DBC files or transport GO data0 field.", goinfo->moTransport.taxiPathId);
            delete t;
            continue;
        }

        float x = t->m_WayPoints[0].x;
        float y = t->m_WayPoints[0].y;
        float z = t->m_WayPoints[0].z;
        uint32 mapid = t->m_WayPoints[0].mapid;
        float o = 1.0f;

         // creates the Gameobject -- Gunship
        if (!t->Create(lowguid, entry, mapid, x, y, z, o, 100))
         {
            delete t;
            continue;
        }

        m_Transports.insert(t);

        for (std::set<uint32>::const_iterator i = mapsUsed.begin(); i != mapsUsed.end(); ++i)
            m_TransportsByMap[*i].insert(t);

        //If we someday decide to use the grid to track transports, here:
        t->SetMap(sMapMgr->CreateBaseMap(mapid));
        t->AddToWorld();

        ++count;
    }
    while (result->NextRow());

    // check transport data DB integrity
    result = WorldDatabase.Query("SELECT gameobject.guid, gameobject.id, transports.name FROM gameobject, transports WHERE gameobject.id = transports.entry");
    if (result) // wrong data found
    {
        do
        {
            Field* fields = result->Fetch();

            uint32 guid = fields[0].GetUInt32();
            uint32 entry = fields[1].GetUInt32();
            std::string name = fields[2].GetString();
            TC_LOG_ERROR("sql.sql", "Transport %u '%s' have record (GUID: %u) in `gameobject`. Transports must not have any records in `gameobject` or its behavior will be unpredictable/bugged.", entry, name.c_str(), guid);
        }
        while (result->NextRow());
    }

    TC_LOG_ERROR("entities.transport", ">> Loaded %u transports in %u ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void MapManager::LoadTransportNPCs()
{
    uint32 oldMSTime = getMSTime();

    // 0 1 2 3 4 5 6 7
    QueryResult result = WorldDatabase.Query("SELECT guid, npc_entry, transport_entry, TransOffsetX, TransOffsetY, TransOffsetZ, TransOffsetO, emote FROM creature_transport");

    if (!result)
    {
        TC_LOG_ERROR("entities.transport", ">> Loaded 0 transport NPCs. DB table `creature_transport` is empty!");
        return;
    }

    uint32 count = 0;

    do
    {
        Field* fields = result->Fetch();
        uint32 guid = fields[0].GetInt32();
        uint32 entry = fields[1].GetInt32();
        uint32 transportEntry = fields[2].GetInt32();
        float tX = fields[3].GetFloat();
        float tY = fields[4].GetFloat();
        float tZ = fields[5].GetFloat();
        float tO = fields[6].GetFloat();
        uint32 anim = fields[7].GetInt32();

		    CreatureData& data = sObjectMgr->NewOrExistCreatureData(guid);
            data.id = fields[1].GetInt32();
            data.posX = fields[3].GetFloat();
            data.posY = fields[4].GetFloat();
            data.posZ = fields[5].GetFloat();
            data.orientation = fields[6].GetFloat();

        for (MapManager::TransportSet::iterator itr = m_Transports.begin(); itr != m_Transports.end(); ++itr)
        {
            if ((*itr)->GetEntry() == transportEntry)
            {
                (*itr)->CreateNPCPassenger(guid, &data);
                break;
            }
        }

        ++count;
    }
    while (result->NextRow());

    TC_LOG_ERROR("entities.transport", ">> Loaded %u transMapManager::LoadTransportInMapport npcs in %u ms", count, GetMSTimeDiffToNow(oldMSTime));
}

Transport::Transport(uint32 period, uint32 script) : GameObject(),
    currenttguid(0), m_nextNodeTime(0), m_pathTime(0), m_timer(0), m_period(period), ScriptId(script)
{
    m_updateFlag = UPDATEFLAG_TRANSPORT | UPDATEFLAG_LOWGUID | UPDATEFLAG_STATIONARY_POSITION | UPDATEFLAG_ROTATION;
}

Transport::Transport() : GameObject(),
    _transportInfo(NULL), _isMoving(true), _pendingStop(false),
    _triggeredArrivalEvent(false), _triggeredDepartureEvent(false)
{
    m_updateFlag = UPDATEFLAG_TRANSPORT | UPDATEFLAG_LOWGUID | UPDATEFLAG_STATIONARY_POSITION | UPDATEFLAG_ROTATION;
}

Transport::~Transport()
{
    ASSERT(_passengers.empty());
    UnloadStaticPassengers();
}

bool Transport::Create(uint32 guidlow, uint32 entry, uint32 mapid, float x, float y, float z, float ang, uint32 animprogress)
{
    Relocate(x, y, z, ang);

    if (!IsPositionValid())
    {
        TC_LOG_ERROR("entities.transport", "Transport (GUID: %u) not created. Suggested coordinates isn't valid (X: %f Y: %f)",
            guidlow, x, y);
        return false;
    }

    Object::_Create(guidlow, 0, HIGHGUID_MO_TRANSPORT);

    GameObjectTemplate const* goinfo = sObjectMgr->GetGameObjectTemplate(entry);

    if (!goinfo)
    {
        TC_LOG_ERROR("sql.sql", "Transport not created: entry in `gameobject_template` not found, guidlow: %u map: %u  (X: %f Y: %f Z: %f) ang: %f", guidlow, mapid, x, y, z, ang);
        return false;
    }

    m_goInfo = goinfo;

    TransportTemplate const* tInfo = sTransportMgr->GetTransportTemplate(entry);
    if (!tInfo)
    {
        TC_LOG_ERROR("sql.sql", "Transport %u (name: %s) will not be created, missing `transport_template` entry.", entry, goinfo->name.c_str());
        return false;
    }

    _transportInfo = tInfo;

    // initialize waypoints
    _nextFrame = tInfo->keyFrames.begin();
    _currentFrame = _nextFrame++;
    _triggeredArrivalEvent = false;
    _triggeredDepartureEvent = false;

    m_goValue.Transport.PathProgress = 0;
    SetFloatValue(OBJECT_FIELD_SCALE_X, goinfo->size);
    SetUInt32Value(GAMEOBJECT_FACTION, goinfo->faction);
    //SetUInt32Value(GAMEOBJECT_FLAGS, goinfo->flags);
    SetUInt32Value(GAMEOBJECT_FLAGS, MAKE_PAIR32(0x28, 0x64));
    SetPeriod(tInfo->pathTime);
    SetEntry(goinfo->entry);
    SetDisplayId(goinfo->displayId);
    SetGoState(!goinfo->moTransport.canBeStopped ? GO_STATE_READY : GO_STATE_ACTIVE);
    SetGoType(GAMEOBJECT_TYPE_MO_TRANSPORT);
    SetGoAnimProgress(animprogress);
    SetName(goinfo->name);
    UpdateRotationFields(0.0f, 1.0f);
    return true;
}

void Transport::Update(uint32 diff)
{
    uint32 const positionUpdateDelay = 200;

    if (AI())
        AI()->UpdateAI(diff);
    else if (!AIM_Initialize())
        TC_LOG_ERROR("entities.transport", "Could not initialize GameObjectAI for Transport");

    if (GetKeyFrames().size() <= 1)
        return;

    if (IsMoving() || !_pendingStop)
        m_goValue.Transport.PathProgress += diff;

    uint32 timer = m_goValue.Transport.PathProgress % GetPeriod();

    // Set current waypoint
    // Desired outcome: _currentFrame->DepartureTime < timer < _nextFrame->ArriveTime
    // ... arrive | ... delay ... | departure
    //      event /         event /
    for (;;)
    {
        if (timer >= _currentFrame->ArriveTime)
        {
            if (!_triggeredArrivalEvent)
            {
                DoEventIfAny(*_currentFrame, false);
                _triggeredArrivalEvent = true;
            }

            if (timer < _currentFrame->DepartureTime)
            {
                SetMoving(false);
                if (_pendingStop && GetGoState() != GO_STATE_READY)
                {
                    SetGoState(GO_STATE_READY);
                    m_goValue.Transport.PathProgress = (m_goValue.Transport.PathProgress / GetPeriod());
                    m_goValue.Transport.PathProgress *= GetPeriod();
                    m_goValue.Transport.PathProgress += _currentFrame->ArriveTime;
                }
                break;  // its a stop frame and we are waiting
            }
        }

        if (timer >= _currentFrame->DepartureTime && !_triggeredDepartureEvent)
        {
            DoEventIfAny(*_currentFrame, true); // departure event
            _triggeredDepartureEvent = true;
        }

        // not waiting anymore
        SetMoving(true);

        // Enable movement
        if (GetGOInfo()->moTransport.canBeStopped)
            SetGoState(GO_STATE_ACTIVE);

        if (timer >= _currentFrame->DepartureTime && timer < _currentFrame->NextArriveTime)
            break;  // found current waypoint

        MoveToNextWaypoint();

        sScriptMgr->OnRelocate(this, _currentFrame->Node->index, _currentFrame->Node->mapid, _currentFrame->Node->x, _currentFrame->Node->y, _currentFrame->Node->z);

        TC_LOG_DEBUG("entities.transport", "Transport %u (%s) moved to node %u %u %f %f %f", GetEntry(), GetName().c_str(), _currentFrame->Node->index, _currentFrame->Node->mapid, _currentFrame->Node->x, _currentFrame->Node->y, _currentFrame->Node->z);

        // Departure event
        if (_currentFrame->IsTeleportFrame())
            if (TeleportTransport(_nextFrame->Node->mapid, _nextFrame->Node->x, _nextFrame->Node->y, _nextFrame->Node->z, _nextFrame->InitialOrientation))
                return; // Update more in new map thread
    }

    // Set position
    _positionChangeTimer.Update(diff);
    if (_positionChangeTimer.Passed())
    {
        _positionChangeTimer.Reset(positionUpdateDelay);
        if (IsMoving())
        {
            float t = CalculateSegmentPos(float(timer) * 0.001f);
            G3D::Vector3 pos, dir;
            _currentFrame->Spline->evaluate_percent(_currentFrame->Index, t, pos);
            _currentFrame->Spline->evaluate_derivative(_currentFrame->Index, t, dir);
            UpdatePosition(pos.x, pos.y, pos.z, atan2(dir.y, dir.x) + M_PI);
        }
        else
        {
            /* There are four possible scenarios that trigger loading/unloading passengers:
              1. transport moves from inactive to active grid
              2. the grid that transport is currently in becomes active
              3. transport moves from active to inactive grid
              4. the grid that transport is currently in unloads
            */
            if (_staticPassengers.empty() && GetMap()->IsGridLoaded(GetPositionX(), GetPositionY())) // 2.
                LoadStaticPassengers();
        }
    }

    sScriptMgr->OnTransportUpdate(this, diff);
}

void Transport::AddPassenger(WorldObject* passenger)
{
    if (_passengers.insert(passenger).second)
    {
        TC_LOG_DEBUG("entities.transport", "Object %s boarded transport %s.", passenger->GetName().c_str(), GetName().c_str());

        if (Player* plr = passenger->ToPlayer())
            sScriptMgr->OnAddPassenger(this, plr);
    }
}

void Transport::RemovePassenger(WorldObject* passenger)
{
    if (_passengers.erase(passenger) || _staticPassengers.erase(passenger)) // static passenger can remove itself in case of grid unload
    {
        TC_LOG_DEBUG("entities.transport", "Object %s removed from transport %s.", passenger->GetName().c_str(), GetName().c_str());

        if (Player* plr = passenger->ToPlayer())
            sScriptMgr->OnRemovePassenger(this, plr);
    }
}

Creature* Transport::CreateNPCPassenger(uint32 guid, CreatureData const* data)
{
    Map* map = GetMap();
    Creature* creature = new Creature();

    if (!creature->LoadCreatureFromDB(guid, map, false))
    {
        delete creature;
        return NULL;
    }

    float x = data->posX;
    float y = data->posY;
    float z = data->posZ;
    float o = data->orientation;

    creature->SetTransport(this);
    creature->AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
    creature->m_movementInfo.transport.guid = GetGUID();
    creature->m_movementInfo.transport.pos.Relocate(x, y, z, o);
    CalculatePassengerPosition(x, y, z, &o);
    creature->Relocate(x, y, z, o);
    creature->SetHomePosition(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(), creature->GetOrientation());
    creature->SetTransportHomePosition(creature->m_movementInfo.transport.pos);

    /// @HACK - transport models are not added to map's dynamic LoS calculations
    ///         because the current GameObjectModel cannot be moved without recreating
    creature->AddUnitState(UNIT_STATE_IGNORE_PATHFINDING);

    if (!creature->IsPositionValid())
    {
        TC_LOG_ERROR("entities.transport", "Creature (guidlow %d, entry %d) not created. Suggested coordinates aren't valid (X: %f Y: %f)",creature->GetGUIDLow(),creature->GetEntry(),creature->GetPositionX(),creature->GetPositionY());
        delete creature;
        return NULL;
    }

    if (!map->AddToMap(creature))
    {
        delete creature;
        return NULL;
    }

    _staticPassengers.insert(creature);
    sScriptMgr->OnAddCreaturePassenger(this, creature);
    return creature;
}

GameObject* Transport::CreateGOPassenger(uint32 guid, GameObjectData const* data)
{
    Map* map = GetMap();
    GameObject* go = new GameObject();

    if (!go->LoadGameObjectFromDB(guid, map, false))
    {
        delete go;
        return NULL;
    }

    float x = data->posX;
    float y = data->posY;
    float z = data->posZ;
    float o = data->orientation;

    go->SetTransport(this);
    go->m_movementInfo.transport.guid = GetGUID();
    go->m_movementInfo.transport.pos.Relocate(x, y, z, o);
    CalculatePassengerPosition(x, y, z, &o);
    go->Relocate(x, y, z, o);

    if (!go->IsPositionValid())
    {
        TC_LOG_ERROR("entities.transport", "GameObject (guidlow %d, entry %d) not created. Suggested coordinates aren't valid (X: %f Y: %f)", go->GetGUIDLow(), go->GetEntry(), go->GetPositionX(), go->GetPositionY());
        delete go;
        return NULL;
    }

    if (!map->AddToMap(go))
    {
        delete go;
        return NULL;
    }

    _staticPassengers.insert(go);
    return go;
}
// gunship data
Creature* Transport::AddNPCPassengerInInstance(uint32 entry, float x, float y, float z, float o, uint32 anim)
{
    Map* map = GetMap();
    Creature* creature = new Creature;

    if (!creature->Create(sObjectMgr->GenerateLowGuid(HIGHGUID_UNIT), map, GetPhaseMask(), entry, 0, GetGOInfo()->faction, 0, 0, 0, 0))
    {
        delete creature;
        return 0;
    }

    creature->SetTransport(this);
    creature->AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
    creature->m_movementInfo.guid = GetGUID();
    creature->m_movementInfo.transport.pos.Relocate(x, y, z, o);

    creature->Relocate(
    GetPositionX() + (x * cos(GetOrientation()) + y * sin(GetOrientation() + float(M_PI))),
    GetPositionY() + (y * cos(GetOrientation()) + x * sin(GetOrientation())),
    z + GetPositionZ(),
    o + GetOrientation());

    creature->SetHomePosition(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(), creature->GetOrientation());

    if (!creature->IsPositionValid())
    {
        delete creature;
        return 0;
    }

    map->AddToMap(creature);
    m_NPCPassengerSet.insert(creature);

    creature->setActive(true);
    sScriptMgr->OnAddCreaturePassenger(this, creature);
    return creature;
}

void Transport::UpdatePosition(float x, float y, float z, float o)
{
    bool newActive = GetMap()->IsGridLoaded(x, y);

    Relocate(x, y, z, o);

    UpdatePassengerPositions(_passengers);

    /* There are four possible scenarios that trigger loading/unloading passengers:
      1. transport moves from inactive to active grid
      2. the grid that transport is currently in becomes active
      3. transport moves from active to inactive grid
      4. the grid that transport is currently in unloads
    */
    if (_staticPassengers.empty() && newActive) // 1.
        LoadStaticPassengers();
    else if (!_staticPassengers.empty() && !newActive && Cell(x, y).DiffGrid(Cell(GetPositionX(), GetPositionY()))) // 3.
        UnloadStaticPassengers();
    else
	{
        UpdatePassengerPositions(_staticPassengers);
		UpdatePlayerPositions();
	}
    // 4. is handed by grid unload

}

void Transport::LoadStaticPassengers()
{
    if (uint32 mapId = GetGOInfo()->moTransport.mapID)
    {
        CellObjectGuidsMap const& cells = sObjectMgr->GetMapObjectGuids(mapId, GetMap()->GetSpawnMode());
        CellGuidSet::const_iterator guidEnd;
        for (CellObjectGuidsMap::const_iterator cellItr = cells.begin(); cellItr != cells.end(); ++cellItr)
        {
            // Creatures on transport
            guidEnd = cellItr->second.creatures.end();
            for (CellGuidSet::const_iterator guidItr = cellItr->second.creatures.begin(); guidItr != guidEnd; ++guidItr)
                CreateNPCPassenger(*guidItr, sObjectMgr->GetCreatureData(*guidItr));

            // GameObjects on transport
            guidEnd = cellItr->second.gameobjects.end();
            for (CellGuidSet::const_iterator guidItr = cellItr->second.gameobjects.begin(); guidItr != guidEnd; ++guidItr)
                CreateGOPassenger(*guidItr, sObjectMgr->GetGOData(*guidItr));
        }
    }
}

void Transport::UnloadStaticPassengers()
{
    while (!_staticPassengers.empty())
    {
        WorldObject* obj = *_staticPassengers.begin();
        obj->AddObjectToRemoveList();   // also removes from _staticPassengers
    }
}

void Transport::EnableMovement(bool enabled)
{
    if (!GetGOInfo()->moTransport.canBeStopped)
        return;

    _pendingStop = !enabled;
}

void Transport::MoveToNextWaypoint()
{
    // Clear events flagging
    _triggeredArrivalEvent = false;
    _triggeredDepartureEvent = false;

    // Set frames
    _currentFrame = _nextFrame++;
    if (_nextFrame == GetKeyFrames().end())
        _nextFrame = GetKeyFrames().begin();
}

float Transport::CalculateSegmentPos(float now)
{
    KeyFrame const& frame = *_currentFrame;
    const float speed = float(m_goInfo->moTransport.moveSpeed);
    const float accel = float(m_goInfo->moTransport.accelRate);
    float timeSinceStop = frame.TimeFrom + (now - (1.0f/IN_MILLISECONDS) * frame.DepartureTime);
    float timeUntilStop = frame.TimeTo - (now - (1.0f/IN_MILLISECONDS) * frame.DepartureTime);
    float segmentPos, dist;
    float accelTime = _transportInfo->accelTime;
    float accelDist = _transportInfo->accelDist;
    // calculate from nearest stop, less confusing calculation...
    if (timeSinceStop < timeUntilStop)
    {
        if (timeSinceStop < accelTime)
            dist = 0.5f * accel * timeSinceStop * timeSinceStop;
        else
            dist = accelDist + (timeSinceStop - accelTime) * speed;
        segmentPos = dist - frame.DistSinceStop;
    }
    else
    {
        if (timeUntilStop < _transportInfo->accelTime)
            dist = 0.5f * accel * timeUntilStop * timeUntilStop;
        else
            dist = accelDist + (timeUntilStop - accelTime) * speed;
        segmentPos = frame.DistUntilStop - dist;
    }

    return segmentPos / frame.NextDistFromPrev;
}

bool Transport::GenerateWaypoints(uint32 pathid, std::set<uint32> &mapids)
{
    if (pathid >= sTaxiPathNodesByPath.size())
        return false;

    TaxiPathNodeList const& path = sTaxiPathNodesByPath[pathid];

    std::vector<KeyFrame> keyFrames;
    int mapChange = 0;
    mapids.clear();
    for (size_t i = 1; i < path.size() - 1; ++i)
    {
        if (mapChange == 0)
        {
            TaxiPathNodeEntry const& node_i = path[i];
            if (node_i.mapid == path[i+1].mapid)
            {
                KeyFrame k(node_i);
                keyFrames.push_back(k);
                mapids.insert(k.Node->mapid);
            }
            else
            {
                mapChange = 1;
            }
        }
        else
        {
            --mapChange;
        }
    }

    int lastStop = -1;
    int firstStop = -1;

    // first cell is arrived at by teleportation :S
    keyFrames[0].DistFromPrev = 0;
    if (keyFrames[0].Node->actionFlag == 2)
    {
        lastStop = 0;
    }

    // find the rest of the distances between key points
    for (size_t i = 1; i < keyFrames.size(); ++i)
    {
        if ((keyFrames[i].Node->actionFlag == 1) || (keyFrames[i].Node->mapid != keyFrames[i-1].Node->mapid))
        {
            keyFrames[i].DistFromPrev = 0;
        }
        else
        {
            keyFrames[i].DistFromPrev =
                sqrt(pow(keyFrames[i].Node->x - keyFrames[i - 1].Node->x, 2) +
                    pow(keyFrames[i].Node->y - keyFrames[i - 1].Node->y, 2) +
                    pow(keyFrames[i].Node->z - keyFrames[i - 1].Node->z, 2));
        }
        if (keyFrames[i].Node->actionFlag == 2)
        {
            // remember first stop frame
            if (firstStop == -1)
                firstStop = i;
            lastStop = i;
        }
    }

    float tmpDist = 0;
    for (size_t i = 0; i < keyFrames.size(); ++i)
    {
        int j = (i + lastStop) % keyFrames.size();
        if (keyFrames[j].Node->actionFlag == 2)
            tmpDist = 0;
        else
            tmpDist += keyFrames[j].DistFromPrev;
        keyFrames[j].DistSinceStop = tmpDist;
    }

    for (int i = int(keyFrames.size()) - 1; i >= 0; i--)
    {
        int j = (i + (firstStop+1)) % keyFrames.size();
        tmpDist += keyFrames[(j + 1) % keyFrames.size()].DistFromPrev;
        keyFrames[j].DistUntilStop = tmpDist;
        if (keyFrames[j].Node->actionFlag == 2)
            tmpDist = 0;
    }

    for (size_t i = 0; i < keyFrames.size(); ++i)
    {
        if (keyFrames[i].DistSinceStop < (30 * 30 * 0.5f))
            keyFrames[i].TimeFrom = sqrt(2 * keyFrames[i].DistSinceStop);
        else
            keyFrames[i].TimeFrom = ((keyFrames[i].DistSinceStop - (30 * 30 * 0.5f)) / 30) + 30;

        if (keyFrames[i].DistUntilStop < (30 * 30 * 0.5f))
            keyFrames[i].TimeTo = sqrt(2 * keyFrames[i].DistUntilStop);
        else
            keyFrames[i].TimeTo = ((keyFrames[i].DistUntilStop - (30 * 30 * 0.5f)) / 30) + 30;

        keyFrames[i].TimeFrom *= 1000;
        keyFrames[i].TimeTo *= 1000;
    }

    // for (int i = 0; i < keyFrames.size(); ++i) {
    // sLog->outInfo(LOG_FILTER_TRANSPORTS, "%f, %f, %f, %f, %f, %f, %f", keyFrames[i].x, keyFrames[i].y, keyFrames[i].DistUntilStop, keyFrames[i].DistSinceStop, keyFrames[i].DistFromPrev, keyFrames[i].TimeFrom, keyFrames[i].TimeTo);
    // }

    // Now we're completely set up; we can move along the length of each waypoint at 100 ms intervals
    // speed = max(30, t) (remember x = 0.5s^2, and when accelerating, a = 1 unit/s^2
    int t = 0;
    bool teleport = false;
    if (keyFrames[keyFrames.size() - 1].Node->mapid != keyFrames[0].Node->mapid)
        teleport = true;

    m_WayPoints[0] = WayPoint(keyFrames[0].Node->mapid, keyFrames[0].Node->x, keyFrames[0].Node->y, keyFrames[0].Node->z, teleport, 0,
        keyFrames[0].Node->arrivalEventID, keyFrames[0].Node->departureEventID);

    t += keyFrames[0].Node->delay * 1000;

    uint32 cM = keyFrames[0].Node->mapid;
    for (size_t i = 0; i < keyFrames.size() - 1; ++i)
    {
        float d = 0;
        float TimeFrom = keyFrames[i].TimeFrom;
        float TimeTo = keyFrames[i].TimeTo;

        // keep the generation of all these points; we use only a few now, but may need the others later
        if (((d < keyFrames[i + 1].DistFromPrev) && (TimeTo > 0)))
        {
            while ((d < keyFrames[i + 1].DistFromPrev) && (TimeTo > 0))
            {
                TimeFrom += 100;
                TimeTo -= 100;

                if (d > 0)
                {
                    float newX = keyFrames[i].Node->x + (keyFrames[i + 1].Node->x - keyFrames[i].Node->x) * d / keyFrames[i + 1].DistFromPrev;
                    float newY = keyFrames[i].Node->y + (keyFrames[i + 1].Node->y - keyFrames[i].Node->y) * d / keyFrames[i + 1].DistFromPrev;
                    float newZ = keyFrames[i].Node->z + (keyFrames[i + 1].Node->z - keyFrames[i].Node->z) * d / keyFrames[i + 1].DistFromPrev;

                    teleport = false;
                    if (keyFrames[i].Node->mapid != cM)
                    {
                        teleport = true;
                        cM = keyFrames[i].Node->mapid;
                    }

                    // sLog->outInfo(LOG_FILTER_TRANSPORTS, "T: %d, D: %f, x: %f, y: %f, z: %f", t, d, newX, newY, newZ);
                    if (teleport)
                        m_WayPoints[t] = WayPoint(keyFrames[i].Node->mapid, newX, newY, newZ, teleport, 0);
                }

                if (TimeFrom < TimeTo) // caught in TimeFrom dock's "gravitational pull"
                {
                    if (TimeFrom <= 30000)
                    {
                        d = 0.5f * (TimeFrom / 1000) * (TimeFrom / 1000);
                    }
                    else
                    {
                        d = 0.5f * 30 * 30 + 30 * ((TimeFrom - 30000) / 1000);
                    }
                    d = d - keyFrames[i].DistSinceStop;
                }
                else
                {
                    if (TimeTo <= 30000)
                    {
                        d = 0.5f * (TimeTo / 1000) * (TimeTo / 1000);
                    }
                    else
                    {
                        d = 0.5f * 30 * 30 + 30 * ((TimeTo - 30000) / 1000);
                    }
                    d = keyFrames[i].DistUntilStop - d;
                }
                t += 100;
            }
            t -= 100;
        }

        if (keyFrames[i + 1].TimeFrom > keyFrames[i + 1].TimeTo)
            t += 100 - ((long)keyFrames[i + 1].TimeTo % 100);
        else
            t += (long)keyFrames[i + 1].TimeTo % 100;

        teleport = false;
        if ((keyFrames[i + 1].Node->actionFlag == 1) || (keyFrames[i + 1].Node->mapid != keyFrames[i].Node->mapid))
        {
            teleport = true;
            cM = keyFrames[i + 1].Node->mapid;
        }

        m_WayPoints[t] = WayPoint(keyFrames[i + 1].Node->mapid, keyFrames[i + 1].Node->x, keyFrames[i + 1].Node->y, keyFrames[i + 1].Node->z, teleport,
            0, keyFrames[i + 1].Node->arrivalEventID, keyFrames[i + 1].Node->departureEventID);
        // sLog->outInfo(LOG_FILTER_TRANSPORTS, "T: %d, x: %f, y: %f, z: %f, t:%d", t, pos.x, pos.y, pos.z, teleport);

        t += keyFrames[i + 1].Node->delay * 1000;
    }

    uint32 timer = t;

    // sLog->outInfo(LOG_FILTER_TRANSPORTS, " Generated %lu waypoints, total time %u.", (unsigned long)m_WayPoints.size(), timer);

    m_curr = m_WayPoints.begin();
    m_next = GetNextWayPoint();
    m_pathTime = timer;

    m_nextNodeTime = m_curr->first;

    return true;
}

Transport::WayPointMap::const_iterator Transport::GetNextWayPoint()
{
    WayPointMap::const_iterator iter = m_curr;
    ++iter;
    if (iter == m_WayPoints.end())
        iter = m_WayPoints.begin();
    return iter;
}
bool Transport::TeleportTransport(uint32 newMapid, float x, float y, float z, float o)
{
    Map const* oldMap = GetMap();

    if (oldMap->GetId() != newMapid)
    {
        Map* newMap = sMapMgr->CreateBaseMap(newMapid);
        UnloadStaticPassengers();
        GetMap()->RemoveFromMap<Transport>(this, false);
        SetMap(newMap);

        for (std::set<WorldObject*>::iterator itr = _passengers.begin(); itr != _passengers.end();)
        {
            WorldObject* obj = (*itr++);

            float destX, destY, destZ, destO;
            obj->m_movementInfo.transport.pos.GetPosition(destX, destY, destZ, destO);
            TransportBase::CalculatePassengerPosition(destX, destY, destZ, &destO, x, y, z, o);

            switch (obj->GetTypeId())
            {
                case TYPEID_UNIT:
                    if (!IS_PLAYER_GUID(obj->ToUnit()->GetOwnerGUID()))  // pets should be teleported with player
                        obj->ToCreature()->FarTeleportTo(newMap, destX, destY, destZ, destO);
                    break;
                case TYPEID_GAMEOBJECT:
                {
                    GameObject* go = obj->ToGameObject();
                    go->GetMap()->RemoveFromMap(go, false);
                    go->Relocate(destX, destY, destZ, destO);
                    go->SetMap(newMap);
                    newMap->AddToMap(go);
                    break;
                }
                case TYPEID_PLAYER:
                    if (!obj->ToPlayer()->TeleportTo(newMapid, destX, destY, destZ, destO, TELE_TO_NOT_LEAVE_TRANSPORT))
                        _passengers.erase(obj);
                    break;
                default:
                    break;
            }
        }

        Relocate(x, y, z, o);
        GetMap()->AddToMap<Transport>(this);
        return true;
    }
    else
    {
        // Teleport players, they need to know it
        for (std::set<WorldObject*>::iterator itr = _passengers.begin(); itr != _passengers.end(); ++itr)
        {
            if ((*itr)->GetTypeId() == TYPEID_PLAYER)
            {
                float destX, destY, destZ, destO;
                (*itr)->m_movementInfo.transport.pos.GetPosition(destX, destY, destZ, destO);
                TransportBase::CalculatePassengerPosition(destX, destY, destZ, &destO, x, y, z, o);

                (*itr)->ToUnit()->NearTeleportTo(destX, destY, destZ, destO);
            }
        }

        UpdatePosition(x, y, z, o);
        return false;
    }
}

void Transport::UpdatePassengerPositions(std::set<WorldObject*>& passengers)
{
    for (std::set<WorldObject*>::iterator itr = passengers.begin(); itr != passengers.end(); ++itr)
    {
        WorldObject* passenger = *itr;
        // transport teleported but passenger not yet (can happen for players)
        if (passenger->GetMap() != GetMap())
            continue;

        // if passenger is on vehicle we have to assume the vehicle is also on transport
        // and its the vehicle that will be updating its passengers
        if (Unit* unit = passenger->ToUnit())
            if (unit->GetVehicle())
                continue;

        // Do not use Unit::UpdatePosition here, we don't want to remove auras
        // as if regular movement occurred
        float x, y, z, o;
        passenger->m_movementInfo.transport.pos.GetPosition(x, y, z, o);
        CalculatePassengerPosition(x, y, z, &o);
        switch (passenger->GetTypeId())
        {
            case TYPEID_UNIT:
            {
                Creature* creature = passenger->ToCreature();
                GetMap()->CreatureRelocation(creature, x, y, z, o, false);
                creature->GetTransportHomePosition(x, y, z, o);
                CalculatePassengerPosition(x, y, z, &o);
                creature->SetHomePosition(x, y, z, o);
                break;
            }
            case TYPEID_PLAYER:
                //relocate only passengers in world and skip any player that might be still logging in/teleporting
                if (passenger->IsInWorld())
                    GetMap()->PlayerRelocation(passenger->ToPlayer(), x, y, z, o);
                break;
            case TYPEID_GAMEOBJECT:
                GetMap()->GameObjectRelocation(passenger->ToGameObject(), x, y, z, o, false);
                break;
            default:
                break;
        }

        if (Unit* unit = passenger->ToUnit())
            if (Vehicle* vehicle = unit->GetVehicleKit())
                vehicle->RelocatePassengers();
    }
}
// gunship Data
void Transport::UpdatePlayerPositions()
{
    for (PlayerSet::iterator itr = m_passengers.begin(); itr != m_passengers.end(); ++itr)
    {
        Player* plr = *itr;

        float x, y, z, o;
        o = GetOrientation() + plr->m_movementInfo.transport.pos.m_orientation;
        x = GetPositionX() + (plr->m_movementInfo.transport.pos.m_positionX * cos(GetOrientation()) + plr->m_movementInfo.transport.pos.m_positionY * sin(GetOrientation() + M_PI));
        y = GetPositionY() + (plr->m_movementInfo.transport.pos.m_positionY * cos(GetOrientation()) + plr->m_movementInfo.transport.pos.m_positionX * sin(GetOrientation()));
        z = GetPositionZ() + plr->m_movementInfo.transport.pos.m_positionZ;
        plr->Relocate(x, y, z, o);
        UpdateData transData;
        WorldPacket packet;
        transData.BuildPacket(&packet);
        plr->SendDirectMessage(&packet);
    }
}

void Transport::DoEventIfAny(KeyFrame const& node, bool departure)
{
    if (uint32 eventid = departure ? node.Node->departureEventID : node.Node->arrivalEventID)
    {
        TC_LOG_DEBUG("maps.script", "Taxi %s event %u of node %u of %s path", departure ? "departure" : "arrival", eventid, node.Node->index, GetName().c_str());
        GetMap()->ScriptsStart(sEventScripts, eventid, this, this);
        EventInform(eventid);
    }
}

void Transport::BuildUpdate(UpdateDataMapType& data_map)
{
    Map::PlayerList const& players = GetMap()->GetPlayers();
    if (players.isEmpty())
        return;

    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        BuildFieldsUpdate(itr->GetSource(), data_map);

    ClearUpdateMask(true);
}
