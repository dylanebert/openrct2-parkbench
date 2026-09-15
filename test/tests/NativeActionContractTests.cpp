/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "NativeActionContractRideProjection.h"
#include "TestData.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
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
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/management/Marketing.h>
#include <openrct2/management/NewsItem.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/peep/RideUseSystem.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/world/Banner.h>
#include <openrct2/world/Entrance.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <openrct2/world/tile_element/EntranceElement.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
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

    enum class AcceptedCaseId : uint16_t
    {
        untyped,
        entrancePlace,
        exitPlace,
    };

    enum class InvalidCaseId : uint16_t
    {
        untyped,
        entranceXOffMap,
        entranceYOffMap,
        entranceDirectionOutOfRange,
        entranceRideMissing,
        entranceStationOutOfRange,
        exitXOffMap,
    };

    enum class ArgumentKey : uint8_t
    {
        x,
        y,
        direction,
        ride,
        station,
        isExit,
    };

    enum class OwnerResultId : uint8_t
    {
        untyped,
        entranceOffMap,
        exitOffMap,
        entranceDirectionOutOfRange,
        entranceRideNotFound,
        entranceStationOutOfRange,
        entranceNotClosed,
    };

    enum class StalePredicateId : uint8_t
    {
        untyped,
        entranceNotClosed,
    };

    struct ExpectedOwnerResult
    {
        GameActions::Status status;
        StringId title;
        StringId message;
    };

    struct ArgumentAssignment
    {
        ArgumentKey key;
        json_t value;
    };

    struct IntegerAdd
    {
        int64_t amount;
    };

    struct ReplaceLiteral
    {
        json_t before;
        json_t after;
    };

    struct EndpointFromArguments
    {
        ArgumentKey x;
        ArgumentKey y;
        ArgumentKey direction;
    };

    struct InsertedEntranceElement
    {
        bool isExit;
        int32_t count;
    };

    using DeltaRelation = std::variant<IntegerAdd, ReplaceLiteral, EndpointFromArguments, InsertedEntranceElement>;

    struct NamedFieldDelta
    {
        json_t::json_pointer path;
        DeltaRelation relation;

        NamedFieldDelta(std::string path_, DeltaRelation relation_)
            : path(std::move(path_))
            , relation(std::move(relation_))
        {
        }
    };

    struct SemanticInvalidPartition
    {
        std::string parameter;
        std::function<void(json_t&)> makeInvalid;
        std::string caseName;
        bool expectAccepted;
        std::function<void(GameState_t&, const json_t&)> prepareState;
        std::set<std::string> changedKeys;
        std::string acceptedCaseName = "legal";
        InvalidCaseId id = InvalidCaseId::untyped;
        AcceptedCaseId acceptedCaseId = AcceptedCaseId::untyped;
        std::vector<AcceptedCaseId> acceptedDependencies;
        std::vector<ArgumentAssignment> assignments;
        std::set<ArgumentKey> exactChangedKeys;
        OwnerResultId expectedOwner = OwnerResultId::untyped;

        SemanticInvalidPartition(
            std::string parameter_, std::function<void(json_t&)> makeInvalid_, std::string caseName_ = {},
            bool expectAccepted_ = false, std::function<void(GameState_t&, const json_t&)> prepareState_ = {},
            std::set<std::string> changedKeys_ = {}, std::string acceptedCaseName_ = "legal",
            OwnerResultId expectedOwner_ = OwnerResultId::untyped)
            : parameter(std::move(parameter_))
            , makeInvalid(std::move(makeInvalid_))
            , caseName(std::move(caseName_))
            , expectAccepted(expectAccepted_)
            , prepareState(std::move(prepareState_))
            , changedKeys(changedKeys_.empty() ? std::set<std::string>{ parameter } : std::move(changedKeys_))
            , acceptedCaseName(std::move(acceptedCaseName_))
            , expectedOwner(expectedOwner_)
        {
        }
    };

    struct AcceptedFixtureCase
    {
        std::string name;
        AcceptedCaseId id = AcceptedCaseId::untyped;
        std::function<json_t()> args;
        std::function<void(GameState_t&, const json_t&)> prepareState;
        std::vector<NamedFieldDelta> namedDeltas;

        AcceptedFixtureCase(
            std::string name_, std::function<json_t()> args_,
            std::function<void(GameState_t&, const json_t&)> prepareState_ = {},
            std::vector<NamedFieldDelta> namedDeltas_ = {})
            : name(std::move(name_))
            , args(std::move(args_))
            , prepareState(std::move(prepareState_))
            , namedDeltas(std::move(namedDeltas_))
        {
        }
    };

    struct StaleStateCase
    {
        std::string name;
        std::function<void(GameState_t&, const json_t&)> mutate;
        std::string predicate;
        StalePredicateId id = StalePredicateId::untyped;
        AcceptedCaseId base = AcceptedCaseId::untyped;
        OwnerResultId expectedOwner = OwnerResultId::untyped;

        StaleStateCase(std::string name_, std::function<void(GameState_t&, const json_t&)> mutate_, std::string predicate_ = {})
            : name(std::move(name_))
            , mutate(std::move(mutate_))
            , predicate(predicate_.empty() ? name : std::move(predicate_))
        {
        }
    };

    struct ProjectionContractSelector
    {
        bool enabled = false;

        explicit operator bool() const
        {
            return enabled;
        }

        bool operator()(const json_t& projection, std::string* failure) const
        {
            return OpenRCT2::Testing::ValidateRideProjectionStores(projection, failure);
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
        std::set<StalePredicateId> requiredTypedStalePredicates;
        std::vector<AcceptedFixtureCase> acceptedCases;
        std::function<json_t(const GameState_t&, const json_t&)> rejectionProjection;
        std::function<json_t(const GameState_t&, const json_t&)> acceptedPostStateProjection;
        ProjectionContractSelector projectionContract;
        // Test-only evidence corruption; RunFixture remains the sole source of truth.
        std::function<void(const SemanticInvalidPartition&, json_t&)> mutateExecuteResponse;
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

    std::string ValidateChangedKeys(const json_t& accepted, const json_t& invalid, const std::set<std::string>& declared)
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

    std::string ArgumentKeyName(ArgumentKey key)
    {
        switch (key)
        {
            case ArgumentKey::x: return "x";
            case ArgumentKey::y: return "y";
            case ArgumentKey::direction: return "direction";
            case ArgumentKey::ride: return "ride";
            case ArgumentKey::station: return "station";
            case ArgumentKey::isExit: return "isExit";
        }
        return {};
    }

    const ExpectedOwnerResult* ExpectedOwnerResultFor(OwnerResultId id)
    {
        static const std::array<ExpectedOwnerResult, 7> results{
            ExpectedOwnerResult{ GameActions::Status::invalidParameters, STR_CANT_BUILD_MOVE_ENTRANCE_FOR_THIS_RIDE_ATTRACTION,
                                 STR_OFF_EDGE_OF_MAP },
            ExpectedOwnerResult{ GameActions::Status::invalidParameters, STR_CANT_BUILD_MOVE_EXIT_FOR_THIS_RIDE_ATTRACTION,
                                 STR_OFF_EDGE_OF_MAP },
            ExpectedOwnerResult{ GameActions::Status::invalidParameters, STR_CANT_BUILD_MOVE_ENTRANCE_FOR_THIS_RIDE_ATTRACTION,
                                 STR_ERR_VALUE_OUT_OF_RANGE },
            ExpectedOwnerResult{ GameActions::Status::invalidParameters, STR_CANT_BUILD_MOVE_ENTRANCE_FOR_THIS_RIDE_ATTRACTION,
                                 STR_ERR_RIDE_NOT_FOUND },
            ExpectedOwnerResult{ GameActions::Status::invalidParameters, STR_CANT_BUILD_MOVE_ENTRANCE_FOR_THIS_RIDE_ATTRACTION,
                                 STR_ERR_VALUE_OUT_OF_RANGE },
            ExpectedOwnerResult{ GameActions::Status::notClosed, STR_CANT_BUILD_MOVE_ENTRANCE_FOR_THIS_RIDE_ATTRACTION,
                                 STR_MUST_BE_CLOSED_FIRST },
            ExpectedOwnerResult{ GameActions::Status::unknown, kStringIdNone, kStringIdNone },
        };
        if (id == OwnerResultId::untyped)
            return nullptr;
        const auto index = static_cast<size_t>(id) - 1;
        return index < results.size() ? &results[index] : nullptr;
    }

    std::string ActionStatusCode(GameActions::Status status)
    {
        switch (status)
        {
            case GameActions::Status::invalidParameters: return "invalid_parameters";
            case GameActions::Status::notClosed: return "not_closed";
            default: return "unknown";
        }
    }

    std::string ValidateOwnerResult(const json_t& response, OwnerResultId id)
    {
        const auto* expected = ExpectedOwnerResultFor(id);
        if (expected == nullptr)
            return "semantic partition has no runner-owned expected owner result";
        if (!response.contains("status") || response["status"] != static_cast<uint16_t>(expected->status))
            return "owner result status does not match runner-owned result";
        if (!response.contains("accepted") || response["accepted"] != false)
            return "owner result accepted flag does not match runner-owned result";
        const auto expectedResult = GameActions::Result(expected->status, expected->title, expected->message);
        const json_t expectedRejection{
            { "code", ActionStatusCode(expected->status) },
            { "title", expectedResult.getErrorTitle() },
            { "message", expectedResult.getErrorMessage() },
            { "detail", { { "status", static_cast<uint16_t>(expected->status) } } },
        };
        if (!response.contains("rejection") || response["rejection"] != expectedRejection)
            return "owner result/title/message/detail does not match runner-owned result";
        return {};
    }

    std::string ValidateProjectionStores(const json_t& projection)
    {
        static constexpr std::array<std::string_view, 11> requiredStores{
            "rides",     "vehicles", "guests",  "rideUseHistory", "news",  "banners",
            "campaigns", "tiles",    "finance", "parkValue",      "watch",
        };
        for (const auto key : requiredStores)
            if (!projection.contains(key) || projection.at(key).empty())
                return "projection store is omitted or empty: " + std::string(key);
        if (projection.contains("authoritative") || projection.contains("activePeepLinks"))
            return "projection contains a fake or relabeled store";
        if (!projection.at("news").contains("recent") || !projection.at("news").contains("archived"))
            return "news projection does not preserve both ordered queues";
        return {};
    }

    std::string ValidateNamedDelta(
        const json_t& before, const json_t& after, const json_t& args, const std::vector<NamedFieldDelta>& deltas)
    {
        if (deltas.empty())
            return "accepted case declares no exact JSON field delta";
        for (const auto& delta : deltas)
        {
            try
            {
                const auto& beforeValue = before.at(delta.path);
                const auto& afterValue = after.at(delta.path);
                const auto argument = [&args](ArgumentKey key) -> const json_t& { return args.at(ArgumentKeyName(key)); };
                const auto result = std::visit(
                    [&beforeValue, &afterValue, &args, &argument](const auto& relation) -> bool {
                        using Relation = std::decay_t<decltype(relation)>;
                        if constexpr (std::is_same_v<Relation, IntegerAdd>)
                        {
                            return beforeValue.is_number_integer() && afterValue.is_number_integer()
                                && afterValue.get<int64_t>() == beforeValue.get<int64_t>() + relation.amount;
                        }
                        else if constexpr (std::is_same_v<Relation, ReplaceLiteral>)
                        {
                            return beforeValue == relation.before && afterValue == relation.after;
                        }
                        else if constexpr (std::is_same_v<Relation, EndpointFromArguments>)
                        {
                            if (!beforeValue.is_null() || !afterValue.is_object())
                                return false;
                            return afterValue.value("x", std::numeric_limits<int32_t>::min())
                                    == argument(relation.x).template get<int32_t>() / kCoordsXYStep
                                && afterValue.value("y", std::numeric_limits<int32_t>::min())
                                    == argument(relation.y).template get<int32_t>() / kCoordsXYStep
                                && afterValue.value("direction", 255) == argument(relation.direction).template get<uint8_t>()
                                && afterValue.contains("z") && afterValue.value("z", -1) == afterValue.value("stationBaseZ", -2);
                        }
                        else if constexpr (std::is_same_v<Relation, InsertedEntranceElement>)
                        {
                            if (!beforeValue.is_null() || !afterValue.is_object() || relation.count != 1)
                                return false;
                            const auto expectedType = relation.isExit ? ENTRANCE_TYPE_RIDE_EXIT : ENTRANCE_TYPE_RIDE_ENTRANCE;
                            if (!afterValue.contains("stationBaseZ") || !afterValue.contains("clearanceHeight"))
                                return false;
                            const auto expectedClearance = afterValue.at("stationBaseZ").get<int32_t>()
                                + (relation.isExit ? RideExitHeight : RideEntranceHeight) / kCoordsZStep;
                            return afterValue.value("count", 0) == relation.count
                                && afterValue.value("type", -1) == expectedType
                                && afterValue.value("ride", -1) == args.value("ride", -2)
                                && afterValue.value("station", -1) == args.value("station", -2)
                                && afterValue.value("direction", -1) == args.value("direction", -2)
                                && afterValue.value("ghost", true) == false
                                && afterValue.value("baseHeight", -1) == afterValue.value("stationBaseZ", -2)
                                && afterValue.at("clearanceHeight").get<int32_t>() == expectedClearance;
                        }
                    },
                    delta.relation);
                if (!result)
                    return "accepted case did not satisfy intended field delta: " + delta.path.to_string()
                        + " before=" + beforeValue.dump() + " after=" + afterValue.dump() + " args=" + args.dump();
            }
            catch (const std::exception&)
            {
                return "accepted case is missing intended JSON path: " + delta.path.to_string();
            }
        }
        return {};
    }

    std::string ValidateOwnerResult(const json_t& response, const json_t& expectedRejection)
    {
        if (!response.contains("rejection") || response["rejection"] != expectedRejection)
            return "owner result/title/message does not match the declared owner result";
        return {};
    }

    std::string ValidateOwnerResultIdentity(const json_t& queried, const json_t& executed)
    {
        static constexpr std::array<std::string_view, 3> ownerResultFields{ "status", "accepted", "rejection" };
        for (const auto field : ownerResultFields)
        {
            if (!queried.contains(field) || !executed.contains(field) || queried.at(field) != executed.at(field))
                return "execute owner result does not match the prior query rejection";
        }
        return {};
    }

    std::string ValidateStalePredicateSet(
        const std::set<std::string>& requiredPredicates, const std::set<std::string>& executedPredicates)
    {
        return requiredPredicates == executedPredicates ? std::string{}
                                                        : "executed stale-predicate set differs from required census";
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
                        location = { CoordsXY{ tile.x * kCoordsXYStep, tile.y * kCoordsXYStep }, track->getBaseZ(),
                                     track->getDirection() };
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

    void PopulateRideProjectionFixture(GameState_t& state, RideId rideId)
    {
        // These are deliberate values, not presence aliases. They make every
        // store in the action projection observable and keep the watch
        // identities meaningful if demolition/clearing is added later.
        state.newsItems[0] = {
            News::ItemType::ride, 0, rideId.ToUnderlying(), 7, 12, 3, "S2 recent target news",
        };
        state.newsItems[News::ItemHistoryStart] = {
            News::ItemType::ride, 1, rideId.ToUnderlying(), 9, 11, 2, "S2 archived target news",
        };
        MarketingCampaign campaign{};
        campaign.type = 0;
        campaign.weeksLeft = 3;
        campaign.rideId = rideId;
        state.park.marketingCampaigns.push_back(campaign);
        state.park.value = 12345;
        state.park.valueHistory[0] = 12000;

        if (auto* banner = CreateBanner(); banner != nullptr)
        {
            banner->type = 0;
            banner->rideIndex = rideId;
            banner->text = "S2 target banner";
            banner->colour = Drawing::Colour::brightRed;
            banner->textColour = Drawing::TextColour::white;
            banner->position = { 1, 1 };
        }

        Guest* guest = nullptr;
        for (const auto id : state.entities.GetEntityList(EntityType::guest))
        {
            guest = state.entities.GetEntity<Guest>(id);
            if (guest != nullptr)
                break;
        }
        if (guest == nullptr)
            guest = state.entities.CreateEntity<Guest>();
        if (guest != nullptr)
        {
            guest->CurrentRide = rideId;
            guest->CurrentRideStation = StationIndex::FromUnderlying(0);
            guest->CurrentTrain = 0;
            guest->CurrentCar = 0;
            guest->CurrentSeat = 0;
            guest->rejoinQueueTimeout = 4;
            guest->previousRide = rideId;
            guest->previousRideTimeOut = 8;
            guest->voucherRideId = rideId;
            guest->photo1RideRef = rideId;
            guest->favouriteRide = rideId;
            guest->guestNextInQueue = EntityId::GetNull();
            guest->thoughts[0].type = PeepThoughtType::wasGreat;
            guest->thoughts[0].rideId = rideId;
            guest->thoughts[0].freshness = 1;
            guest->thoughts[0].fresh_timeout = 2;
            RideUse::GetHistory().Add(guest->id, rideId);

            auto* vehicle = state.entities.CreateEntity<Vehicle>();
            if (vehicle != nullptr)
            {
                vehicle->ride = rideId;
                vehicle->num_seats = 2;
                vehicle->num_peeps = 1;
                vehicle->next_free_seat = 1;
                vehicle->peep[0] = guest->id;
                if (auto* ride = GetRide(rideId); ride != nullptr)
                {
                    ride->vehicles[0] = vehicle->id;
                    ride->numTrains = std::max<uint8_t>(ride->numTrains, 1);
                }
                guest->CurrentTrain = 0;
                guest->CurrentCar = 0;
                guest->CurrentSeat = 0;
            }
        }
    }

    json_t RideListProjection(const GameState_t& state)
    {
        json_t rides = json_t::array();
        for (RideId::UnderlyingType i = 0; i < Limits::kMaxRidesInPark; ++i)
        {
            const auto* ride = GetRide(RideId::FromUnderlying(i));
            if (ride == nullptr)
                continue;
            rides.push_back(
                {
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
                rides.back()["trackColours"].push_back(
                    {
                        { "main", static_cast<uint8_t>(colours.main) },
                        { "additional", static_cast<uint8_t>(colours.additional) },
                        { "supports", static_cast<uint8_t>(colours.supports) },
                    });
            }
            for (const auto& colours : ride->vehicleColours)
            {
                rides.back()["vehicleColours"].push_back(
                    {
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
                    trackState.push_back(
                        {
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
                fixture.untrustedParameters = { "rideType", "rideObject", "entranceObject",
                                                "colour1",  "colour2",    "inspectionInterval" };
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
                    { "renewal",
                      [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "modifyType", 1 } }; },
                      {} },
                    { "demolition",
                      [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "modifyType", 0 } }; },
                      {} },
                };
                fixture.staleStateCases = {
                    { "renewal-open-ride",
                      [values](GameState_t&, const json_t&) {
                          if (auto* ride = GetRide(values->ride); ride != nullptr)
                              ride->status = RideStatus::open;
                      } },
                    { "renewal-has-riders",
                      [values](GameState_t&, const json_t&) {
                          if (auto* ride = GetRide(values->ride); ride != nullptr)
                              ride->numRiders = 1;
                      } },
                    { "renewal-not-needed",
                      [values](GameState_t&, const json_t&) {
                          if (auto* ride = GetRide(values->ride); ride != nullptr)
                              ride->flags.unset(RideFlag::everBeenOpened);
                      } },
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
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
                        { "x", values->track.x },
                        { "y", values->track.y },
                        { "z", values->track.z },
                        { "direction", static_cast<uint8_t>(values->track.direction) },
                        { "trackType", static_cast<uint16_t>(values->trackType) },
                        { "colourScheme", 1 },
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
                    const CoordsXYZD location{ args.at("x").get<int32_t>(), args.at("y").get<int32_t>(),
                                               args.at("z").get<int32_t>(),
                                               static_cast<Direction>(args.at("direction").get<uint8_t>()) };
                    if (auto* track = MapGetTrackElementAtOfType(
                            location, static_cast<TrackElemType>(args.at("trackType").get<uint16_t>()));
                        track != nullptr)
                    {
                        TileElementRemove(reinterpret_cast<TileElement*>(track));
                    }
                };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return TrackProjection(state, args);
                };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                    return TrackProjection(state, args);
                };
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
                fixture.legalArgs = [values] {
                    return json_t{ { "ride", values->ride.ToUnderlying() }, { "name", "S2 Native Ride" } };
                };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "name", [](json_t& args) { args["name"] = "This ride name is intentionally too long"; }, "name-length" },
                };
                fixture.staleStateCases = {
                    { "duplicate-name",
                      [values](GameState_t& state, const json_t& args) {
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
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
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
                fixture.legalArgs = [values] {
                    return json_t{ { "ride", values->ride.ToUnderlying() }, { "price", 1 }, { "isPrimaryPrice", true } };
                };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "price", [](json_t& args) { args["price"] = -1; }, "below-minimum" },
                    { "price", [](json_t& args) { args["price"] = kRideMaxPrice + 1; }, "above-maximum" },
                    { "isPrimaryPrice", [](json_t& args) { args["isPrimaryPrice"] = false; }, "secondary-price-legal", true },
                };
                fixture.acceptedCases = {
                    { "primary-price",
                      [values] {
                          return json_t{ { "ride", values->ride.ToUnderlying() }, { "price", 1 }, { "isPrimaryPrice", true } };
                      },
                      {} },
                    { "secondary-price",
                      [values] {
                          return json_t{ { "ride", values->ride.ToUnderlying() },
                                         { "price", kRideMaxPrice },
                                         { "isPrimaryPrice", false } };
                      },
                      {} },
                };
                fixture.staleStateCases = {
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                    { "unloaded-ride-object",
                      [values](GameState_t&, const json_t&) {
                          if (auto* ride = GetRide(values->ride); ride != nullptr)
                              ride->subtype = kObjectEntryIndexNull;
                      } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
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
                    { "open-test-simulate-topology",
                      [values](GameState_t& state, const json_t&) {
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
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
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
                fixture.legalArgs = [values] {
                    return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 0 }, { "value", 100 } };
                };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "type", [](json_t& args) { args["type"] = 255; }, "rating-type-domain" },
                    { "value", [](json_t& args) { args["value"] = 0; }, "rating-value-domain" },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return RideProjection(state, args);
                };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                    return RideProjection(state, args);
                };
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
                fixture.legalArgs = [values] {
                    return json_t{ { "ride", values->ride.ToUnderlying() },
                                   { "type", 7 },
                                   { "value", values->entranceObject },
                                   { "index", 0 } };
                };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "type", [](json_t& args) { args["type"] = 255; }, "invalid-type" },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 0;
                          args["value"] = 255;
                      },
                      "track-main-colour",
                      false,
                      {},
                      { "type", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 3;
                          args["value"] = 255;
                      },
                      "vehicle-body-colour",
                      false,
                      {},
                      { "type", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 6;
                          args["value"] = 255;
                      },
                      "vehicle-colour-scheme",
                      false,
                      {},
                      { "type", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 7;
                          args["value"] = 254;
                      },
                      "entrance-object-identity",
                      false,
                      {},
                      { "value" } },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 8;
                          args["value"] = 2;
                      },
                      "random-colour-boolean",
                      false,
                      {},
                      { "type", "value" } },
                    { "index",
                      [](json_t& args) {
                          args["type"] = 0;
                          args["value"] = 1;
                          args["index"] = 255;
                      },
                      "appearance-index",
                      false,
                      {},
                      { "type", "value", "index" } },
                };
                fixture.acceptedCases = {
                    { "track-main",
                      [values] {
                          return json_t{
                              { "ride", values->ride.ToUnderlying() }, { "type", 0 }, { "value", 2 }, { "index", 0 }
                          };
                      },
                      {} },
                    { "track-additional",
                      [values] {
                          return json_t{
                              { "ride", values->ride.ToUnderlying() }, { "type", 1 }, { "value", 2 }, { "index", 0 }
                          };
                      },
                      {} },
                    { "track-supports",
                      [values] {
                          return json_t{
                              { "ride", values->ride.ToUnderlying() }, { "type", 2 }, { "value", 3 }, { "index", 0 }
                          };
                      },
                      {} },
                    { "vehicle-body",
                      [values] {
                          return json_t{
                              { "ride", values->ride.ToUnderlying() }, { "type", 3 }, { "value", 1 }, { "index", 0 }
                          };
                      },
                      {} },
                    { "vehicle-trim",
                      [values] {
                          return json_t{
                              { "ride", values->ride.ToUnderlying() }, { "type", 4 }, { "value", 2 }, { "index", 0 }
                          };
                      },
                      {} },
                    { "vehicle-tertiary",
                      [values] {
                          return json_t{
                              { "ride", values->ride.ToUnderlying() }, { "type", 5 }, { "value", 3 }, { "index", 0 }
                          };
                      },
                      {} },
                    { "vehicle-scheme",
                      [values] {
                          return json_t{
                              { "ride", values->ride.ToUnderlying() }, { "type", 6 }, { "value", 1 }, { "index", 0 }
                          };
                      },
                      {} },
                };
                fixture.staleStateCases = {
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                    { "non-applicable-entrance",
                      [values](GameState_t&, const json_t&) {
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
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
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
                fixture.legalArgs = [values] {
                    return json_t{ { "ride", values->ride.ToUnderlying() }, { "type", 0 }, { "value", 1 }, { "colour", 0 } };
                };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "type", [](json_t& args) { args["type"] = 255; }, "invalid-type" },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 0;
                          args["value"] = 0;
                      },
                      "num-trains-zero" },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 1;
                          args["value"] = 0;
                      },
                      "cars-per-train-zero",
                      false,
                      {},
                      { "type", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 1;
                          args["value"] = 255;
                      },
                      "cars-per-train-descriptor-bound",
                      false,
                      {},
                      { "type", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 2;
                          args["value"] = 65535;
                      },
                      "ride-entry-identity",
                      false,
                      {},
                      { "type", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 3;
                          args["value"] = 2;
                      },
                      "reversed-trains-domain",
                      false,
                      {},
                      { "type", "value" } },
                    { "colour",
                      [values](json_t& args) {
                          args["type"] = 2;
                          args["value"] = values->rideObject;
                          args["colour"] = 254;
                      },
                      "loaded-entry-colour-preset",
                      false,
                      [](GameState_t& state, const json_t&) { state.cheats.ignoreResearchStatus = true; },
                      { "type", "value", "colour" } },
                    { "value",
                      [](json_t& args) {
                          args["type"] = 1;
                          args["value"] = 0;
                      },
                      "cars-cheat-storage-zero",
                      false,
                      [](GameState_t& state, const json_t&) { state.cheats.disableTrainLengthLimit = true; },
                      { "type", "value" } },
                };
                fixture.acceptedCases = {
                    { "cars-per-train-cheat",
                      [values] {
                          return json_t{
                              { "ride", values->ride.ToUnderlying() }, { "type", 1 }, { "value", 255 }, { "colour", 0 }
                          };
                      },
                      [](GameState_t& state, const json_t&) { state.cheats.disableTrainLengthLimit = true; } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.staleStateCases = {
                    { "broken-ride",
                      [values](GameState_t&, const json_t&) {
                          if (auto* ride = GetRide(values->ride); ride != nullptr)
                              ride->flags.set(RideFlag::brokenDown);
                      } },
                    { "open-ride",
                      [values](GameState_t&, const json_t&) {
                          if (auto* ride = GetRide(values->ride); ride != nullptr)
                              ride->status = RideStatus::open;
                      } },
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
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
                    return json_t{ { "ride", values->ride.ToUnderlying() },
                                   { "setting", 0 },
                                   { "value", ride == nullptr ? 0 : static_cast<uint8_t>(ride->mode) } };
                };
                fixture.semanticInvalidPartitions = {
                    { "ride", [](json_t& args) { args["ride"] = 65535; }, "missing-ride" },
                    { "setting", [](json_t& args) { args["setting"] = 255; }, "invalid-setting" },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 0;
                          args["value"] = 255;
                      },
                      "mode-domain" },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 2;
                          args["value"] = 251;
                      },
                      "minimum-wait-bound",
                      false,
                      {},
                      { "setting", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 3;
                          args["value"] = 251;
                      },
                      "maximum-wait-bound",
                      false,
                      {},
                      { "setting", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 4;
                          args["value"] = 255;
                      },
                      "operation-descriptor-bound",
                      false,
                      {},
                      { "setting", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 5;
                          args["value"] = 255;
                      },
                      "inspection-domain",
                      false,
                      {},
                      { "setting", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 6;
                          args["value"] = 2;
                      },
                      "music-boolean",
                      false,
                      {},
                      { "setting", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 7;
                          args["value"] = 255;
                      },
                      "music-object-identity",
                      false,
                      {},
                      { "setting", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 8;
                          args["value"] = 255;
                      },
                      "lift-hill-descriptor-bound",
                      false,
                      {},
                      { "setting", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 9;
                          args["value"] = 255;
                      },
                      "circuit-cable-lift-bound",
                      false,
                      {},
                      { "setting", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 10;
                          args["value"] = 255;
                      },
                      "ride-type-domain",
                      false,
                      {},
                      { "setting", "value" } },
                    { "value",
                      [](json_t& args) {
                          args["setting"] = 1;
                          args["value"] = 255;
                      },
                      "departure-value-domain", true },
                };
                fixture.acceptedCases = {
                    { "mode",
                      [values] {
                          const auto* ride = GetRide(values->ride);
                          return json_t{ { "ride", values->ride.ToUnderlying() },
                                         { "setting", 0 },
                                         { "value", ride == nullptr ? 0 : static_cast<uint8_t>(ride->mode) } };
                      },
                      {} },
                    { "departure",
                      [values] {
                          return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 1 }, { "value", 255 } };
                      },
                      {} },
                    { "minimum-wait",
                      [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 2 }, { "value", 10 } }; },
                      {} },
                    { "maximum-wait",
                      [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 3 }, { "value", 20 } }; },
                      {} },
                    { "operation",
                      [values] {
                          const auto* ride = GetRide(values->ride);
                          return json_t{ { "ride", values->ride.ToUnderlying() },
                                         { "setting", 4 },
                                         { "value", ride == nullptr ? 0 : ride->operationOption } };
                      },
                      {} },
                    { "inspection",
                      [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 5 }, { "value", 0 } }; },
                      {} },
                    { "music-off",
                      [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 6 }, { "value", 0 } }; },
                      {} },
                    { "music-on",
                      [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 6 }, { "value", 1 } }; },
                      {} },
                    { "music-object",
                      [values] {
                          return json_t{ { "ride", values->ride.ToUnderlying() },
                                         { "setting", 7 },
                                         { "value", GetRide(values->ride) == nullptr ? 0 : GetRide(values->ride)->music } };
                      },
                      {} },
                    { "lift-hill",
                      [values] {
                          return json_t{ { "ride", values->ride.ToUnderlying() },
                                         { "setting", 8 },
                                         { "value",
                                           GetRide(values->ride) == nullptr ? 0 : GetRide(values->ride)->liftHillSpeed } };
                      },
                      {} },
                    { "circuits",
                      [values] { return json_t{ { "ride", values->ride.ToUnderlying() }, { "setting", 9 }, { "value", 1 } }; },
                      {} },
                    { "ride-type-cheat",
                      [values] {
                          return json_t{ { "ride", values->ride.ToUnderlying() },
                                         { "setting", 10 },
                                         { "value", GetRide(values->ride) == nullptr ? 0 : GetRide(values->ride)->type } };
                      },
                      [](GameState_t& state, const json_t&) { state.cheats.allowArbitraryRideTypeChanges = true; } },
                };
                fixture.staleStateCases = {
                    { "broken-operating-ride",
                      [values](GameState_t&, const json_t&) {
                          if (auto* ride = GetRide(values->ride); ride != nullptr)
                              ride->flags.set(RideFlag::brokenDown);
                      } },
                    { "open-operating-ride",
                      [values](GameState_t&, const json_t&) {
                          if (auto* ride = GetRide(values->ride); ride != nullptr)
                              ride->status = RideStatus::open;
                      } },
                    { "deleted-ride", [values](GameState_t&, const json_t&) { RideDelete(values->ride); } },
                };
                fixture.mutateRelevantState = [values](GameState_t&, const json_t&) { RideDelete(values->ride); };
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                    return ExactRideActionProjection(state, args);
                };
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
                fixture.rejectionProjection = [](const GameState_t& state, const json_t& args) {
                    return TrackProjection(state, args);
                };
                fixture.acceptedPostStateProjection = [](const GameState_t& state, const json_t& args) {
                    return TrackProjection(state, args);
                };
                fixture.makeOrdinaryAction = [](const json_t& args) {
                    return std::make_unique<GameActions::RideSetVisibilityAction>(
                        RideId::FromUnderlying(args.at("ride").get<uint16_t>()),
                        static_cast<GameActions::RideSetVisibilityType>(args.at("visiblity").get<uint8_t>()));
                };
                fixtures.push_back(std::move(fixture));
            }

            for (auto& fixture : fixtures)
            {
                if (fixture.acceptedCases.empty())
                    fixture.acceptedCases.push_back({ "legal", fixture.legalArgs, {} });
                for (auto& partition : fixture.semanticInvalidPartitions)
                    partition.acceptedCaseName = "legal";
                fixture.requiredStalePredicates = RequiredStalePredicates(fixture.registration);
            }
            return fixtures;
        }

        NativeActionFixture MakeEntranceFixture()
        {
            NativeActionFixture fixture;
            const auto legalArgs = std::make_shared<json_t>();
            const auto exitArgs = std::make_shared<json_t>();
            fixture.registration = "RideEntranceExitPlaceAction";
            fixture.ownerFamily = "station-track-maze";
            fixture.publicUse = "paused monitor construction of a ride entrance";
            fixture.untrustedParameters = { "x", "y", "direction", "ride", "station", "isExit" };
            fixture.transportOmissions = { "queue", "network", "replay", "action-log", "autosave", "ui" };
            fixture.prepareLegalState = [legalArgs, exitArgs](GameState_t& state) {
                state.cheats.disableClearanceChecks = true;
                state.cheats.sandboxMode = true;
                auto* ride = FindRideWithEntranceAndTrack();
                if (ride == nullptr)
                    return;
                ride->status = RideStatus::closed;
                const auto station = StationIndex::FromUnderlying(0);
                const auto entrance = ride->getStation(station).Entrance;
                const auto exit = ride->getStation(station).Exit;
                if (entrance.IsNull() || exit.IsNull())
                    return;
                *legalArgs = {
                    { "x", entrance.ToCoordsXY().x },
                    { "y", entrance.ToCoordsXY().y },
                    { "direction", static_cast<uint8_t>(entrance.direction) },
                    { "ride", ride->id.ToUnderlying() },
                    { "station", 0 },
                    { "isExit", false },
                };
                *exitArgs = {
                    { "x", exit.ToCoordsXY().x },
                    { "y", exit.ToCoordsXY().y },
                    { "direction", static_cast<uint8_t>(exit.direction) },
                    { "ride", ride->id.ToUnderlying() },
                    { "station", 0 },
                    { "isExit", true },
                };
                PopulateRideProjectionFixture(state, ride->id);
            };
            fixture.legalArgs = [legalArgs] { return *legalArgs; };
            const auto removeEndpoint = [](GameState_t& state, const json_t& args) {
                const auto rideId = RideId::FromUnderlying(args.at("ride").get<uint16_t>());
                const auto station = StationIndex::FromUnderlying(args.at("station").get<uint8_t>());
                GameActions::RideEntranceExitRemoveAction remove(
                    CoordsXY{ args.at("x").get<int32_t>(), args.at("y").get<int32_t>() }, rideId, station,
                    args.at("isExit").get<bool>());
                remove.SetFlags(
                    { GameActions::CommandFlag::apply, GameActions::CommandFlag::allowDuringPaused,
                      GameActions::CommandFlag::noSpend });
                if (!ExecuteSetupAction(remove, state))
                    return;
                if (auto* ride = GetRide(rideId); ride != nullptr)
                {
                    auto& stationData = ride->getStation(station);
                    stationData.LastPeepInQueue = EntityId::FromUnderlying(1);
                    stationData.QueueLength = 37;
                    stationData.QueueTime = 19;
                }
            };
            fixture.semanticInvalidPartitions = {
                { "x", {}, "entranceXOffMap" },
                { "y", {}, "entranceYOffMap" },
                { "direction", {}, "entranceDirectionOutOfRange" },
                { "ride", {}, "entranceRideMissing" },
                { "station", {}, "entranceStationOutOfRange" },
                { "x", {}, "exitXOffMap" },
            };
            fixture.semanticInvalidPartitions[0].id = InvalidCaseId::entranceXOffMap;
            fixture.semanticInvalidPartitions[0].acceptedCaseId = AcceptedCaseId::entrancePlace;
            fixture.semanticInvalidPartitions[0].acceptedDependencies = { AcceptedCaseId::entrancePlace };
            fixture.semanticInvalidPartitions[0].assignments = { { ArgumentKey::x, -1 } };
            fixture.semanticInvalidPartitions[0].exactChangedKeys = { ArgumentKey::x };
            fixture.semanticInvalidPartitions[0].expectedOwner = OwnerResultId::entranceOffMap;
            fixture.semanticInvalidPartitions[1].id = InvalidCaseId::entranceYOffMap;
            fixture.semanticInvalidPartitions[1].acceptedCaseId = AcceptedCaseId::entrancePlace;
            fixture.semanticInvalidPartitions[1].acceptedDependencies = { AcceptedCaseId::entrancePlace };
            fixture.semanticInvalidPartitions[1].assignments = { { ArgumentKey::y, -1 } };
            fixture.semanticInvalidPartitions[1].exactChangedKeys = { ArgumentKey::y };
            fixture.semanticInvalidPartitions[1].expectedOwner = OwnerResultId::entranceOffMap;
            fixture.semanticInvalidPartitions[2].id = InvalidCaseId::entranceDirectionOutOfRange;
            fixture.semanticInvalidPartitions[2].acceptedCaseId = AcceptedCaseId::entrancePlace;
            fixture.semanticInvalidPartitions[2].acceptedDependencies = { AcceptedCaseId::entrancePlace };
            fixture.semanticInvalidPartitions[2].assignments = { { ArgumentKey::direction, 4 } };
            fixture.semanticInvalidPartitions[2].exactChangedKeys = { ArgumentKey::direction };
            fixture.semanticInvalidPartitions[2].expectedOwner = OwnerResultId::entranceDirectionOutOfRange;
            fixture.semanticInvalidPartitions[3].id = InvalidCaseId::entranceRideMissing;
            fixture.semanticInvalidPartitions[3].acceptedCaseId = AcceptedCaseId::entrancePlace;
            fixture.semanticInvalidPartitions[3].acceptedDependencies = { AcceptedCaseId::entrancePlace };
            fixture.semanticInvalidPartitions[3].assignments = { { ArgumentKey::ride, 65535 } };
            fixture.semanticInvalidPartitions[3].exactChangedKeys = { ArgumentKey::ride };
            fixture.semanticInvalidPartitions[3].expectedOwner = OwnerResultId::entranceRideNotFound;
            fixture.semanticInvalidPartitions[4].id = InvalidCaseId::entranceStationOutOfRange;
            fixture.semanticInvalidPartitions[4].acceptedCaseId = AcceptedCaseId::entrancePlace;
            fixture.semanticInvalidPartitions[4].acceptedDependencies = { AcceptedCaseId::entrancePlace };
            fixture.semanticInvalidPartitions[4].assignments = { { ArgumentKey::station, 255 } };
            fixture.semanticInvalidPartitions[4].exactChangedKeys = { ArgumentKey::station };
            fixture.semanticInvalidPartitions[4].expectedOwner = OwnerResultId::entranceStationOutOfRange;
            fixture.semanticInvalidPartitions[5].id = InvalidCaseId::exitXOffMap;
            fixture.semanticInvalidPartitions[5].acceptedCaseId = AcceptedCaseId::exitPlace;
            fixture.semanticInvalidPartitions[5].acceptedDependencies = { AcceptedCaseId::exitPlace };
            fixture.semanticInvalidPartitions[5].assignments = { { ArgumentKey::x, -1 } };
            fixture.semanticInvalidPartitions[5].exactChangedKeys = { ArgumentKey::x };
            fixture.semanticInvalidPartitions[5].expectedOwner = OwnerResultId::exitOffMap;
            fixture.mutateRelevantState = [](GameState_t&, const json_t& args) {
                const auto rideValue = args.value("ride", -1);
                if (rideValue >= 0 && rideValue < Limits::kMaxRidesInPark)
                {
                    if (auto* ride = GetRide(RideId::FromUnderlying(rideValue)); ride != nullptr)
                        ride->status = RideStatus::open;
                }
            };
            fixture.staleStateCases = {
                { "entranceNotClosed",
                  [](GameState_t&, const json_t& args) {
                      if (auto* ride = GetRide(RideId::FromUnderlying(args.at("ride").get<uint16_t>())); ride != nullptr)
                          ride->status = RideStatus::open;
                  } },
            };
            fixture.staleStateCases.front().id = StalePredicateId::entranceNotClosed;
            fixture.staleStateCases.front().base = AcceptedCaseId::entrancePlace;
            fixture.staleStateCases.front().expectedOwner = OwnerResultId::entranceNotClosed;
            fixture.requiredTypedStalePredicates = { StalePredicateId::entranceNotClosed };
            fixture.requiredStalePredicates.clear();
            const auto addPlacementProjection = [](json_t& projection, const GameState_t& state, const json_t& args) {
                const auto rideValue = args.value("ride", -1);
                const auto stationValue = args.value("station", 0);
                if (rideValue < 0 || static_cast<size_t>(rideValue) >= state.rides.size()
                    || stationValue < 0 || stationValue >= Limits::kMaxStationsPerRide)
                    return;
                const auto& station = state.rides[static_cast<size_t>(rideValue)].getStation(
                    StationIndex::FromUnderlying(stationValue));
                const auto endpoint = args.value("isExit", false) ? station.Exit : station.Entrance;
                projection["endpoint"] = endpoint.IsNull() ? json_t(nullptr) : json_t{
                    { "x", endpoint.x }, { "y", endpoint.y }, { "z", endpoint.z },
                    { "direction", endpoint.direction }, { "stationBaseZ", station.GetBaseZ() / kCoordsZStep },
                };
                projection["stationBaseZ"] = station.GetBaseZ() / kCoordsZStep;
                projection["queueLastPeep"] = station.LastPeepInQueue.IsNull()
                    ? json_t(nullptr) : json_t(station.LastPeepInQueue.ToUnderlying());
                projection["queueLength"] = station.QueueLength;
                projection["insertedElement"] = nullptr;
                const auto tile = TileCoordsXY{ args.value("x", 0) / kCoordsXYStep, args.value("y", 0) / kCoordsXYStep };
                auto* element = MapGetFirstElementAt(tile);
                if (element == nullptr)
                    return;
                while (true)
                {
                    if (const auto* entrance = element->asEntrance(); entrance != nullptr
                        && entrance->GetRideIndex().ToUnderlying() == rideValue
                        && entrance->GetStationIndex().ToUnderlying() == stationValue
                        && entrance->GetEntranceType()
                            == (args.value("isExit", false) ? ENTRANCE_TYPE_RIDE_EXIT : ENTRANCE_TYPE_RIDE_ENTRANCE))
                    {
                        projection["insertedElement"] = {
                            { "count", 1 }, { "type", entrance->GetEntranceType() },
                            { "ride", entrance->GetRideIndex().ToUnderlying() },
                            { "station", entrance->GetStationIndex().ToUnderlying() },
                            { "direction", static_cast<uint8_t>(entrance->getDirection()) },
                            { "baseHeight", element->baseHeight }, { "clearanceHeight", element->clearanceHeight },
                            { "stationBaseZ", station.GetBaseZ() / kCoordsZStep }, { "ghost", element->isGhost() },
                        };
                        break;
                    }
                    if (element->isLastForTile())
                        break;
                    ++element;
                }
            };
            fixture.rejectionProjection = [addPlacementProjection](const GameState_t& state, const json_t& args) {
                const auto rideValue = args.value("ride", -1);
                json_t projection = OpenRCT2::Testing::SerializeRideProjection(state, args);
                projection["cash"] = state.park.cash;
                projection["tileElements"] = TileElementCount({ args.value("x", 0), args.value("y", 0) });
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
                addPlacementProjection(projection, state, args);
                return projection;
            };
            fixture.acceptedPostStateProjection = [addPlacementProjection](const GameState_t& state, const json_t& args) {
                const auto rideValue = args.value("ride", -1);
                json_t projection = OpenRCT2::Testing::SerializeRideProjection(state, args);
                projection["tileElements"] = TileElementCount({ args.value("x", 0), args.value("y", 0) });
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
                addPlacementProjection(projection, state, args);
                return projection;
            };
            fixture.makeOrdinaryAction = [](const json_t& args) {
                return std::make_unique<GameActions::RideEntranceExitPlaceAction>(
                    CoordsXY{ args.at("x").get<int32_t>(), args.at("y").get<int32_t>() },
                    static_cast<Direction>(args.at("direction").get<uint32_t>()),
                    RideId::FromUnderlying(args.at("ride").get<uint32_t>()),
                    StationIndex::FromUnderlying(args.at("station").get<uint32_t>()), args.at("isExit").get<bool>());
            };
            fixture.acceptedCases = {
                { "place-entrance",
                  fixture.legalArgs,
                  [removeEndpoint](GameState_t& state, const json_t& args) { removeEndpoint(state, args); },
                  { { "/tileElements", IntegerAdd{ 1 } },
                    { "/endpoint", EndpointFromArguments{ ArgumentKey::x, ArgumentKey::y, ArgumentKey::direction } },
                    { "/insertedElement", InsertedEntranceElement{ false, 1 } },
                    { "/queueLastPeep", ReplaceLiteral{ json_t(1), json_t(nullptr) } },
                    { "/queueLength", ReplaceLiteral{ json_t(37), json_t(0) } } } },
                { "place-exit",
                  [exitArgs] { return *exitArgs; },
                  [removeEndpoint](GameState_t& state, const json_t& args) { removeEndpoint(state, args); },
                  { { "/tileElements", IntegerAdd{ 1 } },
                    { "/endpoint", EndpointFromArguments{ ArgumentKey::x, ArgumentKey::y, ArgumentKey::direction } },
                    { "/insertedElement", InsertedEntranceElement{ true, 1 } } } },
            };
            fixture.acceptedCases[0].id = AcceptedCaseId::entrancePlace;
            fixture.acceptedCases[1].id = AcceptedCaseId::exitPlace;
            for (auto& partition : fixture.semanticInvalidPartitions)
            {
                partition.acceptedCaseName = "place-entrance";
            }
            fixture.projectionContract.enabled = true;
            fixture.requiredStalePredicates.clear();
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
            std::vector<AcceptedFixtureCase> acceptedCases = fixture.acceptedCases;
            if (acceptedCases.empty())
                acceptedCases.push_back({ "legal", fixture.legalArgs, {} });
            if (fixture.registration == "RideEntranceExitPlaceAction"
                && std::none_of(acceptedCases.begin(), acceptedCases.end(), [](const auto& accepted) {
                       return accepted.id == AcceptedCaseId::exitPlace;
                   }))
            {
                failure = "typed entrance runner has no accepted exit branch";
                return false;
            }
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
                const auto caseName = partition.caseName.empty() ? partition.parameter + "#" + std::to_string(partitionIndex)
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
                    // isExit is a closed boolean domain. Its false/true
                    // branches are accepted cases, not an invalid partition.
                    if (fixture.registration == "RideEntranceExitPlaceAction" && parameter == "isExit")
                        continue;
                    failure = "uncovered semantic parameter: " + parameter;
                    return false;
                }
            }
            if (fixture.transportOmissions.empty())
            {
                failure = "transport omissions are not classified";
                return false;
            }

            // Every invalid partition names and first proves an accepted
            // branch. Invalid arguments are derived only from that branch.
            for (const auto& partition : fixture.semanticInvalidPartitions)
            {
                const auto acceptedIt = std::find_if(
                    acceptedCases.begin(), acceptedCases.end(), [&](const auto& accepted) {
                        if (partition.acceptedCaseId != AcceptedCaseId::untyped)
                            return accepted.id == partition.acceptedCaseId;
                        return accepted.name == partition.acceptedCaseName;
                    });
                if (acceptedIt == acceptedCases.end())
                {
                    failure = "semantic partition does not reference a named accepted fixture case: " + partition.caseName;
                    return false;
                }
                LoadPark("small_park_with_ferris_wheel.sv6");
                auto& state = OpenRCT2::getGameState();
                fixture.prepareLegalState(state);
                const auto acceptedArgs = acceptedIt->args();
                if (acceptedIt->prepareState)
                    acceptedIt->prepareState(state, acceptedArgs);
                if (acceptedArgs.empty())
                {
                    failure = "accepted fixture case has empty args: " + acceptedIt->name;
                    return false;
                }
                if (partition.id != InvalidCaseId::untyped)
                {
                    if (partition.acceptedDependencies.empty())
                    {
                        failure = "typed invalid case has no accepted dependencies: " + partition.caseName;
                        return false;
                    }
                    for (const auto dependency : partition.acceptedDependencies)
                    {
                        const auto dependencyIt = std::find_if(
                            acceptedCases.begin(), acceptedCases.end(), [dependency](const auto& accepted) {
                                return accepted.id == dependency;
                            });
                        if (dependencyIt == acceptedCases.end())
                        {
                            failure = "typed invalid case names a missing accepted dependency: " + partition.caseName;
                            return false;
                        }
                        const auto dependencyArgs = dependencyIt->args();
                        if (dependencyArgs.empty())
                        {
                            failure = "accepted dependency has empty args: " + dependencyIt->name;
                            return false;
                        }
                        const auto dependencyQuery = QueryNativeAction(fixture.registration, dependencyArgs, state);
                        if (!dependencyQuery.ok || !dependencyQuery.value.value("accepted", false))
                        {
                            failure = "accepted dependency did not prove before invalid mutation: " + dependencyIt->name;
                            return false;
                        }
                    }
                }
                else
                {
                    const auto acceptedQuery = QueryNativeAction(fixture.registration, acceptedArgs, state);
                    if (!acceptedQuery.ok || !acceptedQuery.value.value("accepted", false))
                    {
                        failure = "named accepted fixture case did not prove before invalid mutation: " + acceptedIt->name;
                        return false;
                    }
                }
                const auto acceptedWatch = OpenRCT2::Testing::CaptureRideProjectionWatchSet(state, acceptedArgs);
                auto args = acceptedArgs;
                if (partition.id != InvalidCaseId::untyped)
                {
                    for (const auto& assignment : partition.assignments)
                        args[ArgumentKeyName(assignment.key)] = assignment.value;
                    std::set<ArgumentKey> changedKeys;
                    for (const auto& [key, value] : acceptedArgs.items())
                    {
                        if (!args.contains(key) || args.at(key) != value)
                        {
                            for (const auto candidate : { ArgumentKey::x, ArgumentKey::y, ArgumentKey::direction,
                                                          ArgumentKey::ride, ArgumentKey::station, ArgumentKey::isExit })
                            {
                                if (ArgumentKeyName(candidate) == key)
                                    changedKeys.insert(candidate);
                            }
                        }
                    }
                    for (const auto& [key, value] : args.items())
                    {
                        if (!acceptedArgs.contains(key) || acceptedArgs.at(key) != value)
                        {
                            for (const auto candidate : { ArgumentKey::x, ArgumentKey::y, ArgumentKey::direction,
                                                          ArgumentKey::ride, ArgumentKey::station, ArgumentKey::isExit })
                            {
                                if (ArgumentKeyName(candidate) == key)
                                    changedKeys.insert(candidate);
                            }
                        }
                    }
                    if (changedKeys != partition.exactChangedKeys)
                    {
                        failure = "typed semantic partition changed keys do not match declaration: " + partition.caseName;
                        return false;
                    }
                }
                else
                {
                    partition.makeInvalid(args);
                    if (const auto changedKeyFailure = ValidateChangedKeys(acceptedArgs, args, partition.changedKeys);
                        !changedKeyFailure.empty())
                    {
                        failure = changedKeyFailure + ": " + partition.caseName;
                        return false;
                    }
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
                if (partition.id != InvalidCaseId::untyped)
                {
                    if (const auto ownerFailure = ValidateOwnerResult(queried.value, partition.expectedOwner); !ownerFailure.empty())
                    {
                        failure = ownerFailure + ": " + partition.caseName;
                        return false;
                    }
                }
                else if (!queried.value.contains("rejection") || !queried.value["rejection"].is_object())
                {
                    failure = "untyped semantic partition has no structured rejection: " + partition.caseName;
                    return false;
                }
                OpenRCT2::Testing::SetRideProjectionWatchSet(acceptedWatch);
                const auto before = fixture.rejectionProjection(state, args);
                if (before.empty())
                {
                    failure = "rejection projection is empty for partition: " + partition.parameter;
                    return false;
                }
                if (fixture.projectionContract)
                {
                    std::string projectionFailure;
                    if (!fixture.projectionContract(before, &projectionFailure))
                    {
                        failure = projectionFailure.empty() ? "action projection contract rejected pre-state" : projectionFailure;
                        return false;
                    }
                }
                auto executed = ExecuteNativeAction(fixture.registration, args, state);
                if (fixture.mutateExecuteResponse)
                    fixture.mutateExecuteResponse(partition, executed.value);
                if (!executed.ok || executed.value.value("accepted", true))
                {
                    failure = "execution accepted semantic invalid partition: " + partition.parameter;
                    return false;
                }
                if (partition.id != InvalidCaseId::untyped)
                {
                    if (const auto ownerFailure = ValidateOwnerResult(executed.value, partition.expectedOwner);
                        !ownerFailure.empty())
                    {
                        failure = ownerFailure + ": " + partition.caseName;
                        return false;
                    }
                    if (const auto identityFailure = ValidateOwnerResultIdentity(queried.value, executed.value);
                        !identityFailure.empty())
                    {
                        failure = identityFailure + ": " + partition.caseName;
                        return false;
                    }
                }
                const auto after = fixture.rejectionProjection(state, args);
                if (before != after)
                {
                    failure = "rejected execution mutated the declared projection: " + partition.parameter;
                    return false;
                }
                if (fixture.projectionContract)
                {
                    std::string projectionFailure;
                    if (!fixture.projectionContract(after, &projectionFailure))
                    {
                        failure = projectionFailure.empty() ? "action projection contract rejected post-state" : projectionFailure;
                        return false;
                    }
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
            if (fixture.requiredStalePredicates.empty() && fixture.requiredTypedStalePredicates.empty())
            {
                failure = "required stale-predicate census is empty";
                return false;
            }
            std::set<std::string> executedStalePredicates;
            std::set<StalePredicateId> executedTypedStalePredicates;
            for (const auto& staleCase : staleCases)
            {
                const auto predicate = staleCase.predicate.empty() ? staleCase.name : staleCase.predicate;
                if (staleCase.id != StalePredicateId::untyped)
                {
                    if (!fixture.requiredTypedStalePredicates.contains(staleCase.id))
                    {
                        failure = "typed stale witness is not in required predicate census: " + staleCase.name;
                        return false;
                    }
                    executedTypedStalePredicates.insert(staleCase.id);
                }
                else
                {
                    if (!fixture.requiredStalePredicates.contains(predicate))
                    {
                        failure = "stale witness is not in required predicate census: " + predicate;
                        return false;
                    }
                    executedStalePredicates.insert(predicate);
                }
                LoadPark("small_park_with_ferris_wheel.sv6");
                auto& staleState = OpenRCT2::getGameState();
                fixture.prepareLegalState(staleState);
                const auto staleAccepted = staleCase.base == AcceptedCaseId::untyped
                    ? std::find_if(acceptedCases.begin(), acceptedCases.end(), [](const auto& accepted) {
                          return accepted.id == AcceptedCaseId::untyped;
                      })
                    : std::find_if(acceptedCases.begin(), acceptedCases.end(), [&](const auto& accepted) {
                          return accepted.id == staleCase.base;
                      });
                if (staleAccepted == acceptedCases.end())
                {
                    failure = "stale witness names a missing accepted case: " + staleCase.name;
                    return false;
                }
                const auto staleArgs = staleAccepted->args();
                if (staleAccepted->prepareState)
                    staleAccepted->prepareState(staleState, staleArgs);
                const auto staleQuery = QueryNativeAction(fixture.registration, staleArgs, staleState);
                if (!staleQuery.ok || !staleQuery.value.value("accepted", false))
                {
                    failure = "legal fixture query did not accept before stale-state mutation case=" + staleCase.name
                        + " args=" + staleArgs.dump() + " result=" + staleQuery.value.dump();
                    return false;
                }
                OpenRCT2::Testing::SetRideProjectionWatchSet(
                    OpenRCT2::Testing::CaptureRideProjectionWatchSet(staleState, staleArgs));
                staleCase.mutate(staleState, staleArgs);
                const auto staleBefore = fixture.rejectionProjection(staleState, staleArgs);
                if (fixture.projectionContract)
                {
                    std::string projectionFailure;
                    if (!fixture.projectionContract(staleBefore, &projectionFailure))
                    {
                        failure = projectionFailure.empty() ? "action projection contract rejected stale pre-state" : projectionFailure;
                        return false;
                    }
                }
                const auto staleExecution = ExecuteNativeAction(fixture.registration, staleArgs, staleState);
                if (!staleExecution.ok || staleExecution.value.value("accepted", true))
                {
                    failure = "execution did not perform a fresh query after stale-state mutation: " + staleCase.name;
                    return false;
                }
                if (staleCase.id != StalePredicateId::untyped)
                {
                    if (const auto ownerFailure = ValidateOwnerResult(staleExecution.value, staleCase.expectedOwner);
                        !ownerFailure.empty())
                    {
                        failure = ownerFailure + ": " + staleCase.name;
                        return false;
                    }
                }
                const auto staleAfter = fixture.rejectionProjection(staleState, staleArgs);
                if (staleBefore.empty() || staleBefore != staleAfter)
                {
                    failure = "stale-query rejection mutated the declared projection: " + staleCase.name;
                    return false;
                }
                if (fixture.projectionContract)
                {
                    std::string projectionFailure;
                    if (!fixture.projectionContract(staleAfter, &projectionFailure))
                    {
                        failure = projectionFailure.empty() ? "action projection contract rejected stale post-state" : projectionFailure;
                        return false;
                    }
                }
            }
            if (executedStalePredicates != fixture.requiredStalePredicates
                || executedTypedStalePredicates != fixture.requiredTypedStalePredicates)
            {
                failure = "executed stale-predicate set differs from required census (executed="
                    + std::to_string(executedTypedStalePredicates.size()) + ", required="
                    + std::to_string(fixture.requiredTypedStalePredicates.size()) + ", executed legacy="
                    + std::to_string(executedStalePredicates.size()) + ", required legacy="
                    + std::to_string(fixture.requiredStalePredicates.size()) + ")";
                return false;
            }

            // Independent fresh states are used for every accepted discriminated
            // case, not just the first enum value. Only the paused transport
            // omissions are excluded from parity.
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
                if (fixture.projectionContract)
                {
                    std::string projectionFailure;
                    if (!fixture.projectionContract(ordinaryPre, &projectionFailure))
                    {
                        failure = projectionFailure.empty() ? "action projection contract rejected accepted pre-state" : projectionFailure;
                        return false;
                    }
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
                if (fixture.projectionContract)
                {
                    std::string projectionFailure;
                    if (!fixture.projectionContract(ordinaryPost, &projectionFailure))
                    {
                        failure = projectionFailure.empty() ? "action projection contract rejected accepted post-state" : projectionFailure;
                        return false;
                    }
                }
                if (const auto deltaFailure = ValidateNamedDelta(ordinaryPre, ordinaryPost, ordinaryArgs, acceptedCase.namedDeltas);
                    !deltaFailure.empty())
                {
                    failure = deltaFailure + ": " + acceptedCase.name;
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
                if (fixture.projectionContract)
                {
                    std::string projectionFailure;
                    if (!fixture.projectionContract(publicPre, &projectionFailure))
                    {
                        failure = projectionFailure.empty() ? "action projection contract rejected public pre-state" : projectionFailure;
                        return false;
                    }
                }
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
                if (fixture.projectionContract)
                {
                    std::string projectionFailure;
                    if (!fixture.projectionContract(publicPost, &projectionFailure))
                    {
                        failure = projectionFailure.empty() ? "action projection contract rejected public post-state" : projectionFailure;
                        return false;
                    }
                }
                if (const auto deltaFailure = ValidateNamedDelta(publicPre, publicPost, publicArgs, acceptedCase.namedDeltas);
                    !deltaFailure.empty())
                {
                    failure = deltaFailure + ": public " + acceptedCase.name;
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

class NativeActionContractRide : public NativeActionContractHarness
{
};

TEST_F(NativeActionContractRide, RideSetVehicleDirectExecuteRejectsMissingEntryBeforeMutation)
{
    for (const bool cheat : { false, true })
    {
        LoadPark("small_park_with_ferris_wheel.sv6");
        auto& state = OpenRCT2::getGameState();
        auto* ride = FindFixtureRide();
        ASSERT_NE(ride, nullptr);
        ride->status = RideStatus::closed;
        PopulateRideProjectionFixture(state, ride->id);
        ride->subtype = kObjectEntryIndexNull;
        state.cheats.disableTrainLengthLimit = cheat;
        const json_t args{ { "ride", ride->id.ToUnderlying() }, { "type", 0 }, { "value", 1 }, { "colour", 0 } };
        const auto watch = OpenRCT2::Testing::CaptureRideProjectionWatchSet(state, args);
        OpenRCT2::Testing::SetRideProjectionWatchSet(watch);
        const auto before = OpenRCT2::Testing::SerializeRideProjection(state, args);
        GameActions::RideSetVehicleAction action(ride->id, GameActions::RideSetVehicleType::numTrains, 1, 0);
        const auto result = action.Execute(state, state.park);
        EXPECT_NE(result.error, GameActions::Status::ok) << "cheat=" << cheat;
        EXPECT_EQ(before, OpenRCT2::Testing::SerializeRideProjection(state, args)) << "cheat=" << cheat;
    }
}

TEST_F(NativeActionContractRide, CreatePreservesExplicitSubtypeSelectionUntilOwnerStage)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    _context->GetObjectManager().UnloadAll();
    ASSERT_NE(_context->GetObjectManager().LoadObject(ObjectEntryDescriptor("rct2.ride.enterp"), 10), nullptr)
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

TEST_F(NativeActionContractRide, ProvisionalRideInventoryAndRetainedEntranceConformance)
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
        rows.push_back(
            {
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
        "RideCreateAction",     "RideDemolishAction",   "RideSetColourSchemeAction", "RideSetNameAction",
        "RideSetPriceAction",   "RideSetStatusAction",  "RideFreezeRatingAction",    "RideSetAppearanceAction",
        "RideSetVehicleAction", "RideSetSettingAction", "RideSetVisibilityAction",
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

class NativeActionContractTypedRunner : public NativeActionContractHarness
{
};

TEST_F(NativeActionContractTypedRunner, RetainedEntranceTypedFixtureConforms)
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

TEST_F(NativeActionContractHarness, CoupledPartitionMutationReachesRunFixture)
{
    auto fixture = MakeEntranceFixture();
    fixture.semanticInvalidPartitions.front().assignments.push_back({ ArgumentKey::y, -1 });
    std::string failure;
    EXPECT_FALSE(RunFixture(fixture, failure));
    EXPECT_NE(failure.find("changed keys"), std::string::npos);
}

TEST_F(NativeActionContractHarness, MissingAcceptedExitDependencyReachesRunFixture)
{
    auto fixture = MakeEntranceFixture();
    fixture.acceptedCases.erase(fixture.acceptedCases.begin() + 1);
    std::string failure;
    EXPECT_FALSE(RunFixture(fixture, failure));
    EXPECT_TRUE(failure.find("accepted fixture case") != std::string::npos
        || failure.find("accepted exit branch") != std::string::npos) << failure;
}

TEST_F(NativeActionContractHarness, RemovedRequiredNotClosedWitnessReachesRunFixture)
{
    auto fixture = MakeEntranceFixture();
    fixture.staleStateCases.clear();
    std::string failure;
    EXPECT_FALSE(RunFixture(fixture, failure));
    EXPECT_NE(failure.find("stale"), std::string::npos) << failure;
}

TEST_F(NativeActionContractHarness, WrongOwnerResultMutationReachesRunFixture)
{
    auto fixture = MakeEntranceFixture();
    fixture.semanticInvalidPartitions.front().expectedOwner = OwnerResultId::entranceNotClosed;
    std::string failure;
    EXPECT_FALSE(RunFixture(fixture, failure));
    EXPECT_NE(failure.find("owner result"), std::string::npos);
}

TEST_F(NativeActionContractHarness, ExecuteOwnerResultMutationReachesRunFixture)
{
    auto fixture = MakeEntranceFixture();
    fixture.mutateExecuteResponse = [](const SemanticInvalidPartition& partition, json_t& response) {
        if (partition.id == InvalidCaseId::entranceXOffMap)
            response["rejection"]["message"] = "corrupted execute response";
    };
    std::string failure;
    EXPECT_FALSE(RunFixture(fixture, failure));
    EXPECT_NE(failure.find("owner result"), std::string::npos) << failure;
}

TEST_F(NativeActionContractHarness, WrongIntegerDeltaMutationReachesRunFixture)
{
    auto fixture = MakeEntranceFixture();
    fixture.acceptedCases.front().namedDeltas.front().relation = IntegerAdd{ 2 };
    std::string failure;
    EXPECT_FALSE(RunFixture(fixture, failure));
    EXPECT_NE(failure.find("intended field delta"), std::string::npos);
}

TEST_F(NativeActionContractHarness, WrongEntranceClearanceMutationReachesRunFixture)
{
    for (const bool isExit : { false, true })
    {
        auto fixture = MakeEntranceFixture();
        bool mutated = false;
        const auto original = fixture.acceptedPostStateProjection;
        fixture.acceptedPostStateProjection = [original, isExit, &mutated](const GameState_t& state, const json_t& args) {
            auto projection = original(state, args);
            const auto expectedType = isExit ? ENTRANCE_TYPE_RIDE_EXIT : ENTRANCE_TYPE_RIDE_ENTRANCE;
            auto& inserted = projection["insertedElement"];
            if (inserted.is_object() && inserted.value("type", -1) == expectedType)
            {
                inserted["clearanceHeight"] = inserted.at("clearanceHeight").get<int32_t>() + 1;
                mutated = true;
            }
            return projection;
        };
        std::string failure;
        EXPECT_FALSE(RunFixture(fixture, failure)) << "isExit=" << isExit;
        EXPECT_TRUE(mutated) << "isExit=" << isExit;
        EXPECT_NE(failure.find("intended field delta"), std::string::npos) << failure;
    }
}

TEST_F(NativeActionContractHarness, OmittedAndFakeProjectionMutationsReachRunFixture)
{
    for (const bool fake : { false, true })
    {
        auto fixture = MakeEntranceFixture();
        const auto original = fixture.rejectionProjection;
        fixture.rejectionProjection = [original, fake](const GameState_t& state, const json_t& args) {
            auto projection = original(state, args);
            if (fake)
                projection["activePeepLinks"] = json_t::array({ 1 });
            else
                projection.erase("campaigns");
            return projection;
        };
        std::string failure;
        EXPECT_FALSE(RunFixture(fixture, failure)) << "fake=" << fake;
        EXPECT_TRUE(failure.find("projection store") != std::string::npos || failure.find("fake") != std::string::npos)
            << failure;
    }
}

TEST_F(NativeActionContractHarness, SerializesBoundedAuthoritativeStores)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& state = OpenRCT2::getGameState();
    auto* ride = FindFixtureRide();
    ASSERT_NE(ride, nullptr);
    PopulateRideProjectionFixture(state, ride->id);
    const auto projection = OpenRCT2::Testing::SerializeRideProjection(state, { { "ride", ride->id.ToUnderlying() } });
    EXPECT_TRUE(projection.contains("news"));
    EXPECT_TRUE(projection["news"].contains("recent"));
    EXPECT_TRUE(projection["news"].contains("archived"));
    EXPECT_TRUE(projection.contains("guests"));
    EXPECT_TRUE(projection.contains("vehicles"));
    EXPECT_TRUE(projection.contains("tiles"));
    EXPECT_TRUE(projection.contains("campaigns"));
    EXPECT_TRUE(projection.contains("finance"));
    EXPECT_TRUE(projection.contains("parkValue"));
    EXPECT_EQ(projection["parkValue"], 12345);
    ASSERT_FALSE(projection["news"]["recent"].empty());
    EXPECT_EQ(projection["news"]["recent"][0]["item"]["text"], "S2 recent target news");
    ASSERT_FALSE(projection["news"]["archived"].empty());
    EXPECT_EQ(projection["news"]["archived"][0]["item"]["text"], "S2 archived target news");
    ASSERT_FALSE(projection["campaigns"].empty());
    EXPECT_EQ(projection["campaigns"][0]["ride"], ride->id.ToUnderlying());
    ASSERT_FALSE(projection["banners"].empty());
    EXPECT_EQ(projection["banners"][0]["text"], "S2 target banner");
    EXPECT_TRUE(projection["watch"].contains("tiles"));
    EXPECT_TRUE(projection["watch"].contains("recentNewsSlots"));
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
    const std::set<std::string> requiredStalePredicates{ "ride-exists", "entry-exists" };
    const std::set<std::string> executedStalePredicates{ "ride-exists" };
    EXPECT_NE(
        ValidateStalePredicateSet(requiredStalePredicates, executedStalePredicates).find("stale-predicate"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, OmittedProjectionStoreRed)
{
    json_t projection{
        { "rides", json_t::array({ 1 }) },
        { "vehicles", json_t::array({ 1 }) },
        { "guests", json_t::array({ 1 }) },
        { "rideUseHistory", json_t::array({ 1 }) },
        { "news", { { "recent", json_t::array({ 1 }) }, { "archived", json_t::array({ 1 }) } } },
        { "banners", json_t::array({ 1 }) },
        { "campaigns", json_t::array({ 1 }) },
        { "tiles", json_t::array({ 1 }) },
        { "finance", json_t::object({ { "cash", 1 } }) },
        { "parkValue", 1 },
        { "watch", json_t::object({ { "vehicles", 1 } }) },
    };
    projection.erase("campaigns");
    EXPECT_NE(ValidateProjectionStores(projection).find("campaigns"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, FakeProjectionStoreRed)
{
    json_t projection{
        { "rides", json_t::array({ 1 }) },
        { "vehicles", json_t::array({ 1 }) },
        { "guests", json_t::array({ 1 }) },
        { "rideUseHistory", json_t::array({ 1 }) },
        { "news", { { "recent", json_t::array({ 1 }) }, { "archived", json_t::array({ 1 }) } } },
        { "banners", json_t::array({ 1 }) },
        { "campaigns", json_t::array({ 1 }) },
        { "tiles", json_t::array({ 1 }) },
        { "finance", json_t::object({ { "cash", 1 } }) },
        { "parkValue", 1 },
        { "watch", json_t::object({ { "vehicles", 1 } }) },
        { "activePeepLinks", json_t::array({ 1 }) },
    };
    EXPECT_NE(ValidateProjectionStores(projection).find("fake"), std::string::npos);
}

TEST(NativeActionContractHarnessMutations, AcceptedNoOpRed)
{
    const std::vector<NamedFieldDelta> deltas{
        { "/ride", ReplaceLiteral{ json_t(1), json_t(2) } },
    };
    const auto failure = ValidateNamedDelta({ { "ride", 1 } }, { { "ride", 1 } }, json_t::object(), deltas);
    EXPECT_NE(failure.find("intended field delta"), std::string::npos);
}

TEST_F(NativeActionContractHarness, FormerVehicleRemainsInPersistentWatchAfterRemoval)
{
    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& state = OpenRCT2::getGameState();
    auto* ride = FindFixtureRide();
    ASSERT_NE(ride, nullptr);
    auto* fixtureVehicle = state.entities.CreateEntity<Vehicle>();
    ASSERT_NE(fixtureVehicle, nullptr) << "serializer fixture could not create a vehicle identity";
    fixtureVehicle->ride = ride->id;
    fixtureVehicle->num_seats = 2;
    fixtureVehicle->num_peeps = 0;
    const auto formerId = fixtureVehicle->id;
    const json_t args{ { "ride", ride->id.ToUnderlying() } };
    const auto watch = OpenRCT2::Testing::CaptureRideProjectionWatchSet(state, args);
    ASSERT_TRUE(watch.vehicleIds.contains(formerId.ToUnderlying()));
    OpenRCT2::Testing::SetRideProjectionWatchSet(watch);
    state.entities.EntityRemove(state.entities.GetEntity<Vehicle>(formerId));
    const auto projection = OpenRCT2::Testing::SerializeRideProjection(state, args);
    bool foundFormer = false;
    for (const auto& vehicle : projection["vehicles"])
    {
        if (vehicle.value("id", std::numeric_limits<uint16_t>::max()) == formerId.ToUnderlying())
        {
            foundFormer = true;
            EXPECT_FALSE(vehicle.value("exists", true));
        }
    }
    EXPECT_TRUE(foundFormer) << "former vehicle disappeared from persistent watch serialization";
}

TEST(NativeActionContractHarnessMutations, FormerVehicleWatchEntryRed)
{
    const auto failure = ValidateFormerVehicle({ { "vehicles", json_t::array({ { { "id", 4 }, { "exists", true } } }) } }, 4);
    EXPECT_NE(failure.find("not serialized as removed"), std::string::npos);
}
