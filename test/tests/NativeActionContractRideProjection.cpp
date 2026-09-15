#include "NativeActionContractRideProjection.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/management/NewsItem.h>
#include <openrct2/peep/RideUseSystem.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/world/Banner.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/world/Map.h>
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
                thoughts.push_back(
                    {
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
                    elements.push_back(
                        {
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
            json_t stations = json_t::array();
            for (uint8_t index = 0; index < ride.numStations; ++index)
            {
                const auto& station = ride.getStation(StationIndex::FromUnderlying(index));
                const auto endpoint = [](const TileCoordsXYZD& value) -> json_t {
                    if (value.IsNull())
                        return nullptr;
                    return { { "x", value.x }, { "y", value.y }, { "z", value.z }, { "direction", value.direction } };
                };
                stations.push_back({
                    { "index", index },
                    { "start", { { "x", station.Start.x }, { "y", station.Start.y } } },
                    { "height", station.Height },
                    { "length", station.Length },
                    { "entrance", endpoint(station.Entrance) },
                    { "exit", endpoint(station.Exit) },
                });
            }
            json_t vehicleIds = json_t::array();
            for (const auto id : ride.vehicles)
                vehicleIds.push_back(IdValue(id));
            return {
                { "id", ride.id.ToUnderlying() },
                { "type", ride.type },
                { "subtype", ride.subtype },
                { "mode", static_cast<uint8_t>(ride.mode) },
                { "departure", ride.departFlags },
                { "minWaitingTime", ride.minWaitingTime },
                { "maxWaitingTime", ride.maxWaitingTime },
                { "operation", ride.operationOption },
                { "music", ride.music },
                { "liftHillSpeed", ride.liftHillSpeed },
                { "numCircuits", ride.numCircuits },
                { "entranceStyle", ride.entranceStyle },
                { "vehicleColourSettings", static_cast<uint8_t>(ride.vehicleColourSettings) },
                { "randomShopColours", ride.flags.has(RideFlag::randomShopColours) },
                { "reversedTrains", ride.flags.has(RideFlag::reversedTrains) },
                { "proposedNumTrains", ride.proposedNumTrains },
                { "proposedNumCarsPerTrain", ride.proposedNumCarsPerTrain },
                { "vehicleIds", std::move(vehicleIds) },
                { "stations", std::move(stations) },
                { "name", ride.getName() },
                { "status", static_cast<uint8_t>(ride.status) },
                { "stations", ride.numStations },
                { "trains", ride.numTrains },
                { "carsPerTrain", ride.numCarsPerTrain },
                { "price", ride.price[0] },
                { "price0", ride.price[0] },
                { "price1", ride.price[1] },
                { "value", ride.value },
                { "ratings",
                  {
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
        for (size_t index = 0; index < News::ItemHistoryStart; ++index)
            watch.recentNewsIndices.push_back(index);
        for (size_t index = 0; index < News::MaxItemsArchive; ++index)
            watch.archivedNewsIndices.push_back(index);
        for (const auto& campaign : state.park.marketingCampaigns)
            watch.campaignKeys.emplace_back(campaign.type, campaign.rideId.ToUnderlying());
        for (const auto& banner : state.banners)
        {
            if (!banner.isNull())
                watch.bannerIds.push_back(banner.id.ToUnderlying());
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
            auto rideValue = SerializeRide(ride);
            rideValue["price0"] = ride.price[0];
            rideValue["price1"] = ride.price[1];
            rideValue["trackColours"] = json_t::array();
            for (const auto& colours : ride.trackColours)
                rideValue["trackColours"].push_back(
                    {
                        { "main", static_cast<uint8_t>(colours.main) },
                        { "additional", static_cast<uint8_t>(colours.additional) },
                        { "supports", static_cast<uint8_t>(colours.supports) },
                    });
            rideValue["vehicleColours"] = json_t::array();
            for (const auto& colours : ride.vehicleColours)
                rideValue["vehicleColours"].push_back(
                    {
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
                { "ride", found->rideId.ToUnderlying() },
                { "exists", true },
            });
        }

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
        return true;
    }
} // namespace OpenRCT2::Testing
