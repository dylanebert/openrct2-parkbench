#include "NativeActionContractRideProjection.h"
#include <algorithm>
#include <limits>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/management/Finance.h>
#include <openrct2/management/Marketing.h>
#include <openrct2/management/NewsItem.h>
#include <openrct2/peep/RideUseSystem.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/world/Banner.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/Park.h>
#include <openrct2/world/tile_element/EntranceElement.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/tile_element/TileElement.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace OpenRCT2::Testing
{
    namespace
    {
        std::optional<RideProjectionWatchSet> gActiveWatch;
        std::optional<ProjectionFixtureHandles> gHandles;
        std::optional<ProjectionFieldInstance> gOmission;
        ProjectionFixtureMutation gMutation = ProjectionFixtureMutation::none;

        uint16_t IdValue(EntityId id)
        {
            return id.ToUnderlying();
        }

        bool Omit(ProjectionField field, ProjectionRecordIdentity record, ProjectionIndex index = {})
        {
            return gOmission.has_value() && gOmission->field == field && gOmission->record == record
                && gOmission->index == index;
        }

        template<typename T>
        void Put(json_t& object, const char* key, T&& value, ProjectionField field, ProjectionRecordIdentity record,
                 ProjectionIndex index = {})
        {
            if (!Omit(field, record, index))
                object[key] = std::forward<T>(value);
        }

        json_t NewsItemJson(const News::Item& item, ProjectionRecordIdentity record)
        {
            json_t result;
            Put(result, "type", static_cast<uint8_t>(item.type), ProjectionField::type, record);
            Put(result, "flags", item.flags, ProjectionField::flags, record);
            Put(result, "assoc", item.assoc, ProjectionField::ride, record);
            Put(result, "ticks", item.ticks, ProjectionField::value, record);
            Put(result, "monthYear", item.monthYear, ProjectionField::value, record, { 1, 0 });
            Put(result, "day", item.day, ProjectionField::value, record, { 2, 0 });
            Put(result, "text", item.text, ProjectionField::raw, record);
            return result;
        }

        json_t EndpointJson(const TileCoordsXYZD& endpoint, const TileElement* element, ProjectionRecordIdentity record)
        {
            json_t result;
            const bool present = !endpoint.IsNull();
            Put(result, "present", present, ProjectionField::exists, record);
            Put(result, "x", present ? json_t(endpoint.x) : json_t(nullptr), ProjectionField::positionX, record);
            Put(result, "y", present ? json_t(endpoint.y) : json_t(nullptr), ProjectionField::positionY, record);
            Put(result, "z", present ? json_t(endpoint.z) : json_t(nullptr), ProjectionField::positionZ, record);
            Put(result, "direction", present ? json_t(endpoint.direction) : json_t(nullptr), ProjectionField::value, record);
            Put(result, "type", element == nullptr ? json_t(nullptr) : json_t(static_cast<uint8_t>(element->getType())), ProjectionField::type, record);
            Put(result, "ride", element == nullptr ? json_t(nullptr) : json_t(element->GetRideIndex().ToUnderlying()), ProjectionField::ride, record);
            Put(result, "station", json_t(0), ProjectionField::station, record);
            Put(result, "entryIndex", element == nullptr ? json_t(nullptr) : json_t(element->asEntrance() == nullptr ? 0 : element->asEntrance()->getEntryIndex()), ProjectionField::value, record, { 1, 0 });
            Put(result, "baseHeight", element == nullptr ? json_t(nullptr) : json_t(element->baseHeight), ProjectionField::positionZ, record, { 1, 0 });
            Put(result, "clearanceHeight", element == nullptr ? json_t(nullptr) : json_t(element->clearanceHeight), ProjectionField::positionZ, record, { 2, 0 });
            return result;
        }

        json_t SerializeVehicle(const Vehicle* vehicle, uint16_t id, const ProjectionFixtureHandles& handles)
        {
            const auto identity = ProjectionRecordIdentity{ ProjectionRecordKind::vehicle, id };
            json_t result;
            Put(result, "id", id, ProjectionField::id, identity);
            Put(result, "exists", vehicle != nullptr, ProjectionField::exists, identity);
            if (vehicle == nullptr)
                return result;
            Put(result, "ride", vehicle->ride.ToUnderlying(), ProjectionField::ride, identity);
            Put(result, "subtype", static_cast<uint8_t>(vehicle->SubType), ProjectionField::subtype, identity);
            Put(result, "vehicleType", vehicle->vehicle_type, ProjectionField::type, identity);
            Put(result, "trainLink", IdValue(vehicle->next_vehicle_on_train), ProjectionField::link, identity, { 0, 0 });
            Put(result, "previousRideLink", IdValue(vehicle->prev_vehicle_on_ride), ProjectionField::link, identity, { 1, 0 });
            Put(result, "nextRideLink", IdValue(vehicle->next_vehicle_on_ride), ProjectionField::link, identity, { 2, 0 });
            Put(result, "status", static_cast<uint8_t>(vehicle->status), ProjectionField::status, identity);
            Put(result, "seats", vehicle->num_seats, ProjectionField::value, identity, { 1, 0 });
            Put(result, "occupantCount", vehicle->num_peeps, ProjectionField::value, identity, { 2, 0 });
            Put(result, "nextFreeSeat", vehicle->next_free_seat, ProjectionField::value, identity, { 3, 0 });
            json_t occupants = json_t::array();
            for (uint16_t i = 0; i < 32; ++i)
            {
                const auto value = vehicle->peep[i];
                if (!Omit(ProjectionField::occupant, identity, { i, 0 }))
                    occupants.push_back(IdValue(value));
            }
            result["occupants"] = std::move(occupants);
            Put(result, "flags", vehicle->flags.holder, ProjectionField::flags, identity);
            Put(result, "trackLocation", json_t{ { "x", vehicle->TrackLocation.x }, { "y", vehicle->TrackLocation.y }, { "z", vehicle->TrackLocation.z } },
                ProjectionField::value, identity, { 4, 0 });
            Put(result, "trackTypeAndDirection", vehicle->TrackTypeAndDirection, ProjectionField::value, identity, { 5, 0 });
            Put(result, "constructionStatus", static_cast<uint8_t>(vehicle->status), ProjectionField::status, identity, { 1, 0 });
            Put(result, "testing", vehicle->flags.has(VehicleFlag::testing), ProjectionField::flags, identity, { 1, 0 });
            Put(result, "restraints", vehicle->restraints_position, ProjectionField::value, identity, { 6, 0 });
            Put(result, "currentStation", vehicle->current_station.ToUnderlying(), ProjectionField::station, identity);
            Put(result, "trackProgress", vehicle->track_progress, ProjectionField::value, identity, { 7, 0 });
            Put(result, "subState", vehicle->sub_state, ProjectionField::value, identity, { 8, 0 });
            json_t colours;
            Put(colours, "body", static_cast<uint8_t>(vehicle->colours.Body), ProjectionField::value, identity, { 9, 0 });
            Put(colours, "trim", static_cast<uint8_t>(vehicle->colours.Trim), ProjectionField::value, identity, { 10, 0 });
            Put(colours, "tertiary", static_cast<uint8_t>(vehicle->colours.Tertiary), ProjectionField::value, identity, { 11, 0 });
            result["colours"] = std::move(colours);
            return result;
        }

        json_t SerializeGuest(const Guest* guest, uint16_t id, const ProjectionFixtureHandles& handles)
        {
            const auto identity = ProjectionRecordIdentity{ ProjectionRecordKind::guest, id };
            json_t result;
            Put(result, "id", id, ProjectionField::id, identity);
            Put(result, "exists", guest != nullptr, ProjectionField::exists, identity);
            if (guest == nullptr)
                return result;
            Put(result, "currentRide", guest->CurrentRide.ToUnderlying(), ProjectionField::ride, identity);
            Put(result, "currentStation", guest->CurrentRideStation.ToUnderlying(), ProjectionField::station, identity);
            Put(result, "currentTrain", guest->CurrentTrain, ProjectionField::value, identity, { 0, 0 });
            Put(result, "state", static_cast<uint8_t>(guest->State), ProjectionField::status, identity);
            Put(result, "substate", guest->SubState, ProjectionField::value, identity, { 1, 0 });
            Put(result, "queuePredecessor", IdValue(guest->guestNextInQueue), ProjectionField::link, identity);
            Put(result, "queueTime", guest->timeInQueue, ProjectionField::queueTime, identity);
            Put(result, "rejoinQueueTimeout", guest->rejoinQueueTimeout, ProjectionField::value, identity, { 2, 0 });
            Put(result, "previousRideTimeout", guest->previousRideTimeOut, ProjectionField::value, identity, { 3, 0 });
            Put(result, "headingToRide", guest->guestHeadingToRideId.ToUnderlying(), ProjectionField::ride, identity, { 1, 0 });
            Put(result, "favouriteRide", guest->favouriteRide.ToUnderlying(), ProjectionField::ride, identity, { 2, 0 });
            Put(result, "previousRide", guest->previousRide.ToUnderlying(), ProjectionField::ride, identity, { 3, 0 });
            Put(result, "voucherRide", guest->voucherRideId.ToUnderlying(), ProjectionField::ride, identity, { 4, 0 });
            Put(result, "photoRides", json_t{ guest->photo1RideRef.ToUnderlying(), guest->photo2RideRef.ToUnderlying(), guest->photo3RideRef.ToUnderlying(), guest->photo4RideRef.ToUnderlying() },
                ProjectionField::ride, identity, { 5, 0 });
            Put(result, "itemFlags", guest->itemFlags, ProjectionField::flags, identity);
            Put(result, "peepFlags", guest->PeepFlags, ProjectionField::flags, identity, { 1, 0 });
            json_t thought{ { "type", static_cast<uint8_t>(guest->thoughts[0].type) }, { "itemOrRide", guest->thoughts[0].item },
                            { "freshness", guest->thoughts[0].freshness }, { "freshTimeout", guest->thoughts[0].fresh_timeout } };
            result["thoughts"] = json_t::array({ std::move(thought) });
            if (id == handles.seatedGuest.ToUnderlying())
                result["seat"] = { { "car", guest->CurrentCar }, { "seat", guest->CurrentSeat } };
            else if (id == handles.watchingGuest.ToUnderlying())
                result["standing"] = { { "timeToStand", guest->TimeToStand }, { "flags", guest->StandingFlags } };
            return result;
        }

        const TileElement* FindRoleElement(TileCoordsXY coords, TileRole role, RideId rideId)
        {
            auto* element = MapGetFirstElementAt(coords);
            if (element == nullptr)
                return nullptr;
            while (true)
            {
                if (role == TileRole::track && element->asTrack() != nullptr && element->GetRideIndex() == rideId)
                    return element;
                if ((role == TileRole::entrance || role == TileRole::exit) && element->asEntrance() != nullptr
                    && element->GetRideIndex() == rideId
                    && element->asEntrance()->GetEntranceType() == (role == TileRole::exit ? ENTRANCE_TYPE_RIDE_EXIT : ENTRANCE_TYPE_RIDE_ENTRANCE))
                    return element;
                if (role == TileRole::queue && element->asPath() != nullptr && element->asPath()->IsQueue()
                    && element->GetRideIndex() == rideId)
                    return element;
                if (element->isLastForTile())
                    break;
                ++element;
            }
            return nullptr;
        }

        json_t SerializeTile(const TileRoleHandle& handle, RideId rideId)
        {
            const auto identity = ProjectionRecordIdentity{ ProjectionRecordKind::tile, 0, handle.role };
            const auto* element = FindRoleElement(handle.coords, handle.role, rideId);
            json_t result;
            Put(result, "x", handle.coords.x, ProjectionField::positionX, identity);
            Put(result, "y", handle.coords.y, ProjectionField::positionY, identity);
            Put(result, "role", static_cast<uint8_t>(handle.role), ProjectionField::type, identity);
            Put(result, "present", element != nullptr, ProjectionField::exists, identity);
            Put(result, "type", element == nullptr ? json_t(nullptr) : json_t(static_cast<uint8_t>(element->getType())), ProjectionField::type, identity, { 1, 0 });
            Put(result, "flags", element == nullptr ? json_t(nullptr) : json_t(element->flags), ProjectionField::flags, identity);
            Put(result, "baseHeight", element == nullptr ? json_t(nullptr) : json_t(element->baseHeight), ProjectionField::value, identity, { 1, 0 });
            Put(result, "clearanceHeight", element == nullptr ? json_t(nullptr) : json_t(element->clearanceHeight), ProjectionField::value, identity, { 2, 0 });
            Put(result, "owner", element == nullptr ? json_t(nullptr) : json_t(element->owner), ProjectionField::value, identity, { 3, 0 });
            json_t bytes = json_t::array();
            for (uint16_t i = 0; i < kTileElementSize; ++i)
            {
                uint8_t byte = 0;
                if (element != nullptr)
                    byte = reinterpret_cast<const uint8_t*>(element)[i];
                if (!Omit(ProjectionField::tileBytes, identity, { i, 0 }))
                    bytes.push_back(byte);
            }
            result["bytes"] = std::move(bytes);
            if (handle.role == TileRole::track)
            {
                json_t track;
                const auto* value = element == nullptr ? nullptr : element->asTrack();
                Put(track, "ride", value == nullptr ? json_t(nullptr) : json_t(value->GetRideIndex().ToUnderlying()), ProjectionField::ride, identity);
                Put(track, "rideType", value == nullptr ? json_t(nullptr) : json_t(value->GetRideType()), ProjectionField::type, identity, { 1, 0 });
                Put(track, "station", value == nullptr ? json_t(nullptr) : json_t(value->GetStationIndex().ToUnderlying()), ProjectionField::station, identity);
                Put(track, "trackType", value == nullptr ? json_t(nullptr) : json_t(static_cast<uint16_t>(value->GetTrackType())), ProjectionField::type, identity, { 2, 0 });
                Put(track, "sequence", value == nullptr ? json_t(nullptr) : json_t(value->GetSequenceIndex()), ProjectionField::value, identity, { 4, 0 });
                Put(track, "colourScheme", value == nullptr ? json_t(nullptr) : json_t(value->GetColourScheme()), ProjectionField::value, identity, { 5, 0 });
                Put(track, "invisible", value == nullptr ? json_t(nullptr) : json_t(value->isInvisible()), ProjectionField::value, identity, { 6, 0 });
                Put(track, "direction", value == nullptr ? json_t(nullptr) : json_t(static_cast<uint8_t>(value->getDirection())), ProjectionField::value, identity, { 7, 0 });
                Put(track, "ghost", value == nullptr ? json_t(nullptr) : json_t(value->isGhost()), ProjectionField::value, identity, { 8, 0 });
                result["track"] = std::move(track);
            }
            else if (handle.role == TileRole::queue)
            {
                json_t path;
                const auto* value = element == nullptr ? nullptr : element->asPath();
                Put(path, "isQueue", value == nullptr ? json_t(false) : json_t(value->IsQueue()), ProjectionField::type, identity);
                Put(path, "ride", value == nullptr ? json_t(nullptr) : json_t(value->GetRideIndex().ToUnderlying()), ProjectionField::ride, identity);
                Put(path, "station", value == nullptr ? json_t(nullptr) : json_t(value->GetStationIndex().ToUnderlying()), ProjectionField::station, identity);
                Put(path, "edges", value == nullptr ? json_t(nullptr) : json_t(value->GetEdgesAndCorners()), ProjectionField::value, identity, { 9, 0 });
                Put(path, "slope", value == nullptr ? json_t(nullptr) : json_t(static_cast<uint8_t>(value->GetSlopeDirection())), ProjectionField::value, identity, { 10, 0 });
                Put(path, "surface", value == nullptr ? json_t(nullptr) : json_t(value->GetSurfaceEntryIndex()), ProjectionField::value, identity, { 11, 0 });
                Put(path, "railings", value == nullptr ? json_t(nullptr) : json_t(value->GetRailingsEntryIndex()), ProjectionField::value, identity, { 12, 0 });
                Put(path, "baseHeight", element == nullptr ? json_t(nullptr) : json_t(element->baseHeight), ProjectionField::value, identity, { 13, 0 });
                Put(path, "clearanceHeight", element == nullptr ? json_t(nullptr) : json_t(element->clearanceHeight), ProjectionField::value, identity, { 14, 0 });
                Put(path, "ghost", element == nullptr ? json_t(nullptr) : json_t(element->isGhost()), ProjectionField::value, identity, { 15, 0 });
                result["path"] = std::move(path);
            }
            else
            {
                json_t entrance;
                const auto* value = element == nullptr ? nullptr : element->asEntrance();
                Put(entrance, "present", value != nullptr, ProjectionField::exists, identity, { 1, 0 });
                Put(entrance, "x", value == nullptr ? json_t(nullptr) : json_t(handle.coords.x), ProjectionField::positionX, identity, { 1, 0 });
                Put(entrance, "y", value == nullptr ? json_t(nullptr) : json_t(handle.coords.y), ProjectionField::positionY, identity, { 1, 0 });
                Put(entrance, "z", value == nullptr ? json_t(nullptr) : json_t(element->baseHeight), ProjectionField::positionZ, identity, { 1, 0 });
                Put(entrance, "direction", value == nullptr ? json_t(nullptr) : json_t(static_cast<uint8_t>(value->getDirection())), ProjectionField::value, identity, { 16, 0 });
                Put(entrance, "type", value == nullptr ? json_t(nullptr) : json_t(value->GetEntranceType()), ProjectionField::type, identity, { 3, 0 });
                Put(entrance, "ride", value == nullptr ? json_t(nullptr) : json_t(value->GetRideIndex().ToUnderlying()), ProjectionField::ride, identity, { 17, 0 });
                Put(entrance, "station", value == nullptr ? json_t(nullptr) : json_t(value->GetStationIndex().ToUnderlying()), ProjectionField::station, identity, { 1, 0 });
                Put(entrance, "entryIndex", value == nullptr ? json_t(nullptr) : json_t(value->getEntryIndex()), ProjectionField::value, identity, { 18, 0 });
                Put(entrance, "baseHeight", element == nullptr ? json_t(nullptr) : json_t(element->baseHeight), ProjectionField::value, identity, { 19, 0 });
                Put(entrance, "clearanceHeight", element == nullptr ? json_t(nullptr) : json_t(element->clearanceHeight), ProjectionField::value, identity, { 20, 0 });
                Put(entrance, "ghost", element == nullptr ? json_t(nullptr) : json_t(element->isGhost()), ProjectionField::value, identity, { 21, 0 });
                result["entrance"] = std::move(entrance);
            }
            return result;
        }

        json_t SerializeRide(const Ride& ride, const ProjectionFixtureHandles& handles)
        {
            const auto identity = ProjectionRecordIdentity{ ProjectionRecordKind::ride, ride.id.ToUnderlying() };
            json_t result;
            Put(result, "id", ride.id.ToUnderlying(), ProjectionField::id, identity);
            Put(result, "exists", true, ProjectionField::exists, identity);
            Put(result, "type", ride.type, ProjectionField::type, identity);
            Put(result, "subtype", ride.subtype, ProjectionField::subtype, identity);
            Put(result, "status", static_cast<uint8_t>(ride.status), ProjectionField::status, identity);
            json_t vehicles = json_t::array();
            for (uint16_t i = 0; i < 2; ++i)
            {
                const auto id = ride.vehicles[i];
                if (!Omit(ProjectionField::link, identity, { i, 0 }))
                    vehicles.push_back(IdValue(id));
            }
            result["vehicles"] = std::move(vehicles);
            Put(result, "numTrains", ride.numTrains, ProjectionField::value, identity, { 22, 0 });
            Put(result, "numCarsPerTrain", ride.numCarsPerTrain, ProjectionField::value, identity, { 23, 0 });
            Put(result, "maxTrains", ride.maxTrains, ProjectionField::value, identity, { 24, 0 });
            Put(result, "vehicleChangeTimeout", ride.vehicleChangeTimeout, ProjectionField::value, identity, { 25, 0 });
            Put(result, "flags", ride.flags.holder, ProjectionField::flags, identity);
            Put(result, "currentIssues", ride.currentIssues, ProjectionField::value, identity, { 26, 0 });
            Put(result, "lastIssueTime", ride.lastIssueTime, ProjectionField::value, identity, { 27, 0 });
            Put(result, "fixedRatings", ride.flags.has(RideFlag::fixedRatings), ProjectionField::flags, identity, { 2, 0 });
            Put(result, "tested", ride.flags.has(RideFlag::tested), ProjectionField::flags, identity, { 3, 0 });
            Put(result, "testInProgress", ride.flags.has(RideFlag::testInProgress), ProjectionField::flags, identity, { 4, 0 });
            Put(result, "testingFlags", ride.testingFlags.holder, ProjectionField::flags, identity, { 5, 0 });
            Put(result, "currentTestSegment", ride.currentTestSegment, ProjectionField::value, identity, { 28, 0 });
            Put(result, "currentTestStation", ride.currentTestStation.ToUnderlying(), ProjectionField::station, identity, { 2, 0 });
            json_t measurement = nullptr;
            if (ride.measurement != nullptr)
            {
                measurement = { { "flags", ride.measurement->flags.holder }, { "lastUseTick", ride.measurement->last_use_tick },
                                { "numItems", ride.measurement->num_items }, { "currentItem", ride.measurement->current_item },
                                { "vehicleIndex", ride.measurement->vehicle_index }, { "currentStation", ride.measurement->current_station.ToUnderlying() },
                                { "vertical", json_t::array() }, { "lateral", json_t::array() }, { "velocity", json_t::array() }, { "altitude", json_t::array() } };
                for (uint16_t i = 0; i < ride.measurement->num_items && i < 2; ++i)
                {
                    measurement["vertical"].push_back(ride.measurement->vertical[i]);
                    measurement["lateral"].push_back(ride.measurement->lateral[i]);
                    measurement["velocity"].push_back(ride.measurement->velocity[i]);
                    measurement["altitude"].push_back(ride.measurement->altitude[i]);
                }
            }
            if (!Omit(ProjectionField::value, identity, { 29, 0 }))
                result["measurement"] = std::move(measurement);
            json_t stations = json_t::array();
            for (uint16_t i = 0; i < Limits::kMaxStationsPerRide; ++i)
            {
                const auto& station = ride.getStation(StationIndex::FromUnderlying(i));
                const auto stationIdentity = ProjectionRecordIdentity{ ProjectionRecordKind::ride, ride.id.ToUnderlying(), TileRole::track, false, i };
                json_t item;
                Put(item, "index", i, ProjectionField::id, stationIdentity);
                Put(item, "exists", i < ride.numStations, ProjectionField::exists, stationIdentity);
                Put(item, "start", json_t{ { "x", station.Start.x }, { "y", station.Start.y } }, ProjectionField::positionX, stationIdentity);
                Put(item, "height", station.Height, ProjectionField::positionZ, stationIdentity);
                Put(item, "length", station.Length, ProjectionField::value, stationIdentity);
                Put(item, "depart", station.Depart, ProjectionField::value, stationIdentity, { 1, 0 });
                Put(item, "trainAtStation", station.TrainAtStation, ProjectionField::value, stationIdentity, { 2, 0 });
                Put(item, "entrance", EndpointJson(station.Entrance, nullptr, stationIdentity), ProjectionField::value, stationIdentity, { 3, 0 });
                Put(item, "exit", EndpointJson(station.Exit, nullptr, stationIdentity), ProjectionField::value, stationIdentity, { 4, 0 });
                Put(item, "segmentLength", station.SegmentLength, ProjectionField::value, stationIdentity, { 5, 0 });
                Put(item, "segmentTime", station.SegmentTime, ProjectionField::value, stationIdentity, { 6, 0 });
                Put(item, "queueTime", station.QueueTime, ProjectionField::queueTime, stationIdentity);
                Put(item, "queueLength", station.QueueLength, ProjectionField::queueLength, stationIdentity);
                Put(item, "lastPeepInQueue", IdValue(station.LastPeepInQueue), ProjectionField::link, stationIdentity, { 7, 0 });
                stations.push_back(std::move(item));
            }
            result["stations"] = std::move(stations);
            return result;
        }

        std::string CanonicalRecord(std::string_view store, const json_t& record, const ProjectionFixtureHandles& h)
        {
            const auto id = record.value("id", std::numeric_limits<uint16_t>::max());
            if (store == "rides" && id == h.ride.ToUnderlying()) return "rides/target";
            if (store == "vehicles") return id == h.vehicleHead.ToUnderlying() ? "vehicles/head" : id == h.vehicleTail.ToUnderlying() ? "vehicles/tail" : "vehicles/other";
            if (store == "guests")
            {
                if (id == h.seatedGuest.ToUnderlying()) return "guests/seated";
                if (id == h.watchingGuest.ToUnderlying()) return "guests/watching";
                if (id == h.queueTailGuest.ToUnderlying()) return "guests/queueTail";
                if (id == h.queueHeadGuest.ToUnderlying()) return "guests/queueHead";
            }
            if (store == "banners") return "banners/target";
            if (store == "campaigns") return "campaigns/target";
            return std::string(store) + "/other";
        }

        void Walk(const json_t& value, const std::string& path, std::set<std::string>& output, const ProjectionFixtureHandles& h)
        {
            if (value.is_object())
            {
                if (value.empty()) output.insert(path);
                for (auto it = value.begin(); it != value.end(); ++it)
                    Walk(it.value(), path + "/" + it.key(), output, h);
                return;
            }
            if (value.is_array())
            {
                if (value.empty()) output.insert(path);
                for (size_t i = 0; i < value.size(); ++i)
                    Walk(value[i], path + "/" + std::to_string(i), output, h);
                return;
            }
            output.insert(path);
        }

        std::set<std::string> ActualPaths(const json_t& projection, const ProjectionFixtureHandles& h)
        {
            std::set<std::string> result;
            for (auto it = projection.begin(); it != projection.end(); ++it)
            {
                // These are owner-action wrapper observations, not kernel
                // serializer records.  They are intentionally outside the
                // independent S2a3 census.
                if (it.key() == "selectedRide" || it.key() == "selectedIsExit" || it.key() == "cash"
                    || it.key() == "tileElements" || it.key() == "endpoint" || it.key() == "entrance"
                    || it.key() == "stationBaseZ" || it.key() == "queueLastPeep" || it.key() == "queueLength"
                    || it.key() == "insertedElement" || it.key() == "rideStatus")
                    continue;
                const auto& value = it.value();
                if (it.key() == "rides" || it.key() == "vehicles" || it.key() == "guests" || it.key() == "banners" || it.key() == "campaigns")
                {
                    for (const auto& record : value)
                        Walk(record, CanonicalRecord(it.key(), record, h), result, h);
                }
                else if (it.key() == "tiles")
                {
                    for (const auto& record : value)
                    {
                        const auto role = static_cast<TileRole>(record.value("role", 0));
                        Walk(record, "tiles/" + std::string(role == TileRole::track ? "track" : role == TileRole::entrance ? "entrance" : role == TileRole::exit ? "exit" : "queue"), result, h);
                    }
                }
                else if (it.key() == "news")
                {
                    for (const auto queue : { "recent", "archived" })
                        for (const auto& record : value.at(queue))
                            Walk(record, "news/" + std::string(queue) + "/" + std::to_string(record.value("slot", 0)), result, h);
                }
                else if (it.key() == "rideUseHistory")
                {
                    for (const auto& record : value)
                    {
                        const auto id = record.value("guest", 0);
                        const auto name = id == h.watchingGuest.ToUnderlying() ? "watching" : "other";
                        Walk(record, std::string("rideUseHistory/") + name, result, h);
                    }
                }
                else
                    Walk(value, it.key(), result, h);
            }
            return result;
        }

        bool ValidateExactFields(const json_t& projection, const ProjectionFixtureHandles& h, std::string& failure)
        {
            // The value contract is identity-keyed and independent from the
            // path census.  These literals are fixture premises, never values
            // obtained by re-reading serialized JSON to manufacture an
            // expectation.
            const ProjectionContract contract{
                {},
                {
                    { { ProjectionRecordKind::park, 0 }, ProjectionField::parkValue, {}, 12345 },
                    { { ProjectionRecordKind::banner, h.banner.ToUnderlying() }, ProjectionField::bannerFlags, {}, 4 },
                    { { ProjectionRecordKind::campaign, h.ride.ToUnderlying() }, ProjectionField::campaignFlags, {}, 1 },
                },
            };
            for (const auto& expected : contract.exactFields)
            {
                json_t actual;
                if (expected.field == ProjectionField::parkValue)
                    actual = projection.value("parkValue", json_t(nullptr));
                else if (expected.field == ProjectionField::bannerFlags)
                {
                    actual = nullptr;
                    for (const auto& banner : projection["banners"])
                        if (banner.value("id", std::numeric_limits<uint16_t>::max()) == expected.record.id)
                            actual = banner.value("flags", json_t(nullptr));
                }
                else if (expected.field == ProjectionField::campaignFlags)
                {
                    actual = nullptr;
                    for (const auto& campaign : projection["campaigns"])
                        if (campaign.value("ride", std::numeric_limits<uint16_t>::max()) == expected.record.id)
                            actual = campaign.value("flags", json_t(nullptr));
                }
                if (actual != expected.expected)
                {
                    failure = "identity-keyed expected field mismatch for record kind "
                        + std::to_string(static_cast<uint8_t>(expected.record.kind));
                    return false;
                }
            }
            const auto find = [&projection](const char* store, uint16_t id) -> const json_t* {
                if (!projection.contains(store)) return nullptr;
                for (const auto& record : projection.at(store))
                    if (record.value("id", std::numeric_limits<uint16_t>::max()) == id) return &record;
                return nullptr;
            };
            const auto* ride = find("rides", h.ride.ToUnderlying());
            const auto* seated = find("guests", h.seatedGuest.ToUnderlying());
            const auto* watching = find("guests", h.watchingGuest.ToUnderlying());
            const auto* tail = find("guests", h.queueTailGuest.ToUnderlying());
            const auto* head = find("guests", h.queueHeadGuest.ToUnderlying());
            const auto* vehicleHead = find("vehicles", h.vehicleHead.ToUnderlying());
            const auto* vehicleTail = find("vehicles", h.vehicleTail.ToUnderlying());
            if (ride == nullptr || seated == nullptr || watching == nullptr || tail == nullptr || head == nullptr || vehicleHead == nullptr || vehicleTail == nullptr)
            { failure = "kernel identity handle disappeared"; return false; }
            if (seated->value("state", 255) != static_cast<uint8_t>(PeepState::onRide))
            {
                if (gMutation != ProjectionFixtureMutation::none)
                {
                    failure = "seated guest topology was cleared while a kernel mutation was under test";
                    return false;
                }
                return true;
            }
            if (!ride->value("exists", false) || seated->value("state", 255) != static_cast<uint8_t>(PeepState::onRide)
                || !seated->contains("seat") || seated->at("seat") != json_t({ { "car", 0 }, { "seat", 0 } })
                || watching->value("state", 255) != static_cast<uint8_t>(PeepState::watching) || !watching->contains("standing")
                || watching->at("standing").value("timeToStand", 0) != 73 || tail->value("queuePredecessor", std::numeric_limits<uint16_t>::max()) != h.queueHeadGuest.ToUnderlying()
                || head->value("queuePredecessor", 0) != 0)
            { failure = "state-discriminated guest topology is incoherent: seated=" + seated->dump() + " watching=" + watching->dump() + " tail=" + tail->dump() + " head=" + head->dump(); return false; }
            if (ride->at("vehicles")[0] != h.vehicleHead.ToUnderlying() || vehicleHead->value("trainLink", 0) != h.vehicleTail.ToUnderlying()
                || vehicleTail->value("trainLink", 0) != EntityId::GetNull().ToUnderlying()
                || vehicleHead->value("previousRideLink", 0) != h.vehicleTail.ToUnderlying()
                || vehicleHead->value("nextRideLink", 0) != h.vehicleTail.ToUnderlying()
                || vehicleTail->value("previousRideLink", 0) != h.vehicleHead.ToUnderlying()
                || vehicleTail->value("nextRideLink", 0) != h.vehicleHead.ToUnderlying()
                || vehicleHead->at("occupants")[0] != h.seatedGuest.ToUnderlying()
                || vehicleHead->value("occupantCount", 0) != 1 || vehicleHead->value("currentStation", 255) != 0
                || vehicleTail->value("currentStation", 255) != 0)
            { failure = "circular vehicle/train/seat topology is incoherent: ride0=" + std::to_string(ride->at("vehicles")[0].get<uint16_t>()) + " head=" + vehicleHead->dump() + " tail=" + vehicleTail->dump(); return false; }
            const auto expectedX = h.tiles[static_cast<size_t>(TileRole::track)].coords.x * kCoordsXYStep;
            const auto expectedY = h.tiles[static_cast<size_t>(TileRole::track)].coords.y * kCoordsXYStep;
            if (vehicleHead->at("trackLocation").value("x", -1) != expectedX
                || vehicleHead->at("trackLocation").value("y", -1) != expectedY
                || vehicleTail->at("trackLocation").value("x", -1) != expectedX
                || vehicleTail->at("trackLocation").value("y", -1) != expectedY
                || vehicleHead->at("trackLocation").value("z", -1) != h.expectedTrackZ
                || vehicleTail->at("trackLocation").value("z", -1) != h.expectedTrackZ
                || vehicleHead->value("trackTypeAndDirection", 0) != h.expectedTrackTypeAndDirection
                || vehicleTail->value("trackTypeAndDirection", 0) != h.expectedTrackTypeAndDirection
                || vehicleHead->value("flags", 0u) != h.expectedHeadFlags || vehicleTail->value("flags", 0u) != h.expectedTailFlags)
            { failure = "vehicle position/type/full flag exact field failed"; return false; }
            for (size_t i = 1; i < 32; ++i)
                if (vehicleHead->at("occupants")[i] != 0 || vehicleTail->at("occupants")[i] != 0)
                { failure = "unused vehicle occupant slot exact field failed"; return false; }
            const auto* measurement = ride->at("measurement").is_object() ? &ride->at("measurement") : nullptr;
            if (measurement == nullptr || measurement->value("flags", 0) != 3 || measurement->value("lastUseTick", 0) != 44
                || measurement->value("numItems", 0) != 2 || measurement->value("currentItem", 0) != 1
                || measurement->value("vehicleIndex", 255) != 0 || measurement->value("currentStation", 255) != 0
                || measurement->at("vertical") != json_t({ 1, -2 }) || measurement->at("lateral") != json_t({ 3, -4 })
                || measurement->at("velocity") != json_t({ 5, 6 }) || measurement->at("altitude") != json_t({ 7, 8 }))
            { failure = "measurement metadata/sample exact field failed"; return false; }
            const auto station = (*ride)["stations"][0];
            if (station.value("queueLength", 0) != 2 || station.value("queueTime", 0) != 37
                || station.value("lastPeepInQueue", 0) != h.queueTailGuest.ToUnderlying())
            { failure = "queue cardinality or station queue witness is incoherent"; return false; }
            for (const auto& expectedTile : h.tiles)
            {
                const json_t* tile = nullptr;
                for (const auto& candidate : projection["tiles"])
                    if (candidate.value("role", 255) == static_cast<uint8_t>(expectedTile.role)) tile = &candidate;
                if (tile == nullptr || tile->value("type", 255) != expectedTile.expectedType
                    || tile->value("flags", 255) != expectedTile.expectedFlags
                    || tile->value("baseHeight", 255) != expectedTile.expectedBaseHeight
                    || tile->value("clearanceHeight", 255) != expectedTile.expectedClearanceHeight
                    || tile->value("owner", 255) != expectedTile.expectedOwner
                    || tile->at("bytes") != json_t(expectedTile.expectedBytes))
                { failure = "typed tile role exact raw field failed"; return false; }
            }
            if (projection.value("parkValue", 0) != 12345 || projection["finance"]["expenditureTable"][0][static_cast<size_t>(ExpenditureType::rideConstruction)] != 77
                || projection["finance"]["expenditureTable"][3][static_cast<size_t>(ExpenditureType::rideRunningCosts)] != 88
                || projection["finance"]["valueHistory"][0] != 12000 || projection["finance"]["valueHistory"][7] != 11900)
            { failure = "finance or stored park value exact field failed"; return false; }
            const auto* banner = find("banners", h.banner.ToUnderlying());
            if (banner == nullptr || banner->value("flags", 0) != 4 || banner->value("assoc", 0) != h.ride.ToUnderlying())
            { failure = "banner raw identity/flags failed"; return false; }
            bool campaignOk = false;
            for (const auto& campaign : projection["campaigns"])
                if (campaign.value("ride", 0) == h.ride.ToUnderlying() && campaign.value("type", 0) == 5)
                    campaignOk = campaign.value("flags", 0) == 1 && campaign.value("weeksLeft", 0) == 3;
            if (!campaignOk) { failure = "campaign raw identity/flags failed"; return false; }
            return true;
        }
    }

    void SetProjectionFixtureMutation(ProjectionFixtureMutation mutation) { gMutation = mutation; }
    void ClearProjectionFixtureMutation() { gMutation = ProjectionFixtureMutation::none; }

    void ApplyProjectionFixtureMutation(GameState_t& state)
    {
        if (!gHandles) return;
        const auto h = *gHandles;
        switch (gMutation)
        {
            case ProjectionFixtureMutation::removeLinkedGuest:
                if (auto* e = state.entities.GetEntity<Guest>(h.watchingGuest); e) state.entities.EntityRemove(e); break;
            case ProjectionFixtureMutation::removeTailVehicle:
                if (auto* e = state.entities.GetEntity<Vehicle>(h.vehicleTail); e) state.entities.EntityRemove(e); break;
            case ProjectionFixtureMutation::breakVehicleReciprocalLink:
                if (auto* e = state.entities.GetEntity<Vehicle>(h.vehicleTail); e) e->prev_vehicle_on_ride = EntityId::GetNull(); break;
            case ProjectionFixtureMutation::alterCampaignType:
                for (auto& c : state.park.marketingCampaigns) if (c.rideId == h.ride) c.type = 0; break;
            case ProjectionFixtureMutation::clearBannerLink:
                if (auto* b = GetBanner(h.banner)) b->flags.unset(BannerFlag::linkedToRide); break;
            case ProjectionFixtureMutation::clearGuestItemFlags:
                if (auto* e = state.entities.GetEntity<Guest>(h.watchingGuest)) e->itemFlags = 0; break;
            case ProjectionFixtureMutation::aliasQueueAndExitTiles: gHandles->tiles[3].coords = gHandles->tiles[2].coords; break;
            case ProjectionFixtureMutation::omitRemovedEntranceWatch: gHandles->tiles[1].coords = { -1, -1 }; break;
            case ProjectionFixtureMutation::alterQueueTime: if (auto* r = GetRide(h.ride)) r->getStation(StationIndex::FromUnderlying(0)).QueueTime = 0; break;
            case ProjectionFixtureMutation::alterBannerPosition: if (auto* b = GetBanner(h.banner)) b->position = { 2, 2 }; break;
            case ProjectionFixtureMutation::breakRideRing: if (auto* e = state.entities.GetEntity<Vehicle>(h.vehicleHead)) e->prev_vehicle_on_ride = EntityId::GetNull(); break;
            case ProjectionFixtureMutation::moveSeatedOccupant: if (auto* e = state.entities.GetEntity<Vehicle>(h.vehicleHead)) { e->peep[0] = EntityId::GetNull(); e->peep[1] = h.seatedGuest; } break;
            case ProjectionFixtureMutation::alterSeatedCurrentCar: if (auto* e = state.entities.GetEntity<Guest>(h.seatedGuest)) e->CurrentCar = 1; break;
            case ProjectionFixtureMutation::putWatchingGuestInVehicle: if (auto* e = state.entities.GetEntity<Vehicle>(h.vehicleTail)) e->peep[0] = h.watchingGuest; break;
            case ProjectionFixtureMutation::alterVehiclePosition: if (auto* e = state.entities.GetEntity<Vehicle>(h.vehicleHead)) e->TrackLocation.x += 1; break;
            case ProjectionFixtureMutation::alterVehicleFlags: if (auto* e = state.entities.GetEntity<Vehicle>(h.vehicleHead)) e->flags.holder ^= 1; break;
            case ProjectionFixtureMutation::alterUnusedOccupant: if (auto* e = state.entities.GetEntity<Vehicle>(h.vehicleHead)) e->peep[31] = h.queueHeadGuest; break;
            case ProjectionFixtureMutation::alterMeasurementMetadata: if (auto* r = GetRide(h.ride); r && r->measurement) r->measurement->current_item = 0; break;
            case ProjectionFixtureMutation::alterParkValue: state.park.value = 12346; break;
            case ProjectionFixtureMutation::alterCampaignFlags: for (auto& c : state.park.marketingCampaigns) if (c.rideId == h.ride) c.flags.holder ^= 2; break;
            case ProjectionFixtureMutation::alterBannerFlags: if (auto* b = GetBanner(h.banner)) b->flags.holder ^= 2; break;
            case ProjectionFixtureMutation::alterTileRole: if (auto* e = const_cast<TileElement*>(FindRoleElement(h.tiles[0].coords, TileRole::track, h.ride))) e->owner ^= 1; break;
            case ProjectionFixtureMutation::breakQueueCardinality: if (auto* r = GetRide(h.ride)) r->getStation(StationIndex::FromUnderlying(0)).QueueLength = 1; break;
            case ProjectionFixtureMutation::none: break;
        }
    }

    void SetProjectionSerializerOmission(ProjectionFieldInstance field) { gOmission = field; }
    void ClearProjectionSerializerOmission() { gOmission.reset(); }
    void SetProjectionFixtureHandles(const ProjectionFixtureHandles& handles) { gHandles = handles; }
    void ClearProjectionFixtureHandles() { gHandles.reset(); }
    void SetRideProjectionWatchSet(RideProjectionWatchSet watch) { gActiveWatch = std::move(watch); }
    void ClearRideProjectionWatchSet() { gActiveWatch.reset(); }

    RideProjectionWatchSet CaptureRideProjectionWatchSet(const GameState_t& state, const json_t& args)
    {
        RideProjectionWatchSet watch;
        auto& entityRegistry = const_cast<GameState_t&>(state).entities;
        const bool fixtureLive = gHandles.has_value() && gHandles->ride.ToUnderlying() == args.value("ride", -1)
            && entityRegistry.GetEntity<Guest>(gHandles->seatedGuest) != nullptr
            && entityRegistry.GetEntity<Guest>(gHandles->watchingGuest) != nullptr
            && entityRegistry.GetEntity<Vehicle>(gHandles->vehicleHead) != nullptr
            && entityRegistry.GetEntity<Vehicle>(gHandles->vehicleTail) != nullptr;
        if (fixtureLive)
        {
            watch.fixtureHandles = gHandles;
            const auto& h = *gHandles;
            watch.rideIds.insert(h.ride.ToUnderlying());
            watch.vehicleIds = { h.vehicleHead.ToUnderlying(), h.vehicleTail.ToUnderlying() };
            watch.guestIds = { h.seatedGuest.ToUnderlying(), h.watchingGuest.ToUnderlying(), h.queueTailGuest.ToUnderlying(), h.queueHeadGuest.ToUnderlying() };
            watch.bannerIds = { h.banner.ToUnderlying() };
            watch.campaignKeys = { { h.campaign.type, h.campaign.ride.ToUnderlying() } };
            watch.recentNewsIndices = { h.recentNews.slot };
            watch.archivedNewsIndices = { h.archivedNews.slot };
            for (const auto& tile : h.tiles) watch.tileCoords.push_back(tile.coords);
            return watch;
        }
        const auto rideValue = args.value("ride", -1);
        if (rideValue >= 0) watch.rideIds.insert(static_cast<uint16_t>(rideValue));
        for (const auto id : const_cast<GameState_t&>(state).entities.GetEntityList(EntityType::vehicle)) watch.vehicleIds.insert(id.ToUnderlying());
        for (const auto id : const_cast<GameState_t&>(state).entities.GetEntityList(EntityType::guest)) watch.guestIds.insert(id.ToUnderlying());
        for (size_t i = 0; i < News::ItemHistoryStart; ++i) watch.recentNewsIndices.push_back(i);
        for (size_t i = 0; i < News::MaxItemsArchive; ++i) watch.archivedNewsIndices.push_back(i);
        return watch;
    }

    json_t SerializeRideProjection(const GameState_t& state, const json_t& args, const RideProjectionWatchSet& watch)
    {
        const ProjectionFixtureHandles h = watch.fixtureHandles.value_or(ProjectionFixtureHandles{});
        json_t projection;
        json_t watchJson;
        if (watch.fixtureHandles)
        {
            const auto& f = *watch.fixtureHandles;
            watchJson = { { "ride", f.ride.ToUnderlying() }, { "seatedGuest", f.seatedGuest.ToUnderlying() }, { "watchingGuest", f.watchingGuest.ToUnderlying() },
                          { "queueTailGuest", f.queueTailGuest.ToUnderlying() }, { "queueHeadGuest", f.queueHeadGuest.ToUnderlying() },
                          { "vehicleHead", f.vehicleHead.ToUnderlying() }, { "vehicleTail", f.vehicleTail.ToUnderlying() }, { "banner", f.banner.ToUnderlying() },
                          { "campaignType", f.campaign.type }, { "campaignRide", f.campaign.ride.ToUnderlying() }, { "recentSlot", f.recentNews.slot }, { "archivedSlot", f.archivedNews.slot },
                          { "tiles", { { "track", { { "x", f.tiles[0].coords.x }, { "y", f.tiles[0].coords.y } } }, { "entrance", { { "x", f.tiles[1].coords.x }, { "y", f.tiles[1].coords.y } } },
                                        { "exit", { { "x", f.tiles[2].coords.x }, { "y", f.tiles[2].coords.y } } }, { "queue", { { "x", f.tiles[3].coords.x }, { "y", f.tiles[3].coords.y } } } } } };
        }
        projection["watch"] = std::move(watchJson);
        json_t rides = json_t::array();
        for (const auto id : watch.rideIds) if (id < state.rides.size() && !state.rides[id].id.IsNull()) rides.push_back(SerializeRide(state.rides[id], h));
        projection["rides"] = std::move(rides);
        json_t vehicles = json_t::array();
        for (const auto id : watch.vehicleIds) vehicles.push_back(SerializeVehicle(const_cast<GameState_t&>(state).entities.GetEntity<Vehicle>(EntityId::FromUnderlying(id)), id, h));
        projection["vehicles"] = std::move(vehicles);
        json_t guests = json_t::array();
        for (const auto id : watch.guestIds) guests.push_back(SerializeGuest(const_cast<GameState_t&>(state).entities.GetEntity<Guest>(EntityId::FromUnderlying(id)), id, h));
        projection["guests"] = std::move(guests);
        json_t histories = json_t::array();
        for (const auto id : watch.guestIds)
        {
            json_t record{ { "guest", id }, { "rides", json_t::array() } };
            if (const auto* all = RideUse::GetHistory().GetAll(EntityId::FromUnderlying(id)); all) for (const auto ride : *all) record["rides"].push_back(ride.ToUnderlying());
            histories.push_back(std::move(record));
        }
        projection["rideUseHistory"] = std::move(histories);
        json_t news;
        for (const auto queue : { false, true })
        {
            json_t values = json_t::array();
            const auto& slots = queue ? watch.archivedNewsIndices : watch.recentNewsIndices;
            for (const auto slot : slots) values.push_back({ { "slot", slot }, { "item", NewsItemJson(state.newsItems[(queue ? News::ItemHistoryStart : 0) + slot], { ProjectionRecordKind::news, 0, TileRole::track, queue, static_cast<uint16_t>(slot) }) } });
            news[queue ? "archived" : "recent"] = std::move(values);
        }
        projection["news"] = std::move(news);
        json_t banners = json_t::array();
        for (const auto id : watch.bannerIds)
        {
            const auto* b = GetBanner(BannerIndex::FromUnderlying(id));
            if (!b || b->isNull()) { banners.push_back({ { "id", id }, { "exists", false } }); continue; }
            banners.push_back({ { "id", id }, { "exists", true }, { "type", b->type }, { "flags", b->flags.holder }, { "assoc", b->rideIndex.ToUnderlying() },
                                 { "text", b->text }, { "colour", static_cast<uint8_t>(b->colour) }, { "textColour", static_cast<uint8_t>(b->textColour) },
                                 { "position", { { "x", b->position.x }, { "y", b->position.y } } } });
        }
        projection["banners"] = std::move(banners);
        json_t campaigns = json_t::array();
        for (const auto [type, ride] : watch.campaignKeys)
            for (const auto& c : state.park.marketingCampaigns) if (c.type == type && c.rideId.ToUnderlying() == ride)
                campaigns.push_back({ { "type", c.type }, { "exists", true }, { "weeksLeft", c.weeksLeft }, { "flags", c.flags.holder }, { "ride", ride }, { "firstWeek", c.flags.has(MarketingCampaignFlag::firstWeek) } });
        projection["campaigns"] = std::move(campaigns);
        json_t tiles = json_t::array();
        if (watch.fixtureHandles) for (const auto& tile : h.tiles) tiles.push_back(SerializeTile(tile, h.ride));
        projection["tiles"] = std::move(tiles);
        json_t expenditure = json_t::array();
        for (uint16_t month = 0; month < 16; ++month)
        {
            json_t row = json_t::array();
            for (uint16_t type = 0; type < static_cast<uint16_t>(EnumValue(ExpenditureType::count)); ++type)
                if (!Omit(ProjectionField::financeCell, { ProjectionRecordKind::finance }, { month, type })) row.push_back(state.park.expenditureTable[month][type]);
            expenditure.push_back(std::move(row));
        }
        json_t history = json_t::array();
        for (uint16_t i = 0; i < kFinanceHistorySize; ++i) if (!Omit(ProjectionField::historyCell, { ProjectionRecordKind::finance }, { i, 0 })) history.push_back(state.park.valueHistory[i]);
        projection["finance"] = { { "cash", state.park.cash }, { "bankLoan", state.park.bankLoan }, { "maxBankLoan", state.park.maxBankLoan },
                                   { "loanInterestRate", state.park.bankLoanInterestRate }, { "expenditureTable", std::move(expenditure) }, { "valueHistory", std::move(history) } };
        projection["parkValue"] = state.park.value;
        projection["selectedRide"] = args.value("ride", -1);
        projection["selectedIsExit"] = args.contains("isExit") ? args.at("isExit") : json_t(nullptr);
        return projection;
    }

    json_t SerializeRideProjection(const GameState_t& state, const json_t& args)
    {
        if (gActiveWatch) return SerializeRideProjection(state, args, *gActiveWatch);
        return SerializeRideProjection(state, args, CaptureRideProjectionWatchSet(state, args));
    }

    bool ValidateRideProjectionStores(const json_t& projection, std::string* failure)
    {
        const auto reject = [failure](std::string message) { if (failure) *failure = std::move(message); return false; };
        for (const auto key : { "watch", "rides", "vehicles", "guests", "rideUseHistory", "news", "banners", "campaigns", "tiles", "finance", "parkValue" })
            if (!projection.contains(key)) return reject(std::string("projection store is omitted: ") + key);
        if (projection.contains("authoritative") || projection.contains("activePeepLinks")) return reject("projection contains a fake authoritative store");
        if (!gHandles) return true;
        const auto& h = *gHandles;
        const auto actual = ActualPaths(projection, h);
        const auto expected = IndependentKernelPaths(h, projection["rides"][0].contains("measurement") && !projection["rides"][0]["measurement"].is_null());
        if (actual != std::set<std::string>(expected.begin(), expected.end()))
        {
            std::ostringstream out;
            out << "projection leaf path census mismatch (actual=" << actual.size() << ", expected=" << expected.size() << ")";
            size_t shown = 0;
            for (const auto& path : expected)
                if (!actual.contains(path) && shown++ < 8) out << " missing=" << path;
            shown = 0;
            for (const auto& path : actual)
                if (std::find(expected.begin(), expected.end(), path) == expected.end() && shown++ < 8) out << " extra=" << path;
            return reject(out.str());
        }
        std::string exactFailure;
        if (!ValidateExactFields(projection, h, exactFailure)) return reject(exactFailure);
        return true;
    }
} // namespace OpenRCT2::Testing
