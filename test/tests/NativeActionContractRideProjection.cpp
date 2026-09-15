#include "NativeActionContractRideProjection.h"

#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/management/NewsItem.h>
#include <openrct2/peep/RideUseSystem.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/TileElement.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <openrct2/world/tile_element/EntranceElement.h>
#include <openrct2/world/tile_element/PathElement.h>

#include <algorithm>
#include <limits>
#include <optional>

namespace OpenRCT2::Testing
{
    namespace
    {
        std::optional<RideProjectionWatchSet> gActiveWatchSet;
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
            return {
                { "id", id },
                { "exists", true },
                { "ride", vehicle->ride.ToUnderlying() },
                { "subtype", static_cast<uint8_t>(vehicle->SubType) },
                { "trainLink", IdValue(vehicle->next_vehicle_on_train) },
                { "previousRideLink", IdValue(vehicle->prev_vehicle_on_ride) },
                { "nextRideLink", IdValue(vehicle->next_vehicle_on_ride) },
                { "status", static_cast<uint8_t>(vehicle->status) },
                { "seats", vehicle->num_seats },
                { "occupants", std::move(occupants) },
                { "occupantCount", vehicle->num_peeps },
                { "nextFreeSeat", vehicle->next_free_seat },
                { "colours", {
                      { "body", static_cast<uint8_t>(vehicle->colours.Body) },
                      { "trim", static_cast<uint8_t>(vehicle->colours.Trim) },
                      { "tertiary", static_cast<uint8_t>(vehicle->colours.Tertiary) },
                  } },
                { "flags", vehicle->flags.holder },
                { "trackLocation", {
                      { "x", vehicle->TrackLocation.x },
                      { "y", vehicle->TrackLocation.y },
                      { "z", vehicle->TrackLocation.z },
                  } },
                { "trackTypeAndDirection", vehicle->TrackTypeAndDirection },
                { "constructionStatus", static_cast<uint8_t>(vehicle->status) },
                { "restraints", vehicle->restraints_position },
                { "currentStation", vehicle->current_station.ToUnderlying() },
            };
        }

        json_t SerializeGuest(const Guest* guest, uint16_t id)
        {
            if (guest == nullptr)
                return { { "id", id }, { "exists", false } };

            json_t thoughts = json_t::array();
            for (const auto& thought : guest->thoughts)
            {
                thoughts.push_back({
                    { "type", static_cast<uint8_t>(thought.type) },
                    { "itemOrRide", thought.item },
                    { "freshness", thought.freshness },
                    { "freshTimeout", thought.fresh_timeout },
                });
            }
            return {
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
                { "timeToStand", guest->TimeToStand },
                { "headingToRide", guest->guestHeadingToRideId.ToUnderlying() },
                { "favouriteRide", guest->favouriteRide.ToUnderlying() },
                { "previousRide", guest->previousRide.ToUnderlying() },
                { "voucherRide", guest->voucherRideId.ToUnderlying() },
                { "photoRides", {
                      guest->photo1RideRef.ToUnderlying(), guest->photo2RideRef.ToUnderlying(),
                      guest->photo3RideRef.ToUnderlying(), guest->photo4RideRef.ToUnderlying(),
                  } },
                { "itemFlags", guest->itemFlags },
                { "peepFlags", guest->PeepFlags },
                { "thoughts", std::move(thoughts) },
            };
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
                    elements.push_back({
                        { "type", static_cast<uint8_t>(element->getType()) },
                        { "flags", element->flags },
                        { "baseHeight", element->baseHeight },
                        { "clearanceHeight", element->clearanceHeight },
                        { "owner", element->owner },
                        { "direction", static_cast<uint8_t>(element->getDirection()) },
                        { "ride", element->GetRideIndex().ToUnderlying() },
                        { "bytes", std::move(bytes) },
                    });
                    if (element->isLastForTile())
                        break;
                    ++element;
                }
            }
            return { { "x", coords.x }, { "y", coords.y }, { "elements", std::move(elements) } };
        }

        json_t SerializeRide(const Ride& ride)
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
                { "price0", ride.price[0] },
                { "price1", ride.price[1] },
                { "value", ride.value },
                { "ratings", {
                      { "excitement", static_cast<int32_t>(ride.ratings.excitement) },
                      { "intensity", static_cast<int32_t>(ride.ratings.intensity) },
                      { "nausea", static_cast<int32_t>(ride.ratings.nausea) },
                  } },
                { "numRiders", ride.numRiders },
                { "totalCustomers", ride.totalCustomers },
                { "totalProfit", ride.totalProfit },
                { "profit", ride.profit },
                { "flags", ride.flags.holder },
                { "everBeenOpened", ride.flags.has(RideFlag::everBeenOpened) },
                { "lastCrashType", ride.lastCrashType },
                { "reliability", ride.reliability },
                { "reliabilityPercentage", ride.reliabilityPercentage },
                { "breakdownReason", static_cast<uint8_t>(ride.breakdownReason) },
                { "inspectionStation", ride.inspectionStation.ToUnderlying() },
            };
        }

        bool TileBelongsToRide(const TileCoordsXY& coords, int32_t rideValue)
        {
            auto* element = MapGetFirstElementAt(coords);
            if (element == nullptr)
                return false;
            while (true)
            {
                if (const auto* track = element->asTrack(); track != nullptr
                    && track->GetRideIndex().ToUnderlying() == rideValue)
                    return true;
                if (const auto* entrance = element->asEntrance(); entrance != nullptr
                    && entrance->GetRideIndex().ToUnderlying() == rideValue)
                    return true;
                if (const auto* path = element->asPath(); path != nullptr
                    && path->GetRideIndex().ToUnderlying() == rideValue)
                    return true;
                if (element->isLastForTile())
                    break;
                ++element;
            }
            return false;
        }
    }

    RideProjectionWatchSet CaptureRideProjectionWatchSet(const GameState_t& state, const json_t& args)
    {
        RideProjectionWatchSet watch;
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
                const auto* vehicle = const_cast<GameState_t&>(state).entities.GetEntity<Vehicle>(EntityId::FromUnderlying(rawId));
                if (vehicle == nullptr)
                    continue;
                for (const auto linked : { vehicle->next_vehicle_on_train, vehicle->prev_vehicle_on_ride,
                                           vehicle->next_vehicle_on_ride })
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

        for (int32_t x = 0; x < state.mapSize.x; ++x)
        {
            for (int32_t y = 0; y < state.mapSize.y; ++y)
            {
                const TileCoordsXY coords{ x, y };
                if (rideValue >= 0 && TileBelongsToRide(coords, rideValue))
                    watch.tileCoords.push_back(coords);
            }
        }
        return watch;
    }

    json_t SerializeRideProjection(const GameState_t& state, const json_t& args, const RideProjectionWatchSet& watch)
    {
        json_t recent = json_t::array();
        for (const auto& item : state.newsItems.getRecent())
            recent.push_back(SerializeNewsItem(item));
        json_t archived = json_t::array();
        for (const auto& item : state.newsItems.getArchived())
            archived.push_back(SerializeNewsItem(item));

        json_t vehicles = json_t::array();
        for (const auto id : watch.vehicleIds)
            vehicles.push_back(SerializeVehicle(const_cast<GameState_t&>(state).entities.GetEntity<Vehicle>(EntityId::FromUnderlying(id)), id));
        json_t guests = json_t::array();
        for (const auto id : watch.guestIds)
            guests.push_back(SerializeGuest(const_cast<GameState_t&>(state).entities.GetEntity<Guest>(EntityId::FromUnderlying(id)), id));

        json_t history = json_t::array();
        for (const auto id : watch.guestIds)
        {
            const auto* rides = RideUse::GetHistory().GetAll(EntityId::FromUnderlying(id));
            if (rides == nullptr)
                continue;
            for (const auto ride : *rides)
                history.push_back({ { "guest", id }, { "ride", ride.ToUnderlying() } });
        }

        json_t tiles = json_t::array();
        for (const auto& coords : watch.tileCoords)
        {
            // The fixed coordinate watch set, rather than a post-action scan,
            // owns which complete ordered tile records are serialized.
            tiles.push_back(SerializeTile(coords));
        }

        json_t banners = json_t::array();
        for (const auto& banner : state.banners)
        {
            if (!banner.isNull())
                banners.push_back({
                    { "id", banner.id.ToUnderlying() }, { "type", banner.type }, { "flags", banner.flags.holder },
                    { "assoc", banner.rideIndex.ToUnderlying() }, { "text", banner.text },
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
            auto rideValue = SerializeRide(ride);
            rideValue["price0"] = ride.price[0];
            rideValue["price1"] = ride.price[1];
            rideValue["trackColours"] = json_t::array();
            for (const auto& colours : ride.trackColours)
                rideValue["trackColours"].push_back({
                    { "main", static_cast<uint8_t>(colours.main) },
                    { "additional", static_cast<uint8_t>(colours.additional) },
                    { "supports", static_cast<uint8_t>(colours.supports) },
                });
            rideValue["vehicleColours"] = json_t::array();
            for (const auto& colours : ride.vehicleColours)
                rideValue["vehicleColours"].push_back({
                    { "body", static_cast<uint8_t>(colours.Body) },
                    { "trim", static_cast<uint8_t>(colours.Trim) },
                    { "tertiary", static_cast<uint8_t>(colours.Tertiary) },
                });
            rideValue["everBeenOpened"] = ride.flags.has(RideFlag::everBeenOpened);
            rideValue["lastCrashType"] = ride.lastCrashType;
            rideValue["reliability"] = ride.reliability;
            rideValue["reliabilityPercentage"] = ride.reliabilityPercentage;
            rideValue["breakdownReason"] = static_cast<uint8_t>(ride.breakdownReason);
            rideValue["inspectionStation"] = ride.inspectionStation.ToUnderlying();
            rides.push_back(std::move(rideValue));
        }

        json_t campaigns = json_t::array();
        for (const auto& campaign : state.park.marketingCampaigns)
            campaigns.push_back({
                { "type", campaign.type }, { "weeksLeft", campaign.weeksLeft }, { "flags", campaign.flags.holder },
                { "ride", campaign.rideId.ToUnderlying() },
            });

        const auto& park = state.park;
        const json_t finance = {
            { "cash", park.cash },
            { "bankLoan", park.bankLoan },
            { "maxBankLoan", park.maxBankLoan },
            { "loanInterestRate", park.bankLoanInterestRate },
            { "currentProfit", park.currentProfit },
            { "currentExpenditure", park.currentExpenditure },
            { "companyValue", park.companyValue },
        };
        return {
            { "watch", {
                  { "rides", watch.rideIds }, { "vehicles", watch.vehicleIds }, { "guests", watch.guestIds },
                  { "tiles", watch.tileCoords.size() },
              } },
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
        };
    }

    json_t SerializeRideProjection(const GameState_t& state, const json_t& args)
    {
        if (gActiveWatchSet.has_value())
            return SerializeRideProjection(state, args, *gActiveWatchSet);
        return SerializeRideProjection(state, args, CaptureRideProjectionWatchSet(state, args));
    }
}
