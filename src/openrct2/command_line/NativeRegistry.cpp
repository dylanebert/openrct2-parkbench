#include "NativeRegistry.h"

#include "../Context.h"
#include "../PlatformEnvironment.h"
#include "../ReplayManager.h"
#include "../interface/Screenshot.h"
#include "../Date.h"
#include "../Game.h"
#include "../GameState.h"
#include "../actions/GameAction.hpp"
#include "../actions/GameActionParameterVisitor.h"
#include "../actions/GameActionRegistry.h"
#include "../actions/GameActionRunner.h"
#include "../entity/Guest.h"
#include "../entity/Peep.h"
#include "../ride/Vehicle.h"
#include "../object/ObjectManager.h"
#include "../object/ObjectList.h"
#include "../park/ParkFile.h"
#include "../scenario/Scenario.h"
#include "../world/Map.h"
#include "../world/tile_element/EntranceElement.h"
#include "../world/tile_element/PathElement.h"
#include "../world/tile_element/TrackElement.h"
#include "../world/tile_element/SurfaceElement.h"
#include "../world/tile_element/TileElement.h"

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
        std::string gRecordingRoot;
        std::string gCaptureRoot;

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
                { "ratings", {
                      { "excitement", static_cast<int32_t>(ride.ratings.excitement) },
                      { "intensity", static_cast<int32_t>(ride.ratings.intensity) },
                      { "nausea", static_cast<int32_t>(ride.ratings.nausea) },
                  } },
                { "queueLength", ride.getTotalQueueLength() },
                { "occupancy", ride.numRiders },
                { "totalCustomers", ride.totalCustomers },
                { "totalProfit", ride.totalProfit },
                { "profit", ride.profit },
                { "breakdown", {
                      { "pending", ride.flags.has(RideFlag::breakdownPending) },
                      { "broken", ride.flags.has(RideFlag::brokenDown) },
                      { "pendingReason", static_cast<uint8_t>(ride.breakdownReasonPending) },
                      { "reason", static_cast<uint8_t>(ride.breakdownReason) },
                  } },
            };
        }

        json_t RideVehicleIds(const Ride& ride, GameState_t* state)
        {
            json_t vehicles = json_t::array();
            for (uint8_t train = 0; train < ride.numTrains; ++train)
            {
                auto id = ride.vehicles[train];
                for (uint8_t car = 0; !id.IsNull() && car < Limits::kMaxCarsPerTrain; ++car)
                {
                    vehicles.push_back({ { "train", train }, { "car", car }, { "id", id.ToUnderlying() } });
                    if (state == nullptr)
                        break;
                    const auto* vehicle = state->entities.GetEntity<Vehicle>(id);
                    if (vehicle == nullptr)
                        break;
                    id = vehicle->next_vehicle_on_train;
                }
            }
            return vehicles;
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

        const char* ActionStatusCode(GameActions::Status status)
        {
            switch (status)
            {
                case GameActions::Status::ok:
                    return "ok";
                case GameActions::Status::invalidParameters:
                    return "invalid_parameters";
                case GameActions::Status::disallowed:
                    return "disallowed";
                case GameActions::Status::gamePaused:
                    return "game_paused";
                case GameActions::Status::insufficientFunds:
                    return "insufficient_funds";
                case GameActions::Status::notInEditorMode:
                    return "not_in_editor_mode";
                case GameActions::Status::notOwned:
                    return "not_owned";
                case GameActions::Status::tooLow:
                    return "too_low";
                case GameActions::Status::tooHigh:
                    return "too_high";
                case GameActions::Status::noClearance:
                    return "no_clearance";
                case GameActions::Status::itemAlreadyPlaced:
                    return "item_already_placed";
                case GameActions::Status::notClosed:
                    return "not_closed";
                case GameActions::Status::broken:
                    return "broken";
                case GameActions::Status::noFreeElements:
                    return "no_free_elements";
                case GameActions::Status::unknown:
                    return "unknown";
            }
            return "unknown";
        }

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
                const auto result = execute ? GameActions::ExecuteSynchronous(action.get(), state)
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
                        { "code", ActionStatusCode(result.error) },
                        { "title", result.getErrorTitle() },
                        { "message", result.getErrorMessage() },
                        { "detail", { { "status", static_cast<uint16_t>(result.error) } } },
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

        json_t StationEndpointValue(const TileCoordsXYZD& endpoint)
        {
            if (endpoint.IsNull())
                return nullptr;
            return {
                { "x", endpoint.x },
                { "y", endpoint.y },
                { "z", endpoint.z },
                { "direction", endpoint.direction },
            };
        }

        json_t StationDetails(const Ride& ride)
        {
            json_t stations = json_t::array();
            for (uint8_t index = 0; index < ride.numStations; ++index)
            {
                const auto& station = ride.getStation(StationIndex::FromUnderlying(index));
                stations.push_back({
                    { "index", index },
                    { "start", { { "x", station.Start.x }, { "y", station.Start.y } } },
                    { "baseZ", station.GetBaseZ() },
                    { "entrance", StationEndpointValue(station.Entrance) },
                    { "exit", StationEndpointValue(station.Exit) },
                });
            }
            return stations;
        }

        json_t ReadRide(const json_t& args, GameState_t& state)
        {
            int32_t id = 0;
            if (!IsObject(args) || !ReadInt(args, "id", id) || id < 0 || static_cast<size_t>(id) >= state.rides.size())
                return { { "error", "id must identify an existing ride" } };
            const auto& ride = state.rides[static_cast<size_t>(id)];
            if (ride.id.IsNull())
                return { { "error", "ride does not exist" }, { "id", id } };
            auto result = RideValue(ride);
            result["stationDetails"] = StationDetails(ride);
            result["vehicleIds"] = RideVehicleIds(ride, &state);
            return result;
        }

        json_t VehicleValue(const Vehicle& vehicle)
        {
            json_t occupants = json_t::array();
            for (uint8_t seat = 0; seat < vehicle.num_seats; ++seat)
            {
                const auto guest = vehicle.peep[seat];
                occupants.push_back(guest.IsNull() ? nullptr : json_t(guest.ToUnderlying()));
            }
            return {
                { "id", vehicle.id.ToUnderlying() },
                { "type", static_cast<uint8_t>(vehicle.type) },
                { "x", vehicle.x },
                { "y", vehicle.y },
                { "z", vehicle.z },
                { "orientation", vehicle.orientation },
                { "ride", vehicle.ride.ToUnderlying() },
                { "station", vehicle.current_station.ToUnderlying() },
                { "status", static_cast<uint8_t>(vehicle.status) },
                { "substate", vehicle.sub_state },
                { "trackLocation", {
                      { "x", vehicle.TrackLocation.x },
                      { "y", vehicle.TrackLocation.y },
                      { "z", vehicle.TrackLocation.z },
                  } },
                { "trackType", static_cast<uint16_t>(vehicle.GetTrackType()) },
                { "trackDirection", vehicle.GetTrackDirection() },
                { "trackProgress", vehicle.track_progress },
                { "nextVehicleOnTrain", vehicle.next_vehicle_on_train.IsNull()
                                               ? nullptr
                                               : json_t(vehicle.next_vehicle_on_train.ToUnderlying()) },
                { "prevVehicleOnRide", vehicle.prev_vehicle_on_ride.IsNull()
                                              ? nullptr
                                              : json_t(vehicle.prev_vehicle_on_ride.ToUnderlying()) },
                { "nextVehicleOnRide", vehicle.next_vehicle_on_ride.IsNull()
                                              ? nullptr
                                              : json_t(vehicle.next_vehicle_on_ride.ToUnderlying()) },
                { "seatCount", vehicle.num_seats },
                { "occupants", std::move(occupants) },
            };
        }

        json_t ReadVehicle(const json_t& args, GameState_t& state)
        {
            int32_t id = 0;
            if (!IsObject(args) || !ReadInt(args, "id", id) || id < 0 || id >= kMaxEntities)
                return { { "error", "id must identify an existing vehicle" } };
            const auto* vehicle = state.entities.GetEntity<Vehicle>(EntityId::FromUnderlying(static_cast<uint16_t>(id)));
            if (vehicle == nullptr)
                return { { "error", "vehicle does not exist" }, { "id", id } };
            return VehicleValue(*vehicle);
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
            result["state"] = static_cast<uint8_t>(guest->State);
            result["substate"] = guest->SubState;
            result["nextLocation"] = {
                { "x", guest->NextLoc.x },
                { "y", guest->NextLoc.y },
                { "z", guest->NextLoc.z },
            };
            result["currentStation"] = guest->CurrentRideStation.ToUnderlying();
            result["currentTrain"] = guest->CurrentTrain;
            result["currentCar"] = guest->CurrentCar;
            result["currentSeat"] = guest->CurrentSeat;
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

        json_t TileElementValue(const TileElement& element)
        {
            json_t value = {
                { "type", static_cast<uint8_t>(element.getType()) },
                { "baseZ", element.getBaseZ() },
                { "clearanceZ", element.getClearanceZ() },
                { "direction", static_cast<uint8_t>(element.getDirection()) },
                { "ghost", element.isGhost() },
            };
            if (const auto* track = element.asTrack())
            {
                value["track"] = {
                    { "ride", track->GetRideIndex().ToUnderlying() },
                    { "trackType", static_cast<uint16_t>(track->GetTrackType()) },
                    { "sequence", track->GetSequenceIndex() },
                    { "station", track->GetStationIndex().ToUnderlying() },
                };
            }
            else if (const auto* entrance = element.asEntrance())
            {
                value["entrance"] = {
                    { "ride", entrance->GetRideIndex().ToUnderlying() },
                    { "station", entrance->GetStationIndex().ToUnderlying() },
                    { "type", entrance->GetEntranceType() },
                };
            }
            else if (const auto* path = element.asPath())
            {
                value["path"] = {
                    { "edges", path->GetEdges() },
                    { "slope", path->IsSloped() },
                    { "slopeDirection", static_cast<uint8_t>(path->GetSlopeDirection()) },
                    { "queue", path->IsQueue() },
                    { "rideQueue", path->GetRideIndex().ToUnderlying() },
                    { "station", path->GetStationIndex().ToUnderlying() },
                };
            }
            return value;
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
            if (args.contains("includePath") && !args["includePath"].is_boolean())
                return { { "error", "includePath must be a boolean" } };
            if (args.contains("includeElements") && !args["includeElements"].is_boolean())
                return { { "error", "includeElements must be a boolean" } };

            json_t result = {
                { "x", x },
                { "y", y },
                { "slope", surface->GetSlope() },
                { "waterHeight", surface->GetWaterHeight() },
                { "grassLength", surface->GetGrassLength() },
                { "ownership", surface->GetOwnership() },
                { "surfaceObject", surface->GetSurfaceObjectIndex() },
                { "edgeObject", surface->GetEdgeObjectIndex() },
            };
            if (args.value("includePath", false))
            {
                json_t path = { { "present", false } };
                auto* element = MapGetFirstElementAt(TileCoordsXY{ x, y });
                while (element != nullptr)
                {
                    if (const auto* pathElement = element->asPath())
                    {
                        path = {
                            { "present", true },
                            { "baseZ", pathElement->getBaseZ() },
                            { "clearanceZ", pathElement->getClearanceZ() },
                            { "surfaceObject", pathElement->GetSurfaceEntryIndex() },
                            { "railingsObject", pathElement->GetRailingsEntryIndex() },
                            { "queue", pathElement->IsQueue() },
                            { "edges", pathElement->GetEdges() },
                        };
                        break;
                    }
                    if (element->isLastForTile())
                        break;
                    ++element;
                }
                result["path"] = std::move(path);
            }
            if (args.value("includeElements", false))
            {
                json_t elements = json_t::array();
                auto* element = MapGetFirstElementAt(TileCoordsXY{ x, y });
                while (element != nullptr)
                {
                    elements.push_back(TileElementValue(*element));
                    if (element->isLastForTile())
                        break;
                    ++element;
                }
                result["elements"] = std::move(elements);
            }
            return result;
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

        const std::array<NativeResourceDescriptor, 12> kResources = {
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
                { { "id", "ride" }, { "price", "money" }, { "vehicleIds", "entity" } }, "engine", json_t::array(), "native", ReadRide },
            { "vehicle", "Authoritative state for one ride vehicle.", ObjectSchema({ { "id", { { "type", "integer" }, { "minimum", 0 } } } }, { "id" }),
                { { "id", "entity" }, { "ride", "ride" }, { "x", "map-units" }, { "trackProgress", "track-progress" } }, "engine", json_t::array(), "native", ReadVehicle },
            { "guests", "Authoritative collection of guest entities.", EmptySchema(), { { "id", "entity" }, { "x", "map-units" } },
                "engine", json_t::array(), "native", ReadGuests },
            { "guest", "Authoritative state for one guest entity.", ObjectSchema({ { "id", { { "type", "integer" }, { "minimum", 0 } } } }, { "id" }),
                { { "id", "entity" }, { "x", "map-units" } }, "engine", json_t::array(), "native", ReadGuest },
            { "objects", "Authoritative loaded object identities.", EmptySchema(), { { "count", "objects" } },
                "engine", json_t::array(), "native", ReadObjects },
            { "tile", "Authoritative surface and optional path state at a map tile.", ObjectSchema({ { "x", { { "type", "integer" } } }, { "y", { { "type", "integer" } } }, { "includePath", { { "type", "boolean" } } }, { "includeElements", { { "type", "boolean" } } } }, { "x", "y" }),
                { { "x", "map-units" }, { "y", "map-units" }, { "waterHeight", "height-units" }, { "elements", "ordered" } }, "engine", json_t::array(), "native", ReadTile },
            { "region", "Derived map-boundary projection from mapSize.", EmptySchema(), { { "mapWidth", "map-units" }, { "mapHeight", "map-units" } },
                "derived", { "mapSize" }, "native", ReadRegion },
        };
    }

    const std::array<NativeResourceDescriptor, 12>& NativeResources()
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
                json_t::array(),
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
            { "inputs", descriptor.inputs },
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

    void SetNativeRecordingRoot(std::string root)
    {
        std::lock_guard lock(gSaveRootMutex);
        gRecordingRoot = std::move(root);
    }

    std::string NativeRecordingRoot()
    {
        std::lock_guard lock(gSaveRootMutex);
        return gRecordingRoot;
    }

    void SetNativeCaptureRoot(std::string root)
    {
        std::lock_guard lock(gSaveRootMutex);
        gCaptureRoot = std::move(root);
    }

    std::string NativeCaptureRoot()
    {
        std::lock_guard lock(gSaveRootMutex);
        return gCaptureRoot;
    }

    NativeDispatchResult StartNativeRecording(std::string_view path)
    {
        if (!NativePathContained(NativeRecordingRoot(), path))
            return Failure("recording_containment", "recording path must remain below the owned recording root");
        auto* replay = GetContext()->GetReplayManager();
        if (replay == nullptr || replay->IsRecording())
            return Failure("recording_active", "native recording is already active");
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
        if (error || !replay->StartRecording(std::string(path), k_MaxReplayTicks, IReplayManager::RecordType::SILENT))
            return Failure("recording_start_failed", "the engine could not start native recording", { { "path", path } });
        return Success({ { "status", "active" }, { "path", path }, { "tick", getGameState().currentTicks } });
    }

    NativeDispatchResult StopNativeRecording()
    {
        auto* replay = GetContext()->GetReplayManager();
        if (replay == nullptr || !replay->IsRecording())
            return Failure("recording_inactive", "native recording is not active");
        ReplayRecordInfo info{};
        replay->GetCurrentReplayInfo(info);
        if (!replay->StopRecording())
            return Failure("recording_finalize_failed", "the engine could not finalize native recording", { { "path", info.FilePath } });
        return Success({ { "status", "final" }, { "path", info.FilePath }, { "tick", getGameState().currentTicks } });
    }

    NativeDispatchResult ValidateNativeCaptureView(const json_t& view, int32_t mapWidth, int32_t mapHeight)
    {
        if (!view.is_object())
            return Failure("capture_view_invalid", "capture view must be a JSON object");
        const auto required = { "width", "height", "center", "zoom", "rotation" };
        for (const auto* key : required)
        {
            if (!view.contains(key))
                return Failure("capture_view_invalid", "capture view is missing a required field", { { "field", key } });
        }
        if (view.size() != 5 || !view["center"].is_object() || view["center"].size() != 2
            || !view["center"].contains("x") || !view["center"].contains("y"))
            return Failure("capture_view_invalid", "capture view has an invalid center object");
        const auto& center = view["center"];
        if (!view["width"].is_number_integer() || !view["height"].is_number_integer()
            || !center["x"].is_number_integer() || !center["y"].is_number_integer()
            || !view["zoom"].is_number_integer() || !view["rotation"].is_number_integer())
            return Failure("capture_view_invalid", "capture view fields must be integers");
        const auto width = view["width"].get<int64_t>();
        const auto height = view["height"].get<int64_t>();
        const auto x = center["x"].get<int64_t>();
        const auto y = center["y"].get<int64_t>();
        const auto zoom = view["zoom"].get<int64_t>();
        const auto rotation = view["rotation"].get<int64_t>();
        if (width < 1 || width > 1920 || height < 1 || height > 1080
            || width * height > 2'073'600)
            return Failure("capture_view_dimensions", "capture view dimensions exceed the bounded pixel limits");
        const auto maxX = static_cast<int64_t>(mapWidth) * 32;
        const auto maxY = static_cast<int64_t>(mapHeight) * 32;
        if (x < 16 || x > maxX - 16 || y < 16 || y > maxY - 16)
            return Failure("capture_view_center", "capture view center must remain inside the map");
        if (zoom < 0 || zoom > 3)
            return Failure("capture_view_zoom", "capture view zoom is not supported");
        if (rotation < 0 || rotation > 3)
            return Failure("capture_view_rotation", "capture view rotation is not supported");
        return Success({
            { "width", width },
            { "height", height },
            { "center", { { "x", x }, { "y", y } } },
            { "zoom", zoom },
            { "rotation", rotation },
        });
    }

    NativeDispatchResult ValidateNativeCaptureView(const json_t& view, const GameState_t& state)
    {
        return ValidateNativeCaptureView(view, state.mapSize.x, state.mapSize.y);
    }

    NativeDispatchResult CaptureNativeFrame(std::string_view path, const json_t& view)
    {
        if (!NativePathContained(NativeCaptureRoot(), path))
            return Failure("capture_containment", "capture path must remain below the owned capture root");
        const bool explicitView = !view.is_null();
        if (explicitView)
        {
            const auto validation = ValidateNativeCaptureView(view, getGameState());
            if (!validation.ok)
                return validation;
        }
        std::error_code error;
        const auto destination = std::filesystem::path(path);
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error)
            return Failure("capture_failed", "unable to create the capture destination", { { "path", path } });

        const auto screenshotRoot = std::filesystem::path(
            GetContext()->GetPlatformEnvironment().GetDirectoryPath(DirBase::user, DirId::screenshots));
        std::filesystem::create_directories(screenshotRoot, error);
        if (error)
            return Failure("capture_failed", "unable to create the native screenshot directory", { { "path", path }, { "error", error.message() } });
        const auto temporaryName = std::string("parkbench-frame-") + std::to_string(getGameState().currentTicks) + ".png";
        const auto temporary = screenshotRoot / temporaryName;
        std::filesystem::remove(temporary, error);
        try
        {
            CaptureOptions options;
            options.Filename = temporaryName;
            const auto& state = getGameState();
            const auto selected = explicitView
                ? view
                : json_t{
                    { "width", 640 },
                    { "height", 480 },
                    { "center", { { "x", state.mapSize.x * 16 }, { "y", state.mapSize.y * 16 } } },
                    { "zoom", 0 },
                    { "rotation", 0 },
                };
            options.View = CaptureView{
                selected["width"].get<int32_t>(),
                selected["height"].get<int32_t>(),
                CoordsXY{ selected["center"]["x"].get<int32_t>(), selected["center"]["y"].get<int32_t>() },
            };
            options.Zoom = ZoomLevel(selected["zoom"].get<int8_t>());
            options.Rotation = selected["rotation"].get<uint8_t>();
            CaptureImage(options);
        }
        catch (const std::exception& exception)
        {
            return Failure("capture_failed", "the native renderer could not capture a frame", { { "path", path }, { "error", exception.what() } });
        }
        if (!std::filesystem::is_regular_file(temporary, error) || error)
            return Failure("capture_failed", "the native renderer produced no frame", { { "path", path } });
        std::filesystem::rename(temporary, destination, error);
        if (error)
            return Failure("capture_failed", "unable to retain the rendered frame", { { "path", path }, { "error", error.message() } });
        const auto& state = getGameState();
        const auto capturedView = explicitView
            ? view
            : json_t{
                { "width", 640 },
                { "height", 480 },
                { "center", { { "x", state.mapSize.x * 16 }, { "y", state.mapSize.y * 16 } } },
                { "zoom", 0 },
                { "rotation", 0 },
            };
        return Success({ { "status", "captured" }, { "path", path }, { "tick", state.currentTicks }, { "view", capturedView } });
    }
}
