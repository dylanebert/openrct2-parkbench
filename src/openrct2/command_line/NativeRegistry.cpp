#include "NativeRegistry.h"

#include "../Context.h"
#include "../Date.h"
#include "../Game.h"
#include "../GameState.h"
#include "../actions/GameAction.hpp"
#include "../actions/GameActionParameterVisitor.h"
#include "../actions/GameActionRegistry.h"
#include "../actions/GameActionRunner.h"
#include "../entity/Guest.h"
#include "../entity/Peep.h"
#include "../object/ObjectManager.h"
#include "../object/ObjectList.h"
#include "../park/ParkFile.h"
#include "../scenario/Scenario.h"
#include "../world/Map.h"
#include "../world/tile_element/SurfaceElement.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace OpenRCT2::CommandLine
{
    namespace
    {
        using json_t = nlohmann::json;
        std::mutex gSaveRootMutex;
        std::string gSaveRoot;

        json_t ObjectSchema(json_t properties, json_t required = json_t::array())
        {
            return {
                { "type", "object" },
                { "additionalProperties", false },
                { "properties", std::move(properties) },
                { "required", std::move(required) },
            };
        }

        json_t EmptySchema()
        {
            return ObjectSchema(json_t::object());
        }

        NativeDispatchResult Failure(std::string code, std::string message, json_t detail = json_t::object())
        {
            NativeDispatchResult result;
            result.code = std::move(code);
            result.message = std::move(message);
            result.detail = std::move(detail);
            return result;
        }

        NativeDispatchResult Success(json_t value)
        {
            NativeDispatchResult result;
            result.ok = true;
            result.value = std::move(value);
            return result;
        }

        bool IsObject(const json_t& value)
        {
            return value.is_object();
        }

        bool ReadInt(const json_t& args, const char* name, int32_t& output)
        {
            const auto it = args.find(name);
            if (it == args.end() || !it->is_number_integer())
                return false;
            const auto value = it->get<int64_t>();
            if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max())
                return false;
            output = static_cast<int32_t>(value);
            return true;
        }

        json_t Entity(const EntityBase& entity)
        {
            return {
                { "id", entity.id.ToUnderlying() },
                { "type", static_cast<uint8_t>(entity.type) },
                { "x", entity.x },
                { "y", entity.y },
                { "z", entity.z },
                { "orientation", entity.orientation },
            };
        }

        json_t RideValue(const Ride& ride)
        {
            return {
                { "id", ride.id.ToUnderlying() },
                { "type", ride.type },
                { "name", ride.getName() },
                { "status", static_cast<uint8_t>(ride.status) },
                { "stations", ride.numStations },
                { "trains", ride.numTrains },
                { "carsPerTrain", ride.numCarsPerTrain },
                { "price", ride.price[0] },
                { "value", ride.value },
                { "totalCustomers", ride.totalCustomers },
                { "totalProfit", ride.totalProfit },
            };
        }

        class SchemaVisitor final : public GameActions::GameActionParameterVisitor
        {
        public:
            json_t properties = json_t::object();
            json_t required = json_t::array();

            void Add(std::string_view name, json_t schema)
            {
                const std::string key(name);
                properties[key] = std::move(schema);
                required.push_back(key);
            }

            void Visit(std::string_view name, bool&) override
            {
                Add(name, { { "type", "boolean" } });
            }

            void Visit(std::string_view name, int32_t&) override
            {
                Add(name, { { "type", "integer" } });
            }

            void Visit(std::string_view name, std::string&) override
            {
                Add(name, { { "type", "string" } });
            }
        };

        class JsonVisitor final : public GameActions::GameActionParameterVisitor
        {
        public:
            explicit JsonVisitor(const json_t& source)
                : _source(source)
            {
            }

            std::string error;
            std::string field;

            void Visit(std::string_view name, bool& value) override
            {
                const auto* item = Find(name);
                if (item == nullptr || !item->is_boolean())
                    Invalid(name, "boolean");
                else
                    value = item->get<bool>();
            }

            void Visit(std::string_view name, int32_t& value) override
            {
                const auto* item = Find(name);
                if (item == nullptr || !item->is_number_integer())
                {
                    Invalid(name, "integer");
                    return;
                }
                const auto number = item->get<int64_t>();
                if (number < std::numeric_limits<int32_t>::min() || number > std::numeric_limits<int32_t>::max())
                {
                    Invalid(name, "32-bit integer");
                    return;
                }
                value = static_cast<int32_t>(number);
            }

            void Visit(std::string_view name, std::string& value) override
            {
                const auto* item = Find(name);
                if (item == nullptr || !item->is_string())
                    Invalid(name, "string");
                else
                    value = item->get<std::string>();
            }

        private:
            const json_t& _source;

            const json_t* Find(std::string_view name)
            {
                const auto it = _source.find(std::string(name));
                return it == _source.end() ? nullptr : &*it;
            }

            void Invalid(std::string_view name, std::string_view expected)
            {
                if (error.empty())
                {
                    field = std::string(name);
                    error = "field '" + field + "' must be a " + std::string(expected);
                }
            }
        };

        NativeDispatchResult FindAction(
            std::string_view name, const json_t& args, GameState_t& state, bool execute)
        {
            if (!args.is_object())
                return Failure("invalid_arguments", "action arguments must be an object");
            if (args.contains("flags") || args.contains("apply") || args.contains("allowDuringPaused")
                || args.contains("ghost") || args.contains("noSpend") || args.contains("networked")
                || args.contains("replay") || args.contains("trackDesign"))
            {
                return Failure("policy_flag", "universal action flags are controlled by the native dispatcher", {
                    { "allowed", false },
                });
            }

            for (const auto& registration : GameActions::GetRegistrations())
            {
                if (name != registration.name)
                    continue;

                auto action = std::unique_ptr<GameActions::GameAction>((*registration.factory)());
                SchemaVisitor schemaVisitor;
                action->AcceptParameters(schemaVisitor);
                for (auto it = args.begin(); it != args.end(); ++it)
                {
                    if (!schemaVisitor.properties.contains(it.key()))
                        return Failure("unknown_argument", "argument is not in the native action schema", {
                            { "name", it.key() },
                        });
                }
                JsonVisitor valueVisitor(args);
                action->AcceptParameters(valueVisitor);
                if (!valueVisitor.error.empty())
                    return Failure("invalid_arguments", valueVisitor.error, { { "field", valueVisitor.field } });

                // These are not caller policy. Native dispatch always applies the
                // action at the paused simulation boundary and lets the engine's
                // own Query/Execute implementation decide whether it is valid.
                action->SetFlags({ GameActions::CommandFlag::apply, GameActions::CommandFlag::allowDuringPaused });
                const auto result = execute ? GameActions::ExecuteNested(action.get(), state)
                                            : GameActions::Query(action.get(), state);
                json_t response = {
                    { "action", registration.name },
                    { "status", static_cast<uint16_t>(result.error) },
                    { "accepted", result.error == GameActions::Status::ok },
                    { "cost", result.cost },
                    { "position", {
                          { "x", result.position.x },
                          { "y", result.position.y },
                          { "z", result.position.z },
                      } },
                };
                if (result.error != GameActions::Status::ok)
                {
                    response["rejection"] = {
                        { "title", result.getErrorTitle() },
                        { "message", result.getErrorMessage() },
                    };
                }
                return Success(std::move(response));
            }
            return Failure("unknown_action", "native action is not registered", { { "name", name } });
        }

        json_t ReadPark(const json_t&, GameState_t& state)
        {
            const auto& park = state.park;
            return {
                { "name", park.name },
                { "flags", park.flags },
                { "open", (park.flags & PARK_FLAGS_PARK_OPEN) != 0 },
                { "rating", park.rating },
                { "entranceFee", park.entranceFee },
                { "size", park.size },
                { "cash", park.cash },
                { "value", park.value },
                { "guests", park.numGuestsInPark },
            };
        }

        json_t ReadDate(const json_t&, GameState_t& state)
        {
            return {
                { "year", state.date.GetYear() },
                { "month", state.date.GetMonth() },
                { "day", state.date.GetDay() },
                { "monthsElapsed", state.date.GetMonthsElapsed() },
                { "monthTicks", state.date.GetMonthTicks() },
            };
        }

        json_t ReadFinance(const json_t&, GameState_t& state)
        {
            const auto& park = state.park;
            return {
                { "cash", park.cash },
                { "bankLoan", park.bankLoan },
                { "maxBankLoan", park.maxBankLoan },
                { "loanInterestRate", park.bankLoanInterestRate },
                { "currentProfit", park.currentProfit },
                { "currentExpenditure", park.currentExpenditure },
                { "companyValue", park.companyValue },
            };
        }

        json_t ReadRides(const json_t&, GameState_t& state)
        {
            json_t rides = json_t::array();
            for (const auto& ride : state.rides)
            {
                if (!ride.id.IsNull())
                    rides.push_back(RideValue(ride));
            }
            return { { "rides", std::move(rides) } };
        }

        json_t ReadRide(const json_t& args, GameState_t& state)
        {
            int32_t id = 0;
            if (!IsObject(args) || !ReadInt(args, "id", id) || id < 0 || static_cast<size_t>(id) >= state.rides.size())
                return { { "error", "id must identify an existing ride" } };
            const auto& ride = state.rides[static_cast<size_t>(id)];
            if (ride.id.IsNull())
                return { { "error", "ride does not exist" }, { "id", id } };
            return RideValue(ride);
        }

        json_t ReadGuests(const json_t&, GameState_t& state)
        {
            json_t guests = json_t::array();
            for (const auto id : state.entities.GetEntityList(EntityType::guest))
            {
                if (const auto* guest = state.entities.GetEntity<Guest>(id))
                    guests.push_back(Entity(*guest));
            }
            return { { "guests", std::move(guests) } };
        }

        json_t ReadGuest(const json_t& args, GameState_t& state)
        {
            int32_t id = 0;
            if (!IsObject(args) || !ReadInt(args, "id", id) || id < 0 || id >= kMaxEntities)
                return { { "error", "id must identify an existing guest" } };
            const auto* guest = state.entities.GetEntity<Guest>(EntityId::FromUnderlying(static_cast<uint16_t>(id)));
            if (guest == nullptr)
                return { { "error", "guest does not exist" }, { "id", id } };
            auto result = Entity(*guest);
            result["name"] = guest->GetName();
            result["peepId"] = guest->PeepId;
            result["currentRide"] = guest->CurrentRide.ToUnderlying();
            return result;
        }

        json_t ReadObjects(const json_t&, GameState_t&)
        {
            json_t objects = json_t::array();
            if (GetContext() != nullptr)
            {
                for (const auto& entry : GetContext()->GetObjectManager().GetLoadedObjects())
                    objects.push_back(entry.ToString());
            }
            return { { "objects", std::move(objects) } };
        }

        json_t ReadTile(const json_t& args, GameState_t&)
        {
            int32_t x = 0;
            int32_t y = 0;
            if (!IsObject(args) || !ReadInt(args, "x", x) || !ReadInt(args, "y", y))
                return { { "error", "x and y are required tile coordinates" } };
            auto* surface = MapGetSurfaceElementAt(TileCoordsXY{ x, y });
            if (surface == nullptr)
                return { { "error", "tile is outside the map" }, { "x", x }, { "y", y } };
            return {
                { "x", x },
                { "y", y },
                { "slope", surface->GetSlope() },
                { "waterHeight", surface->GetWaterHeight() },
                { "grassLength", surface->GetGrassLength() },
                { "ownership", surface->GetOwnership() },
                { "surfaceObject", surface->GetSurfaceObjectIndex() },
                { "edgeObject", surface->GetEdgeObjectIndex() },
            };
        }

        json_t ReadRegion(const json_t&, GameState_t& state)
        {
            // This is intentionally a named derived projection. It exposes the
            // engine map boundary and its inputs rather than inventing reachability.
            return {
                { "mapWidth", state.mapSize.x },
                { "mapHeight", state.mapSize.y },
                { "algorithm", "map-boundary" },
                { "inputs", { "mapSize" } },
            };
        }

        const std::array<NativeResourceDescriptor, 11> kResources = {
            NativeResourceDescriptor{
                "session", "Current native engine session state.", ObjectSchema({}), { { "tick", "ticks" }, { "sequence", "requests" } },
                "engine", json_t::array(), "native", [](const json_t&, GameState_t& state) {
                    return json_t{ { "tick", state.currentTicks }, { "paused", GameIsPaused() } };
                } },
            { "park", "Authoritative park state.", EmptySchema(), { { "cash", "money" }, { "entranceFee", "money" }, { "rating", "rating" } },
                "engine", json_t::array(), "native", ReadPark },
            { "date", "Authoritative simulation calendar.", EmptySchema(), { { "day", "days" }, { "month", "months" }, { "year", "years" } },
                "engine", json_t::array(), "native", ReadDate },
            { "finance", "Authoritative park finance state.", EmptySchema(), { { "cash", "money" }, { "bankLoan", "money" } },
                "engine", json_t::array(), "native", ReadFinance },
            { "rides", "Authoritative collection of rides.", EmptySchema(), { { "id", "ride" }, { "price", "money" } },
                "engine", json_t::array(), "native", ReadRides },
            { "ride", "Authoritative state for one ride.", ObjectSchema({ { "id", { { "type", "integer" }, { "minimum", 0 } } } }, { "id" }),
                { { "id", "ride" }, { "price", "money" } }, "engine", json_t::array(), "native", ReadRide },
            { "guests", "Authoritative collection of guest entities.", EmptySchema(), { { "id", "entity" }, { "x", "map-units" } },
                "engine", json_t::array(), "native", ReadGuests },
            { "guest", "Authoritative state for one guest entity.", ObjectSchema({ { "id", { { "type", "integer" }, { "minimum", 0 } } } }, { "id" }),
                { { "id", "entity" }, { "x", "map-units" } }, "engine", json_t::array(), "native", ReadGuest },
            { "objects", "Authoritative loaded object identities.", EmptySchema(), { { "count", "objects" } },
                "engine", json_t::array(), "native", ReadObjects },
            { "tile", "Authoritative surface state at a map tile.", ObjectSchema({ { "x", { { "type", "integer" } } }, { "y", { { "type", "integer" } } } }, { "x", "y" }),
                { { "x", "map-units" }, { "y", "map-units" }, { "waterHeight", "height-units" } }, "engine", json_t::array(), "native", ReadTile },
            { "region", "Derived map-boundary projection from mapSize.", EmptySchema(), { { "mapWidth", "map-units" }, { "mapHeight", "map-units" } },
                "derived", { "mapSize" }, "native", ReadRegion },
        };
    }

    const std::array<NativeResourceDescriptor, 11>& NativeResources()
    {
        return kResources;
    }

    json_t NativeResourceDescriptorJson(const NativeResourceDescriptor& descriptor)
    {
        return {
            { "name", descriptor.name },
            { "description", descriptor.description },
            { "schema", descriptor.schema },
            { "units", descriptor.units },
            { "authority", descriptor.authority },
            { "classification", descriptor.authority == std::string_view("derived") ? "derived" : "authoritative" },
            { "inputs", descriptor.inputs },
            { "capability", descriptor.capability },
        };
    }

    std::vector<NativeActionDescriptor> NativeActions()
    {
        std::vector<NativeActionDescriptor> result;
        for (const auto& registration : GameActions::GetRegistrations())
        {
            auto action = std::unique_ptr<GameActions::GameAction>((*registration.factory)());
            SchemaVisitor visitor;
            action->AcceptParameters(visitor);
            result.push_back({
                static_cast<uint32_t>(registration.command),
                registration.name,
                std::string("Native engine Game Action ") + registration.name + ".",
                ObjectSchema(std::move(visitor.properties), std::move(visitor.required)),
                { { "x", "map-units" }, { "y", "map-units" }, { "z", "height-units" }, { "cost", "money" } },
                "engine",
                "native",
            });
        }
        return result;
    }

    json_t NativeActionDescriptorJson(const NativeActionDescriptor& descriptor)
    {
        return {
            { "id", descriptor.id },
            { "name", descriptor.name },
            { "description", descriptor.description },
            { "schema", descriptor.schema },
            { "units", descriptor.units },
            { "authority", descriptor.authority },
            { "classification", "effect" },
            { "capability", descriptor.capability },
            { "policy", { { "flags", "native-controlled" } } },
        };
    }

    NativeDispatchResult ReadNativeResource(std::string_view name, const json_t& args, GameState_t& state)
    {
        if (!args.is_object())
            return Failure("invalid_arguments", "resource arguments must be an object");
        for (const auto& descriptor : NativeResources())
        {
            if (name == descriptor.name)
                return Success(descriptor.read(args, state));
        }
        return Failure("unknown_resource", "native resource is not registered", { { "name", name } });
    }

    NativeDispatchResult QueryNativeAction(std::string_view name, const json_t& args, GameState_t& state)
    {
        return FindAction(name, args, state, false);
    }

    NativeDispatchResult ExecuteNativeAction(std::string_view name, const json_t& args, GameState_t& state)
    {
        return FindAction(name, args, state, true);
    }

    bool NativePathContained(std::string_view root, std::string_view path)
    {
        if (root.empty() || path.empty())
            return false;
        std::error_code error;
        const auto rootPath = std::filesystem::weakly_canonical(std::filesystem::path(root), error);
        if (error)
            return false;
        const auto targetPath = std::filesystem::weakly_canonical(std::filesystem::path(path), error);
        if (error)
            return false;
        auto rootIt = rootPath.begin();
        auto targetIt = targetPath.begin();
        for (; rootIt != rootPath.end() && targetIt != targetPath.end(); ++rootIt, ++targetIt)
        {
            if (*rootIt != *targetIt)
                return false;
        }
        return rootIt == rootPath.end();
    }

    NativeDispatchResult SaveNativeGame(std::string_view path, GameState_t& state)
    {
        if (!GameIsPaused())
            return Failure("paused_required", "save is only accepted while the engine is paused");
        const auto root = NativeSaveRoot();
        if (!NativePathContained(root, path))
            return Failure("save_containment", "save path must remain below the owned native save root", {
                { "root", root },
                { "path", path },
            });
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
        if (error)
            return Failure("save_failed", "unable to create the save parent directory", { { "path", path } });
        if (ScenarioSave(state, path, 0) == 0)
            return Failure("save_failed", "the engine rejected the save", { { "path", path } });
        return Success({ { "path", path }, { "tick", state.currentTicks } });
    }

    void SetNativeSaveRoot(std::string root)
    {
        std::lock_guard lock(gSaveRootMutex);
        gSaveRoot = std::move(root);
    }

    std::string NativeSaveRoot()
    {
        std::lock_guard lock(gSaveRootMutex);
        return gSaveRoot;
    }
}
