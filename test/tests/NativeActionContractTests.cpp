/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TestData.h"

#include <gtest/gtest.h>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/actions/CommandFlag.h>
#include <openrct2/actions/GameAction.hpp>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/ride/RideEntranceExitPlaceAction.h>
#include <openrct2/actions/ride/RideEntranceExitRemoveAction.h>
#include <openrct2/command_line/NativeRegistry.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/world/Map.h>

#include <algorithm>
#include <functional>
#include <map>
#include <filesystem>
#include <memory>
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
        const std::vector<InventoryRow>& rows,
        const std::map<std::string, size_t>& expectedFamilyPopulation,
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
                remove.SetFlags({
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
                { "isExit", [](json_t& args) {
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
                if (rideValue >= 0 && static_cast<size_t>(rideValue) < state.rides.size()
                    && stationValue >= 0 && stationValue < Limits::kMaxStationsPerRide)
                {
                    const auto& ride = state.rides[static_cast<size_t>(rideValue)];
                    const auto& station = ride.getStation(StationIndex::FromUnderlying(stationValue));
                    projection["rideStatus"] = static_cast<uint8_t>(ride.status);
                    projection["entrance"] = station.Entrance.IsNull()
                        ? json_t(nullptr)
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
                if (rideValue >= 0 && static_cast<size_t>(rideValue) < state.rides.size()
                    && stationValue >= 0 && stationValue < Limits::kMaxStationsPerRide)
                {
                    const auto& station = state.rides[static_cast<size_t>(rideValue)].getStation(
                        StationIndex::FromUnderlying(stationValue));
                    projection["entrance"] = station.Entrance.IsNull()
                        ? json_t(nullptr)
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
                    StationIndex::FromUnderlying(args.at("station").get<uint32_t>()),
                    args.at("isExit").get<bool>());
            };
            return fixture;
        }

        bool RunFixture(const NativeActionFixture& fixture, std::string& failure)
        {
            const auto descriptors = NativeActions();
            const auto descriptor = std::find_if(descriptors.begin(), descriptors.end(), [&](const auto& item) {
                return fixture.registration == item.name;
            });
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

}

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
        { { "domain", 1 } }, { { "domain", 2 } },
    };
    const auto result = ValidateInventory(
        { row, row }, { { "station-track-maze", 2 } }, { "RideEntranceExitPlaceAction" });
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.failure.find("duplicate fixture registration"), std::string::npos);
}

TEST(NativeActionContractInventory, RejectsMissingRegistration)
{
    const std::vector<InventoryRow> rows{
        { "MissingAction", "station-track-maze", "use", {}, { { "x", 1 } }, { "x" }, { "x" },
          { { "domain", 1 } }, { { "domain", 2 } } },
    };
    const auto result = ValidateInventory(rows, { { "station-track-maze", 1 } }, { "RideEntranceExitPlaceAction" });
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.failure.find("missing from the engine census"), std::string::npos);
}

TEST(NativeActionContractInventory, RejectsMissingOwnerFamily)
{
    const std::vector<InventoryRow> rows{
        { "RideEntranceExitPlaceAction", {}, "use", {}, { { "x", 1 } }, { "x" }, { "x" },
          { { "domain", 1 } }, { { "domain", 2 } } },
    };
    const auto result = ValidateInventory(rows, { { "station-track-maze", 1 } }, { "RideEntranceExitPlaceAction" });
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.failure.find("no owner family"), std::string::npos);
}

TEST(NativeActionContractInventory, RejectsZeroExpectedFamilyPopulation)
{
    const std::vector<InventoryRow> rows{
        { "RideEntranceExitPlaceAction", "station-track-maze", "use", {}, { { "x", 1 } }, { "x" }, { "x" },
          { { "domain", 1 } }, { { "domain", 2 } } },
    };
    const auto result = ValidateInventory(rows, { { "station-track-maze", 0 } }, { "RideEntranceExitPlaceAction" });
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.failure.find("zero expected population"), std::string::npos);
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
