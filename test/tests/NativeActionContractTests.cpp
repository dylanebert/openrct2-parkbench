/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TestData.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/actions/CommandFlag.h>
#include <openrct2/actions/GameAction.hpp>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/ride/RideCreateAction.h>
#include <openrct2/actions/ride/RideEntranceExitPlaceAction.h>
#include <openrct2/actions/ride/RideEntranceExitRemoveAction.h>
#include <openrct2/actions/track/TrackPlaceAction.h>
#include <openrct2/command_line/NativeRegistry.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/management/Research.h>
#include <openrct2/object/Object.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/ride/RideColour.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/Track.h>
#include <openrct2/ride/ted/TrackElemType.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace OpenRCT2;
using namespace OpenRCT2::CommandLine;
using json_t = nlohmann::json;

namespace
{
    enum class ExpectedCostClass
    {
        engineComputed,
    };

    struct SemanticInvalidPartition
    {
        std::string parameter;
        std::function<void(json_t&)> makeInvalid;
    };

    struct NativeActionFixture
    {
        std::string registration;
        std::string ownerFamily;
        std::string publicUse;
        std::string withdrawalReason;
        std::vector<std::string> untrustedParameters;
        std::function<void(GameState_t&)> prepareLegalState;
        std::function<json_t()> legalArgs;
        std::vector<SemanticInvalidPartition> semanticInvalidPartitions;
        std::function<void(GameState_t&, const json_t&)> mutateRelevantState;
        std::function<json_t(const GameState_t&, const json_t&)> rejectionProjection;
        std::function<json_t(const GameState_t&, const json_t&)> acceptedPostStateProjection;
        std::function<GameActions::GameAction::Ptr(const json_t&)> makeOrdinaryAction;
        ExpectedCostClass expectedCostClass = ExpectedCostClass::engineComputed;
        std::vector<std::string> transportOmissions;
    };

    struct InventoryRow
    {
        std::string registration;
        std::string ownerFamily;
        std::string publicUse;
        std::string withdrawalReason;
        json_t legalArgs = json_t::object();
        std::vector<std::string> untrustedParameters;
        std::vector<std::string> semanticPartitions;
        json_t rejectionProjection = json_t::object();
        json_t acceptedPostStateProjection = json_t::object();
    };

    struct InventoryResult
    {
        bool ok = true;
        std::string failure;
    };

    InventoryResult ValidateInventory(
        const std::vector<InventoryRow>& rows, const std::map<std::string, size_t>& expectedFamilyPopulation,
        const std::vector<std::string>& registrations)
    {
        auto fail = [](std::string message) { return InventoryResult{ false, std::move(message) }; };

        if (rows.empty())
            return fail("owner-fixture inventory is empty; registration/schema presence is not conformance");
        if (expectedFamilyPopulation.empty())
            return fail("expected owner-family population is empty");

        std::set<std::string> registered(registrations.begin(), registrations.end());
        if (registered.size() != registrations.size())
            return fail("registration inventory contains duplicates");

        std::set<std::string> seenRegistrations;
        std::map<std::string, size_t> actualFamilyPopulation;
        for (const auto& row : rows)
        {
            if (row.registration.empty())
                return fail("fixture has no registration");
            if (!seenRegistrations.insert(row.registration).second)
                return fail("duplicate fixture registration: " + row.registration);
            if (!registered.contains(row.registration))
                return fail("fixture registration is missing from the engine census: " + row.registration);
            if (row.ownerFamily.empty())
                return fail("fixture has no owner family: " + row.registration);
            if (row.publicUse.empty() == row.withdrawalReason.empty())
                return fail("fixture must declare exactly one public use or withdrawal reason: " + row.registration);
            if (!row.legalArgs.is_object() || row.legalArgs.empty())
                return fail("fixture has no legal starting args: " + row.registration);
            if (row.untrustedParameters.empty())
                return fail("fixture has no untrusted parameter declarations: " + row.registration);
            if (row.rejectionProjection.empty())
                return fail("fixture rejection projection is empty: " + row.registration);
            if (row.acceptedPostStateProjection.empty())
                return fail("fixture accepted post-state projection is empty: " + row.registration);

            std::set<std::string> parameters(row.untrustedParameters.begin(), row.untrustedParameters.end());
            std::set<std::string> partitions(row.semanticPartitions.begin(), row.semanticPartitions.end());
            if (parameters.size() != row.untrustedParameters.size())
                return fail("fixture repeats an untrusted parameter: " + row.registration);
            for (const auto& parameter : parameters)
            {
                if (!partitions.contains(parameter))
                    return fail("fixture has no semantic invalid partition for parameter '" + parameter + "'");
            }
            ++actualFamilyPopulation[row.ownerFamily];
        }

        for (const auto& [family, expected] : expectedFamilyPopulation)
        {
            if (expected == 0)
                return fail("owner family has zero expected population: " + family);
            const auto actual = actualFamilyPopulation.find(family);
            if (actual == actualFamilyPopulation.end() || actual->second != expected)
            {
                std::ostringstream message;
                message << "owner family population mismatch for " << family << ": expected " << expected << ", got "
                        << (actual == actualFamilyPopulation.end() ? 0 : actual->second);
                return fail(message.str());
            }
        }
        for (const auto& [family, actual] : actualFamilyPopulation)
        {
            if (!expectedFamilyPopulation.contains(family))
                return fail("fixture owner family has no expected population: " + family);
            if (actual == 0)
                return fail("owner family population is zero: " + family);
        }
        return {};
    }

    struct HarnessObservation
    {
        bool priorQuery = true;
        bool freshExecuteTimeQuery = true;
        json_t rejectionBefore = { { "tile", 1 }, { "finance", 2 } };
        json_t rejectionAfter = rejectionBefore;
        bool resultParity = true;
        bool costParity = true;
        bool financeParity = true;
        bool postStateParity = true;
        std::vector<std::string> untrustedParameters{ "x", "ride" };
        std::vector<std::string> semanticPartitions{ "x", "ride" };
    };

    std::string ValidateHarnessObservation(const HarnessObservation& observation)
    {
        if (!observation.priorQuery)
            return "fixture must perform a prior query before relevant-state mutation";
        if (!observation.freshExecuteTimeQuery)
            return "fresh execute-time query was not observed";
        if (observation.rejectionBefore.empty() || observation.rejectionAfter.empty())
            return "rejection projection is empty";
        if (observation.rejectionBefore != observation.rejectionAfter)
            return "rejected execution mutated the declared projection";
        if (!observation.resultParity)
            return "ordinary and public result diverged";
        if (!observation.costParity)
            return "ordinary and public cost diverged";
        if (!observation.financeParity)
            return "ordinary and public finance charge diverged";
        if (!observation.postStateParity)
            return "ordinary and public accepted post-state diverged";

        std::set<std::string> parameters(observation.untrustedParameters.begin(), observation.untrustedParameters.end());
        std::set<std::string> partitions(observation.semanticPartitions.begin(), observation.semanticPartitions.end());
        for (const auto& parameter : parameters)
        {
            if (!partitions.contains(parameter))
                return "uncovered semantic parameter: " + parameter;
        }
        return {};
    }

    size_t TileElementCount(const CoordsXY& location)
    {
        auto* element = MapGetFirstElementAt(location);
        if (element == nullptr)
            return 0;

        size_t count = 0;
        do
        {
            ++count;
        } while (!(element++)->isLastForTile());
        return count;
    }

    Ride* FindRideWithEntranceAndTrack()
    {
        for (RideId::UnderlyingType i = 0; i < Limits::kMaxRidesInPark; ++i)
        {
            auto* ride = GetRide(RideId::FromUnderlying(i));
            if (ride != nullptr && !ride->getStation(StationIndex::FromUnderlying(0)).Entrance.IsNull())
                return ride;
        }
        return nullptr;
    }

    bool ExecuteSetupAction(GameActions::GameAction& action, GameState_t& state)
    {
        const auto oldInUpdateCode = gInUpdateCode;
        gInUpdateCode = true;
        const auto result = GameActions::ExecuteSynchronous(&action, state);
        gInUpdateCode = oldInUpdateCode;
        return result.error == GameActions::Status::ok;
    }

    json_t RideCreateArgs(
        ride_type_t rideType = static_cast<ride_type_t>(81), ObjectEntryIndex rideObject = static_cast<ObjectEntryIndex>(10),
        uint8_t trackColour = 0, uint8_t vehicleColour = 0, ObjectEntryIndex entranceObject = static_cast<ObjectEntryIndex>(10),
        RideInspection inspection = RideInspection::never)
    {
        return {
            { "rideType", rideType },
            { "rideObject", rideObject },
            { "entranceObject", entranceObject },
            { "colour1", trackColour },
            { "colour2", vehicleColour },
            { "inspectionInterval", static_cast<uint8_t>(inspection) },
        };
    }

    json_t RideCreateResultProjection(const GameActions::Result& result)
    {
        return {
            { "status", static_cast<uint16_t>(result.error) },
            { "accepted", result.error == GameActions::Status::ok },
            { "cost", result.cost },
            { "position", { { "x", result.position.x }, { "y", result.position.y }, { "z", result.position.z } } },
        };
    }

    json_t RideCreateRejectedResult(GameActions::Status status, StringId title, StringId message, std::string_view code)
    {
        const GameActions::Result expected(status, title, message);
        auto result = RideCreateResultProjection(expected);
        result["action"] = "RideCreateAction";
        result["rejection"] = {
            { "code", code },
            { "title", expected.getErrorTitle() },
            { "message", expected.getErrorMessage() },
            { "detail", { { "status", static_cast<uint16_t>(status) } } },
        };
        return result;
    }

    json_t RideCreateRideProjection(const Ride& ride)
    {
        auto coords = [](const auto& value) {
            json_t result = { { "x", value.x }, { "y", value.y } };
            if constexpr (requires { value.z; })
                result["z"] = value.z;
            return result;
        };
        auto vehicleColour = [](const VehicleColour& value) {
            return json_t{
                { "body", static_cast<uint8_t>(value.Body) },
                { "trim", static_cast<uint8_t>(value.Trim) },
                { "tertiary", static_cast<uint8_t>(value.Tertiary) },
            };
        };
        json_t stations = json_t::array();
        for (const auto& station : ride.getStations())
        {
            stations.push_back(
                {
                    { "start", coords(station.Start) },
                    { "entrance", coords(station.Entrance) },
                    { "exit", coords(station.Exit) },
                });
        }
        json_t trackColours = json_t::array();
        for (const auto& colour : ride.trackColours)
        {
            trackColours.push_back(
                {
                    { "main", static_cast<uint8_t>(colour.main) },
                    { "additional", static_cast<uint8_t>(colour.additional) },
                    { "supports", static_cast<uint8_t>(colour.supports) },
                });
        }
        json_t vehicleColours = json_t::array();
        for (const auto& colour : ride.vehicleColours)
            vehicleColours.push_back(vehicleColour(colour));
        return {
            { "id", ride.id.ToUnderlying() },
            { "type", ride.type },
            { "subtype", ride.subtype },
            { "trackColours", std::move(trackColours) },
            { "vehicleColourSettings", static_cast<uint8_t>(ride.vehicleColourSettings) },
            { "vehicleColours", std::move(vehicleColours) },
            { "overallView", { { "x", ride.overallView.x }, { "y", ride.overallView.y } } },
            { "stations", std::move(stations) },
            { "status", static_cast<uint8_t>(ride.status) },
            { "numTrains", ride.numTrains },
            { "proposedNumTrains", ride.proposedNumTrains },
            { "maxTrains", ride.maxTrains },
            { "numCarsPerTrain", ride.numCarsPerTrain },
            { "proposedNumCarsPerTrain", ride.proposedNumCarsPerTrain },
            { "minCarsPerTrain", ride.minCarsPerTrain },
            { "maxCarsPerTrain", ride.maxCarsPerTrain },
            { "minWaitingTime", ride.minWaitingTime },
            { "maxWaitingTime", ride.maxWaitingTime },
            { "departFlags", ride.departFlags },
            { "operationOption", ride.operationOption },
            { "liftHillSpeed", ride.liftHillSpeed },
            { "mode", static_cast<uint8_t>(ride.mode) },
            { "music", ride.music },
            { "prices", { ride.price[0], ride.price[1] } },
            { "value", ride.value },
            { "satisfaction", ride.satisfaction },
            { "popularity", ride.popularity },
            { "buildDate", ride.buildDate },
            { "ratings", { ride.ratings.excitement, ride.ratings.intensity, ride.ratings.nausea } },
            { "breakdownReason", static_cast<uint8_t>(ride.breakdownReason) },
            { "upkeepCost", ride.upkeepCost },
            { "reliability", ride.reliability },
            { "unreliabilityFactor", ride.unreliabilityFactor },
            { "inspectionInterval", static_cast<uint8_t>(ride.inspectionInterval) },
            { "lastCrashType", ride.lastCrashType },
            { "incomePerHour", ride.incomePerHour },
            { "profit", ride.profit },
            { "entranceStyle", ride.entranceStyle },
            { "numCircuits", ride.numCircuits },
        };
    }

    json_t RideCreateAllocationProjection(const GameState_t& state)
    {
        json_t rides = json_t::array();
        for (const auto& ride : state.rides)
        {
            if (!ride.id.IsNull())
                rides.push_back(RideCreateRideProjection(ride));
        }
        return {
            { "nextFree", GetNextFreeRideId().IsNull() ? json_t(nullptr) : json_t(GetNextFreeRideId().ToUnderlying()) },
            { "count", RideGetCount() },
            { "rides", std::move(rides) },
        };
    }

    void ExpectRideCreateRejected(
        GameState_t& state, const json_t& args, GameActions::Status status, StringId title, StringId message,
        std::string_view code)
    {
        const auto before = RideCreateAllocationProjection(state);
        const auto expected = RideCreateRejectedResult(status, title, message, code);
        const auto queried = QueryNativeAction("RideCreateAction", args, state);
        ASSERT_TRUE(queried.ok) << queried.message;
        EXPECT_EQ(queried.value, expected);
        EXPECT_EQ(RideCreateAllocationProjection(state), before);

        const auto executed = ExecuteNativeAction("RideCreateAction", args, state);
        ASSERT_TRUE(executed.ok) << executed.message;
        EXPECT_EQ(executed.value, expected);
        EXPECT_EQ(RideCreateAllocationProjection(state), before);
    }

    void ExpectRideCreateAssignments(const Ride& ride, const GameState_t& state, const json_t& args)
    {
        const auto rideType = args.at("rideType").get<ride_type_t>();
        const auto rideObject = args.at("rideObject").get<ObjectEntryIndex>();
        const auto* entry = GetRideEntryByIndex(ride.subtype);
        ASSERT_NE(entry, nullptr);
        const auto& descriptor = GetRideTypeDescriptor(rideType);
        ASSERT_LT(args.at("colour1").get<uint8_t>(), descriptor.ColourPresets.count);

        EXPECT_EQ(ride.type, rideType);
        if (rideObject != kObjectEntryIndexNull)
            EXPECT_EQ(ride.subtype, rideObject);
        if (rideType == static_cast<ride_type_t>(81))
        {
            EXPECT_TRUE(ride.customName.empty());
            EXPECT_EQ(ride.defaultNameNumber, 1);
        }
        for (const auto& colour : ride.trackColours)
        {
            EXPECT_EQ(colour.main, descriptor.ColourPresets.list[args.at("colour1").get<uint8_t>()].main);
            EXPECT_EQ(colour.additional, descriptor.ColourPresets.list[args.at("colour1").get<uint8_t>()].additional);
            EXPECT_EQ(colour.supports, descriptor.ColourPresets.list[args.at("colour1").get<uint8_t>()].supports);
        }
        const auto* presets = entry->vehicle_preset_list;
        if (presets->count > 0 && presets->count != 255)
        {
            EXPECT_EQ(ride.vehicleColourSettings, VehicleColourSettings::same);
            EXPECT_EQ(ride.vehicleColours[0].Body, presets->list[args.at("colour2").get<uint8_t>()].Body);
            EXPECT_EQ(ride.vehicleColours[0].Trim, presets->list[args.at("colour2").get<uint8_t>()].Trim);
            EXPECT_EQ(ride.vehicleColours[0].Tertiary, presets->list[args.at("colour2").get<uint8_t>()].Tertiary);
        }
        else
        {
            EXPECT_EQ(ride.vehicleColourSettings, VehicleColourSettings::perTrain);
        }

        EXPECT_TRUE(ride.overallView.IsNull());
        for (const auto& station : ride.getStations())
        {
            EXPECT_TRUE(station.Start.IsNull());
            EXPECT_TRUE(station.Entrance.IsNull());
            EXPECT_TRUE(station.Exit.IsNull());
        }
        EXPECT_EQ(ride.status, RideStatus::closed);
        EXPECT_EQ(ride.numTrains, 1);
        EXPECT_EQ(ride.maxTrains, Limits::kMaxTrainsPerRide);
        EXPECT_EQ(ride.numCarsPerTrain, 1);
        EXPECT_EQ(ride.proposedNumCarsPerTrain, entry->max_cars_in_train);
        const auto expectedProposedTrains = state.cheats.disableTrainLengthLimit
            ? (entry->cars_per_flat_ride == kNoFlatRideCars ? 12 : entry->cars_per_flat_ride)
            : 32;
        EXPECT_EQ(ride.proposedNumTrains, expectedProposedTrains);
        EXPECT_EQ(ride.minWaitingTime, 10);
        EXPECT_EQ(ride.maxWaitingTime, 60);
        EXPECT_EQ(ride.departFlags, RIDE_DEPART_WAIT_FOR_MINIMUM_LENGTH | 3);
        EXPECT_EQ(
            ride.operationOption, (descriptor.OperatingSettings.MinValue * 3 + descriptor.OperatingSettings.MaxValue) / 4);
        EXPECT_EQ(ride.liftHillSpeed, descriptor.LiftData.minimum_speed);
        EXPECT_EQ(ride.mode, ride.getDefaultMode());
        EXPECT_EQ(ride.musicTuneId, kTuneIDNull);
        if (rideType == static_cast<ride_type_t>(81))
        {
            auto& objectManager = GetContext()->GetObjectManager();
            const auto expectedMusic = objectManager.GetLoadedObjectEntryIndex(descriptor.DefaultMusic);
            ASSERT_NE(expectedMusic, kObjectEntryIndexNull);
            EXPECT_EQ(ride.music, expectedMusic);
            EXPECT_FALSE(ride.flags.has(RideFlag::music));
            EXPECT_TRUE(descriptor.flags.has(RtdFlag::allowMusic));
            EXPECT_FALSE(descriptor.flags.has(RtdFlag::hasMusicByDefault));
            EXPECT_EQ(state.park.flags & PARK_FLAGS_NO_MONEY, 0);
            ASSERT_GT(state.park.entranceFee, 0);
            EXPECT_EQ(ride.price[0], 0);
            EXPECT_EQ(ride.price[1], 0);
        }
        EXPECT_TRUE(ride.ratings.isNull());
        EXPECT_EQ(ride.value, kRideValueUndefined);
        EXPECT_EQ(ride.satisfaction, 255);
        EXPECT_EQ(ride.popularity, 255);
        EXPECT_EQ(ride.buildDate, state.date.GetMonthsElapsed());
        EXPECT_EQ(ride.breakdownReason, Breakdown::none);
        EXPECT_EQ(ride.upkeepCost, kMoney64Undefined);
        EXPECT_EQ(ride.reliability, kRideInitialReliability);
        EXPECT_EQ(ride.unreliabilityFactor, 1);
        EXPECT_EQ(ride.inspectionInterval, static_cast<RideInspection>(args.at("inspectionInterval").get<uint8_t>()));
        EXPECT_EQ(ride.lastCrashType, RIDE_CRASH_TYPE_NONE);
        EXPECT_EQ(ride.incomePerHour, kMoney64Undefined);
        EXPECT_EQ(ride.profit, kMoney64Undefined);
        EXPECT_EQ(ride.entranceStyle, args.at("entranceObject").get<ObjectEntryIndex>());
        EXPECT_EQ(ride.numCircuits, 1);
        if (state.park.flags & PARK_FLAGS_NO_MONEY)
        {
            EXPECT_EQ(ride.price[0], 0);
            EXPECT_EQ(ride.price[1], 0);
        }
        if (state.scenarioOptions.objective.Type == Scenario::ObjectiveType::buildTheBest)
            EXPECT_EQ(ride.price[0], 0);
        EXPECT_EQ(ride.minCarsPerTrain, entry->min_cars_in_train);
        EXPECT_EQ(ride.maxCarsPerTrain, entry->max_cars_in_train);
    }

    class NativeActionContractHarness : public testing::Test
    {
    protected:
        std::unique_ptr<IContext> _context;

        void LoadPark(const std::string& name)
        {
            _context.reset();
            gOpenRCT2Headless = true;
            gOpenRCT2NoGraphics = true;
            _context = OpenRCT2::CreateContext();
            ASSERT_NE(_context, nullptr);
            const auto resources = std::filesystem::current_path() / "OpenRCT2.app/Contents/Resources";
            _context->GetPlatformEnvironment().SetBasePath(OpenRCT2::DirBase::openrct2, resources.string());
            ASSERT_TRUE(_context->Initialise());

            auto importer = OpenRCT2::ParkImporter::CreateS6(_context->GetObjectRepository());
            auto loadResult = importer->LoadSavedGame(TestData::GetParkPath(name).c_str(), false);
            _context->GetObjectManager().LoadObjects(loadResult.RequiredObjects);
            importer->Import(OpenRCT2::getGameState());
        }

        NativeActionFixture MakeEntranceFixture()
        {
            NativeActionFixture fixture;
            const auto legalArgs = std::make_shared<json_t>();
            fixture.registration = "RideEntranceExitPlaceAction";
            fixture.ownerFamily = "station-track-maze";
            fixture.publicUse = "paused monitor construction of a ride entrance";
            fixture.untrustedParameters = { "x", "y", "direction", "ride", "station", "isExit" };
            fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
            fixture.prepareLegalState = [legalArgs](GameState_t& state) {
                state.cheats.disableClearanceChecks = true;
                state.cheats.sandboxMode = true;
                auto* ride = FindRideWithEntranceAndTrack();
                if (ride == nullptr)
                    return;
                ride->status = RideStatus::closed;
                const auto station = StationIndex::FromUnderlying(0);
                const auto endpoint = ride->getStation(station).Entrance;
                if (endpoint.IsNull())
                    return;
                *legalArgs = {
                    { "x", endpoint.ToCoordsXY().x },
                    { "y", endpoint.ToCoordsXY().y },
                    { "direction", static_cast<uint8_t>(endpoint.direction) },
                    { "ride", ride->id.ToUnderlying() },
                    { "station", 0 },
                    { "isExit", false },
                };
                auto remove = GameActions::RideEntranceExitRemoveAction(endpoint.ToCoordsXY(), ride->id, station, false);
                remove.SetFlags(
                    {
                        GameActions::CommandFlag::apply,
                        GameActions::CommandFlag::allowDuringPaused,
                        GameActions::CommandFlag::noSpend,
                    });
                ExecuteSetupAction(remove, state);
            };
            fixture.legalArgs = [legalArgs] { return *legalArgs; };
            fixture.semanticInvalidPartitions = {
                { "x", [](json_t& args) { args["x"] = -1; } },
                { "y", [](json_t& args) { args["y"] = -1; } },
                { "direction", [](json_t& args) { args["direction"] = 4; } },
                { "ride", [](json_t& args) { args["ride"] = 65535; } },
                { "station", [](json_t& args) { args["station"] = 255; } },
                { "isExit",
                  [](json_t& args) {
                      args["isExit"] = true;
                      args["x"] = -1;
                      args["y"] = -1;
                  } },
            };
            fixture.mutateRelevantState = [](GameState_t&, const json_t& args) {
                const auto rideValue = args.value("ride", -1);
                if (rideValue >= 0 && rideValue < Limits::kMaxRidesInPark)
                {
                    if (auto* ride = GetRide(RideId::FromUnderlying(rideValue)); ride != nullptr)
                        ride->status = RideStatus::open;
                }
            };
            fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                const auto rideValue = args.value("ride", -1);
                json_t projection = {
                    { "cash", state.park.cash },
                    { "tileElements", TileElementCount({ args.value("x", 0), args.value("y", 0) }) },
                };
                const auto stationValue = args.value("station", 0);
                if (rideValue >= 0 && static_cast<size_t>(rideValue) < state.rides.size() && stationValue >= 0
                    && stationValue < Limits::kMaxStationsPerRide)
                {
                    const auto& ride = state.rides[static_cast<size_t>(rideValue)];
                    const auto& station = ride.getStation(StationIndex::FromUnderlying(stationValue));
                    projection["rideStatus"] = static_cast<uint8_t>(ride.status);
                    projection["entrance"] = station.Entrance.IsNull() ? json_t(nullptr)
                                                                       : json_t{
                                                                             { "x", station.Entrance.x },
                                                                             { "y", station.Entrance.y },
                                                                             { "z", station.Entrance.z },
                                                                             { "direction", station.Entrance.direction },
                                                                         };
                }
                return projection;
            };
            fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                const auto rideValue = args.value("ride", -1);
                json_t projection = {
                    { "tileElements", TileElementCount({ args.value("x", 0), args.value("y", 0) }) },
                };
                const auto stationValue = args.value("station", 0);
                if (rideValue >= 0 && static_cast<size_t>(rideValue) < state.rides.size() && stationValue >= 0
                    && stationValue < Limits::kMaxStationsPerRide)
                {
                    const auto& station = state.rides[static_cast<size_t>(rideValue)].getStation(
                        StationIndex::FromUnderlying(stationValue));
                    projection["entrance"] = station.Entrance.IsNull() ? json_t(nullptr)
                                                                       : json_t{
                                                                             { "x", station.Entrance.x },
                                                                             { "y", station.Entrance.y },
                                                                             { "z", station.Entrance.z },
                                                                             { "direction", station.Entrance.direction },
                                                                         };
                }
                return projection;
            };
            fixture.makeOrdinaryAction = [](const json_t& args) {
                return std::make_unique<GameActions::RideEntranceExitPlaceAction>(
                    CoordsXY{ args.at("x").get<int32_t>(), args.at("y").get<int32_t>() },
                    static_cast<Direction>(args.at("direction").get<uint32_t>()),
                    RideId::FromUnderlying(args.at("ride").get<uint32_t>()),
                    StationIndex::FromUnderlying(args.at("station").get<uint32_t>()), args.at("isExit").get<bool>());
            };
            return fixture;
        }

        bool RunFixture(const NativeActionFixture& fixture, std::string& failure)
        {
            const auto descriptors = NativeActions();
            const auto descriptor = std::find_if(
                descriptors.begin(), descriptors.end(), [&](const auto& item) { return fixture.registration == item.name; });
            if (descriptor == descriptors.end())
            {
                failure = "fixture registration is not in NativeActions census";
                return false;
            }
            if (fixture.publicUse.empty() == fixture.withdrawalReason.empty())
            {
                failure = "fixture must declare public use or withdrawal reason";
                return false;
            }
            if (fixture.untrustedParameters.empty() || fixture.semanticInvalidPartitions.empty())
            {
                failure = "fixture has no semantic parameter partitions";
                return false;
            }
            std::set<std::string> parameters(fixture.untrustedParameters.begin(), fixture.untrustedParameters.end());
            std::set<std::string> partitions;
            for (const auto& partition : fixture.semanticInvalidPartitions)
                partitions.insert(partition.parameter);
            for (const auto& parameter : parameters)
            {
                if (!partitions.contains(parameter))
                {
                    failure = "uncovered semantic parameter: " + parameter;
                    return false;
                }
            }
            if (fixture.transportOmissions.empty())
            {
                failure = "transport omissions are not classified";
                return false;
            }

            // Every semantic invalid partition is run from a fresh legal state and
            // its projection is sampled on both sides of public execution.
            for (const auto& partition : fixture.semanticInvalidPartitions)
            {
                LoadPark("small_park_with_ferris_wheel.sv6");
                auto& state = OpenRCT2::getGameState();
                fixture.prepareLegalState(state);
                auto args = fixture.legalArgs();
                if (args.empty())
                {
                    failure = "legal starting args are empty";
                    return false;
                }
                partition.makeInvalid(args);
                const auto queried = QueryNativeAction(fixture.registration, args, state);
                if (!queried.ok || queried.value.value("accepted", true))
                {
                    failure = "semantic invalid partition was accepted: " + partition.parameter;
                    return false;
                }
                const auto before = fixture.rejectionProjection(state, args);
                if (before.empty())
                {
                    failure = "rejection projection is empty for partition: " + partition.parameter;
                    return false;
                }
                const auto executed = ExecuteNativeAction(fixture.registration, args, state);
                if (!executed.ok || executed.value.value("accepted", true))
                {
                    failure = "execution accepted semantic invalid partition: " + partition.parameter;
                    return false;
                }
                if (before != fixture.rejectionProjection(state, args))
                {
                    failure = "rejected execution mutated the declared projection: " + partition.parameter;
                    return false;
                }
            }

            // A successful query is intentionally made stale by the owner-state
            // mutator. Public execution must query again rather than trusting it.
            LoadPark("small_park_with_ferris_wheel.sv6");
            auto& staleState = OpenRCT2::getGameState();
            fixture.prepareLegalState(staleState);
            const auto staleArgs = fixture.legalArgs();
            const auto staleQuery = QueryNativeAction(fixture.registration, staleArgs, staleState);
            if (!staleQuery.ok || !staleQuery.value.value("accepted", false))
            {
                failure = "legal fixture query did not accept before stale-state mutation";
                return false;
            }
            fixture.mutateRelevantState(staleState, staleArgs);
            const auto staleBefore = fixture.rejectionProjection(staleState, staleArgs);
            const auto staleExecution = ExecuteNativeAction(fixture.registration, staleArgs, staleState);
            if (!staleExecution.ok || staleExecution.value.value("accepted", true))
            {
                failure = "execution did not perform a fresh query after relevant-state mutation";
                return false;
            }
            if (staleBefore.empty() || staleBefore != fixture.rejectionProjection(staleState, staleArgs))
            {
                failure = "stale-query rejection mutated the declared projection";
                return false;
            }

            // Independent fresh states are used for ordinary and synchronous-public
            // accepted execution. Only the paused transport omissions are excluded.
            LoadPark("small_park_with_ferris_wheel.sv6");
            auto& ordinaryState = OpenRCT2::getGameState();
            fixture.prepareLegalState(ordinaryState);
            const auto ordinaryArgs = fixture.legalArgs();
            const auto ordinaryBefore = ordinaryState.park.cash;
            auto ordinaryAction = fixture.makeOrdinaryAction(ordinaryArgs);
            ordinaryAction->SetFlags({ GameActions::CommandFlag::apply, GameActions::CommandFlag::allowDuringPaused });
            const auto oldInUpdateCode = gInUpdateCode;
            gInUpdateCode = true;
            const auto ordinaryResult = GameActions::Execute(ordinaryAction.get(), ordinaryState);
            gInUpdateCode = oldInUpdateCode;
            if (ordinaryResult.error != GameActions::Status::ok)
            {
                failure = "ordinary execution rejected legal fixture";
                return false;
            }
            const auto ordinaryCashDelta = ordinaryState.park.cash - ordinaryBefore;
            const auto ordinaryPost = fixture.acceptedPostStateProjection(ordinaryState, ordinaryArgs);
            if (ordinaryPost.empty())
            {
                failure = "accepted post-state projection is empty";
                return false;
            }

            LoadPark("small_park_with_ferris_wheel.sv6");
            auto& publicState = OpenRCT2::getGameState();
            fixture.prepareLegalState(publicState);
            const auto publicArgs = fixture.legalArgs();
            const auto publicBefore = publicState.park.cash;
            const auto publicQuery = QueryNativeAction(fixture.registration, publicArgs, publicState);
            if (!publicQuery.ok || !publicQuery.value.value("accepted", false))
            {
                failure = "public query rejected legal fixture";
                return false;
            }
            const auto publicExecution = ExecuteNativeAction(fixture.registration, publicArgs, publicState);
            if (!publicExecution.ok || !publicExecution.value.value("accepted", false))
            {
                failure = "public synchronous execution rejected legal fixture";
                return false;
            }
            const json_t ordinaryPosition{
                { "x", ordinaryResult.position.x },
                { "y", ordinaryResult.position.y },
                { "z", ordinaryResult.position.z },
            };
            if (publicExecution.value["status"] != static_cast<uint16_t>(ordinaryResult.error)
                || publicExecution.value["cost"] != ordinaryResult.cost
                || publicExecution.value["position"] != ordinaryPosition)
            {
                failure = "ordinary and public result/status/cost/position diverged";
                return false;
            }
            if (publicState.park.cash - publicBefore != ordinaryCashDelta)
            {
                failure = "ordinary and public finance delta diverged";
                return false;
            }
            if (ordinaryPost != fixture.acceptedPostStateProjection(publicState, publicArgs))
            {
                failure = "ordinary and public accepted post-state diverged";
                return false;
            }
            return true;
        }
    };

} // namespace

TEST(NativeActionContractInventory, DeclaresOneNonEmptyOwnerFixtureRow)
{
    const std::vector<InventoryRow> rows{
        {
            "RideEntranceExitPlaceAction",
            "station-track-maze",
            "paused monitor construction of a ride entrance",
            {},
            { { "x", 1 }, { "y", 1 } },
            { "x", "y" },
            { "x", "y" },
            { { "tileElements", 1 } },
            { { "tileElements", 2 } },
        },
    };
    const auto result = ValidateInventory(rows, { { "station-track-maze", 1 } }, { "RideEntranceExitPlaceAction" });
    EXPECT_TRUE(result.ok) << result.failure;
}

TEST(NativeActionContractInventory, RejectsDuplicateRegistration)
{
    auto row = InventoryRow{
        "RideEntranceExitPlaceAction", "station-track-maze", "use", {}, { { "x", 1 } }, { "x" }, { "x" },
        { { "domain", 1 } },           { { "domain", 2 } },
    };
    const auto result = ValidateInventory({ row, row }, { { "station-track-maze", 2 } }, { "RideEntranceExitPlaceAction" });
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.failure.find("duplicate fixture registration"), std::string::npos);
}

TEST(NativeActionContractInventory, RejectsMissingRegistration)
{
    const std::vector<InventoryRow> rows{
        { "MissingAction",
          "station-track-maze",
          "use",
          {},
          { { "x", 1 } },
          { "x" },
          { "x" },
          { { "domain", 1 } },
          { { "domain", 2 } } },
    };
    const auto result = ValidateInventory(rows, { { "station-track-maze", 1 } }, { "RideEntranceExitPlaceAction" });
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.failure.find("missing from the engine census"), std::string::npos);
}

TEST(NativeActionContractInventory, RejectsMissingOwnerFamily)
{
    const std::vector<InventoryRow> rows{
        { "RideEntranceExitPlaceAction",
          {},
          "use",
          {},
          { { "x", 1 } },
          { "x" },
          { "x" },
          { { "domain", 1 } },
          { { "domain", 2 } } },
    };
    const auto result = ValidateInventory(rows, { { "station-track-maze", 1 } }, { "RideEntranceExitPlaceAction" });
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.failure.find("no owner family"), std::string::npos);
}

TEST(NativeActionContractInventory, RejectsZeroExpectedFamilyPopulation)
{
    const std::vector<InventoryRow> rows{
        { "RideEntranceExitPlaceAction",
          "station-track-maze",
          "use",
          {},
          { { "x", 1 } },
          { "x" },
          { "x" },
          { { "domain", 1 } },
          { { "domain", 2 } } },
    };
    const auto result = ValidateInventory(rows, { { "station-track-maze", 0 } }, { "RideEntranceExitPlaceAction" });
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.failure.find("zero expected population"), std::string::npos);
}

class NativeActionContractRideCreate : public NativeActionContractHarness
{
protected:
    void LoadPark(const std::string& name)
    {
        NativeActionContractHarness::LoadPark(name);
        auto& objectManager = GetContext()->GetObjectManager();
        std::vector<ObjectEntryDescriptor> unload;
        if (auto* object = objectManager.GetLoadedObject(ObjectType::ride, 10); object != nullptr)
            unload.push_back(object->GetDescriptor());
        const auto enterpriseSlot = objectManager.GetLoadedObjectEntryIndex("rct2.ride.enterp");
        if (enterpriseSlot != kObjectEntryIndexNull && enterpriseSlot != 10)
        {
            if (auto* object = objectManager.GetLoadedObject(ObjectType::ride, enterpriseSlot); object != nullptr)
                unload.push_back(object->GetDescriptor());
        }
        if (!unload.empty())
            objectManager.UnloadObjects(unload);
        ASSERT_NE(objectManager.LoadObject(ObjectEntryDescriptor("rct2.ride.enterp"), 10), nullptr);
        ASSERT_NE(GetRideEntryByIndex(static_cast<ObjectEntryIndex>(10)), nullptr);
        ASSERT_EQ(
            GetRideEntryByIndex(static_cast<ObjectEntryIndex>(10))->GetFirstNonNullRideType(), static_cast<ride_type_t>(81));
    }

    void LoadResearchObjects()
    {
        LoadPark("small_park_with_ferris_wheel.sv6");
        auto& objectManager = GetContext()->GetObjectManager();
        std::vector<ObjectEntryDescriptor> unload;
        for (const auto slot : { static_cast<ObjectEntryIndex>(10), static_cast<ObjectEntryIndex>(11) })
        {
            if (auto* object = objectManager.GetLoadedObject(ObjectType::ride, slot); object != nullptr)
                unload.push_back(object->GetDescriptor());
        }
        if (!unload.empty())
            objectManager.UnloadObjects(unload);
        unload.clear();

        for (const auto* objectName : { "rct2.ride.ptct1", "rct2.ride.ptct2" })
        {
            const auto slot = objectManager.GetLoadedObjectEntryIndex(objectName);
            if (slot != kObjectEntryIndexNull)
            {
                auto* object = objectManager.GetLoadedObject(ObjectType::ride, slot);
                ASSERT_NE(object, nullptr);
                unload.push_back(object->GetDescriptor());
            }
        }
        if (!unload.empty())
            objectManager.UnloadObjects(unload);

        ASSERT_NE(objectManager.LoadObject(ObjectEntryDescriptor("rct2.ride.ptct1"), 10), nullptr);
        ASSERT_NE(objectManager.LoadObject(ObjectEntryDescriptor("rct2.ride.ptct2"), 11), nullptr);
        const auto& entries = objectManager.GetAllRideEntries(RIDE_TYPE_WOODEN_ROLLER_COASTER);
        ASSERT_EQ(entries.size(), 2u);
        EXPECT_EQ(entries[0], static_cast<ObjectEntryIndex>(10));
        EXPECT_EQ(entries[1], static_cast<ObjectEntryIndex>(11));
        for (const auto entryIndex : entries)
        {
            const auto* entry = GetRideEntryByIndex(entryIndex);
            ASSERT_NE(entry, nullptr);
            EXPECT_NE(std::ranges::find(entry->ride_type, RIDE_TYPE_WOODEN_ROLLER_COASTER), std::end(entry->ride_type));
        }
    }
};

TEST_F(NativeActionContractRideCreate, MalformedEnterpriseObjectRejectedBeforeAllocation)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& state = OpenRCT2::getGameState();
    const auto* entry = GetRideEntryByIndex(static_cast<ObjectEntryIndex>(10));
    ASSERT_NE(entry, nullptr) << "Enterprise object slot 10 is required by this direct fixture";
    EXPECT_NE(std::ranges::find(entry->ride_type, static_cast<ride_type_t>(81)), std::end(entry->ride_type));
    EXPECT_EQ(std::ranges::find(entry->ride_type, static_cast<ride_type_t>(33)), std::end(entry->ride_type));
    ExpectRideCreateRejected(
        state, RideCreateArgs(static_cast<ride_type_t>(33)), GameActions::Status::invalidParameters,
        STR_CANT_CREATE_NEW_RIDE_ATTRACTION, STR_INVALID_RIDE_TYPE, "invalid_parameters");
}

TEST_F(NativeActionContractRideCreate, RejectsInspectedBoundsAndPreservesAllocation)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& inspectionState = OpenRCT2::getGameState();
    ExpectRideCreateRejected(
        inspectionState, RideCreateArgs(81, 10, 0, 0, 10, static_cast<RideInspection>(7)),
        GameActions::Status::invalidParameters, STR_CANT_CHANGE_OPERATING_MODE, STR_ERR_VALUE_OUT_OF_RANGE,
        "invalid_parameters");

    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& typeState = OpenRCT2::getGameState();
    ExpectRideCreateRejected(
        typeState, RideCreateArgs(static_cast<ride_type_t>(RIDE_TYPE_COUNT)), GameActions::Status::invalidParameters,
        STR_CANT_CREATE_NEW_RIDE_ATTRACTION, STR_INVALID_RIDE_TYPE, "invalid_parameters");

    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& colourState = OpenRCT2::getGameState();
    const auto colourCount = GetRideTypeDescriptor(static_cast<ride_type_t>(81)).ColourPresets.count;
    ASSERT_LT(colourCount, 255);
    ExpectRideCreateRejected(
        colourState, RideCreateArgs(81, 10, colourCount), GameActions::Status::invalidParameters,
        STR_CANT_CREATE_NEW_RIDE_ATTRACTION, STR_ERR_INVALID_COLOUR, "invalid_parameters");

    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& objectState = OpenRCT2::getGameState();
    ExpectRideCreateRejected(
        objectState, RideCreateArgs(81, static_cast<ObjectEntryIndex>(254)), GameActions::Status::invalidParameters,
        STR_CANT_CREATE_NEW_RIDE_ATTRACTION, STR_UNKNOWN_OBJECT_TYPE, "invalid_parameters");

    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& vehicleState = OpenRCT2::getGameState();
    const auto* entry = GetRideEntryByIndex(static_cast<ObjectEntryIndex>(10));
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->vehicle_preset_list, nullptr);
    ASSERT_GT(entry->vehicle_preset_list->count, 0);
    ASSERT_LT(entry->vehicle_preset_list->count, 255);
    ExpectRideCreateRejected(
        vehicleState, RideCreateArgs(81, 10, 0, entry->vehicle_preset_list->count), GameActions::Status::invalidParameters,
        STR_CANT_CREATE_NEW_RIDE_ATTRACTION, kStringIdNone, "invalid_parameters");
}

TEST_F(NativeActionContractRideCreate, NullSubtypeWithNoLoadedEntriesRejectsBeforeAllocation)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& objectManager = GetContext()->GetObjectManager();
    auto* enterprise = objectManager.GetLoadedObject(ObjectType::ride, static_cast<ObjectEntryIndex>(10));
    ASSERT_NE(enterprise, nullptr);
    objectManager.UnloadObjects({ enterprise->GetDescriptor() });
    auto& entries = objectManager.GetAllRideEntries(static_cast<ride_type_t>(81));
    ASSERT_TRUE(entries.empty());
    ExpectRideCreateRejected(
        OpenRCT2::getGameState(), RideCreateArgs(81, static_cast<ObjectEntryIndex>(kObjectEntryIndexNull)),
        GameActions::Status::invalidParameters, STR_CANT_CREATE_NEW_RIDE_ATTRACTION, STR_INVALID_RIDE_TYPE,
        "invalid_parameters");
}

TEST_F(NativeActionContractRideCreate, RejectsFreeSlotExhaustionBeforeAllocation)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& state = OpenRCT2::getGameState();
    for (RideId::UnderlyingType i = 0; i < Limits::kMaxRidesInPark; ++i)
    {
        const auto rideId = RideId::FromUnderlying(i);
        if (GetRide(rideId) == nullptr)
            ASSERT_NE(RideAllocateAtIndex(rideId), nullptr);
    }
    ASSERT_TRUE(GetNextFreeRideId().IsNull());
    ExpectRideCreateRejected(
        state, RideCreateArgs(), GameActions::Status::noFreeElements, STR_CANT_CREATE_NEW_RIDE_ATTRACTION, STR_TOO_MANY_RIDES,
        "no_free_elements");
}

TEST_F(NativeActionContractRideCreate, AcceptedEnterpriseMatchesOrdinaryExecutionAndAssignedFields)
{
    const auto args = RideCreateArgs();

    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& ordinaryState = OpenRCT2::getGameState();
    const auto ordinaryTarget = GetNextFreeRideId();
    auto ordinaryAction = std::make_unique<GameActions::RideCreateAction>(
        args.at("rideType").get<ride_type_t>(), args.at("rideObject").get<ObjectEntryIndex>(),
        args.at("colour1").get<uint8_t>(), args.at("colour2").get<uint8_t>(), args.at("entranceObject").get<ObjectEntryIndex>(),
        static_cast<RideInspection>(args.at("inspectionInterval").get<uint8_t>()));
    ordinaryAction->SetFlags({ GameActions::CommandFlag::apply, GameActions::CommandFlag::allowDuringPaused });
    const auto ordinaryCashBefore = ordinaryState.park.cash;
    const auto oldInUpdateCode = gInUpdateCode;
    gInUpdateCode = true;
    const auto ordinaryResult = GameActions::Execute(ordinaryAction.get(), ordinaryState);
    gInUpdateCode = oldInUpdateCode;
    ASSERT_EQ(ordinaryResult.error, GameActions::Status::ok);
    ASSERT_EQ(ordinaryResult.expenditure, ExpenditureType::rideConstruction);
    ASSERT_EQ(ordinaryResult.cost, 0);
    ASSERT_EQ(ordinaryResult.getData<RideId>(), ordinaryTarget);
    ASSERT_NE(GetRide(ordinaryTarget), nullptr);
    ExpectRideCreateAssignments(*GetRide(ordinaryTarget), ordinaryState, args);
    const auto ordinaryProjection = RideCreateRideProjection(*GetRide(ordinaryTarget));

    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& publicState = OpenRCT2::getGameState();
    const auto publicTarget = GetNextFreeRideId();
    const auto publicCashBefore = publicState.park.cash;
    const auto publicQuery = QueryNativeAction("RideCreateAction", args, publicState);
    ASSERT_TRUE(publicQuery.ok) << publicQuery.message;
    ASSERT_TRUE(publicQuery.value["accepted"]) << publicQuery.value.dump();
    const auto publicExecution = ExecuteNativeAction("RideCreateAction", args, publicState);
    ASSERT_TRUE(publicExecution.ok) << publicExecution.message;
    ASSERT_TRUE(publicExecution.value["accepted"]) << publicExecution.value.dump();
    ASSERT_EQ(publicTarget, ordinaryTarget);
    ASSERT_NE(GetRide(publicTarget), nullptr);
    ExpectRideCreateAssignments(*GetRide(publicTarget), publicState, args);
    EXPECT_EQ(publicState.park.cash - publicCashBefore, ordinaryState.park.cash - ordinaryCashBefore);
    EXPECT_EQ(publicExecution.value, [&] {
        auto expected = RideCreateResultProjection(ordinaryResult);
        expected["action"] = "RideCreateAction";
        return expected;
    }());
    EXPECT_EQ(RideCreateRideProjection(*GetRide(publicTarget)), ordinaryProjection);
}

TEST_F(NativeActionContractRideCreate, NullSubtypeSelectionHonorsResearchAndIgnoreResearch)
{
    for (const bool ignoreResearch : { false, true })
    {
        LoadResearchObjects();
        auto& state = OpenRCT2::getGameState();
        const auto& entries = GetContext()->GetObjectManager().GetAllRideEntries(RIDE_TYPE_WOODEN_ROLLER_COASTER);
        ASSERT_EQ(entries.size(), 2u);
        ASSERT_FALSE(GetRideTypeDescriptor(RIDE_TYPE_WOODEN_ROLLER_COASTER).flags.has(RtdFlag::listVehiclesSeparately));
        SetEveryRideEntryNotInvented();
        for (const auto entry : entries)
            ASSERT_FALSE(RideEntryIsInvented(entry));
        const auto frontEntry = entries.front();
        const auto researchedEntry = entries.back();
        ASSERT_NE(frontEntry, researchedEntry);
        RideEntrySetInvented(researchedEntry);
        ASSERT_FALSE(RideEntryIsInvented(frontEntry));
        ASSERT_TRUE(RideEntryIsInvented(researchedEntry));
        ASSERT_NE(
            std::ranges::find(GetRideEntryByIndex(researchedEntry)->ride_type, RIDE_TYPE_WOODEN_ROLLER_COASTER),
            std::end(GetRideEntryByIndex(researchedEntry)->ride_type));
        state.cheats.ignoreResearchStatus = ignoreResearch;
        auto args = RideCreateArgs(RIDE_TYPE_WOODEN_ROLLER_COASTER, static_cast<ObjectEntryIndex>(kObjectEntryIndexNull));
        const auto queried = QueryNativeAction("RideCreateAction", args, state);
        ASSERT_TRUE(queried.ok) << queried.message;
        ASSERT_TRUE(queried.value["accepted"]) << queried.value.dump();
        const auto target = GetNextFreeRideId();
        const auto executed = ExecuteNativeAction("RideCreateAction", args, state);
        ASSERT_TRUE(executed.ok) << executed.message;
        ASSERT_TRUE(executed.value["accepted"]) << executed.value.dump();
        ASSERT_NE(GetRide(target), nullptr);
        EXPECT_EQ(GetRide(target)->subtype, ignoreResearch ? frontEntry : researchedEntry);
        ExpectRideCreateAssignments(*GetRide(target), state, args);
    }
}

TEST_F(NativeActionContractRideCreate, PublicExecutionRequeriesFreeSlotPremise)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& state = OpenRCT2::getGameState();
    const auto args = RideCreateArgs();
    const auto queried = QueryNativeAction("RideCreateAction", args, state);
    ASSERT_TRUE(queried.ok) << queried.message;
    ASSERT_TRUE(queried.value["accepted"]) << queried.value.dump();
    for (RideId::UnderlyingType i = 0; i < Limits::kMaxRidesInPark; ++i)
    {
        const auto rideId = RideId::FromUnderlying(i);
        if (GetRide(rideId) == nullptr)
            ASSERT_NE(RideAllocateAtIndex(rideId), nullptr);
    }
    ASSERT_TRUE(GetNextFreeRideId().IsNull());
    const auto before = RideCreateAllocationProjection(state);
    const auto executed = ExecuteNativeAction("RideCreateAction", args, state);
    ASSERT_TRUE(executed.ok) << executed.message;
    EXPECT_FALSE(executed.value["accepted"]) << executed.value.dump();
    EXPECT_EQ(
        executed.value,
        RideCreateRejectedResult(
            GameActions::Status::noFreeElements, STR_CANT_CREATE_NEW_RIDE_ATTRACTION, STR_TOO_MANY_RIDES, "no_free_elements"));
    EXPECT_EQ(RideCreateAllocationProjection(state), before);
}

TEST_F(NativeActionContractRideCreate, MoneyObjectiveAndTrainCheatBranchesRemainOwnerVisible)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& noMoneyState = OpenRCT2::getGameState();
    noMoneyState.park.flags |= PARK_FLAGS_NO_MONEY;
    const auto noMoneyTarget = GetNextFreeRideId();
    const auto noMoneyArgs = RideCreateArgs();
    const auto noMoneyResult = ExecuteNativeAction("RideCreateAction", noMoneyArgs, noMoneyState);
    ASSERT_TRUE(noMoneyResult.ok) << noMoneyResult.message;
    ASSERT_TRUE(noMoneyResult.value["accepted"]) << noMoneyResult.value.dump();
    auto* noMoneyRide = GetRide(noMoneyTarget);
    ASSERT_NE(noMoneyRide, nullptr);
    EXPECT_EQ(noMoneyRide->price[0], 0);
    EXPECT_EQ(noMoneyRide->price[1], 0);

    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& objectiveState = OpenRCT2::getGameState();
    objectiveState.scenarioOptions.objective.Type = Scenario::ObjectiveType::buildTheBest;
    const auto objectiveTarget = GetNextFreeRideId();
    const auto objectiveArgs = RideCreateArgs();
    const auto objectiveResult = ExecuteNativeAction("RideCreateAction", objectiveArgs, objectiveState);
    ASSERT_TRUE(objectiveResult.ok) << objectiveResult.message;
    ASSERT_TRUE(objectiveResult.value["accepted"]) << objectiveResult.value.dump();
    auto* objectiveRide = GetRide(objectiveTarget);
    ASSERT_NE(objectiveRide, nullptr);
    EXPECT_EQ(objectiveRide->price[0], 0);

    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& cheatState = OpenRCT2::getGameState();
    cheatState.cheats.disableTrainLengthLimit = true;
    const auto cheatTarget = GetNextFreeRideId();
    const auto cheatArgs = RideCreateArgs();
    const auto cheatResult = ExecuteNativeAction("RideCreateAction", cheatArgs, cheatState);
    ASSERT_TRUE(cheatResult.ok) << cheatResult.message;
    ASSERT_TRUE(cheatResult.value["accepted"]) << cheatResult.value.dump();
    ASSERT_NE(GetRide(cheatTarget), nullptr);
    const auto* entry = GetRideEntryByIndex(static_cast<ObjectEntryIndex>(10));
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(
        GetRide(cheatTarget)->proposedNumTrains, entry->cars_per_flat_ride == kNoFlatRideCars ? 12 : entry->cars_per_flat_ride);
}

TEST_F(NativeActionContractHarness, RetainedEntranceFixtureConforms)
{
    const auto fixture = MakeEntranceFixture();
    std::string failure;
    ASSERT_TRUE(RunFixture(fixture, failure)) << failure;
}

TEST(NativeActionContractHarnessMutations, CachedQueryMutationRed)
{
    auto observation = HarnessObservation{};
    observation.freshExecuteTimeQuery = false;
    const auto failure = ValidateHarnessObservation(observation);
    EXPECT_NE(failure.find("fresh execute-time query"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, RejectionMutationRed)
{
    auto observation = HarnessObservation{};
    observation.rejectionAfter["tile"] = 9;
    const auto failure = ValidateHarnessObservation(observation);
    EXPECT_NE(failure.find("rejected execution mutated"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, SuppressedChargeRed)
{
    auto observation = HarnessObservation{};
    observation.financeParity = false;
    const auto failure = ValidateHarnessObservation(observation);
    EXPECT_NE(failure.find("finance charge"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, ChangedResultRed)
{
    auto observation = HarnessObservation{};
    observation.resultParity = false;
    const auto failure = ValidateHarnessObservation(observation);
    EXPECT_NE(failure.find("result diverged"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, ChangedPostStateRed)
{
    auto observation = HarnessObservation{};
    observation.postStateParity = false;
    const auto failure = ValidateHarnessObservation(observation);
    EXPECT_NE(failure.find("post-state diverged"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, EmptyProjectionRed)
{
    auto observation = HarnessObservation{};
    observation.rejectionBefore = json_t::object();
    const auto failure = ValidateHarnessObservation(observation);
    EXPECT_NE(failure.find("projection is empty"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, UncoveredSemanticParameterRed)
{
    auto observation = HarnessObservation{};
    observation.semanticPartitions = { "x" };
    const auto failure = ValidateHarnessObservation(observation);
    EXPECT_NE(failure.find("uncovered semantic parameter: ride"), std::string::npos);
}

namespace
{
    constexpr int32_t kTrackOriginX = 320;
    constexpr int32_t kTrackOriginY = 320;
    constexpr int32_t kTrackOriginZ = 16;
    constexpr uint8_t kTrackColour = 2;
    constexpr uint8_t kSeatRotation = 4;

    json_t TrackPlaceArgs(RideId ride)
    {
        return {
            { "x", kTrackOriginX },
            { "y", kTrackOriginY },
            { "z", kTrackOriginZ },
            { "direction", 0 },
            { "ride", ride.ToUnderlying() },
            { "trackType", static_cast<uint16_t>(TrackElemType::flatTrack4x4) },
            { "rideType", static_cast<ride_type_t>(81) },
            { "brakeSpeed", 0 },
            { "colour", kTrackColour },
            { "seatRotation", kSeatRotation },
            { "trackPlaceFlags", 0 },
            { "isFromTrackDesign", false },
        };
    }

    json_t TrackPlaceResultProjection(const GameActions::Result& result)
    {
        return {
            { "status", static_cast<uint16_t>(result.error) },
            { "accepted", result.error == GameActions::Status::ok },
            { "cost", result.cost },
            { "position", { { "x", result.position.x }, { "y", result.position.y }, { "z", result.position.z } } },
        };
    }

    json_t TrackPlaceRejectedResult(GameActions::Status status, StringId title, StringId message, std::string_view code)
    {
        const GameActions::Result expected(status, title, message);
        auto result = TrackPlaceResultProjection(expected);
        result["action"] = "TrackPlaceAction";
        result["rejection"] = {
            { "code", code },
            { "title", expected.getErrorTitle() },
            { "message", expected.getErrorMessage() },
            { "detail", { { "status", static_cast<uint16_t>(status) } } },
        };
        return result;
    }

    json_t TrackElementProjection(const TrackElement& element)
    {
        return {
            { "baseZ", element.getBaseZ() },
            { "clearanceZ", element.getClearanceZ() },
            { "direction", static_cast<uint8_t>(element.getDirection()) },
            { "ghost", element.isGhost() },
            { "ride", element.GetRideIndex().ToUnderlying() },
            { "rideType", element.GetRideType() },
            { "trackType", static_cast<uint16_t>(element.GetTrackType()) },
            { "sequence", element.GetSequenceIndex() },
            { "station", element.GetStationIndex().ToUnderlying() },
            { "colour", element.GetColourScheme() },
            { "seatRotation", element.GetSeatRotation() },
            { "brakeSpeed", element.GetBrakeBoosterSpeed() },
            { "chain", element.HasChain() },
            { "inverted", element.IsInverted() },
        };
    }

    json_t TrackTileProjection(const CoordsXY& location)
    {
        json_t elements = json_t::array();
        auto* element = MapGetFirstElementAt(location);
        while (element != nullptr)
        {
            json_t value = {
                { "type", static_cast<uint8_t>(element->getType()) },
                { "baseZ", element->getBaseZ() },
                { "clearanceZ", element->getClearanceZ() },
                { "direction", static_cast<uint8_t>(element->getDirection()) },
                { "ghost", element->isGhost() },
            };
            if (const auto* track = element->asTrack())
                value["track"] = TrackElementProjection(*track);
            elements.push_back(std::move(value));
            if (element->isLastForTile())
                break;
            ++element;
        }
        return elements;
    }

    json_t TrackPlaceProjection(const GameState_t& state, RideId rideId)
    {
        const auto* ride = GetRide(rideId);
        json_t projection = {
            { "cash", state.park.cash },
            { "tiles", json_t::array() },
        };
        if (ride == nullptr)
        {
            projection["ride"] = nullptr;
            return projection;
        }

        const auto station = ride->getStation(StationIndex::FromUnderlying(0));
        projection["ride"] = {
            { "overallView",
              ride->overallView.IsNull() ? json_t(nullptr)
                                         : json_t{ { "x", ride->overallView.x }, { "y", ride->overallView.y } } },
            { "numStations", ride->numStations },
            { "station0",
              { { "startX", station.Start.x },
                { "startY", station.Start.y },
                { "height", station.Height },
                { "length", station.Length },
                { "depart", station.Depart } } },
            { "numTrains", ride->numTrains },
            { "maxTrains", ride->maxTrains },
            { "numCarsPerTrain", ride->numCarsPerTrain },
            { "minCarsPerTrain", ride->minCarsPerTrain },
            { "maxCarsPerTrain", ride->maxCarsPerTrain },
        };

        auto& tiles = projection["tiles"];
        for (const auto& offset :
             std::array<CoordsXY, 16>{ CoordsXY{ 0, 0 }, CoordsXY{ 0, 32 }, CoordsXY{ 0, 64 }, CoordsXY{ 0, 96 },
                                       CoordsXY{ 32, 0 }, CoordsXY{ 32, 32 }, CoordsXY{ 32, 64 }, CoordsXY{ 32, 96 },
                                       CoordsXY{ 64, 0 }, CoordsXY{ 64, 32 }, CoordsXY{ 64, 64 }, CoordsXY{ 64, 96 },
                                       CoordsXY{ 96, 0 }, CoordsXY{ 96, 32 }, CoordsXY{ 96, 64 }, CoordsXY{ 96, 96 } })
        {
            const CoordsXY location{ kTrackOriginX + offset.x, kTrackOriginY + offset.y };
            tiles.push_back({ { "x", location.x }, { "y", location.y }, { "elements", TrackTileProjection(location) } });
        }
        return projection;
    }

    class NativeActionContractTrackPlace : public NativeActionContractHarness
    {
    protected:
        void LoadEnterprisePark()
        {
            LoadPark("small_park_with_ferris_wheel.sv6");
            auto& objectManager = GetContext()->GetObjectManager();
            std::vector<ObjectEntryDescriptor> unload;
            if (auto* object = objectManager.GetLoadedObject(ObjectType::ride, 10); object != nullptr)
                unload.push_back(object->GetDescriptor());
            const auto enterpriseSlot = objectManager.GetLoadedObjectEntryIndex("rct2.ride.enterp");
            if (enterpriseSlot != kObjectEntryIndexNull && enterpriseSlot != 10)
            {
                if (auto* object = objectManager.GetLoadedObject(ObjectType::ride, enterpriseSlot); object != nullptr)
                    unload.push_back(object->GetDescriptor());
            }
            if (!unload.empty())
                objectManager.UnloadObjects(unload);
            ASSERT_NE(objectManager.LoadObject(ObjectEntryDescriptor("rct2.ride.enterp"), 10), nullptr);
            ASSERT_NE(GetRideEntryByIndex(static_cast<ObjectEntryIndex>(10)), nullptr);
            ASSERT_EQ(
                GetRideEntryByIndex(static_cast<ObjectEntryIndex>(10))->GetFirstNonNullRideType(),
                static_cast<ride_type_t>(81));
        }

        RideId CreateEnterpriseRide(GameState_t& state)
        {
            state.cheats.sandboxMode = true;
            state.cheats.disableClearanceChecks = true;
            const auto rideId = GetNextFreeRideId();
            const auto created = ExecuteNativeAction("RideCreateAction", RideCreateArgs(), state);
            if (!created.ok || !created.value.value("accepted", false))
            {
                ADD_FAILURE() << (created.ok ? created.value.dump() : created.message);
                return RideId::GetNull();
            }
            auto* ride = GetRide(rideId);
            if (ride == nullptr || ride->type != static_cast<ride_type_t>(81)
                || ride->subtype != static_cast<ObjectEntryIndex>(10))
            {
                ADD_FAILURE() << "Enterprise ride was not created in the expected slot and type";
                return RideId::GetNull();
            }
            ride->status = RideStatus::closed;
            ride->overallView = {};
            return rideId;
        }

        void PrepareLegalState(GameState_t& state, RideId& rideId, json_t& args)
        {
            rideId = CreateEnterpriseRide(state);
            args = TrackPlaceArgs(rideId);
            ASSERT_GT(state.mapSize.x, 12);
            ASSERT_GT(state.mapSize.y, 12);
        }

        void ExpectRejected(
            GameState_t& state, RideId rideId, const json_t& legalArgs, const json_t& invalidArgs, GameActions::Status status,
            StringId message)
        {
            const auto expected = TrackPlaceRejectedResult(
                status, STR_RIDE_CONSTRUCTION_CANT_CONSTRUCT_THIS_HERE, message,
                status == GameActions::Status::noClearance ? "no_clearance" : "invalid_parameters");
            const auto before = TrackPlaceProjection(state, rideId);
            const auto queried = QueryNativeAction("TrackPlaceAction", invalidArgs, state);
            ASSERT_TRUE(queried.ok) << queried.message;
            EXPECT_EQ(queried.value, expected);
            EXPECT_EQ(TrackPlaceProjection(state, rideId), before);
            const auto executed = ExecuteNativeAction("TrackPlaceAction", invalidArgs, state);
            ASSERT_TRUE(executed.ok) << executed.message;
            EXPECT_EQ(executed.value, expected);
            EXPECT_EQ(TrackPlaceProjection(state, rideId), before);
            (void)legalArgs;
        }
    };
} // namespace

TEST_F(NativeActionContractTrackPlace, AcceptedEnterpriseFlatTrackHasLiteralResultAndTouchedFields)
{
    LoadEnterprisePark();
    auto& ordinaryState = OpenRCT2::getGameState();
    RideId ordinaryRide{};
    json_t ordinaryArgs;
    PrepareLegalState(ordinaryState, ordinaryRide, ordinaryArgs);
    auto ordinaryAction = std::make_unique<GameActions::TrackPlaceAction>(
        ordinaryRide, TrackElemType::flatTrack4x4, static_cast<ride_type_t>(81),
        CoordsXYZD{ kTrackOriginX, kTrackOriginY, kTrackOriginZ, static_cast<Direction>(0) }, 0, kTrackColour, kSeatRotation,
        SelectedLiftAndInverted{}, false);
    ordinaryAction->SetFlags({ GameActions::CommandFlag::apply, GameActions::CommandFlag::allowDuringPaused });
    const auto ordinaryCashBefore = ordinaryState.park.cash;
    const auto oldInUpdateCode = gInUpdateCode;
    gInUpdateCode = true;
    const auto ordinaryResult = GameActions::Execute(ordinaryAction.get(), ordinaryState);
    gInUpdateCode = oldInUpdateCode;
    ASSERT_EQ(ordinaryResult.error, GameActions::Status::ok);
    EXPECT_EQ(ordinaryResult.expenditure, ExpenditureType::rideConstruction);
    EXPECT_EQ(ordinaryResult.cost, 8800);
    EXPECT_EQ(ordinaryResult.position.x, 336);
    EXPECT_EQ(ordinaryResult.position.y, 336);
    EXPECT_EQ(ordinaryResult.position.z, 16);
    EXPECT_EQ(ordinaryState.park.cash - ordinaryCashBefore, -8800);
    const auto ordinaryProjection = TrackPlaceProjection(ordinaryState, ordinaryRide);

    LoadEnterprisePark();
    auto& publicState = OpenRCT2::getGameState();
    RideId publicRide{};
    json_t publicArgs;
    PrepareLegalState(publicState, publicRide, publicArgs);
    ASSERT_EQ(publicRide, ordinaryRide);
    const auto publicCashBefore = publicState.park.cash;
    const auto queried = QueryNativeAction("TrackPlaceAction", publicArgs, publicState);
    ASSERT_TRUE(queried.ok) << queried.message;
    EXPECT_EQ(
        queried.value,
        (json_t{ { "action", "TrackPlaceAction" },
                 { "status", 0 },
                 { "accepted", true },
                 { "cost", 8800 },
                 { "position", { { "x", 336 }, { "y", 336 }, { "z", 16 } } } }));
    const auto executed = ExecuteNativeAction("TrackPlaceAction", publicArgs, publicState);
    ASSERT_TRUE(executed.ok) << executed.message;
    EXPECT_EQ(executed.value, [&] {
        auto expected = TrackPlaceResultProjection(ordinaryResult);
        expected["action"] = "TrackPlaceAction";
        return expected;
    }());
    EXPECT_EQ(publicState.park.cash - publicCashBefore, -8800);
    const auto publicProjection = TrackPlaceProjection(publicState, publicRide);
    EXPECT_EQ(publicProjection, ordinaryProjection);

    const auto& tiles = ordinaryProjection.at("tiles");
    ASSERT_EQ(tiles.size(), 16u);
    for (size_t sequence = 0; sequence < tiles.size(); ++sequence)
    {
        ASSERT_EQ(tiles[sequence]["x"], kTrackOriginX + static_cast<int32_t>(sequence / 4) * 32);
        ASSERT_EQ(tiles[sequence]["y"], kTrackOriginY + static_cast<int32_t>(sequence % 4) * 32);
        const auto& elements = tiles[sequence]["elements"];
        size_t trackCount = 0;
        for (const auto& element : elements)
        {
            if (!element.contains("track"))
                continue;
            ++trackCount;
            const auto& track = element["track"];
            EXPECT_EQ(track["baseZ"], 16);
            EXPECT_EQ(track["clearanceZ"], 176);
            EXPECT_EQ(track["direction"], 0);
            EXPECT_FALSE(track["ghost"]);
            EXPECT_EQ(track["ride"], ordinaryRide.ToUnderlying());
            EXPECT_EQ(track["rideType"], 81);
            EXPECT_EQ(track["trackType"], 259);
            EXPECT_EQ(track["sequence"], sequence);
            EXPECT_EQ(track["station"], 0);
            EXPECT_EQ(track["colour"], kTrackColour);
            EXPECT_EQ(track["seatRotation"], kSeatRotation);
            EXPECT_EQ(track["brakeSpeed"], 0);
            EXPECT_FALSE(track["chain"]);
            EXPECT_FALSE(track["inverted"]);
        }
        EXPECT_EQ(trackCount, 1u);
    }
    EXPECT_EQ(ordinaryProjection["ride"]["overallView"], (json_t{ { "x", 416 }, { "y", 416 } }));
    EXPECT_EQ(ordinaryProjection["ride"]["numStations"], 1);
    EXPECT_EQ(ordinaryProjection["ride"]["station0"]["startX"], kTrackOriginX);
    EXPECT_EQ(ordinaryProjection["ride"]["station0"]["startY"], kTrackOriginY);
    EXPECT_EQ(ordinaryProjection["ride"]["station0"]["height"], 2);
    EXPECT_EQ(ordinaryProjection["ride"]["station0"]["length"], 0);
    EXPECT_EQ(ordinaryProjection["ride"]["station0"]["depart"], 1);
    EXPECT_EQ(ordinaryProjection["ride"]["numTrains"], 1);
    EXPECT_EQ(ordinaryProjection["ride"]["maxTrains"], 1);
    EXPECT_EQ(ordinaryProjection["ride"]["numCarsPerTrain"], 1);
    EXPECT_EQ(ordinaryProjection["ride"]["minCarsPerTrain"], 1);
    EXPECT_EQ(ordinaryProjection["ride"]["maxCarsPerTrain"], 1);
}

TEST_F(NativeActionContractTrackPlace, RejectsMissingRideAndRideTypeMismatchWithoutMutation)
{
    LoadEnterprisePark();
    auto& state = OpenRCT2::getGameState();
    RideId rideId{};
    json_t legalArgs;
    PrepareLegalState(state, rideId, legalArgs);
    auto missingRide = legalArgs;
    missingRide["ride"] = 65535;
    ExpectRejected(state, rideId, legalArgs, missingRide, GameActions::Status::invalidParameters, STR_ERR_RIDE_NOT_FOUND);

    LoadEnterprisePark();
    auto& mismatchState = OpenRCT2::getGameState();
    RideId mismatchRide{};
    json_t mismatchArgs;
    PrepareLegalState(mismatchState, mismatchRide, mismatchArgs);
    auto mismatch = mismatchArgs;
    mismatch["rideType"] = 80;
    ExpectRejected(mismatchState, mismatchRide, mismatchArgs, mismatch, GameActions::Status::invalidParameters, kStringIdNone);
}

TEST_F(NativeActionContractTrackPlace, RejectsOriginAndBrakeSpeedWithoutMutation)
{
    LoadEnterprisePark();
    auto& state = OpenRCT2::getGameState();
    RideId rideId{};
    json_t legalArgs;
    PrepareLegalState(state, rideId, legalArgs);
    auto offMap = legalArgs;
    offMap["x"] = -32;
    ExpectRejected(state, rideId, legalArgs, offMap, GameActions::Status::invalidParameters, STR_OFF_EDGE_OF_MAP);

    LoadEnterprisePark();
    auto& heightState = OpenRCT2::getGameState();
    RideId heightRide{};
    json_t heightArgs;
    PrepareLegalState(heightState, heightRide, heightArgs);
    auto invalidHeight = heightArgs;
    invalidHeight["z"] = 17;
    ExpectRejected(
        heightState, heightRide, heightArgs, invalidHeight, GameActions::Status::invalidParameters, STR_INVALID_HEIGHT);

    LoadEnterprisePark();
    auto& directionState = OpenRCT2::getGameState();
    RideId directionRide{};
    json_t directionArgs;
    PrepareLegalState(directionState, directionRide, directionArgs);
    auto invalidDirection = directionArgs;
    invalidDirection["direction"] = 4;
    ExpectRejected(
        directionState, directionRide, directionArgs, invalidDirection, GameActions::Status::invalidParameters,
        STR_ERR_VALUE_OUT_OF_RANGE);

    LoadEnterprisePark();
    auto& speedState = OpenRCT2::getGameState();
    RideId speedRide{};
    json_t speedArgs;
    PrepareLegalState(speedState, speedRide, speedArgs);
    auto excessiveSpeed = speedArgs;
    excessiveSpeed["brakeSpeed"] = kMaximumTrackSpeed + 1;
    ExpectRejected(
        speedState, speedRide, speedArgs, excessiveSpeed, GameActions::Status::invalidParameters, STR_SPEED_TOO_HIGH);
}

TEST_F(NativeActionContractTrackPlace, RejectsTrackTypeColourSeatAndUnknownFlagsWithoutMutation)
{
    for (const auto& invalid : std::array<std::pair<std::string, int32_t>, 3>{
             std::pair{ "trackType", static_cast<int32_t>(TrackElemType::count) },
             std::pair{ "colour", static_cast<int32_t>(kNumRideColourSchemes) }, std::pair{ "seatRotation", 16 } })
    {
        LoadEnterprisePark();
        auto& state = OpenRCT2::getGameState();
        RideId rideId{};
        json_t legalArgs;
        PrepareLegalState(state, rideId, legalArgs);
        auto invalidArgs = legalArgs;
        invalidArgs[invalid.first] = invalid.second;
        ExpectRejected(
            state, rideId, legalArgs, invalidArgs, GameActions::Status::invalidParameters, STR_ERR_VALUE_OUT_OF_RANGE);
    }

    LoadEnterprisePark();
    auto& flagsState = OpenRCT2::getGameState();
    RideId flagsRide{};
    json_t flagsArgs;
    PrepareLegalState(flagsState, flagsRide, flagsArgs);
    auto invalidFlags = flagsArgs;
    invalidFlags["trackPlaceFlags"] = 4;
    ExpectRejected(
        flagsState, flagsRide, flagsArgs, invalidFlags, GameActions::Status::invalidParameters, STR_ERR_VALUE_OUT_OF_RANGE);
}

TEST_F(NativeActionContractTrackPlace, PublicSchemaMakesFromTrackDesignBoolean)
{
    const auto actions = NativeActions();
    const auto descriptor = std::find_if(
        actions.begin(), actions.end(), [](const auto& action) { return action.name == "TrackPlaceAction"; });
    ASSERT_NE(descriptor, actions.end());
    ASSERT_TRUE(descriptor->schema["properties"].contains("isFromTrackDesign"));
    EXPECT_EQ(descriptor->schema["properties"]["isFromTrackDesign"]["type"], "boolean");
}

TEST_F(NativeActionContractTrackPlace, ExecuteRequeriesOccupiedTouchedTile)
{
    LoadEnterprisePark();
    auto& state = OpenRCT2::getGameState();
    RideId rideId{};
    json_t legalArgs;
    PrepareLegalState(state, rideId, legalArgs);
    const auto queried = QueryNativeAction("TrackPlaceAction", legalArgs, state);
    ASSERT_TRUE(queried.ok) << queried.message;
    ASSERT_TRUE(queried.value["accepted"]) << queried.value.dump();

    auto* occupied = TileElementInsert<TrackElement>({ kTrackOriginX, kTrackOriginY, kTrackOriginZ }, 0b1111);
    ASSERT_NE(occupied, nullptr);
    occupied->setClearanceZ(32);
    occupied->setDirection(static_cast<Direction>(0));
    occupied->SetSequenceIndex(0);
    occupied->SetRideIndex(rideId);
    occupied->SetRideType(81);
    occupied->SetTrackType(TrackElemType::flatTrack4x4);
    state.cheats.disableClearanceChecks = false;
    const auto before = TrackPlaceProjection(state, rideId);
    auto expected = TrackPlaceRejectedResult(
        GameActions::Status::noClearance, STR_RIDE_CONSTRUCTION_CANT_CONSTRUCT_THIS_HERE, STR_X_IN_THE_WAY, "no_clearance");
    expected["rejection"]["message"] = "Enterprise 1 in the way";
    const auto executed = ExecuteNativeAction("TrackPlaceAction", legalArgs, state);
    ASSERT_TRUE(executed.ok) << executed.message;
    EXPECT_EQ(executed.value, expected);
    EXPECT_EQ(TrackPlaceProjection(state, rideId), before);
}
