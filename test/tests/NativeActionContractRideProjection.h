#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <set>
#include <vector>

#include <openrct2/GameState.h>
#include <openrct2/world/Location.hpp>

namespace OpenRCT2::Testing
{
    using json_t = nlohmann::json;

    // This is deliberately test-only.  The ids and tile coordinates are fixed
    // before an action runs and are retained when the engine removes an entity
    // or tile element, so a post-state scan cannot make a clearing mutation
    // disappear from the contract projection.
    struct RideProjectionWatchSet
    {
        std::set<uint16_t> rideIds;
        std::set<uint16_t> vehicleIds;
        std::set<uint16_t> guestIds;
        std::vector<TileCoordsXY> tileCoords;
    };

    RideProjectionWatchSet CaptureRideProjectionWatchSet(const GameState_t& state, const json_t& args);
    void SetRideProjectionWatchSet(RideProjectionWatchSet watch);
    void ClearRideProjectionWatchSet();
    json_t SerializeRideProjection(const GameState_t& state, const json_t& args, const RideProjectionWatchSet& watch);
    json_t SerializeRideProjection(const GameState_t& state, const json_t& args);
}
