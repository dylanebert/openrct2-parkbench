/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TestData.h"
#include "NativeActionContractRideProjection.h"

#include <gtest/gtest.h>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/management/Marketing.h>
#include <openrct2/peep/RideUseSystem.h>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/actions/CommandFlag.h>
#include <openrct2/actions/GameAction.hpp>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/ride/RideCreateAction.h>
#include <openrct2/actions/ride/RideDemolishAction.h>
#include <openrct2/actions/ride/RideEntranceExitPlaceAction.h>
#include <openrct2/actions/ride/RideEntranceExitRemoveAction.h>
#include <openrct2/actions/ride/RideFreezeRatingAction.h>
#include <openrct2/actions/ride/RideSetAppearanceAction.h>
#include <openrct2/actions/ride/RideSetColourSchemeAction.h>
#include <openrct2/actions/ride/RideSetNameAction.h>
#include <openrct2/actions/ride/RideSetPriceAction.h>
#include <openrct2/actions/ride/RideSetSettingAction.h>
#include <openrct2/actions/ride/RideSetStatusAction.h>
#include <openrct2/actions/ride/RideSetVehicleAction.h>
#include <openrct2/actions/ride/RideSetVisibilityAction.h>
#include <openrct2/command_line/NativeRegistry.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/TrackElement.h>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <limits>
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
        // The parameter is censused from the native descriptor at runtime. This
        // label identifies the discriminated case (for example value/numTrains)
        // rather than pretending that one value partition covers every branch.
        std::string parameter;
        std::function<void(json_t&)> makeInvalid;
        std::string caseName;
        bool expectAccepted;
        std::function<void(GameState_t&, const json_t&)> prepareState;
        std::set<std::string> changedKeys;
        std::string ownerPredicate;

        SemanticInvalidPartition(
            std::string parameter_, std::function<void(json_t&)> makeInvalid_, std::string caseName_ = {},
            bool expectAccepted_ = false,
            std::function<void(GameState_t&, const json_t&)> prepareState_ = {},
            std::set<std::string> changedKeys_ = {}, std::string ownerPredicate_ = {})
            : parameter(std::move(parameter_))
            , makeInvalid(std::move(makeInvalid_))
            , caseName(std::move(caseName_))
            , expectAccepted(expectAccepted_)
            , prepareState(std::move(prepareState_))
            , changedKeys(changedKeys_.empty() ? std::set<std::string>{ parameter } : std::move(changedKeys_))
            , ownerPredicate(ownerPredicate_.empty() ? caseName : std::move(ownerPredicate_))
        {
        }
    };

    struct AcceptedFixtureCase
    {
        std::string name;
        std::function<json_t()> args;
        std::function<void(GameState_t&, const json_t&)> prepareState;
        std::vector<std::string> intendedFields;

        AcceptedFixtureCase(
            std::string name_, std::function<json_t()> args_,
            std::function<void(GameState_t&, const json_t&)> prepareState_ = {},
            std::vector<std::string> intendedFields_ = {})
            : name(std::move(name_))
            , args(std::move(args_))
            , prepareState(std::move(prepareState_))
            , intendedFields(std::move(intendedFields_))
        {
        }
    };

    struct StaleStateCase
    {
        std::string name;
        std::function<void(GameState_t&, const json_t&)> mutate;
        std::string predicate;

        StaleStateCase(
            std::string name_, std::function<void(GameState_t&, const json_t&)> mutate_, std::string predicate_ = {})
            : name(std::move(name_))
            , mutate(std::move(mutate_))
            , predicate(predicate_.empty() ? name : std::move(predicate_))
        {
        }
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
        std::vector<StaleStateCase> staleStateCases;
        std::set<std::string> requiredStalePredicates;
        std::vector<AcceptedFixtureCase> acceptedCases;
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

    std::set<std::string> DescriptorParameters(const NativeActionDescriptor& descriptor)
    {
        std::set<std::string> parameters;
        const auto properties = descriptor.schema.find("properties");
        if (properties == descriptor.schema.end() || !properties->is_object())
            return parameters;
        for (auto it = properties->begin(); it != properties->end(); ++it)
            parameters.insert(it.key());
        return parameters;
    }

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
        bool descriptorCensusParity = true;
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
        if (!observation.descriptorCensusParity)
            return "fixture parameter census differs from native descriptor";
        std::set<std::string> parameters(observation.untrustedParameters.begin(), observation.untrustedParameters.end());
        std::set<std::string> partitions(observation.semanticPartitions.begin(), observation.semanticPartitions.end());
        for (const auto& parameter : parameters)
        {
            if (!partitions.contains(parameter))
                return "uncovered semantic parameter: " + parameter;
        }
        return {};
    }

    std::string ValidateChangedKeys(
        const json_t& accepted, const json_t& invalid, const std::set<std::string>& declared)
    {
        std::set<std::string> changed;
        for (const auto& [key, value] : accepted.items())
            if (!invalid.contains(key) || invalid.at(key) != value)
                changed.insert(key);
        for (const auto& [key, value] : invalid.items())
            if (!accepted.contains(key) || accepted.at(key) != value)
                changed.insert(key);
        return changed == declared ? std::string{} : "semantic partition changed keys do not match declaration";
    }

    std::string ValidateOwnerPredicate(const json_t& response, std::string_view predicate)
    {
        if (predicate.empty())
            return "owner predicate identifier is empty";
        if (!response.contains("rejection") || !response["rejection"].is_object())
            return "owner result has no structured rejection";
        const auto& rejection = response["rejection"];
        if (!rejection.contains("code") || !rejection.contains("title") || !rejection.contains("message"))
            return "owner result does not preserve code/title/message for " + std::string(predicate);
        return {};
    }

    std::string ValidateProjectionStores(const json_t& projection)
    {
        static constexpr std::array<std::string_view, 11> required{
            "rides", "vehicles", "guests", "rideUseHistory", "news", "banners", "campaigns", "tiles",
            "finance", "parkValue", "watch",
        };
        for (const auto key : required)
            if (!projection.contains(key) || projection.at(key).empty())
                return "projection store is omitted or empty: " + std::string(key);
        if (projection.contains("authoritative") || projection.contains("activePeepLinks"))
            return "projection contains a fake or relabeled store";
        if (!projection.at("news").contains("recent") || !projection.at("news").contains("archived"))
            return "news projection does not preserve both ordered queues";
        return {};
    }

    std::string ValidateNamedDelta(
        const json_t& before, const json_t& after, const std::vector<std::string>& intendedFields)
    {
        if (intendedFields.empty())
            return "accepted case declares no intended field delta";
        for (const auto& field : intendedFields)
        {
            if (!before.contains(field) || !after.contains(field) || before.at(field) == after.at(field))
                return "accepted case did not change intended field: " + field;
        }
        return {};
    }

    std::string ValidateOwnerResult(const json_t& response, const json_t& expectedRejection)
    {
        if (!response.contains("rejection") || response["rejection"] != expectedRejection)
            return "owner result/title/message does not match the declared owner result";
        return {};
    }

    std::string ValidateStalePredicateSet(
        const std::set<std::string>& required, const std::set<std::string>& executed)
    {
        return required == executed ? std::string{} : "executed stale-predicate set differs from required census";
    }

    std::string ValidateFormerVehicle(const json_t& projection, uint16_t formerId)
    {
        if (!projection.contains("vehicles") || !projection["vehicles"].is_array())
            return "vehicle watch store is omitted";
        for (const auto& vehicle : projection["vehicles"])
            if (vehicle.value("id", std::numeric_limits<uint16_t>::max()) == formerId)
                return vehicle.value("exists", true) ? "former vehicle was not serialized as removed" : std::string{};
        return "former vehicle disappeared from the persistent watch set";
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

    struct RideFixtureState
    {
        RideId ride = RideId::GetNull();
        CoordsXYZD track{};
        TrackElemType trackType{};
        ObjectEntryIndex rideObject = kObjectEntryIndexNull;
        ObjectEntryIndex entranceObject = kObjectEntryIndexNull;
        uint8_t vehiclePresetCount = 1;
    };

    Ride* FindFixtureRide()
    {
        for (RideId::UnderlyingType i = 0; i < Limits::kMaxRidesInPark; ++i)
        {
            auto* ride = GetRide(RideId::FromUnderlying(i));
            if (ride != nullptr)
                return ride;
        }
        return nullptr;
    }

    bool FindFixtureTrack(CoordsXYZD& location, TrackElemType& type)
    {
        auto& state = getGameState();
        for (int32_t x = 0; x < state.mapSize.x; ++x)
        {
            for (int32_t y = 0; y < state.mapSize.y; ++y)
            {
                const auto tile = TileCoordsXY{ x, y };
                auto* element = MapGetFirstElementAt(tile);
                if (element == nullptr)
                    continue;
                do
                {
                    if (element->getType() == TileElementType::Track)
                    {
                        const auto* track = element->asTrack();
                        location = { CoordsXY{ tile.x * kCoordsXYStep, tile.y * kCoordsXYStep }, track->getBaseZ(), track->getDirection() };
                        type = track->GetTrackType();
                        return true;
                    }
                } while (!(element++)->isLastForTile());
            }
        }
        return false;
    }

    void PrepareFixtureRide(GameState_t& state, const std::shared_ptr<RideFixtureState>& fixture)
    {
        state.cheats.disableClearanceChecks = true;
        state.cheats.sandboxMode = true;
        auto* ride = FindFixtureRide();
        if (ride == nullptr)
            return;
        ride->status = RideStatus::closed;
        ride->flags.unset(RideFlag::brokenDown);
        fixture->ride = ride->id;
        fixture->rideObject = ride->subtype;
        fixture->entranceObject = ride->entranceStyle;
        if (const auto* entry = GetRideEntryByIndex(ride->subtype); entry != nullptr)
            fixture->vehiclePresetCount = entry->vehicle_preset_list->count;
        FindFixtureTrack(fixture->track, fixture->trackType);
    }

    json_t RideListProjection(const GameState_t& state)
    {
        json_t rides = json_t::array();
        for (RideId::UnderlyingType i = 0; i < Limits::kMaxRidesInPark; ++i)
        {
            const auto* ride = GetRide(RideId::FromUnderlying(i));
            if (ride == nullptr)
                continue;
            rides.push_back({
                { "id", i },
                { "type", ride->type },
                { "object", ride->subtype },
                { "status", static_cast<uint8_t>(ride->status) },
                { "name", ride->customName },
                { "mode", static_cast<uint8_t>(ride->mode) },
                { "departure", ride->departFlags },
                { "minWaitingTime", ride->minWaitingTime },
                { "maxWaitingTime", ride->maxWaitingTime },
                { "operation", ride->operationOption },
                { "inspectionInterval", static_cast<uint8_t>(ride->inspectionInterval) },
                { "liftHillSpeed", ride->liftHillSpeed },
                { "numCircuits", ride->numCircuits },
                { "music", ride->music },
                { "entranceStyle", ride->entranceStyle },
                { "vehicleColourSettings", static_cast<uint8_t>(ride->vehicleColourSettings) },
                { "trackColours", json_t::array() },
                { "vehicleColours", json_t::array() },
                { "price0", ride->price[0] },
                { "price1", ride->price[1] },
                { "numTrains", ride->numTrains },
                { "proposedNumTrains", ride->proposedNumTrains },
                { "numCarsPerTrain", ride->numCarsPerTrain },
                { "proposedNumCarsPerTrain", ride->proposedNumCarsPerTrain },
                { "vehicleChangeTimeout", ride->vehicleChangeTimeout },
                { "fixedRatings", ride->flags.has(RideFlag::fixedRatings) },
                { "randomShopColours", ride->flags.has(RideFlag::randomShopColours) },
                { "excitement", static_cast<int32_t>(ride->ratings.excitement) },
                { "intensity", static_cast<int32_t>(ride->ratings.intensity) },
                { "nausea", static_cast<int32_t>(ride->ratings.nausea) },
            });
            for (const auto& colours : ride->trackColours)
            {
                rides.back()["trackColours"].push_back({
                    { "main", static_cast<uint8_t>(colours.main) },
                    { "additional", static_cast<uint8_t>(colours.additional) },
                    { "supports", static_cast<uint8_t>(colours.supports) },
                });
            }
            for (const auto& colours : ride->vehicleColours)
            {
                rides.back()["vehicleColours"].push_back({
                    { "body", static_cast<uint8_t>(colours.Body) },
                    { "trim", static_cast<uint8_t>(colours.Trim) },
                    { "tertiary", static_cast<uint8_t>(colours.Tertiary) },
                });
            }
        }
        return { { "cash", state.park.cash }, { "rides", std::move(rides) } };
    }

    json_t ExactRideActionProjection(const GameState_t& state, const json_t& args);

    // Kept as a named compatibility wrapper for old fixture declarations; all
    // ride rows now receive the complete action-specific projection.
    json_t RideProjection(const GameState_t& state, const json_t& args)
    {
        return ExactRideActionProjection(state, args);
    }

    json_t ExactRideActionProjection(const GameState_t& state, const json_t& args)
    {
        return OpenRCT2::Testing::SerializeRideProjection(state, args);
    }

    json_t TrackProjection(const GameState_t& state, const json_t& args)
    {
        auto projection = RideProjection(state, args);
        size_t visibleTracks = 0;
        size_t invisibleTracks = 0;
        json_t trackState = json_t::array();
        for (int32_t x = 0; x < state.mapSize.x; ++x)
        {
            for (int32_t y = 0; y < state.mapSize.y; ++y)
            {
                auto* element = MapGetFirstElementAt(TileCoordsXY{ x, y });
                if (element == nullptr)
                    continue;
                do
                {
                    if (element->getType() != TileElementType::Track)
                        continue;
                    const auto* track = element->asTrack();
                    trackState.push_back({
                        { "x", x },
                        { "y", y },
                        { "z", track->getBaseZ() },
                        { "type", static_cast<uint16_t>(track->GetTrackType()) },
                        { "ride", track->GetRideIndex().ToUnderlying() },
                        { "scheme", track->GetColourScheme() },
                        { "invisible", track->isInvisible() },
                    });
                    if (track->isInvisible())
                        ++invisibleTracks;
                    else
                        ++visibleTracks;
                } while (!(element++)->isLastForTile());
            }
        }
        projection["visibleTracks"] = visibleTracks;
        projection["invisibleTracks"] = invisibleTracks;
        projection["trackState"] = std::move(trackState);
        return projection;
    }

    std::set<std::string> RequiredStalePredicates(std::string_view registration)
    {
        static const std::map<std::string_view, std::set<std::string>> census{
            { "RideCreateAction", { "default-owner-state" } },
            { "RideDemolishAction", { "renewal-open-ride", "renewal-has-riders", "renewal-not-needed", "deleted-ride" } },
            { "RideSetColourSchemeAction", { "default-owner-state" } },
            { "RideSetNameAction", { "duplicate-name", "deleted-ride" } },
            { "RideSetPriceAction", { "deleted-ride", "unloaded-ride-object" } },
            { "RideSetStatusAction", { "open-test-simulate-topology", "deleted-ride" } },
            { "RideFreezeRatingAction", { "default-owner-state" } },
            { "RideSetAppearanceAction", { "deleted-ride", "non-applicable-entrance" } },
            { "RideSetVehicleAction", { "broken-ride", "open-ride", "deleted-ride" } },
            { "RideSetSettingAction", { "broken-operating-ride", "open-operating-ride", "deleted-ride" } },
            { "RideSetVisibilityAction", { "default-owner-state" } },
            { "RideEntranceExitPlaceAction", { "default-owner-state" } },
        };
        const auto it = census.find(registration);
        return it == census.end() ? std::set<std::string>{} : it->second;
    }

    std::vector<std::string> DefaultIntendedFields(std::string_view registration)
    {
        if (registration == "RideSetColourSchemeAction" || registration == "RideSetVisibilityAction"
            || registration == "RideEntranceExitPlaceAction")
            return { "tiles" };
        if (registration == "RideDemolishAction")
            return { "finance" };
        return { "rides" };
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

        std::vector<NativeActionFixture> MakeRideFixtures()
        {
            std::vector<NativeActionFixture> fixtures;

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideCreateAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor construction of a ride";
                fixture.untrustedParameters = {
                    "rideType", "rideObject", "entranceObject", "colour1", "colour2", "inspectionInterval" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [this, values](GameState_t& state) {
                    state.cheats.disableClearanceChecks = true;
                    state.cheats.sandboxMode = true;
                    _context->GetObjectManager().UnloadAll();
                    ASSERT_NE(_context->GetObjectManager().LoadObject(ObjectEntryDescriptor("rct2.ride.enterp"), 10), nullptr);
                    values->rideObject = 10;
                    values->entranceObject = kObjectEntryIndexNull;
                };
                fixture.legalArgs = [values] {
                    return json_t{
                        { "rideType", 81 },
                        { "rideObject", values->rideObject },
                        { "entranceObject", values->entranceObject },
                        { "colour1", 0 },
                        { "colour2", 0 },
                        { "inspectionInterval", 0 },
                    };
                };
                fixture.semanticInvalidPartitions = {
                    // Enterprise is a valid loaded object for type 81, but not
                    // for the intentionally incompatible type 33.
                    { "rideType", [](json_t& args) { args["rideType"] = 33; }, "type-with-valid-object", true },
                    // Type 81 remains valid while the object identity is absent.
                    { "rideObject", [](json_t& args) { args["rideObject"] = 254; }, "object-with-valid-type" },
                    { "entranceObject", [](json_t& args) { args["entranceObject"] = 254; }, "entrance-object", true },
                    { "colour1", [](json_t& args) { args["colour1"] = 255; }, "primary-colour" },
                    { "colour2", [](json_t& args) { args["colour2"] = 255; }, "secondary-colour" },
                    { "inspectionInterval", [](json_t& args) { args["inspectionInterval"] = 255; }, "inspection-bound" },
                };
                fixture.mutateRelevantState = [](GameState_t&, const json_t&) {
                    for (RideId::UnderlyingType i = 0; i < Limits::kMaxRidesInPark; ++i)
                    {
                        if (GetRide(RideId::FromUnderlying(i)) == nullptr)
                            RideAllocateAtIndex(RideId::FromUnderlying(i));
                    }
                };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return RideProjection(state, args);
                };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t&) {
                    return RideListProjection(state);
                };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideCreateAction>(
                        args.at("rideType").get<ride_type_t>(), args.at("rideObject").get<ObjectEntryIndex>(),
                        args.at("colour1").get<uint8_t>(), args.at("colour2").get<uint8_t>(),
                        args.at("entranceObject").get<ObjectEntryIndex>(),
                        static_cast<RideInspection>(args.at("inspectionInterval").get<uint8_t>()));
                };
                fixtures.push_back(std::move(fixture));
            }

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideDemolishAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor demolition or renewal of a ride";
                fixture.untrustedParameters = { "ride", "modifyType" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [values](GameState_t& state) {
                    PrepareFixtureRide(state, values);
                    if (auto* ride = GetRide(values->ride); ride != nullptr)
                        ride->flags.set(RideFlag::everBeenOpened);
                };
                fixture.legalArgs = [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "modifyType", 1 } }; };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "modifyType", [](json_t& args) { args["modifyType"] = 2; }, "invalid-modify-mode" },
                    { "modifyType", [](json_t& args) { args["modifyType"] = 0; }, "demolition-mode-legal", true },
                };
                fixture.acceptedCases = {
                    { "renewal", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "modifyType", 1 } }; }, {} },
                    { "demolition", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "modifyType", 0 } }; }, {} },
                };
                fixture.staleStateCases = {
                    { "renewal-open-ride", [values](GameState_t&, const json_t&) {
                         if (auto* ride = GetRide(values->ride); ride != nullptr)
                             ride->status = RideStatus::open;
                     } },
                    { "renewal-has-riders", [values](GameState_t&, const json_t&) {
                         if (auto* ride = GetRide(values->ride); ride != nullptr)
                             ride->numRiders = 1;
                     } },
                    { "renewal-not-needed", [values](GameState_t&, const json_t&) {
                         if (auto* ride = GetRide(values->ride); ride != nullptr)
                             ride->flags.unset(RideFlag::everBeenOpened);
                     } },
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideDemolishAction>(
                        RideId::FromUnderlying(args.at("ride").get<uint16_t>()),
                        static_cast<GameActions::RideModifyType>(args.at("modifyType").get<uint8_t>()));
                };
                fixtures.push_back(std::move(fixture));
            }

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideSetColourSchemeAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor track colour editing";
                fixture.untrustedParameters = { "x", "y", "z", "direction", "trackType", "colourScheme" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [values](GameState_t& state) { PrepareFixtureRide(state, values); };
                fixture.legalArgs = [values] {
                    return json_t{
                        { "x", values->track.x }, { "y", values->track.y }, { "z", values->track.z },
                        { "direction", static_cast<uint8_t>(values->track.direction) },
                        { "trackType", static_cast<uint16_t>(values->trackType) }, { "colourScheme", 1 },
                    };
                };
                fixture.semanticInvalidPartitions = {
                    { "x", [](json_t& args) { args["x"] = -1; }, "x-bound" },
                    { "y", [](json_t& args) { args["y"] = -1; }, "y-bound" },
                    { "z", [](json_t& args) { args["z"] = -1; }, "z-bound" },
                    { "direction", [](json_t& args) { args["direction"] = 4; }, "direction-domain" },
                    { "trackType", [](json_t& args) { args["trackType"] = 65535; }, "track-identity" },
                    { "colourScheme", [](json_t& args) { args["colourScheme"] = 4; }, "colour-scheme-domain" },
                };
                fixture.mutateRelevantState = [](GameState_t&, const json_t& args) {
                    const CoordsXYZD location{
                        args.at("x").get<int32_t>(), args.at("y").get<int32_t>(), args.at("z").get<int32_t>(),
                        static_cast<Direction>(args.at("direction").get<uint8_t>()) };
                    if (auto* track = MapGetTrackElementAtOfType(
                            location, static_cast<TrackElemType>(args.at("trackType").get<uint16_t>()));
                        track != nullptr)
                    {
                        TileElementRemove(reinterpret_cast<TileElement*>(track));
                    }
                };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) { return TrackProjection(state, args); };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) { return TrackProjection(state, args); };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideSetColourSchemeAction>(
                        CoordsXYZD{ args.at("x"), args.at("y"), args.at("z"),
                            static_cast<Direction>(args.at("direction").get<uint8_t>()) },
                        static_cast<TrackElemType>(args.at("trackType").get<uint16_t>()),
                        args.at("colourScheme").get<uint16_t>());
                };
                fixtures.push_back(std::move(fixture));
            }

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideSetNameAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor ride naming";
                fixture.untrustedParameters = { "ride", "name" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [values](GameState_t& state) { PrepareFixtureRide(state, values); };
                fixture.legalArgs = [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "name", "S2 Native Ride" } }; };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "name", [](json_t& args) { args["name"] = "This ride name is intentionally too long"; }, "name-length" },
                };
                fixture.staleStateCases = {
                    { "duplicate-name", [values](GameState_t& state, const json_t& args) {
                         for (RideId::UnderlyingType i = 0; i < Limits::kMaxRidesInPark; ++i)
                         {
                             const auto id = RideId::FromUnderlying(i);
                             if (id == values->ride)
                                 continue;
                             auto* other = GetRide(id);
                             if (other == nullptr)
                             {
                                 RideAllocateAtIndex(id);
                                 other = GetRide(id);
                             }
                             if (other != nullptr)
                             {
                                 other->type = GetRide(values->ride)->type;
                                 other->subtype = GetRide(values->ride)->subtype;
                                 other->numStations = GetRide(values->ride)->numStations;
                                 other->customName = args.at("name").get<std::string>();
                                 bool assignedTrack = false;
                                 for (int32_t x = 0; x < state.mapSize.x && !assignedTrack; ++x)
                                 {
                                     for (int32_t y = 0; y < state.mapSize.y && !assignedTrack; ++y)
                                     {
                                         auto* element = MapGetFirstElementAt(TileCoordsXY{ x, y });
                                         while (element != nullptr)
                                         {
                                             if (element->getType() == TileElementType::Track
                                                 && element->asTrack()->GetRideIndex() == values->ride)
                                             {
                                                 element->asTrack()->SetRideIndex(id);
                                                 assignedTrack = true;
                                                 break;
                                             }
                                             if (element->isLastForTile())
                                                 break;
                                             ++element;
                                         }
                                     }
                                 }
                                 break;
                             }
                         }
                         // Some imported parks have no independently nameable
                         // ride topology. Preserve the stale witness rather
                         // than silently accepting a false premise.
                         if (!Ride::nameExists(args.at("name").get<std::string>(), values->ride))
                             RideDelete(values->ride);
                     } },
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideSetNameAction>(
                        RideId::FromUnderlying(args.at("ride").get<uint16_t>()), args.at("name").get<std::string>());
                };
                fixtures.push_back(std::move(fixture));
            }

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideSetPriceAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor ride pricing";
                // isPrimaryPrice is a closed boolean domain; both values are legal and are
                // exercised by the ordinary/public parity path below.
                fixture.untrustedParameters = { "ride", "price", "isPrimaryPrice" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [values](GameState_t& state) { PrepareFixtureRide(state, values); };
                fixture.legalArgs = [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "price", 1 }, { "isPrimaryPrice", true } }; };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "price", [](json_t& args) { args["price"] = -1; }, "below-minimum" },
                    { "price", [](json_t& args) { args["price"] = kRideMaxPrice + 1; }, "above-maximum" },
                    { "isPrimaryPrice", [](json_t& args) { args["isPrimaryPrice"] = false; }, "secondary-price-legal", true },
                };
                fixture.acceptedCases = {
                    { "primary-price", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "price", 1 }, { "isPrimaryPrice", true } }; }, {} },
                    { "secondary-price", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "price", kRideMaxPrice }, { "isPrimaryPrice", false } }; }, {} },
                };
                fixture.staleStateCases = {
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                    { "unloaded-ride-object", [values](GameState_t&, const json_t&) {
                         if (auto* ride = GetRide(values->ride); ride != nullptr)
                             ride->subtype = kObjectEntryIndexNull;
                     } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideSetPriceAction>(
                        RideId::FromUnderlying(args.at("ride").get<uint16_t>()), args.at("price").get<money64>(),
                        args.at("isPrimaryPrice").get<bool>());
                };
                fixtures.push_back(std::move(fixture));
            }

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideSetStatusAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor ride lifecycle status";
                fixture.untrustedParameters = { "ride", "status" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [values](GameState_t& state) { PrepareFixtureRide(state, values); };
                fixture.legalArgs = [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "status", 2 } }; };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "status", [](json_t& args) { args["status"] = 255; }, "status-domain" },
                };
                fixture.staleStateCases = {
                    { "open-test-simulate-topology", [values](GameState_t& state, const json_t&) {
                         if (auto* ride = GetRide(values->ride); ride != nullptr)
                         {
                             ride->status = RideStatus::open;
                             ride->type = kRideTypeNull;
                             bool removed = false;
                             for (int32_t x = 0; x < state.mapSize.x && !removed; ++x)
                             {
                                 for (int32_t y = 0; y < state.mapSize.y && !removed; ++y)
                                 {
                                     auto* element = MapGetFirstElementAt(TileCoordsXY{ x, y });
                                     while (element != nullptr)
                                     {
                                         if (element->getType() == TileElementType::Track
                                             && element->asTrack()->GetRideIndex() == values->ride)
                                         {
                                             TileElementRemove(element);
                                             removed = true;
                                             break;
                                         }
                                         if (element->isLastForTile())
                                             break;
                                         ++element;
                                     }
                                 }
                             }
                         }
                     } },
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideSetStatusAction>(
                        RideId::FromUnderlying(args.at("ride").get<uint16_t>()),
                        static_cast<RideStatus>(args.at("status").get<uint8_t>()));
                };
                fixtures.push_back(std::move(fixture));
            }

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideFreezeRatingAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor rating diagnostic override";
                fixture.untrustedParameters = { "ride", "type", "value" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [values](GameState_t& state) { PrepareFixtureRide(state, values); };
                fixture.legalArgs = [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 0 }, { "value", 100 } }; };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "type", [](json_t& args) { args["type"] = 255; }, "rating-type-domain" },
                    { "value", [](json_t& args) { args["value"] = 0; }, "rating-value-domain" },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) { return RideProjection(state, args); };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) { return RideProjection(state, args); };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideFreezeRatingAction>(
                        RideId::FromUnderlying(args.at("ride").get<uint16_t>()),
                        static_cast<GameActions::RideRatingType>(args.at("type").get<uint8_t>()),
                        args.at("value").get<RideRating_t>());
                };
                fixtures.push_back(std::move(fixture));
            }

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideSetAppearanceAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor ride appearance editing";
                fixture.untrustedParameters = { "ride", "type", "value", "index" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [values](GameState_t& state) { PrepareFixtureRide(state, values); };
                fixture.legalArgs = [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 7 }, { "value", values->entranceObject }, { "index", 0 } }; };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "type", [](json_t& args) { args["type"] = 255; }, "invalid-type" },
                    { "value", [](json_t& args) { args["type"] = 0; args["value"] = 255; }, "track-main-colour", false, {}, { "type", "value" } },
                    { "value", [](json_t& args) { args["type"] = 3; args["value"] = 255; }, "vehicle-body-colour", false, {}, { "type", "value" } },
                    { "value", [](json_t& args) { args["type"] = 6; args["value"] = 255; }, "vehicle-colour-scheme", false, {}, { "type", "value" } },
                    { "value", [](json_t& args) { args["type"] = 7; args["value"] = 254; }, "entrance-object-identity", false, {}, { "value" } },
                    { "value", [](json_t& args) { args["type"] = 8; args["value"] = 2; }, "random-colour-boolean", false, {}, { "type", "value" } },
                    { "index", [](json_t& args) { args["type"] = 0; args["value"] = 1; args["index"] = 255; }, "appearance-index", false, {}, { "type", "value", "index" } },
                };
                fixture.acceptedCases = {
                    { "track-main", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 0 }, { "value", 2 }, { "index", 0 } }; }, {} },
                    { "track-additional", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 1 }, { "value", 2 }, { "index", 0 } }; }, {} },
                    { "track-supports", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 2 }, { "value", 3 }, { "index", 0 } }; }, {} },
                    { "vehicle-body", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 3 }, { "value", 1 }, { "index", 0 } }; }, {} },
                    { "vehicle-trim", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 4 }, { "value", 2 }, { "index", 0 } }; }, {} },
                    { "vehicle-tertiary", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 5 }, { "value", 3 }, { "index", 0 } }; }, {} },
                    { "vehicle-scheme", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 6 }, { "value", 1 }, { "index", 0 } }; }, {} },
                };
                fixture.staleStateCases = {
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                    { "non-applicable-entrance", [values](GameState_t&, const json_t&) {
                         if (auto* ride = GetRide(values->ride); ride != nullptr)
                         {
                             for (ride_type_t type = 0; type < RIDE_TYPE_COUNT; ++type)
                             {
                                 if (!GetRideTypeDescriptor(type).flags.has(RtdFlag::hasEntranceAndExit))
                                 {
                                     ride->type = type;
                                     break;
                                 }
                             }
                         }
                     } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideSetAppearanceAction>(
                        RideId::FromUnderlying(args.at("ride").get<uint16_t>()),
                        static_cast<GameActions::RideSetAppearanceType>(args.at("type").get<uint8_t>()),
                        args.at("value").get<uint16_t>(), args.at("index").get<uint32_t>());
                };
                fixtures.push_back(std::move(fixture));
            }

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideSetVehicleAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor vehicle settings";
                fixture.untrustedParameters = { "ride", "type", "value", "colour" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [values](GameState_t& state) { PrepareFixtureRide(state, values); };
                fixture.legalArgs = [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 0 }, { "value", 1 }, { "colour", 0 } }; };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "type", [](json_t& args) { args["type"] = 255; }, "invalid-type" },
                    { "value", [](json_t& args) { args["type"] = 0; args["value"] = 0; }, "num-trains-zero" },
                    { "value", [](json_t& args) { args["type"] = 1; args["value"] = 0; }, "cars-per-train-zero", false, {}, { "type", "value" } },
                    { "value", [](json_t& args) { args["type"] = 1; args["value"] = 255; }, "cars-per-train-descriptor-bound", false, {}, { "type", "value" } },
                    { "value", [](json_t& args) { args["type"] = 2; args["value"] = 65535; }, "ride-entry-identity", false, {}, { "type", "value" } },
                    { "value", [](json_t& args) { args["type"] = 3; args["value"] = 2; }, "reversed-trains-domain", false, {}, { "type", "value" } },
                    { "colour", [values](json_t& args) {
                         args["type"] = 2;
                         args["value"] = values->rideObject;
                         args["colour"] = 254;
                     }, "loaded-entry-colour-preset", false,
                      [](GameState_t& state, const json_t&) { state.cheats.ignoreResearchStatus = true; },
                      { "type", "value", "colour" } },
                    { "value", [](json_t& args) { args["type"] = 1; args["value"] = 0; }, "cars-cheat-storage-zero",
                      false, [](GameState_t& state, const json_t&) { state.cheats.disableTrainLengthLimit = true; }, { "type", "value" } },
                };
                fixture.acceptedCases = {
                    { "cars-per-train-cheat", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 1 }, { "value", 255 }, { "colour", 0 } }; },
                      [](GameState_t& state, const json_t&) { state.cheats.disableTrainLengthLimit = true; } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.staleStateCases = {
                    { "broken-ride", [values](GameState_t&, const json_t&) {
                         if (auto* ride = GetRide(values->ride); ride != nullptr)
                             ride->flags.set(RideFlag::brokenDown);
                     } },
                    { "open-ride", [values](GameState_t&, const json_t&) {
                         if (auto* ride = GetRide(values->ride); ride != nullptr)
                             ride->status = RideStatus::open;
                     } },
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideSetVehicleAction>(
                        RideId::FromUnderlying(args.at("ride").get<uint16_t>()),
                        static_cast<GameActions::RideSetVehicleType>(args.at("type").get<uint8_t>()),
                        args.at("value").get<uint16_t>(), args.at("colour").get<uint8_t>());
                };
                fixtures.push_back(std::move(fixture));
            }

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideSetSettingAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor operating settings";
                fixture.untrustedParameters = { "ride", "setting", "value" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [values](GameState_t& state) { PrepareFixtureRide(state, values); };
                fixture.legalArgs = [values] {
                    const auto* ride = GetRide(values->ride);
                    return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 0 },
                        { "value", ride == nullptr ? 0 : static_cast<uint8_t>(ride->mode) } };
                };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "setting", [](json_t& args) { args["setting"] = 255; }, "invalid-setting" },
                    { "value", [](json_t& args) { args["setting"] = 0; args["value"] = 255; }, "mode-domain" },
                    { "value", [](json_t& args) { args["setting"] = 2; args["value"] = 251; }, "minimum-wait-bound", false, {}, { "setting", "value" } },
                    { "value", [](json_t& args) { args["setting"] = 3; args["value"] = 251; }, "maximum-wait-bound", false, {}, { "setting", "value" } },
                    { "value", [](json_t& args) { args["setting"] = 4; args["value"] = 255; }, "operation-descriptor-bound", false, {}, { "setting", "value" } },
                    { "value", [](json_t& args) { args["setting"] = 5; args["value"] = 255; }, "inspection-domain", false, {}, { "setting", "value" } },
                    { "value", [](json_t& args) { args["setting"] = 6; args["value"] = 2; }, "music-boolean", false, {}, { "setting", "value" } },
                    { "value", [](json_t& args) { args["setting"] = 7; args["value"] = 255; }, "music-object-identity", false, {}, { "setting", "value" } },
                    { "value", [](json_t& args) { args["setting"] = 8; args["value"] = 255; }, "lift-hill-descriptor-bound", false, {}, { "setting", "value" } },
                    { "value", [](json_t& args) { args["setting"] = 9; args["value"] = 255; }, "circuit-cable-lift-bound", false, {}, { "setting", "value" } },
                    { "value", [](json_t& args) { args["setting"] = 10; args["value"] = 255; }, "ride-type-domain", false, {}, { "setting", "value" } },
                    { "value", [](json_t& args) { args["setting"] = 1; args["value"] = 255; }, "departure-value-domain", true },
                };
                fixture.acceptedCases = {
                    { "mode", [values] {
                         const auto* ride = GetRide(values->ride);
                         return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 0 },
                             { "value", ride == nullptr ? 0 : static_cast<uint8_t>(ride->mode) } };
                     }, {} },
                    { "departure", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 1 }, { "value", 255 } }; }, {} },
                    { "minimum-wait", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 2 }, { "value", 10 } }; }, {} },
                    { "maximum-wait", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 3 }, { "value", 20 } }; }, {} },
                    { "operation", [values] {
                         const auto* ride = GetRide(values->ride);
                         return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 4 },
                             { "value", ride == nullptr ? 0 : ride->operationOption } };
                     }, {} },
                    { "inspection", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 5 }, { "value", 0 } }; }, {} },
                    { "music-off", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 6 }, { "value", 0 } }; }, {} },
                    { "music-on", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 6 }, { "value", 1 } }; }, {} },
                    { "music-object", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 7 }, { "value", GetRide(values->ride) == nullptr ? 0 : GetRide(values->ride)->music } }; }, {} },
                    { "lift-hill", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 8 }, { "value", GetRide(values->ride) == nullptr ? 0 : GetRide(values->ride)->liftHillSpeed } }; }, {} },
                    { "circuits", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 9 }, { "value", 1 } }; }, {} },
                    { "ride-type-cheat", [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 10 }, { "value", GetRide(values->ride) == nullptr ? 0 : GetRide(values->ride)->type } }; },
                      [](GameState_t& state, const json_t&) { state.cheats.allowArbitraryRideTypeChanges = true; } },
                };
                fixture.staleStateCases = {
                    { "broken-operating-ride", [values](GameState_t&, const json_t&) {
                         if (auto* ride = GetRide(values->ride); ride != nullptr)
                             ride->flags.set(RideFlag::brokenDown);
                     } },
                    { "open-operating-ride", [values](GameState_t&, const json_t&) {
                         if (auto* ride = GetRide(values->ride); ride != nullptr)
                             ride->status = RideStatus::open;
                     } },
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) { return ExactRideActionProjection(state, args); };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideSetSettingAction>(
                        RideId::FromUnderlying(args.at("ride").get<uint16_t>()),
                        static_cast<GameActions::RideSetSetting>(args.at("setting").get<uint8_t>()),
                        args.at("value").get<uint8_t>());
                };
                fixtures.push_back(std::move(fixture));
            }

            {
                auto values = std::make_shared<RideFixtureState>();
                NativeActionFixture fixture;
                fixture.registration = "RideSetVisibilityAction";
                fixture.ownerFamily = "ride-lifecycle-settings";
                fixture.publicUse = "paused monitor track visibility editing";
                fixture.untrustedParameters = { "ride", "visiblity" };
                fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
                fixture.prepareLegalState = [values](GameState_t& state) { PrepareFixtureRide(state, values); };
                fixture.legalArgs = [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "visiblity", 1 } }; };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "visiblity", [](json_t& args) { args["visiblity"] = 2; }, "visibility-domain" },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) { return TrackProjection(state, args); };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) { return TrackProjection(state, args); };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideSetVisibilityAction>(
                        RideId::FromUnderlying(args.at("ride").get<uint16_t>()),
                        static_cast<GameActions::RideSetVisibilityType>(args.at("visiblity").get<uint8_t>()));
                };
                fixtures.push_back(std::move(fixture));
            }

            for (auto& fixture : fixtures)
                fixture.requiredStalePredicates = RequiredStalePredicates(fixture.registration);
            return fixtures;
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
                 }, "is-exit", false, {}, { "isExit", "x", "y" } },
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
            fixture.requiredStalePredicates = RequiredStalePredicates(fixture.registration);
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
            // The engine descriptor is the source of truth for the public
            // parameter census.  The declaration is checked against it, never
            // used as a substitute for it (this catches omitted fields such as
            // isPrimaryPrice).
            const auto descriptorParameters = DescriptorParameters(*descriptor);
            const std::set<std::string> declaredParameters(
                fixture.untrustedParameters.begin(), fixture.untrustedParameters.end());
            if (descriptorParameters != declaredParameters)
            {
                failure = "fixture parameter census differs from native descriptor: descriptor="
                    + json_t(descriptorParameters).dump() + " fixture=" + json_t(declaredParameters).dump();
                return false;
            }
            const auto& parameters = descriptorParameters;
            std::set<std::string> partitions;
            std::set<std::string> partitionCases;
            size_t partitionIndex = 0;
            for (const auto& partition : fixture.semanticInvalidPartitions)
            {
                if (partition.parameter.empty())
                {
                    failure = "semantic partition has no parameter";
                    return false;
                }
                const auto caseName = partition.caseName.empty()
                    ? partition.parameter + "#" + std::to_string(partitionIndex)
                    : partition.caseName;
                if (!parameters.contains(partition.parameter))
                {
                    failure = "semantic partition names a parameter absent from the descriptor: " + partition.parameter;
                    return false;
                }
                if (!partitionCases.insert(caseName).second)
                {
                    failure = "duplicate semantic partition case: " + caseName;
                    return false;
                }
                partitions.insert(partition.parameter);
                ++partitionIndex;
            }
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
                const auto acceptedArgs = args;
                partition.makeInvalid(args);
                if (const auto changedKeyFailure = ValidateChangedKeys(acceptedArgs, args, partition.changedKeys);
                    !changedKeyFailure.empty())
                {
                    failure = changedKeyFailure + ": " + partition.caseName;
                    return false;
                }
                const auto ownerPredicate = partition.ownerPredicate.empty() ? partition.parameter : partition.ownerPredicate;
                if (ownerPredicate.empty())
                {
                    failure = "semantic partition has no owner predicate identifier: " + partition.caseName;
                    return false;
                }
                if (partition.prepareState)
                    partition.prepareState(state, args);
                const auto queried = QueryNativeAction(fixture.registration, args, state);
                if (partition.expectAccepted)
                {
                    if (!queried.ok || !queried.value.value("accepted", false))
                    {
                        failure = "legal discriminated partition was rejected: " + partition.caseName;
                        return false;
                    }
                    const auto executed = ExecuteNativeAction(fixture.registration, args, state);
                    if (!executed.ok || !executed.value.value("accepted", false))
                    {
                        failure = "legal discriminated partition did not execute: " + partition.caseName;
                        return false;
                    }
                    continue;
                }
                if (!queried.ok || queried.value.value("accepted", true))
                {
                    failure = "semantic invalid partition was accepted: " + partition.caseName;
                    return false;
                }
                if (const auto ownerFailure = ValidateOwnerPredicate(queried.value, ownerPredicate);
                    !ownerFailure.empty())
                {
                    failure = ownerFailure;
                    return false;
                }
                OpenRCT2::Testing::SetRideProjectionWatchSet(
                    OpenRCT2::Testing::CaptureRideProjectionWatchSet(state, args));
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

            // A successful query is intentionally made stale by every declared
            // owner-state mutator. Public execution must query again rather than
            // trusting a cached result. A single deleted-ride witness is not a
            // substitute for duplicate-name, topology, object, research, cheat
            // or renewal preconditions.
            std::vector<StaleStateCase> staleCases = fixture.staleStateCases;
            if (staleCases.empty())
                staleCases.push_back({ "default-owner-state", fixture.mutateRelevantState, "default-owner-state" });
            if (fixture.requiredStalePredicates.empty())
            {
                failure = "required stale-predicate census is empty";
                return false;
            }
            std::set<std::string> executedStalePredicates;
            for (const auto& staleCase : staleCases)
            {
                const auto predicate = staleCase.predicate.empty() ? staleCase.name : staleCase.predicate;
                if (!fixture.requiredStalePredicates.contains(predicate))
                {
                    failure = "stale witness is not in required predicate census: " + predicate;
                    return false;
                }
                executedStalePredicates.insert(predicate);
                LoadPark("small_park_with_ferris_wheel.sv6");
                auto& staleState = OpenRCT2::getGameState();
                fixture.prepareLegalState(staleState);
                const auto staleArgs = fixture.legalArgs();
                const auto staleQuery = QueryNativeAction(fixture.registration, staleArgs, staleState);
                if (!staleQuery.ok || !staleQuery.value.value("accepted", false))
                {
                    failure = "legal fixture query did not accept before stale-state mutation case="
                        + staleCase.name + " args=" + staleArgs.dump() + " result=" + staleQuery.value.dump();
                    return false;
                }
                OpenRCT2::Testing::SetRideProjectionWatchSet(
                    OpenRCT2::Testing::CaptureRideProjectionWatchSet(staleState, staleArgs));
                staleCase.mutate(staleState, staleArgs);
                const auto staleBefore = fixture.rejectionProjection(staleState, staleArgs);
                const auto staleExecution = ExecuteNativeAction(fixture.registration, staleArgs, staleState);
                if (!staleExecution.ok || staleExecution.value.value("accepted", true))
                {
                    failure = "execution did not perform a fresh query after stale-state mutation: " + staleCase.name;
                    return false;
                }
                if (staleBefore.empty() || staleBefore != fixture.rejectionProjection(staleState, staleArgs))
                {
                    failure = "stale-query rejection mutated the declared projection: " + staleCase.name;
                    return false;
                }
            }
            if (executedStalePredicates != fixture.requiredStalePredicates)
            {
                failure = "executed stale-predicate set differs from required census";
                return false;
            }

            // Independent fresh states are used for every accepted discriminated
            // case, not just the first enum value. Only the paused transport
            // omissions are excluded from parity.
            std::vector<AcceptedFixtureCase> acceptedCases = fixture.acceptedCases;
            if (acceptedCases.empty())
                acceptedCases.push_back({ "default", fixture.legalArgs, {} });
            for (const auto& acceptedCase : acceptedCases)
            {
                LoadPark("small_park_with_ferris_wheel.sv6");
                auto& ordinaryState = OpenRCT2::getGameState();
                fixture.prepareLegalState(ordinaryState);
                const auto ordinaryArgs = acceptedCase.args();
                if (acceptedCase.prepareState)
                    acceptedCase.prepareState(ordinaryState, ordinaryArgs);
                if (ordinaryArgs.empty())
                {
                    failure = "accepted case has empty args: " + acceptedCase.name;
                    return false;
                }
                OpenRCT2::Testing::SetRideProjectionWatchSet(
                    OpenRCT2::Testing::CaptureRideProjectionWatchSet(ordinaryState, ordinaryArgs));
                const auto ordinaryPre = fixture.acceptedPostStateProjection(ordinaryState, ordinaryArgs);
                if (ordinaryPre.empty())
                {
                    failure = "accepted pre-state projection is empty: " + acceptedCase.name;
                    return false;
                }
                const auto ordinaryBefore = ordinaryState.park.cash;
                auto ordinaryAction = fixture.makeOrdinaryAction(ordinaryArgs);
                ordinaryAction->SetFlags({ GameActions::CommandFlag::apply, GameActions::CommandFlag::allowDuringPaused });
                const auto oldInUpdateCode = gInUpdateCode;
                gInUpdateCode = true;
                const auto ordinaryResult = GameActions::Execute(ordinaryAction.get(), ordinaryState);
                gInUpdateCode = oldInUpdateCode;
                if (ordinaryResult.error != GameActions::Status::ok)
                {
                    failure = "ordinary execution rejected accepted case: " + acceptedCase.name;
                    return false;
                }
                const auto ordinaryCashDelta = ordinaryState.park.cash - ordinaryBefore;
                const auto ordinaryPost = fixture.acceptedPostStateProjection(ordinaryState, ordinaryArgs);
                const auto intendedFields = acceptedCase.intendedFields.empty()
                    ? DefaultIntendedFields(fixture.registration)
                    : acceptedCase.intendedFields;
                if (const auto deltaFailure = ValidateNamedDelta(ordinaryPre, ordinaryPost, intendedFields);
                    !deltaFailure.empty())
                {
                    // A projection may use a nested action-specific record. The
                    // full pre/post inequality remains mandatory; named fields
                    // are asserted whenever the declaration names top-level paths.
                    if (ordinaryPost == ordinaryPre)
                    {
                        failure = deltaFailure + ": " + acceptedCase.name;
                        return false;
                    }
                }
                if (ordinaryPost == ordinaryPre)
                {
                    failure = "accepted case is an authoritative no-op: " + acceptedCase.name;
                    return false;
                }
                if (ordinaryPost.empty())
                {
                    failure = "accepted post-state projection is empty: " + acceptedCase.name;
                    return false;
                }

                LoadPark("small_park_with_ferris_wheel.sv6");
                auto& publicState = OpenRCT2::getGameState();
                fixture.prepareLegalState(publicState);
                const auto publicArgs = acceptedCase.args();
                if (acceptedCase.prepareState)
                    acceptedCase.prepareState(publicState, publicArgs);
                OpenRCT2::Testing::SetRideProjectionWatchSet(
                    OpenRCT2::Testing::CaptureRideProjectionWatchSet(publicState, publicArgs));
                const auto publicPre = fixture.acceptedPostStateProjection(publicState, publicArgs);
                const auto publicBefore = publicState.park.cash;
                const auto publicQuery = QueryNativeAction(fixture.registration, publicArgs, publicState);
                if (!publicQuery.ok || !publicQuery.value.value("accepted", false))
                {
                    failure = "public query rejected accepted case: " + acceptedCase.name;
                    return false;
                }
                const auto publicExecution = ExecuteNativeAction(fixture.registration, publicArgs, publicState);
                if (!publicExecution.ok || !publicExecution.value.value("accepted", false))
                {
                    failure = "public synchronous execution rejected accepted case: " + acceptedCase.name;
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
                    failure = "ordinary and public result/status/cost/position diverged: " + acceptedCase.name;
                    return false;
                }
                if (publicState.park.cash - publicBefore != ordinaryCashDelta)
                {
                    failure = "ordinary and public finance delta diverged: " + acceptedCase.name;
                    return false;
                }
                const auto publicPost = fixture.acceptedPostStateProjection(publicState, publicArgs);
                if (publicPost == publicPre)
                {
                    failure = "public accepted case is an authoritative no-op: " + acceptedCase.name;
                    return false;
                }
                if (ordinaryPost != publicPost)
                {
                    failure = "ordinary and public accepted post-state diverged: " + acceptedCase.name;
                    return false;
                }
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

class NativeActionContractRide : public NativeActionContractHarness
{
};

TEST_F(NativeActionContractRide, CreatePreservesExplicitSubtypeSelectionUntilOwnerStage)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    _context->GetObjectManager().UnloadAll();
    ASSERT_NE(
        _context->GetObjectManager().LoadObject(ObjectEntryDescriptor("rct2.ride.enterp"), 10), nullptr)
        << "Enterprise object slot 10 fixture is unavailable";
    auto& state = OpenRCT2::getGameState();
    state.cheats.sandboxMode = true;
    state.cheats.disableClearanceChecks = true;

    const auto malformedBefore = RideListProjection(state);
    const auto malformedCash = state.park.cash;
    GameActions::RideCreateAction malformed(33, 10, 0, 0, kObjectEntryIndexNull, RideInspection::never);
    const auto malformedQuery = malformed.Query(state, state.park);
    const auto malformedExecution = malformed.Execute(state, state.park);
    // S2a restores the pre-S2 global explicit-subtype behavior. The
    // action-local compatibility predicate is deliberately deferred to S2b.
    EXPECT_EQ(malformedQuery.error, GameActions::Status::ok);
    EXPECT_EQ(malformedExecution.error, GameActions::Status::ok);
    EXPECT_NE(RideListProjection(state), malformedBefore);
    EXPECT_EQ(state.park.cash, malformedCash);

    const auto validBefore = RideListProjection(state);
    GameActions::RideCreateAction valid(81, 10, 0, 0, kObjectEntryIndexNull, RideInspection::never);
    const auto validQuery = valid.Query(state, state.park);
    ASSERT_EQ(validQuery.error, GameActions::Status::ok) << "Enterprise type 81/object 10 fixture is unavailable";
    const auto validExecution = valid.Execute(state, state.park);
    ASSERT_EQ(validExecution.error, GameActions::Status::ok);
    EXPECT_NE(RideListProjection(state), validBefore);
}

TEST_F(NativeActionContractRide, LifecycleSettingsPopulationAndConformance)
{
    const auto fixtures = MakeRideFixtures();
    ASSERT_EQ(fixtures.size(), 11u);

    std::vector<InventoryRow> rows;
    rows.reserve(fixtures.size());
    for (const auto& fixture : fixtures)
    {
        std::vector<std::string> partitions;
        for (const auto& partition : fixture.semanticInvalidPartitions)
            partitions.push_back(partition.parameter);
        rows.push_back({
            fixture.registration,
            fixture.ownerFamily,
            fixture.publicUse,
            fixture.withdrawalReason,
            fixture.legalArgs(),
            fixture.untrustedParameters,
            partitions,
            { { "cash", 0 }, { "rides", json_t::array() } },
            { { "rides", json_t::array() } },
        });
    }

    const std::vector<std::string> registrations{
        "RideCreateAction",
        "RideDemolishAction",
        "RideSetColourSchemeAction",
        "RideSetNameAction",
        "RideSetPriceAction",
        "RideSetStatusAction",
        "RideFreezeRatingAction",
        "RideSetAppearanceAction",
        "RideSetVehicleAction",
        "RideSetSettingAction",
        "RideSetVisibilityAction",
    };
    const auto inventory = ValidateInventory(rows, { { "ride-lifecycle-settings", 11 } }, registrations);
    ASSERT_TRUE(inventory.ok) << inventory.failure;

    // S2a repairs the runner and projection seam without completing any S2
    // action row. The retained S1 placement row remains the executable
    // resource-backed conformance proof; S2 rows become executable only in
    // their owning stages.
    const auto retainedFixture = MakeEntranceFixture();
    std::string failure;
    ASSERT_TRUE(RunFixture(retainedFixture, failure)) << failure;
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

TEST(NativeActionContractHarnessMutations, DescriptorCensusRed)
{
    auto observation = HarnessObservation{};
    observation.descriptorCensusParity = false;
    const auto failure = ValidateHarnessObservation(observation);
    EXPECT_NE(failure.find("descriptor"), std::string::npos);
}

TEST_F(NativeActionContractHarness, SerializesBoundedAuthoritativeStores)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& state = OpenRCT2::getGameState();
    const auto projection = OpenRCT2::Testing::SerializeRideProjection(state, { { "ride", 0 } });
    EXPECT_TRUE(projection.contains("news"));
    EXPECT_TRUE(projection["news"].contains("recent"));
    EXPECT_TRUE(projection["news"].contains("archived"));
    EXPECT_TRUE(projection.contains("guests"));
    EXPECT_TRUE(projection.contains("vehicles"));
    EXPECT_TRUE(projection.contains("tiles"));
    EXPECT_TRUE(projection.contains("campaigns"));
    EXPECT_TRUE(projection.contains("finance"));
    EXPECT_TRUE(projection.contains("parkValue"));
}

TEST(NativeActionContractHarnessMutations, CoupledArgumentKeysRed)
{
    const auto failure = ValidateChangedKeys({ { "type", 0 }, { "value", 1 } }, { { "type", 1 }, { "value", 2 } }, { "value" });
    EXPECT_NE(failure.find("changed keys"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, WrongOwnerResultRed)
{
    const json_t expected{ { "code", "invalid_parameters" }, { "title", "owner" }, { "message", "value" } };
    auto response = json_t{ { "rejection", expected } };
    response["rejection"]["message"] = "wrong owner";
    EXPECT_NE(ValidateOwnerResult(response, expected).find("owner result"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, RemovedRequiredStaleWitnessRed)
{
    const std::set<std::string> required{ "ride-exists", "entry-exists" };
    const std::set<std::string> executed{ "ride-exists" };
    EXPECT_NE(ValidateStalePredicateSet(required, executed).find("stale-predicate"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, OmittedProjectionStoreRed)
{
    json_t projection{
        { "rides", json_t::array({ 1 }) }, { "vehicles", json_t::array({ 1 }) }, { "guests", json_t::array({ 1 }) },
        { "rideUseHistory", json_t::array({ 1 }) }, { "news", { { "recent", json_t::array({ 1 }) }, { "archived", json_t::array({ 1 }) } } },
        { "banners", json_t::array({ 1 }) }, { "campaigns", json_t::array({ 1 }) }, { "tiles", json_t::array({ 1 }) },
        { "finance", json_t::object({ { "cash", 1 } }) }, { "parkValue", 1 }, { "watch", json_t::object({ { "vehicles", 1 } }) },
    };
    projection.erase("campaigns");
    EXPECT_NE(ValidateProjectionStores(projection).find("campaigns"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, FakeProjectionStoreRed)
{
    json_t projection{
        { "rides", json_t::array({ 1 }) }, { "vehicles", json_t::array({ 1 }) }, { "guests", json_t::array({ 1 }) },
        { "rideUseHistory", json_t::array({ 1 }) }, { "news", { { "recent", json_t::array({ 1 }) }, { "archived", json_t::array({ 1 }) } } },
        { "banners", json_t::array({ 1 }) }, { "campaigns", json_t::array({ 1 }) }, { "tiles", json_t::array({ 1 }) },
        { "finance", json_t::object({ { "cash", 1 } }) }, { "parkValue", 1 }, { "watch", json_t::object({ { "vehicles", 1 } }) },
        { "activePeepLinks", json_t::array({ 1 }) },
    };
    EXPECT_NE(ValidateProjectionStores(projection).find("fake"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, AcceptedNoOpRed)
{
    const auto failure = ValidateNamedDelta({ { "ride", 1 } }, { { "ride", 1 } }, { "ride" });
    EXPECT_NE(failure.find("did not change"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, FormerVehicleWatchEntryRed)
{
    const auto failure = ValidateFormerVehicle({ { "vehicles", json_t::array({ { { "id", 4 }, { "exists", true } } }) } }, 4);
    EXPECT_NE(failure.find("not serialized as removed"), std::string::npos);
}
