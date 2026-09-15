#include "NativeActionContractRideProjection.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/management/NewsItem.h>
#include <openrct2/management/Marketing.h>
#include <openrct2/peep/RideUseSystem.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/world/Banner.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/Park.h>
#include <openrct2/management/Finance.h>
#include <openrct2/world/tile_element/EntranceElement.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/tile_element/TileElement.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <optional>

namespace OpenRCT2::Testing
{
    namespace
    {
        std::optional<RideProjectionWatchSet> gActiveWatchSet;
        std::optional<ProjectionFixtureHandles> gFixtureHandles;
        ProjectionFixtureMutation gFixtureMutation = ProjectionFixtureMutation::none;
        std::optional<std::string> gSerializerOmission;
    }

    void SetProjectionFixtureMutation(ProjectionFixtureMutation mutation)
    {
        gFixtureMutation = mutation;
    }

    void ClearProjectionFixtureMutation()
    {
        gFixtureMutation = ProjectionFixtureMutation::none;
    }

    void ApplyProjectionFixtureMutation(GameState_t& state)
    {
        if (!gFixtureHandles.has_value())
            return;
        const auto handles = *gFixtureHandles;
        switch (gFixtureMutation)
        {
            case ProjectionFixtureMutation::removeLinkedGuest:
                if (auto* guest = state.entities.GetEntity<Guest>(handles.linkedGuest); guest != nullptr)
                    state.entities.EntityRemove(guest);
                break;
            case ProjectionFixtureMutation::removeTailVehicle:
                if (auto* vehicle = state.entities.GetEntity<Vehicle>(handles.vehicleTail); vehicle != nullptr)
                    state.entities.EntityRemove(vehicle);
                break;
            case ProjectionFixtureMutation::breakVehicleReciprocalLink:
                if (auto* tail = state.entities.GetEntity<Vehicle>(handles.vehicleTail); tail != nullptr)
                    tail->prev_vehicle_on_ride = EntityId::GetNull();
                break;
            case ProjectionFixtureMutation::alterCampaignType:
                for (auto& campaign : state.park.marketingCampaigns)
                {
                    if (campaign.rideId == handles.ride && campaign.type == handles.campaign.type)
                        campaign.type = 0;
                }
                break;
            case ProjectionFixtureMutation::clearBannerLink:
                if (auto* banner = GetBanner(handles.banner); banner != nullptr)
                    banner->flags.unset(BannerFlag::linkedToRide);
                break;
            case ProjectionFixtureMutation::clearGuestItemFlags:
                if (auto* guest = state.entities.GetEntity<Guest>(handles.linkedGuest); guest != nullptr)
                    guest->itemFlags = 0;
                break;
            case ProjectionFixtureMutation::aliasQueueAndExitTiles:
                gFixtureHandles->tiles[static_cast<size_t>(TileRole::queue)].coords
                    = gFixtureHandles->tiles[static_cast<size_t>(TileRole::exit)].coords;
                break;
            case ProjectionFixtureMutation::omitRemovedEntranceWatch:
                gFixtureHandles->tiles[static_cast<size_t>(TileRole::entrance)].coords = { -1, -1 };
                break;
            case ProjectionFixtureMutation::alterQueueTime:
                if (auto* ride = GetRide(handles.ride); ride != nullptr)
                    ride->getStation(StationIndex::FromUnderlying(0)).QueueTime = 0;
                break;
            case ProjectionFixtureMutation::alterBannerPosition:
                if (auto* banner = GetBanner(handles.banner); banner != nullptr)
                    banner->position = { 2, 2 };
                break;
            case ProjectionFixtureMutation::none:
                break;
        }
    }

    void SetProjectionSerializerOmission(std::string field)
    {
        gSerializerOmission = std::move(field);
    }

    void ClearProjectionSerializerOmission()
    {
        gSerializerOmission.reset();
    }

    namespace
    {
        bool OmitSerializerField(std::string_view field)
        {
            return gSerializerOmission.has_value() && *gSerializerOmission == field;
        }
    }

    void SetProjectionFixtureHandles(const ProjectionFixtureHandles& handles)
    {
        gFixtureHandles = handles;
    }

    void ClearProjectionFixtureHandles()
    {
        gFixtureHandles.reset();
    }

    void SetRideProjectionWatchSet(RideProjectionWatchSet watch)
    {
        gActiveWatchSet = std::move(watch);
    }

    void ClearRideProjectionWatchSet()
    {
        gActiveWatchSet.reset();
    }

    namespace
    {
        uint16_t IdValue(const EntityId id)
        {
            return id.IsNull() ? std::numeric_limits<uint16_t>::max() : id.ToUnderlying();
        }

        json_t SerializeNewsItem(const News::Item& item)
        {
            return {
                { "type", static_cast<uint8_t>(item.type) },
                { "flags", item.flags },
                { "assoc", item.assoc },
                { "ticks", item.ticks },
                { "monthYear", item.monthYear },
                { "day", item.day },
                { "text", item.text },
            };
        }

        json_t SerializeVehicle(const Vehicle* vehicle, uint16_t id)
        {
            if (vehicle == nullptr)
                return { { "id", id }, { "exists", false } };

            json_t occupants = json_t::array();
            for (const auto occupant : vehicle->peep)
                occupants.push_back(IdValue(occupant));
            json_t result{
                { "id", id },
                { "exists", true },
                { "ride", vehicle->ride.ToUnderlying() },
                { "subtype", static_cast<uint8_t>(vehicle->SubType) },
                { "vehicleType", vehicle->vehicle_type },
                { "trainLink", IdValue(vehicle->next_vehicle_on_train) },
                { "previousRideLink", IdValue(vehicle->prev_vehicle_on_ride) },
                { "nextRideLink", IdValue(vehicle->next_vehicle_on_ride) },
                { "status", static_cast<uint8_t>(vehicle->status) },
                { "seats", vehicle->num_seats },
                { "occupants", std::move(occupants) },
                { "occupantCount", vehicle->num_peeps },
                { "nextFreeSeat", vehicle->next_free_seat },
                { "colours",
                  {
                      { "body", static_cast<uint8_t>(vehicle->colours.Body) },
                      { "trim", static_cast<uint8_t>(vehicle->colours.Trim) },
                      { "tertiary", static_cast<uint8_t>(vehicle->colours.Tertiary) },
                  } },
                { "flags", vehicle->flags.holder },
                { "trackLocation",
                  {
                      { "x", vehicle->TrackLocation.x },
                      { "y", vehicle->TrackLocation.y },
                      { "z", vehicle->TrackLocation.z },
                  } },
                { "trackTypeAndDirection", vehicle->TrackTypeAndDirection },
                { "constructionStatus", static_cast<uint8_t>(vehicle->status) },
                { "testing", vehicle->flags.has(VehicleFlag::testing) },
                { "restraints", vehicle->restraints_position },
                { "currentStation", vehicle->current_station.ToUnderlying() },
                { "trackProgress", vehicle->track_progress },
                { "subState", vehicle->sub_state },
            };
            for (const auto field : { "ride", "subtype", "vehicleType", "trainLink", "previousRideLink", "nextRideLink", "status", "seats",
                                      "occupants", "occupantCount", "nextFreeSeat", "colours", "flags", "trackLocation",
                                      "trackTypeAndDirection", "constructionStatus", "testing", "restraints", "currentStation", "trackProgress", "subState" })
            {
                std::string key = "vehicle.";
                key += field;
                if (OmitSerializerField(key))
                    result.erase(field);
            }
            return result;
        }

        json_t SerializeGuest(const Guest* guest, uint16_t id)
        {
            if (guest == nullptr)
                return { { "id", id }, { "exists", false } };

            json_t thoughts = json_t::array();
            for (const auto& thought : guest->thoughts)
            {
                thoughts.push_back(
                    {
                        { "type", static_cast<uint8_t>(thought.type) },
                        { "itemOrRide", thought.item },
                        { "freshness", thought.freshness },
                        { "freshTimeout", thought.fresh_timeout },
                    });
            }
            json_t result{
                { "id", id },
                { "exists", true },
                { "currentRide", guest->CurrentRide.ToUnderlying() },
                { "currentStation", guest->CurrentRideStation.ToUnderlying() },
                { "currentTrain", guest->CurrentTrain },
                { "currentCar", guest->CurrentCar },
                { "currentSeat", guest->CurrentSeat },
                { "state", static_cast<uint8_t>(guest->State) },
                { "substate", guest->SubState },
                { "queuePredecessor", IdValue(guest->guestNextInQueue) },
                { "queueTime", guest->timeInQueue },
                { "rejoinQueueTimeout", guest->rejoinQueueTimeout },
                { "previousRideTimeout", guest->previousRideTimeOut },
                { "timeToStand", guest->TimeToStand },
                { "headingToRide", guest->guestHeadingToRideId.ToUnderlying() },
                { "favouriteRide", guest->favouriteRide.ToUnderlying() },
                { "previousRide", guest->previousRide.ToUnderlying() },
                { "voucherRide", guest->voucherRideId.ToUnderlying() },
                { "photoRides",
                  {
                      guest->photo1RideRef.ToUnderlying(),
                      guest->photo2RideRef.ToUnderlying(),
                      guest->photo3RideRef.ToUnderlying(),
                      guest->photo4RideRef.ToUnderlying(),
                  } },
                { "itemFlags", guest->itemFlags },
                { "peepFlags", guest->PeepFlags },
                { "thoughts", std::move(thoughts) },
                { "rideReferences", { { "current", guest->CurrentRide.ToUnderlying() },
                                       { "previous", guest->previousRide.ToUnderlying() },
                                       { "favourite", guest->favouriteRide.ToUnderlying() },
                                       { "voucher", guest->voucherRideId.ToUnderlying() } } },
            };
            for (const auto field : { "currentRide", "currentStation", "currentTrain", "currentCar", "currentSeat", "state", "substate",
                                      "queuePredecessor", "queueTime", "rejoinQueueTimeout", "previousRideTimeout", "timeToStand",
                                      "headingToRide", "favouriteRide", "previousRide", "voucherRide", "photoRides", "itemFlags", "peepFlags",
                                      "thoughts", "rideReferences" })
            {
                std::string key = "guest.";
                key += field;
                if (OmitSerializerField(key))
                    result.erase(field);
            }
            return result;
        }

        json_t SerializeTile(const TileCoordsXY& coords)
        {
            json_t elements = json_t::array();
            auto* element = MapGetFirstElementAt(coords);
            if (element != nullptr)
            {
                while (true)
                {
                    json_t bytes = json_t::array();
                    const auto* raw = reinterpret_cast<const uint8_t*>(element);
                    for (size_t i = 0; i < kTileElementSize; ++i)
                        bytes.push_back(raw[i]);
                    json_t record{
                        { "type", static_cast<uint8_t>(element->getType()) },
                        { "flags", element->flags },
                        { "baseHeight", element->baseHeight },
                        { "clearanceHeight", element->clearanceHeight },
                        { "owner", element->owner },
                        { "direction", static_cast<uint8_t>(element->getDirection()) },
                        { "ride", element->GetRideIndex().ToUnderlying() },
                        { "ghost", element->isGhost() },
                        { "bytes", std::move(bytes) },
                    };
                    for (const auto field : { "type", "flags", "baseHeight", "clearanceHeight", "owner", "direction", "ride", "ghost", "bytes" })
                    {
                        std::string key = "tile.";
                        key += field;
                        if (OmitSerializerField(key))
                            record.erase(field);
                    }
                    if (const auto* track = element->asTrack(); track != nullptr)
                    {
                        record["track"] = {
                            { "ride", track->GetRideIndex().ToUnderlying() },
                            { "rideType", track->GetRideType() },
                            { "station", track->GetStationIndex().ToUnderlying() },
                            { "trackType", static_cast<uint16_t>(track->GetTrackType()) },
                            { "sequence", track->GetSequenceIndex() },
                            { "colourScheme", track->GetColourScheme() },
                            { "invisible", track->isInvisible() },
                        };
                    }
                    else if (const auto* entrance = element->asEntrance(); entrance != nullptr)
                    {
                        record["entrance"] = {
                            { "entranceType", entrance->GetEntranceType() },
                            { "ride", entrance->GetRideIndex().ToUnderlying() },
                            { "station", entrance->GetStationIndex().ToUnderlying() },
                            { "direction", static_cast<uint8_t>(entrance->getDirection()) },
                            { "entryIndex", entrance->getEntryIndex() },
                            { "baseHeight", element->baseHeight },
                            { "clearanceHeight", element->clearanceHeight },
                            { "ghost", element->isGhost() },
                        };
                    }
                    else if (const auto* path = element->asPath(); path != nullptr)
                    {
                        record["path"] = {
                            { "isQueue", path->IsQueue() },
                            { "ride", path->GetRideIndex().ToUnderlying() },
                            { "station", path->GetStationIndex().ToUnderlying() },
                            { "edges", path->GetEdgesAndCorners() },
                            { "slope", static_cast<uint8_t>(path->GetSlopeDirection()) },
                            { "surface", path->GetSurfaceEntryIndex() },
                            { "railings", path->GetRailingsEntryIndex() },
                            { "baseHeight", element->baseHeight },
                            { "clearanceHeight", element->clearanceHeight },
                            { "ghost", element->isGhost() },
                        };
                    }
                    if (const auto* track = element->asTrack(); track != nullptr && record.contains("track"))
                    {
                        for (const auto* field : { "ride", "station", "trackType", "sequence", "colourScheme", "invisible" })
                        {
                            std::string key = "tile.track.";
                            key += field;
                            if (OmitSerializerField(key))
                                record["track"].erase(field);
                        }
                    }
                    if (const auto* entrance = element->asEntrance(); entrance != nullptr && record.contains("entrance"))
                    {
                        for (const auto* field : { "entranceType", "ride", "station", "direction", "baseHeight", "clearanceHeight",
                                                   "ghost" })
                        {
                            std::string key = "tile.entrance.";
                            key += field;
                            if (OmitSerializerField(key))
                                record["entrance"].erase(field);
                        }
                    }
                    if (const auto* path = element->asPath(); path != nullptr && record.contains("path"))
                    {
                        for (const auto* field : { "isQueue", "ride", "station", "edges", "slope", "surface", "railings", "ghost" })
                        {
                            std::string key = "tile.queue.";
                            key += field;
                            if (OmitSerializerField(key))
                                record["path"].erase(field);
                        }
                    }
                    elements.push_back(std::move(record));
                    if (element->isLastForTile())
                        break;
                    ++element;
                }
            }
            json_t result{ { "x", coords.x }, { "y", coords.y }, { "elements", std::move(elements) } };
            if (OmitSerializerField("tile.elements"))
                result.erase("elements");
            return result;
        }

        json_t SerializeRide(const Ride& ride, uint64_t samePriceThroughoutPark)
        {
            const auto endpoint = [](const TileCoordsXYZD& value) -> json_t {
                if (value.IsNull())
                    return nullptr;
                return { { "x", value.x }, { "y", value.y }, { "z", value.z }, { "direction", value.direction } };
            };
            json_t stations = json_t::array();
            for (uint8_t index = 0; index < Limits::kMaxStationsPerRide; ++index)
            {
                const auto& station = ride.getStation(StationIndex::FromUnderlying(index));
                stations.push_back({
                    { "index", index },
                    { "exists", index < ride.numStations },
                    { "start", { { "x", station.Start.x }, { "y", station.Start.y } } },
                    { "height", station.Height },
                    { "length", station.Length },
                    { "depart", station.Depart },
                    { "trainAtStation", station.TrainAtStation },
                    { "entrance", endpoint(station.Entrance) },
                    { "exit", endpoint(station.Exit) },
                    { "segmentLength", station.SegmentLength },
                    { "segmentTime", station.SegmentTime },
                    { "queueTime", station.QueueTime },
                    { "queueLength", station.QueueLength },
                    { "lastPeepInQueue", IdValue(station.LastPeepInQueue) },
                });
            }
            json_t vehicleIds = json_t::array();
            for (const auto id : ride.vehicles)
                vehicleIds.push_back(IdValue(id));
            json_t trackColours = json_t::array();
            for (const auto& colours : ride.trackColours)
                trackColours.push_back({ { "main", static_cast<uint8_t>(colours.main) },
                                         { "additional", static_cast<uint8_t>(colours.additional) },
                                         { "supports", static_cast<uint8_t>(colours.supports) } });
            json_t vehicleColours = json_t::array();
            for (const auto& colours : ride.vehicleColours)
                vehicleColours.push_back({ { "body", static_cast<uint8_t>(colours.Body) },
                                           { "trim", static_cast<uint8_t>(colours.Trim) },
                                           { "tertiary", static_cast<uint8_t>(colours.Tertiary) } });
            json_t downtime = json_t::array();
            for (const auto value : ride.downtimeHistory)
                downtime.push_back(value);
            json_t measurement = nullptr;
            if (ride.measurement != nullptr)
            {
                json_t vertical = json_t::array();
                json_t lateral = json_t::array();
                json_t velocity = json_t::array();
                json_t altitude = json_t::array();
                for (size_t i = 0; i < ride.measurement->num_items; ++i)
                {
                    vertical.push_back(ride.measurement->vertical[i]);
                    lateral.push_back(ride.measurement->lateral[i]);
                    velocity.push_back(ride.measurement->velocity[i]);
                    altitude.push_back(ride.measurement->altitude[i]);
                }
                measurement = { { "flags", ride.measurement->flags.holder },
                                { "lastUseTick", ride.measurement->last_use_tick },
                                { "numItems", ride.measurement->num_items },
                                { "currentItem", ride.measurement->current_item },
                                { "vehicleIndex", ride.measurement->vehicle_index },
                                { "currentStation", ride.measurement->current_station.ToUnderlying() },
                                { "vertical", std::move(vertical) }, { "lateral", std::move(lateral) },
                                { "velocity", std::move(velocity) }, { "altitude", std::move(altitude) } };
            }
            json_t result{
                { "id", ride.id.ToUnderlying() }, { "exists", !ride.id.IsNull() }, { "type", ride.type },
                { "subtype", ride.subtype }, { "status", static_cast<uint8_t>(ride.status) },
                { "customName", ride.customName }, { "defaultNameNumber", ride.defaultNameNumber },
                { "name", ride.getName() }, { "overallView", { { "x", ride.overallView.x }, { "y", ride.overallView.y } } },
                { "mode", static_cast<uint8_t>(ride.mode) }, { "departure", ride.departFlags },
                { "satisfaction", ride.satisfaction }, { "satisfactionTimeout", ride.satisfactionTimeout },
                { "satisfactionNext", ride.satisfactionNext }, { "popularity", ride.popularity },
                { "popularityTimeout", ride.popularityTimeout }, { "popularityNext", ride.popularityNext },
                { "upkeepCost", ride.upkeepCost }, { "unreliabilityFactor", ride.unreliabilityFactor },
                { "incomePerHour", ride.incomePerHour },
                { "minWaitingTime", ride.minWaitingTime }, { "maxWaitingTime", ride.maxWaitingTime },
                { "operation", ride.operationOption }, { "liftHillSpeed", ride.liftHillSpeed },
                { "numCircuits", ride.numCircuits }, { "music", ride.music }, { "musicEnabled", ride.flags.has(RideFlag::music) },
                { "musicTune", ride.musicTuneId }, { "musicPosition", ride.musicPosition },
                { "musicWindowInvalidateFlags", ride.windowInvalidateFlags.has(RideInvalidateFlag::music) },
                { "entranceStyle", ride.entranceStyle }, { "randomShopColours", ride.flags.has(RideFlag::randomShopColours) },
                { "vehicleColourSettings", static_cast<uint8_t>(ride.vehicleColourSettings) },
                { "trackColours", std::move(trackColours) }, { "vehicleColours", std::move(vehicleColours) },
                { "vehicles", std::move(vehicleIds) },
                { "numStations", ride.numStations }, { "stations", std::move(stations) },
                { "numTrains", ride.numTrains }, { "proposedNumTrains", ride.proposedNumTrains }, { "maxTrains", ride.maxTrains },
                { "numCarsPerTrain", ride.numCarsPerTrain }, { "proposedNumCarsPerTrain", ride.proposedNumCarsPerTrain },
                { "minCarsPerTrain", ride.minCarsPerTrain }, { "maxCarsPerTrain", ride.maxCarsPerTrain },
                { "vehicleChangeTimeout", ride.vehicleChangeTimeout }, { "reversedTrains", ride.flags.has(RideFlag::reversedTrains) },
                { "ratings", { { "excitement", static_cast<int32_t>(ride.ratings.excitement) },
                               { "intensity", static_cast<int32_t>(ride.ratings.intensity) },
                               { "nausea", static_cast<int32_t>(ride.ratings.nausea) } } },
                { "fixedRatings", ride.flags.has(RideFlag::fixedRatings) }, { "tested", ride.flags.has(RideFlag::tested) },
                { "testInProgress", ride.flags.has(RideFlag::testInProgress) }, { "testingFlags", ride.testingFlags.holder },
                { "currentTestSegment", ride.currentTestSegment }, { "currentTestStation", ride.currentTestStation.ToUnderlying() },
                { "measurement", std::move(measurement) }, { "inspectionInterval", static_cast<uint8_t>(ride.inspectionInterval) },
                { "inspectionStation", ride.inspectionStation.ToUnderlying() }, { "dueInspection", ride.flags.has(RideFlag::dueInspection) },
                { "buildDate", ride.buildDate }, { "reliability", ride.reliability }, { "reliabilitySubvalue", ride.reliabilitySubvalue },
                { "reliabilityPercentage", ride.reliabilityPercentage }, { "breakdownPending", ride.flags.has(RideFlag::breakdownPending) },
                { "breakdownReasonPending", static_cast<uint8_t>(ride.breakdownReasonPending) },
                { "breakdownReason", static_cast<uint8_t>(ride.breakdownReason) }, { "lastCrashType", ride.lastCrashType },
                { "downtime", ride.downtime }, { "downtimeHistory", std::move(downtime) },
                { "cableLift", ride.flags.has(RideFlag::cableLift) }, { "cableLiftEntity", IdValue(ride.cableLift) },
                { "cableLiftLoc", { { "x", ride.cableLiftLoc.x }, { "y", ride.cableLiftLoc.y }, { "z", ride.cableLiftLoc.z } } },
                { "raceWinner", IdValue(ride.raceWinner) }, { "passStationNoStopping", ride.flags.has(RideFlag::passStationNoStopping) },
                { "crashed", ride.flags.has(RideFlag::crashed) }, { "broken", ride.flags.has(RideFlag::brokenDown) },
                { "currentIssues", ride.currentIssues }, { "lastIssueTime", ride.lastIssueTime },
                { "windowInvalidateFlags", ride.windowInvalidateFlags.holder }, { "flags", ride.flags.holder },
                { "price", ride.price[0] }, { "price0", ride.price[0] }, { "price1", ride.price[1] }, { "value", ride.value },
                { "samePriceThroughoutPark", samePriceThroughoutPark }, { "numRiders", ride.numRiders }, { "totalCustomers", ride.totalCustomers },
                { "totalProfit", ride.totalProfit }, { "profit", ride.profit },
                { "everBeenOpened", ride.flags.has(RideFlag::everBeenOpened) },
                { "curNumCustomers", ride.curNumCustomers }, { "numCustomersTimeout", ride.numCustomersTimeout },
            };
            if (result.contains("stations"))
            {
                for (const auto field : { "index", "exists", "start", "height", "length", "depart", "trainAtStation", "entrance",
                                          "exit", "segmentLength", "segmentTime", "queueTime", "queueLength", "lastPeepInQueue" })
                {
                    std::string key = "ride.station.";
                    key += field;
                    if (OmitSerializerField(key))
                        for (auto& station : result["stations"])
                            station.erase(field);
                }
            }
            if (OmitSerializerField("ride."))
                return json_t{ { "id", ride.id.ToUnderlying() }, { "exists", true } };
            static constexpr const char* omissionFields[]{
                "id", "exists", "type", "subtype", "status", "customName", "defaultNameNumber", "name", "overallView", "mode",
                "departure", "minWaitingTime", "maxWaitingTime", "operation", "liftHillSpeed", "numCircuits", "music", "musicEnabled",
                "musicTune", "musicPosition", "musicWindowInvalidateFlags", "entranceStyle", "randomShopColours", "vehicleColourSettings",
                "trackColours", "vehicleColours", "vehicles", "numStations", "stations", "numTrains", "proposedNumTrains", "maxTrains",
                "numCarsPerTrain", "proposedNumCarsPerTrain", "minCarsPerTrain", "maxCarsPerTrain", "vehicleChangeTimeout",
                "reversedTrains", "ratings", "fixedRatings", "tested", "testInProgress", "testingFlags", "currentTestSegment",
                "currentTestStation", "measurement", "inspectionInterval", "inspectionStation", "dueInspection", "buildDate", "reliability",
                "reliabilitySubvalue", "reliabilityPercentage", "breakdownPending", "breakdownReasonPending", "breakdownReason", "lastCrashType",
                "downtime", "downtimeHistory", "cableLift", "cableLiftEntity", "cableLiftLoc", "raceWinner", "passStationNoStopping", "crashed",
                "broken", "currentIssues", "lastIssueTime", "windowInvalidateFlags", "flags", "price", "price0", "price1", "value",
                "samePriceThroughoutPark", "numRiders", "totalCustomers", "totalProfit", "profit", "satisfaction", "popularity", "upkeepCost",
                "unreliabilityFactor", "incomePerHour", "everBeenOpened", "curNumCustomers", "numCustomersTimeout",
            };
            for (const auto* field : omissionFields)
            {
                std::string key = "ride.";
                key += field;
                if (OmitSerializerField(key))
                    result.erase(field);
            }
            if (OmitSerializerField("ride.station.QueueTime") && result.contains("stations"))
                result["stations"][0].erase("queueTime");
            if (OmitSerializerField("ride.measurement.samples") && result.contains("measurement"))
            {
                result["measurement"].erase("vertical");
                result["measurement"].erase("lateral");
                result["measurement"].erase("velocity");
                result["measurement"].erase("altitude");
            }
            if (OmitSerializerField("ride.music.invalidation"))
                result.erase("musicWindowInvalidateFlags");
            return result;
        }

        bool TileBelongsToRide(const TileCoordsXY& coords, int32_t rideValue)
        {
            auto* element = MapGetFirstElementAt(coords);
            if (element == nullptr)
                return false;
            while (true)
            {
                if (const auto* track = element->asTrack();
                    track != nullptr && track->GetRideIndex().ToUnderlying() == rideValue)
                    return true;
                if (const auto* entrance = element->asEntrance();
                    entrance != nullptr && entrance->GetRideIndex().ToUnderlying() == rideValue)
                    return true;
                if (const auto* path = element->asPath(); path != nullptr && path->GetRideIndex().ToUnderlying() == rideValue)
                    return true;
                if (element->isLastForTile())
                    break;
                ++element;
            }
            return false;
        }
    } // namespace

    RideProjectionWatchSet CaptureRideProjectionWatchSet(const GameState_t& state, const json_t& args)
    {
        RideProjectionWatchSet watch;
        if (gFixtureHandles.has_value())
        {
            watch.fixtureHandles = gFixtureHandles;
            const auto& handles = *gFixtureHandles;
            watch.rideIds.insert(handles.ride.ToUnderlying());
            watch.guestIds.insert(handles.linkedGuest.ToUnderlying());
            watch.guestIds.insert(handles.queueGuest.ToUnderlying());
            watch.vehicleIds.insert(handles.vehicleHead.ToUnderlying());
            watch.vehicleIds.insert(handles.vehicleTail.ToUnderlying());
            watch.bannerIds.push_back(handles.banner.ToUnderlying());
            watch.campaignKeys.emplace_back(handles.campaign.type, handles.campaign.ride.ToUnderlying());
            watch.recentNewsIndices.push_back(handles.recentNews.slot);
            watch.archivedNewsIndices.push_back(handles.archivedNews.slot);
            for (const auto& tile : handles.tiles)
                watch.tileCoords.push_back(tile.coords);
        }
        const auto rideValue = args.value("ride", -1);
        for (const auto& ride : state.rides)
        {
            if (!ride.id.IsNull())
                watch.rideIds.insert(ride.id.ToUnderlying());
        }
        if (rideValue >= 0 && rideValue < Limits::kMaxRidesInPark)
            watch.rideIds.insert(static_cast<uint16_t>(rideValue));

        for (const auto id : const_cast<GameState_t&>(state).entities.GetEntityList(EntityType::vehicle))
            watch.vehicleIds.insert(id.ToUnderlying());
        for (const auto id : const_cast<GameState_t&>(state).entities.GetEntityList(EntityType::guest))
            watch.guestIds.insert(id.ToUnderlying());

        // Keep the complete reciprocal graph, including vehicles that only
        // point back into the target train/ride after construction clearing.
        bool expanded = true;
        while (expanded)
        {
            expanded = false;
            for (const auto rawId : std::vector<uint16_t>(watch.vehicleIds.begin(), watch.vehicleIds.end()))
            {
                const auto* vehicle = const_cast<GameState_t&>(state).entities.GetEntity<Vehicle>(
                    EntityId::FromUnderlying(rawId));
                if (vehicle == nullptr)
                    continue;
                for (const auto linked :
                     { vehicle->next_vehicle_on_train, vehicle->prev_vehicle_on_ride, vehicle->next_vehicle_on_ride })
                {
                    if (!linked.IsNull() && watch.vehicleIds.insert(linked.ToUnderlying()).second)
                        expanded = true;
                }
                for (const auto occupant : vehicle->peep)
                {
                    if (!occupant.IsNull() && watch.guestIds.insert(occupant.ToUnderlying()).second)
                        expanded = true;
                }
            }
        }

        // News records have no stable engine id. Their queue slot is the
        // identity, so capture every slot before execution and serialize that
        // same slot (including a now-empty slot) afterwards.
        if (!gFixtureHandles.has_value())
        {
            for (size_t index = 0; index < News::ItemHistoryStart; ++index)
                watch.recentNewsIndices.push_back(index);
            for (size_t index = 0; index < News::MaxItemsArchive; ++index)
                watch.archivedNewsIndices.push_back(index);
        }
        if (!gFixtureHandles.has_value())
        {
            for (const auto& campaign : state.park.marketingCampaigns)
                watch.campaignKeys.emplace_back(campaign.type, campaign.rideId.ToUnderlying());
            for (const auto& banner : state.banners)
            {
                if (!banner.isNull())
                    watch.bannerIds.push_back(banner.id.ToUnderlying());
            }
        }

        for (int32_t x = 0; x < state.mapSize.x; ++x)
        {
            for (int32_t y = 0; y < state.mapSize.y; ++y)
            {
                const TileCoordsXY coords{ x, y };
                if (rideValue >= 0 && TileBelongsToRide(coords, rideValue))
                    watch.tileCoords.push_back(coords);
            }
        }
        if (rideValue >= 0 && rideValue < Limits::kMaxRidesInPark)
        {
            const auto* ride = GetRide(RideId::FromUnderlying(rideValue));
            if (ride != nullptr)
            {
                const auto addFixed = [&watch](const TileCoordsXYZD& endpoint) {
                    if (!endpoint.IsNull())
                    {
                        const auto endpointCoords = endpoint.ToCoordsXY();
                        const TileCoordsXY coords{ endpointCoords.x / kCoordsXYStep, endpointCoords.y / kCoordsXYStep };
                        if (std::find(watch.tileCoords.begin(), watch.tileCoords.end(), coords) == watch.tileCoords.end())
                            watch.tileCoords.push_back(coords);
                    }
                };
                for (const auto& station : ride->getStations())
                {
                    addFixed(station.Entrance);
                    addFixed(station.Exit);
                }
            }
        }
        if (gFixtureHandles.has_value())
        {
            for (const auto& tile : gFixtureHandles->tiles)
            {
                if (std::find(watch.tileCoords.begin(), watch.tileCoords.end(), tile.coords) == watch.tileCoords.end())
                    watch.tileCoords.push_back(tile.coords);
            }
        }
        return watch;
    }

    json_t SerializeRideProjection(const GameState_t& state, const json_t& args, const RideProjectionWatchSet& watch)
    {
        json_t recent = json_t::array();
        for (const auto index : watch.recentNewsIndices)
        {
            const auto& item = state.newsItems[index];
            recent.push_back({ { "slot", index }, { "item", SerializeNewsItem(item) } });
        }
        json_t archived = json_t::array();
        for (const auto index : watch.archivedNewsIndices)
        {
            const auto& item = state.newsItems[News::ItemHistoryStart + index];
            archived.push_back({ { "slot", index }, { "item", SerializeNewsItem(item) } });
        }

        json_t vehicles = json_t::array();
        for (const auto id : watch.vehicleIds)
            vehicles.push_back(SerializeVehicle(
                const_cast<GameState_t&>(state).entities.GetEntity<Vehicle>(EntityId::FromUnderlying(id)), id));
        json_t guests = json_t::array();
        for (const auto id : watch.guestIds)
            guests.push_back(
                SerializeGuest(const_cast<GameState_t&>(state).entities.GetEntity<Guest>(EntityId::FromUnderlying(id)), id));

        json_t history = json_t::array();
        for (const auto id : watch.guestIds)
        {
            const auto* rides = RideUse::GetHistory().GetAll(EntityId::FromUnderlying(id));
            json_t guestHistory = json_t::array();
            if (rides != nullptr)
            {
                for (const auto ride : *rides)
                    guestHistory.push_back(ride.ToUnderlying());
            }
            history.push_back({ { "guest", id }, { "rides", std::move(guestHistory) } });
        }

        json_t tiles = json_t::array();
        for (const auto& coords : watch.tileCoords)
        {
            // The fixed coordinate watch set, rather than a post-action scan,
            // owns which complete ordered tile records are serialized.
            tiles.push_back(SerializeTile(coords));
        }

        json_t banners = json_t::array();
        for (const auto id : watch.bannerIds)
        {
            const auto* banner = GetBanner(BannerIndex::FromUnderlying(id));
            if (banner == nullptr || banner->isNull())
            {
                banners.push_back({ { "id", id }, { "exists", false } });
                continue;
            }
            banners.push_back({
                { "id", id },
                { "exists", true },
                { "type", banner->type },
                { "flags", banner->flags.holder },
                { "linkedToRide", banner->flags.has(BannerFlag::linkedToRide) },
                { "assoc", banner->rideIndex.ToUnderlying() },
                { "text", banner->text },
                { "colour", static_cast<uint8_t>(banner->colour) },
                { "textColour", static_cast<uint8_t>(banner->textColour) },
                { "position", { { "x", banner->position.x }, { "y", banner->position.y } } },
            });
        }

        json_t rides = json_t::array();
        for (const auto id : watch.rideIds)
        {
            if (id >= state.rides.size() || state.rides[id].id.IsNull())
            {
                rides.push_back({ { "id", id }, { "exists", false } });
                continue;
            }
            const auto& ride = state.rides[id];
            rides.push_back(SerializeRide(ride, state.park.samePriceThroughoutPark));
        }

        json_t campaigns = json_t::array();
        for (const auto [type, rideId] : watch.campaignKeys)
        {
            const auto found = std::find_if(
                state.park.marketingCampaigns.begin(), state.park.marketingCampaigns.end(),
                [type, rideId](const auto& campaign) {
                    return campaign.type == type && campaign.rideId.ToUnderlying() == rideId;
                });
            if (found == state.park.marketingCampaigns.end())
            {
                campaigns.push_back({ { "type", type }, { "ride", rideId }, { "exists", false } });
                continue;
            }
            campaigns.push_back({
                { "type", found->type },
                { "weeksLeft", found->weeksLeft },
                { "flags", found->flags.holder },
                { "firstWeek", found->flags.has(MarketingCampaignFlag::firstWeek) },
                { "ride", found->rideId.ToUnderlying() },
                { "exists", true },
            });
        }

        const auto& park = state.park;
        json_t expenditure = json_t::array();
        for (size_t month = 0; month < kExpenditureTableMonthCount; ++month)
        {
            json_t cells = json_t::array();
            for (size_t type = 0; type < EnumValue(ExpenditureType::count); ++type)
                cells.push_back(park.expenditureTable[month][type]);
            expenditure.push_back(std::move(cells));
        }
        json_t valueHistory = json_t::array();
        for (const auto value : park.valueHistory)
            valueHistory.push_back(value);
        json_t finance = {
            { "cash", park.cash }, { "bankLoan", park.bankLoan }, { "maxBankLoan", park.maxBankLoan },
            { "loanInterestRate", park.bankLoanInterestRate }, { "historicalProfit", park.historicalProfit },
            { "currentProfit", park.currentProfit }, { "currentExpenditure", park.currentExpenditure },
            { "companyValue", park.companyValue }, { "expenditureTable", std::move(expenditure) },
            { "valueHistory", std::move(valueHistory) },
        };
        if (OmitSerializerField("finance.expenditureTable[0][rideConstruction]"))
            finance["expenditureTable"][0].erase(static_cast<size_t>(ExpenditureType::rideConstruction));
        if (OmitSerializerField("finance.valueHistory[7]"))
            finance["valueHistory"].erase(7);
        json_t watchJson = json_t::object();
        watchJson["rides"] = watch.rideIds;
        watchJson["vehicles"] = watch.vehicleIds;
        watchJson["guests"] = watch.guestIds;
        watchJson["recentNewsSlots"] = watch.recentNewsIndices;
        watchJson["archivedNewsSlots"] = watch.archivedNewsIndices;
        watchJson["banners"] = watch.bannerIds;
        watchJson["campaignKeys"] = json_t::array();
        for (const auto [type, rideId] : watch.campaignKeys)
            watchJson["campaignKeys"].push_back({ { "type", type }, { "ride", rideId } });
        watchJson["tiles"] = json_t::array();
        for (const auto& coords : watch.tileCoords)
            watchJson["tiles"].push_back({ { "x", coords.x }, { "y", coords.y } });
        watchJson["tileRoles"] = json_t::array();
        if (watch.fixtureHandles.has_value())
        {
            const auto& handles = *watch.fixtureHandles;
            watchJson["fixtureIds"] = {
                { "ride", handles.ride.ToUnderlying() }, { "linkedGuest", handles.linkedGuest.ToUnderlying() },
                { "queueGuest", handles.queueGuest.ToUnderlying() }, { "vehicleHead", handles.vehicleHead.ToUnderlying() },
                { "vehicleTail", handles.vehicleTail.ToUnderlying() }, { "banner", handles.banner.ToUnderlying() },
                { "campaignType", handles.campaign.type }, { "campaignRide", handles.campaign.ride.ToUnderlying() },
            };
        }
        if (watch.fixtureHandles.has_value())
        {
            for (const auto& tile : watch.fixtureHandles->tiles)
                watchJson["tileRoles"].push_back({ { "role", static_cast<uint8_t>(tile.role) },
                                                     { "x", tile.coords.x }, { "y", tile.coords.y } });
        }
        return {
            { "watch", std::move(watchJson) },
            { "rides", std::move(rides) },
            { "vehicles", std::move(vehicles) },
            { "guests", std::move(guests) },
            { "rideUseHistory", std::move(history) },
            { "news", { { "recent", std::move(recent) }, { "archived", std::move(archived) } } },
            { "banners", std::move(banners) },
            { "campaigns", std::move(campaigns) },
            { "tiles", std::move(tiles) },
            { "finance", finance },
            { "parkValue", state.park.value },
            { "selectedRide", args.value("ride", -1) },
            { "selectedIsExit", args.contains("isExit") ? json_t(args.at("isExit")) : json_t(nullptr) },
        };
    }

    json_t SerializeRideProjection(const GameState_t& state, const json_t& args)
    {
        if (gActiveWatchSet.has_value())
            return SerializeRideProjection(state, args, *gActiveWatchSet);
        return SerializeRideProjection(state, args, CaptureRideProjectionWatchSet(state, args));
    }

    bool ValidateRideProjectionStores(const json_t& projection, std::string* failure)
    {
        const auto reject = [failure](std::string message) {
            if (failure != nullptr)
                *failure = std::move(message);
            return false;
        };
        static constexpr std::array<const char*, 11> stores{
            "rides", "vehicles", "guests", "rideUseHistory", "news", "banners", "campaigns", "tiles", "finance",
            "parkValue", "watch",
        };
        for (const auto* store : stores)
        {
            if (!projection.contains(store) || projection.at(store).is_null())
                return reject(std::string("projection store is omitted: ") + store);
        }
        static constexpr std::array<const char*, 7> populatedArrays{
            "rides", "vehicles", "guests", "rideUseHistory", "banners", "campaigns", "tiles",
        };
        for (const auto* store : populatedArrays)
        {
            if (!projection.at(store).is_array() || projection.at(store).empty())
                return reject(std::string("projection store is empty: ") + store);
        }
        if (projection.contains("authoritative") || projection.contains("activePeepLinks")
            || projection.contains("demolitionOwnedNews"))
            return reject("projection contains a fake or relabeled authoritative store");
        if (!projection.at("news").is_object() || !projection["news"].contains("recent")
            || !projection["news"].contains("archived"))
            return reject("news projection does not preserve both fixed queues");
        if (!projection.at("watch").is_object() || !projection["watch"].contains("tiles")
            || !projection["watch"]["tiles"].is_array())
            return reject("watch projection does not preserve fixed tile identities");
        if (projection["watch"]["tiles"].empty())
            return reject("watch projection has no fixed tile identities");
        if (gFixtureHandles.has_value())
        {
            const auto& handles = *gFixtureHandles;
            const auto findById = [](const json_t& records, uint16_t id) -> const json_t* {
                for (const auto& record : records)
                {
                    if (record.value("id", std::numeric_limits<uint16_t>::max()) == id)
                        return &record;
                }
                return nullptr;
            };
            const auto* ride = findById(projection["rides"], handles.ride.ToUnderlying());
            if (ride == nullptr || !ride->value("exists", false))
                return reject("target ride identity is missing from projection");
            static constexpr const char* requiredRideFields[]{
                "id", "exists", "type", "subtype", "status", "customName", "defaultNameNumber", "name", "overallView",
                "mode", "departure", "minWaitingTime", "maxWaitingTime", "operation", "liftHillSpeed", "numCircuits", "music",
                "musicEnabled", "musicTune", "musicPosition", "musicWindowInvalidateFlags", "entranceStyle", "randomShopColours",
                "vehicleColourSettings", "trackColours", "vehicleColours", "vehicles", "numStations", "stations", "numTrains",
                "proposedNumTrains", "maxTrains", "numCarsPerTrain", "proposedNumCarsPerTrain", "minCarsPerTrain", "maxCarsPerTrain",
                "vehicleChangeTimeout", "reversedTrains", "ratings", "fixedRatings", "tested", "testInProgress", "testingFlags",
                "currentTestSegment", "currentTestStation", "measurement", "inspectionInterval", "inspectionStation", "dueInspection",
                "buildDate", "reliability", "reliabilitySubvalue", "reliabilityPercentage", "breakdownPending", "breakdownReasonPending",
                "breakdownReason", "lastCrashType", "downtime", "downtimeHistory", "cableLift", "cableLiftEntity", "cableLiftLoc",
                "raceWinner", "passStationNoStopping", "crashed", "broken", "currentIssues", "lastIssueTime", "windowInvalidateFlags",
                "flags", "price0", "price1", "value", "samePriceThroughoutPark", "numRiders", "totalCustomers", "totalProfit", "profit",
                "satisfaction", "popularity", "upkeepCost", "unreliabilityFactor", "incomePerHour", "everBeenOpened",
            };
            for (const auto key : requiredRideFields)
            {
                if (!ride->contains(key))
                    return reject(std::string("ride field is omitted: ") + key);
            }
            if (!ride->at("stations").is_array() || ride->at("stations").size() != Limits::kMaxStationsPerRide)
                return reject("ride stations are not the complete stable slot set");
            static constexpr const char* stationFields[]{
                "index", "exists", "start", "height", "length", "depart", "trainAtStation", "entrance", "exit", "segmentLength",
                "segmentTime", "queueTime", "queueLength", "lastPeepInQueue",
            };
            for (const auto& station : ride->at("stations"))
                for (const auto field : stationFields)
                    if (!station.contains(field))
                        return reject(std::string("station field is omitted: ") + field);
            const auto& station0 = ride->at("stations")[0];
            const bool queueBefore = station0.at("queueLength") == 37 && station0.at("queueTime") == 37
                && station0.at("lastPeepInQueue") == handles.queueGuest.ToUnderlying();
            const bool queueAfter = station0.at("queueLength") == 0 && station0.at("queueTime") == 37
                && station0.at("lastPeepInQueue") == std::numeric_limits<uint16_t>::max();
            if (ride->at("satisfaction") != 17 || ride->at("popularity") != 18 || ride->at("upkeepCost") != 19
                || ride->at("unreliabilityFactor") != 20 || ride->at("incomePerHour") != 21
                || ride->at("musicWindowInvalidateFlags") != true || (!queueBefore && !queueAfter))
                return reject("ride exact configuration/value contract failed");
            if (projection["selectedIsExit"].is_null())
            {
                if (!ride->contains("measurement"))
                    return reject("ride field is omitted: measurement");
                if (!ride->at("measurement").is_object())
                    return reject("ride measurement record is missing");
                for (const auto field : { "vertical", "lateral", "velocity", "altitude" })
                    if (!ride->at("measurement").contains(field))
                        return reject(std::string("measurement sample field is omitted: ") + field);
                if (ride->at("measurement")["vertical"] != json_t({ 1, -2 })
                    || ride->at("measurement")["lateral"] != json_t({ 3, -4 })
                    || ride->at("measurement")["velocity"] != json_t({ 5, 6 })
                    || ride->at("measurement")["altitude"] != json_t({ 7, 8 }))
                    return reject("ride measurement sample content contract failed: " + ride->at("measurement").dump());
            }
            const auto* guest = findById(projection["guests"], handles.linkedGuest.ToUnderlying());
            const auto* queueGuest = findById(projection["guests"], handles.queueGuest.ToUnderlying());
            if (guest == nullptr || !guest->value("exists", false) || queueGuest == nullptr
                || !queueGuest->value("exists", false))
                return reject("linked or queue guest identity is missing");
            for (const auto field : { "currentRide", "currentStation", "currentTrain", "currentCar", "currentSeat", "state", "substate",
                                      "queuePredecessor", "queueTime", "rejoinQueueTimeout", "previousRideTimeout", "timeToStand",
                                      "headingToRide", "favouriteRide", "previousRide", "voucherRide", "photoRides", "itemFlags", "peepFlags",
                                      "thoughts" })
                if (!guest->contains(field))
                    return reject(std::string("guest field is omitted: ") + field);
            const auto& photoRides = guest->at("photoRides");
            const auto expectedItems = (uint64_t{ 1 } << static_cast<uint8_t>(ShopItem::voucher))
                | (uint64_t{ 1 } << static_cast<uint8_t>(ShopItem::photo))
                | (uint64_t{ 1 } << static_cast<uint8_t>(ShopItem::photo2))
                | (uint64_t{ 1 } << static_cast<uint8_t>(ShopItem::photo3))
                | (uint64_t{ 1 } << static_cast<uint8_t>(ShopItem::photo4));
            if (guest->value("currentRide", std::numeric_limits<uint16_t>::max()) != handles.ride.ToUnderlying()
                || guest->value("currentStation", 255) != 0 || guest->value("currentTrain", 255) != 0
                || guest->value("currentCar", 255) != 73 || guest->value("currentSeat", 255) != 0
                || guest->value("queuePredecessor", std::numeric_limits<uint16_t>::max()) != handles.queueGuest.ToUnderlying()
                || guest->value("state", 0) != static_cast<uint8_t>(PeepState::watching) || guest->value("substate", 0) != 3
                || guest->value("queueTime", 0) != 37 || guest->value("rejoinQueueTimeout", 0) != 4
                || guest->value("previousRideTimeout", 0) != 8 || guest->value("timeToStand", 0) != 73
                || guest->value("headingToRide", std::numeric_limits<uint16_t>::max()) != handles.ride.ToUnderlying()
                || guest->value("favouriteRide", std::numeric_limits<uint16_t>::max()) != handles.ride.ToUnderlying()
                || guest->value("previousRide", std::numeric_limits<uint16_t>::max()) != handles.ride.ToUnderlying()
                || guest->value("voucherRide", std::numeric_limits<uint16_t>::max()) != handles.ride.ToUnderlying()
                || photoRides.size() != 4 || photoRides[0] != handles.ride.ToUnderlying() || photoRides[1] != handles.ride.ToUnderlying()
                || photoRides[2] != handles.ride.ToUnderlying() || photoRides[3] != handles.ride.ToUnderlying()
                || guest->value("peepFlags", 0u) != static_cast<uint32_t>(PEEP_FLAGS_LEAVING_PARK)
                || guest->value("itemFlags", uint64_t{ 0 }) != expectedItems
                || !guest->contains("thoughts") || guest->at("thoughts").empty()
                || guest->at("thoughts")[0].value("type", 255) != static_cast<uint8_t>(PeepThoughtType::wasGreat)
                || guest->at("thoughts")[0].value("itemOrRide", std::numeric_limits<uint16_t>::max())
                    != handles.ride.ToUnderlying()
                || guest->at("thoughts")[0].value("freshness", 0) != 1
                || guest->at("thoughts")[0].value("freshTimeout", 0) != 2)
                return reject("linked guest exact identity/value contract failed: " + guest->dump());
            if (queueGuest->value("currentRide", std::numeric_limits<uint16_t>::max()) != handles.ride.ToUnderlying()
                || queueGuest->value("currentStation", 255) != 0 || queueGuest->value("state", 0) != static_cast<uint8_t>(PeepState::queuing)
                || queueGuest->value("substate", 0) != 2)
                return reject("queue guest identity/value contract failed");
            const auto* head = findById(projection["vehicles"], handles.vehicleHead.ToUnderlying());
            const auto* tail = findById(projection["vehicles"], handles.vehicleTail.ToUnderlying());
            if (head == nullptr || tail == nullptr || !head->value("exists", false) || !tail->value("exists", false))
                return reject("reciprocal vehicle identity is missing");
            for (const auto field : { "ride", "subtype", "trainLink", "previousRideLink", "nextRideLink", "status", "seats", "occupants",
                                      "occupantCount", "nextFreeSeat", "colours", "flags", "trackLocation", "trackTypeAndDirection",
                                      "constructionStatus", "testing", "restraints", "currentStation" })
                if (!head->contains(field) || !tail->contains(field))
                    return reject(std::string("vehicle field is omitted: ") + field);
            if (ride->at("vehicles").size() < 2 || ride->at("vehicles")[0] != handles.vehicleHead.ToUnderlying()
                || ride->at("vehicles")[1] != std::numeric_limits<uint16_t>::max() || ride->at("numTrains") != 1
                || ride->at("numCarsPerTrain") != 2)
                return reject("ride train allocation identity/value contract failed");
            if (head->value("ride", std::numeric_limits<uint16_t>::max()) != handles.ride.ToUnderlying()
                || tail->value("ride", std::numeric_limits<uint16_t>::max()) != handles.ride.ToUnderlying()
                || head->value("subtype", 255) != static_cast<uint8_t>(Vehicle::Type::head)
                || tail->value("subtype", 255) != static_cast<uint8_t>(Vehicle::Type::tail)
                || head->value("status", 255) != static_cast<uint8_t>(Vehicle::Status::waitingForPassengers)
                || tail->value("status", 255) != static_cast<uint8_t>(Vehicle::Status::travelling)
                || head->value("trainLink", std::numeric_limits<uint16_t>::max()) != handles.vehicleTail.ToUnderlying()
                || tail->value("trainLink", std::numeric_limits<uint16_t>::max()) != std::numeric_limits<uint16_t>::max()
                || head->value("previousRideLink", 0) != std::numeric_limits<uint16_t>::max()
                || head->value("nextRideLink", std::numeric_limits<uint16_t>::max()) != handles.vehicleTail.ToUnderlying()
                || tail->value("previousRideLink", std::numeric_limits<uint16_t>::max()) != handles.vehicleHead.ToUnderlying()
                || tail->value("nextRideLink", 0) != std::numeric_limits<uint16_t>::max()
                || head->value("seats", 0) != 2 || tail->value("seats", 0) != 2
                || head->value("occupantCount", 0) != 1 || tail->value("occupantCount", 0) != 0
                || head->value("nextFreeSeat", 0) != 1 || tail->value("nextFreeSeat", 255) != 0
                || head->at("occupants")[0] != handles.linkedGuest.ToUnderlying()
                || tail->at("occupants")[0] != std::numeric_limits<uint16_t>::max()
                || !head->value("testing", false) || !tail->value("testing", false)
                || head->value("restraints", 0) != 17 || tail->value("restraints", 0) != 29
                || head->at("trackLocation").value("z", 0) != 12 || tail->at("trackLocation").value("z", 0) != 16
                || head->value("trackTypeAndDirection", 0) != 0x1234
                || tail->value("trackTypeAndDirection", 0) != 0x2345
                || head->at("colours") != json_t({ { "body", static_cast<uint8_t>(Drawing::Colour::brightRed) },
                                                     { "trim", static_cast<uint8_t>(Drawing::Colour::darkBlue) },
                                                     { "tertiary", static_cast<uint8_t>(Drawing::Colour::brightGreen) } })
                || tail->at("colours") != json_t({ { "body", static_cast<uint8_t>(Drawing::Colour::yellow) },
                                                     { "trim", static_cast<uint8_t>(Drawing::Colour::brightPurple) },
                                                     { "tertiary", static_cast<uint8_t>(Drawing::Colour::lightOrange) } }))
                return reject("reciprocal vehicle identity/value contract failed");
            bool bannerFound = false;
            for (const auto& banner : projection["banners"])
            {
                if (banner.value("id", std::numeric_limits<uint16_t>::max()) == handles.banner.ToUnderlying())
                {
                    bannerFound = banner.value("exists", false) && banner.value("type", 0) == 1
                        && banner.value("assoc", std::numeric_limits<uint16_t>::max()) == handles.ride.ToUnderlying()
                        && banner.value("text", "") == "S2 target banner" && banner.value("linkedToRide", false)
                        && banner.value("colour", 0) == static_cast<uint8_t>(Drawing::Colour::brightRed)
                        && banner.value("textColour", 0) == static_cast<uint8_t>(Drawing::TextColour::white)
                        && banner.value("position", json_t::object()) == json_t({ { "x", 1 }, { "y", 1 } });
                }
            }
            if (!bannerFound)
                return reject("linked banner exact identity/value contract failed");
            bool campaignFound = false;
            for (const auto& campaign : projection["campaigns"])
            {
                if (campaign.value("ride", std::numeric_limits<uint16_t>::max()) == handles.ride.ToUnderlying()
                    && campaign.value("type", 0) == ADVERTISING_CAMPAIGN_RIDE)
                    campaignFound = campaign.value("weeksLeft", 0) == 3 && campaign.value("firstWeek", false);
            }
            if (!campaignFound)
                return reject("ride campaign exact identity/value contract failed");
            for (const auto field : { "cash", "bankLoan", "maxBankLoan", "loanInterestRate", "historicalProfit", "currentProfit",
                                      "currentExpenditure", "companyValue", "expenditureTable", "valueHistory" })
                if (!projection["finance"].contains(field))
                    return reject(std::string("finance field is omitted: ") + field);
            if (projection["finance"]["cash"] != 100000 || projection["finance"]["bankLoan"] != 20000
                || projection["finance"]["maxBankLoan"] != 50000 || projection["finance"]["loanInterestRate"] != 7
                || projection["finance"]["historicalProfit"] != 4321 || projection["finance"]["currentProfit"] != 2345
                || projection["finance"]["currentExpenditure"] != 456 || projection["finance"]["companyValue"] != 34567
                || projection["finance"]["expenditureTable"].size() != kExpenditureTableMonthCount
                || projection["finance"]["valueHistory"].size() != kFinanceHistorySize
                || projection["finance"]["expenditureTable"][0].size() != EnumValue(ExpenditureType::count)
                || projection["finance"]["expenditureTable"][3].size() != EnumValue(ExpenditureType::count)
                || !std::all_of(projection["finance"]["expenditureTable"].begin(), projection["finance"]["expenditureTable"].end(),
                                 [](const auto& row) { return row.is_array() && row.size() == EnumValue(ExpenditureType::count); })
                || !std::all_of(projection["finance"]["valueHistory"].begin(), projection["finance"]["valueHistory"].end(),
                                 [](const auto& value) { return value.is_number(); })
                || projection["finance"]["expenditureTable"][0][static_cast<size_t>(ExpenditureType::rideConstruction)] != 77
                || projection["finance"]["expenditureTable"][3][static_cast<size_t>(ExpenditureType::rideRunningCosts)] != 88
                || projection["finance"]["valueHistory"][0] != 12000 || projection["finance"]["valueHistory"][7] != 11900)
                return reject("complete finance identity/value contract failed");
            if (!projection["watch"].contains("tileRoles") || projection["watch"]["tileRoles"].size() != handles.tiles.size())
                return reject("four fixed tile role identities are missing");
            std::set<std::pair<int32_t, int32_t>> roleCoords;
            for (const auto& tile : handles.tiles)
            {
                if (!roleCoords.emplace(tile.coords.x, tile.coords.y).second || tile.coords.x < 0 || tile.coords.y < 0)
                    return reject("tile role identity is aliased or outside the fixed map");
            }
            for (const auto& tile : handles.tiles)
            {
                bool found = false;
                for (const auto& watched : projection["watch"]["tileRoles"])
                {
                    if (watched.value("role", 255) == static_cast<uint8_t>(tile.role)
                        && watched.value("x", -1) == tile.coords.x && watched.value("y", -1) == tile.coords.y)
                        found = true;
                }
                if (!found)
                    return reject("fixed tile role identity is missing");
                const json_t* tileRecord = nullptr;
                for (const auto& record : projection["tiles"])
                {
                    if (record.value("x", -1) == tile.coords.x && record.value("y", -1) == tile.coords.y)
                        tileRecord = &record;
                }
                if (tileRecord == nullptr || !tileRecord->contains("elements"))
                    return reject("fixed tile record is missing");
                bool typedRoleFound = false;
                for (const auto& element : (*tileRecord)["elements"])
                {
                    for (const auto field : { "type", "flags", "baseHeight", "clearanceHeight", "owner", "direction", "ride", "ghost", "bytes" })
                        if (!element.contains(field))
                            return reject(std::string("tile field is omitted: ") + field);
                    if (tile.role == TileRole::track && element.contains("track")
                        && element["track"].value("ride", std::numeric_limits<uint16_t>::max()) == handles.ride.ToUnderlying())
                    {
                        typedRoleFound = true;
                        for (const auto field : { "ride", "rideType", "station", "trackType", "sequence", "colourScheme", "invisible" })
                            if (!element["track"].contains(field))
                                return reject(std::string("track typed field is omitted: ") + field);
                        if (element["track"].value("station", 255) != 0 || element["track"].value("invisible", true))
                            return reject("track typed identity/value contract failed");
                    }
                    if ((tile.role == TileRole::entrance || tile.role == TileRole::exit) && element.contains("entrance")
                        && element["entrance"].value("ride", std::numeric_limits<uint16_t>::max()) == handles.ride.ToUnderlying()
                        && element["entrance"].value("entranceType", 255)
                            == (tile.role == TileRole::exit ? ENTRANCE_TYPE_RIDE_EXIT : ENTRANCE_TYPE_RIDE_ENTRANCE))
                    {
                        typedRoleFound = true;
                        for (const auto field : { "entranceType", "ride", "station", "direction", "baseHeight", "clearanceHeight", "ghost" })
                            if (!element["entrance"].contains(field))
                                return reject(std::string("entrance typed field is omitted: ") + field);
                        if (element["entrance"].value("station", 255) != 0 || element["entrance"].value("ghost", true))
                            return reject("entrance typed identity/value contract failed");
                    }
                    if (tile.role == TileRole::queue && element.contains("path") && element["path"].value("isQueue", false)
                        && element["path"].value("ride", std::numeric_limits<uint16_t>::max()) == handles.ride.ToUnderlying()
                        && element["path"].value("station", 255) == 0)
                    {
                        typedRoleFound = true;
                        for (const auto field : { "isQueue", "ride", "station", "edges", "slope", "surface", "railings", "baseHeight",
                                                   "clearanceHeight", "ghost" })
                            if (!element["path"].contains(field))
                                return reject(std::string("queue typed field is omitted: ") + field);
                        if (element["path"].value("ghost", true))
                            return reject("queue typed identity/value contract failed");
                    }
                }
                const auto endpointKey = tile.role == TileRole::entrance ? "entrance" : "exit";
                const bool endpointRemoved = (tile.role == TileRole::entrance || tile.role == TileRole::exit)
                    && ride->at("stations")[0].at(endpointKey).is_null();
                if (endpointRemoved && !typedRoleFound)
                    continue;
                if (!typedRoleFound)
                    return reject("fixed tile typed role value is missing for role "
                                  + std::to_string(static_cast<uint8_t>(tile.role)) + ": " + tileRecord->dump());
                if (endpointRemoved)
                    return reject("removed endpoint has a typed element in its explicit pre-state");
            }
        }
        for (const auto& item : projection["news"]["recent"])
        {
            if (!item.contains("slot") || !item.contains("item") || !item["item"].contains("type")
                || !item["item"].contains("flags") || !item["item"].contains("assoc") || !item["item"].contains("ticks")
                || !item["item"].contains("monthYear") || !item["item"].contains("day") || !item["item"].contains("text"))
                return reject("recent news record is not authoritative");
        }
        for (const auto& item : projection["news"]["archived"])
        {
            if (!item.contains("slot") || !item.contains("item") || !item["item"].contains("type")
                || !item["item"].contains("flags") || !item["item"].contains("assoc") || !item["item"].contains("ticks")
                || !item["item"].contains("monthYear") || !item["item"].contains("day") || !item["item"].contains("text"))
                return reject("archived news record is not authoritative");
        }
        if (gFixtureHandles.has_value())
        {
            const auto exactNews = [&projection](const char* queue, uint16_t slot, const char* text, int ticks, int monthYear,
                                                  int day) {
                for (const auto& item : projection["news"][queue])
                {
                    if (item.value("slot", std::numeric_limits<uint16_t>::max()) == slot)
                    {
                        const auto& value = item["item"];
                        return value.value("type", 255) == static_cast<uint8_t>(News::ItemType::ride)
                            && value.value("assoc", std::numeric_limits<uint16_t>::max())
                                == gFixtureHandles->ride.ToUnderlying()
                            && value.value("flags", 255) == 0 && value.value("ticks", -1) == ticks
                            && value.value("monthYear", -1) == monthYear && value.value("day", -1) == day
                            && value.value("text", "") == text;
                    }
                }
                return false;
            };
            if (!exactNews("recent", 0, "S2 recent target news", 7, 12, 3)
                || !exactNews("archived", 0, "S2 archived target news", 9, 11, 2))
                return reject("news identity/value contract failed");
            bool historyFound = false;
            for (const auto& history : projection["rideUseHistory"])
            {
                if (history.value("guest", std::numeric_limits<uint16_t>::max()) == gFixtureHandles->linkedGuest.ToUnderlying())
                {
                    historyFound = history.contains("rides")
                        && history.at("rides") == json_t({ gFixtureHandles->ride.ToUnderlying() });
                }
            }
            if (!historyFound)
                return reject("guest RideUse history exact ordered contract failed");
        }
        return true;
    }
} // namespace OpenRCT2::Testing
