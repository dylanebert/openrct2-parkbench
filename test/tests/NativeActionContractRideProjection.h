#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <openrct2/GameState.h>
#include <openrct2/world/Location.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
        // These are identities captured before execution.  They remain in the
        // projection after an action removes the record; a post-state scan may
        // not silently drop evidence of a clearing mutation.
        std::vector<size_t> recentNewsIndices;
        std::vector<size_t> archivedNewsIndices;
        std::vector<uint16_t> bannerIds;
        std::vector<std::pair<uint8_t, uint16_t>> campaignKeys;
        std::vector<TileCoordsXY> tileCoords;
    };

    RideProjectionWatchSet CaptureRideProjectionWatchSet(const GameState_t& state, const json_t& args);
    void SetRideProjectionWatchSet(RideProjectionWatchSet watch);
    void ClearRideProjectionWatchSet();
    json_t SerializeRideProjection(const GameState_t& state, const json_t& args, const RideProjectionWatchSet& watch);
    json_t SerializeRideProjection(const GameState_t& state, const json_t& args);

    // The runner invokes this contract for every action-specific projection.
    // It is intentionally test-only and reads the authoritative stores rather
    // than a native resource alias.
    bool ValidateRideProjectionStores(const json_t& projection, std::string* failure = nullptr);
} // namespace OpenRCT2::Testing
