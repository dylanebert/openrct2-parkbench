#pragma once

// Independent, test-side census for the finite S2a3 kernel.  The serializer
// deliberately does not include this header.  These are canonical record
// templates, not a log produced by serialization.
#include "NativeActionContractRideProjection.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <openrct2/management/Finance.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/world/tile_element/TileElement.h>

namespace OpenRCT2::Testing::ProjectionSchema
{
    struct PathTemplate
    {
        std::string_view value;
    };

    inline constexpr std::array<PathTemplate, 20> kWatchPaths{{
        { "watch/ride" }, { "watch/seatedGuest" }, { "watch/watchingGuest" }, { "watch/queueTailGuest" },
        { "watch/queueHeadGuest" }, { "watch/vehicleHead" }, { "watch/vehicleTail" }, { "watch/banner" },
        { "watch/campaignType" }, { "watch/campaignRide" }, { "watch/recentSlot" }, { "watch/archivedSlot" },
        { "watch/tiles/track/x" }, { "watch/tiles/track/y" }, { "watch/tiles/entrance/x" },
        { "watch/tiles/entrance/y" }, { "watch/tiles/exit/x" }, { "watch/tiles/exit/y" }, { "watch/tiles/queue/x" },
        { "watch/tiles/queue/y" },
    }};

    inline constexpr std::array<std::string_view, 20> kRideFields{{
        "id", "exists", "type", "subtype", "status", "vehicles/0", "vehicles/1", "numTrains", "numCarsPerTrain",
        "maxTrains", "vehicleChangeTimeout", "flags", "currentIssues", "lastIssueTime", "fixedRatings", "tested", "testInProgress",
        "testingFlags", "currentTestSegment", "currentTestStation",
    }};
    inline constexpr std::array<std::string_view, 12> kStationFields{{
        "index", "exists", "start/x", "start/y", "height", "length", "depart", "trainAtStation",
        "segmentLength", "segmentTime", "queueTime", "queueLength", // lastPeepInQueue is added below
    }};
    inline constexpr std::array<std::string_view, 11> kEndpointFields{{
        "x", "y", "z", "direction", "present",
        "type", "ride", "station", "entryIndex", "baseHeight", "clearanceHeight",
    }};
    inline constexpr std::array<std::string_view, 23> kVehicleFields{{
        "id", "exists", "ride", "subtype", "vehicleType", "trainLink", "previousRideLink", "nextRideLink", "status",
        "seats", "occupantCount", "nextFreeSeat", "flags", "trackLocation/x", "trackLocation/y", "trackLocation/z",
        "trackTypeAndDirection", "constructionStatus", "testing", "restraints", "currentStation", "trackProgress", "subState",
    }};
    inline constexpr std::array<std::string_view, 3> kColourFields{{ "body", "trim", "tertiary" }};
    inline constexpr std::array<std::string_view, 20> kGuestFields{{
        "id", "exists", "currentRide", "currentStation", "currentTrain", "state", "substate", "queuePredecessor", "queueTime",
        "rejoinQueueTimeout", "previousRideTimeout", "headingToRide", "favouriteRide", "previousRide", "voucherRide", "itemFlags",
        "peepFlags", "thoughts/0/type", "thoughts/0/itemOrRide", "thoughts/0/freshness",
    }};
    inline constexpr std::array<std::string_view, 8> kNewsFields{{ "slot", "item/type", "item/flags", "item/assoc", "item/ticks", "item/monthYear", "item/day", "item/text" }};
    inline constexpr std::array<std::string_view, 9> kBannerFields{{ "id", "exists", "type", "flags", "assoc", "text", "colour", "textColour", "position/x" }};
    inline constexpr std::array<std::string_view, 6> kCampaignFields{{ "type", "exists", "weeksLeft", "flags", "ride", "firstWeek" }};
    inline constexpr std::array<std::string_view, 9> kTileRawFields{{ "x", "y", "role", "present", "type", "flags", "baseHeight", "clearanceHeight", "owner" }};
    inline constexpr std::array<std::string_view, 9> kTrackFields{{ "ride", "rideType", "station", "trackType", "sequence", "colourScheme", "invisible", "direction", "ghost" }};
    inline constexpr std::array<std::string_view, 10> kPathFields{{ "isQueue", "ride", "station", "edges", "slope", "surface", "railings", "baseHeight", "clearanceHeight", "ghost" }};
    inline constexpr std::array<std::string_view, 4> kFinanceFields{{ "cash", "bankLoan", "maxBankLoan", "loanInterestRate" }};
    inline constexpr std::array<std::string_view, 6> kMeasurementFields{{ "flags", "lastUseTick", "numItems", "currentItem", "vehicleIndex", "currentStation" }};

    inline std::string RoleName(TileRole role)
    {
        switch (role)
        {
            case TileRole::track: return "track";
            case TileRole::entrance: return "entrance";
            case TileRole::exit: return "exit";
            case TileRole::queue: return "queue";
        }
        return "unknown";
    }

    inline void Add(std::vector<std::string>& paths, std::string base, std::string_view suffix)
    {
        paths.push_back(std::move(base) + "/" + std::string(suffix));
    }

    inline std::vector<std::string> ExpandKernelPaths(const ProjectionFixtureHandles& handles, bool measurementPresent = true)
    {
        std::vector<std::string> paths;
        for (const auto& path : kWatchPaths)
            paths.emplace_back(path.value);
        const std::string ride = "rides/target";
        for (const auto field : kRideFields) Add(paths, ride, field);
        for (uint16_t station = 0; station < Limits::kMaxStationsPerRide; ++station)
        {
            const auto base = ride + "/stations/" + std::to_string(station);
            for (const auto field : kStationFields) Add(paths, base, field);
            Add(paths, base, "lastPeepInQueue");
            for (const auto endpoint : { "entrance", "exit" })
            {
                const auto endpointBase = base + "/" + endpoint;
                Add(paths, endpointBase, "present");
                // Null endpoints have only the discriminated presence leaf.
                Add(paths, endpointBase, "x"); Add(paths, endpointBase, "y"); Add(paths, endpointBase, "z");
                Add(paths, endpointBase, "direction"); Add(paths, endpointBase, "type"); Add(paths, endpointBase, "ride");
                Add(paths, endpointBase, "station"); Add(paths, endpointBase, "entryIndex"); Add(paths, endpointBase, "baseHeight");
                Add(paths, endpointBase, "clearanceHeight");
            }
        }
        if (measurementPresent)
        {
            for (const auto field : kMeasurementFields) Add(paths, ride + "/measurement", field);
            for (uint16_t i = 0; i < 2; ++i)
                for (const auto field : { "vertical", "lateral", "velocity", "altitude" })
                    Add(paths, ride + "/measurement/" + std::string(field), std::to_string(i));
        }
        for (const auto vehicleName : { "head", "tail" })
        {
            const auto base = std::string("vehicles/") + vehicleName;
            for (const auto field : kVehicleFields) Add(paths, base, field);
            for (uint16_t i = 0; i < 32; ++i) Add(paths, base, "occupants/" + std::to_string(i));
            for (const auto field : kColourFields) Add(paths, base + "/colours", field);
        }
        for (const auto guestName : { "seated", "watching", "queueTail", "queueHead" })
        {
            const auto base = std::string("guests/") + guestName;
            for (const auto field : kGuestFields) Add(paths, base, field);
            Add(paths, base, "thoughts/0/freshTimeout");
            Add(paths, base, "photoRides/0"); Add(paths, base, "photoRides/1"); Add(paths, base, "photoRides/2"); Add(paths, base, "photoRides/3");
        }
        Add(paths, "guests/seated", "seat/car"); Add(paths, "guests/seated", "seat/seat");
        Add(paths, "guests/watching", "standing/timeToStand"); Add(paths, "guests/watching", "standing/flags");
        Add(paths, "rideUseHistory/watching", "guest"); Add(paths, "rideUseHistory/watching", "rides/0");
        Add(paths, "rideUseHistory/other", "guest"); Add(paths, "rideUseHistory/other", "rides");
        for (const auto queue : { "recent", "archived" }) for (const auto field : kNewsFields) Add(paths, std::string("news/") + queue + "/0", field);
        for (const auto field : kBannerFields) Add(paths, "banners/target", field);
        Add(paths, "banners/target", "position/y");
        for (const auto field : kCampaignFields) Add(paths, "campaigns/target", field);
        for (const auto role : { TileRole::track, TileRole::entrance, TileRole::exit, TileRole::queue })
        {
            const auto base = "tiles/" + RoleName(role);
            for (const auto field : kTileRawFields) Add(paths, base, field);
            for (uint16_t i = 0; i < kTileElementSize; ++i) Add(paths, base + "/bytes", std::to_string(i));
            if (role == TileRole::track)
                for (const auto field : kTrackFields) Add(paths, base + "/track", field);
            else if (role == TileRole::queue)
                for (const auto field : kPathFields) Add(paths, base + "/path", field);
            else
            {
                for (const auto field : kEndpointFields) Add(paths, base + "/entrance", field);
                Add(paths, base + "/entrance", "ghost");
            }
        }
        if (!measurementPresent) paths.emplace_back("rides/target/measurement");
        for (const auto field : kFinanceFields) Add(paths, "finance", field);
        for (uint16_t month = 0; month < 16; ++month)
            for (uint16_t type = 0; type < static_cast<uint16_t>(EnumValue(ExpenditureType::count)); ++type)
                Add(paths, "finance/expenditureTable/" + std::to_string(month), std::to_string(type));
        for (uint16_t i = 0; i < kFinanceHistorySize; ++i) Add(paths, "finance/valueHistory", std::to_string(i));
        paths.emplace_back("parkValue");
        return paths;
    }
} // namespace OpenRCT2::Testing::ProjectionSchema
