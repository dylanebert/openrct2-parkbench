/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "RideSetVehicleAction.h"

#include "../../Cheats.h"
#include "../../Context.h"
#include "../../Diagnostic.h"
#include "../../GameState.h"
#include "../../core/EnumUtils.hpp"
#include "../../drawing/Drawing.h"
#include "../../localisation/StringIds.h"
#include "../../management/Research.h"
#include "../../object/ObjectManager.h"
#include "../../ride/Ride.h"
#include "../../ride/RideData.h"
#include "../../windows/Intent.h"
#include "../../world/Map.h"

#include <limits>

namespace OpenRCT2::GameActions
{
    constexpr static StringId kSetVehicleTypeErrorTitle[] = {
        STR_RIDE_SET_VEHICLE_SET_NUM_TRAINS_FAIL,
        STR_RIDE_SET_VEHICLE_SET_NUM_CARS_PER_TRAIN_FAIL,
        STR_RIDE_SET_VEHICLE_TYPE_FAIL,
        STR_RIDE_SET_VEHICLE_REVERSED_FAIL,
    };

    RideSetVehicleAction::RideSetVehicleAction(RideId rideIndex, RideSetVehicleType type, uint16_t value, uint8_t colour)
        : _rideIndex(rideIndex)
        , _type(type)
        , _value(value)
        , _colour(colour)
    {
    }

    void RideSetVehicleAction::AcceptParameters(GameActionParameterVisitor& visitor)
    {
        visitor.Visit("ride", _rideIndex);
        visitor.Visit("type", _type);
        visitor.Visit("value", _value);
        visitor.Visit("colour", _colour);
    }

    uint16_t RideSetVehicleAction::GetActionFlags() const
    {
        return GameAction::GetActionFlags() | Flags::AllowWhilePaused;
    }

    void RideSetVehicleAction::Serialise(DataSerialiser& stream)
    {
        GameAction::Serialise(stream);
        stream << DS_TAG(_rideIndex) << DS_TAG(_type) << DS_TAG(_value) << DS_TAG(_colour);
    }

    Result RideSetVehicleAction::Query(GameState_t& gameState, Park::ParkData& park) const
    {
        if (_type >= RideSetVehicleType::count)
        {
            LOG_ERROR("Invalid ride vehicle type %d", _type);
            return Result(Status::invalidParameters, STR_RIDE_SET_VEHICLE_TYPE_FAIL, STR_ERR_VALUE_OUT_OF_RANGE);
        }
        auto errTitle = kSetVehicleTypeErrorTitle[EnumValue(_type)];

        auto ride = GetRide(_rideIndex);
        if (ride == nullptr)
        {
            LOG_ERROR("Ride not found for rideIndex %u", _rideIndex.ToUnderlying());
            return Result(Status::invalidParameters, errTitle, STR_ERR_RIDE_NOT_FOUND);
        }

        if (ride->flags.has(RideFlag::brokenDown))
        {
            return Result(Status::broken, errTitle, STR_HAS_BROKEN_DOWN_AND_REQUIRES_FIXING);
        }

        if (ride->status != RideStatus::closed && ride->status != RideStatus::simulating)
        {
            return Result(Status::notClosed, errTitle, STR_MUST_BE_CLOSED_FIRST);
        }

        // Execute clears construction state before applying the requested value;
        // prove the current ride entry before any type-specific or cheat-owned
        // branch can reach that clearing path.
        const auto* currentRideEntry = GetRideEntryByIndex(ride->subtype);
        if (currentRideEntry == nullptr)
        {
            return Result(Status::invalidParameters, errTitle, STR_ERR_VALUE_OUT_OF_RANGE);
        }

        switch (_type)
        {
            case RideSetVehicleType::numTrains:
                if (_value == 0 || _value > ride->maxTrains)
                {
                    return Result(Status::invalidParameters, errTitle, STR_ERR_VALUE_OUT_OF_RANGE);
                }
                break;
            case RideSetVehicleType::numCarsPerTrain:
            {
                // The UI deliberately exposes the extended train-length path
                // while this cheat is enabled. Keep descriptor bounds for
                // ordinary play, but retain the actual byte/storage and engine
                // safety bounds when the cheat owns the wider domain.
                const bool outsideStorageBounds = _value == 0 || _value > std::numeric_limits<uint8_t>::max();
                const bool outsideDescriptorBounds = _value < currentRideEntry->min_cars_in_train
                    || _value > currentRideEntry->max_cars_in_train;
                if (outsideStorageBounds || (!gameState.cheats.disableTrainLengthLimit && outsideDescriptorBounds))
                {
                    return Result(Status::invalidParameters, errTitle, STR_ERR_VALUE_OUT_OF_RANGE);
                }
                break;
            }
            case RideSetVehicleType::trainsReversed:
                if (_value > 1)
                {
                    return Result(Status::invalidParameters, errTitle, STR_ERR_VALUE_OUT_OF_RANGE);
                }
                break;
            case RideSetVehicleType::rideEntry:
            {
                if (!RideIsVehicleTypeValid(gameState, *ride))
                {
                    LOG_ERROR("Invalid vehicle type %d", _value);
                    return Result(Status::invalidParameters, errTitle, STR_ERR_VALUE_OUT_OF_RANGE);
                }
                auto rideEntry = GetRideEntryByIndex(_value);
                if (rideEntry == nullptr)
                {
                    LOG_ERROR("Ride entry not found for _value %d", _value);
                    return Result(Status::invalidParameters, errTitle, kStringIdNone);
                }

                // Validate preset
                VehicleColourPresetList* presetList = rideEntry->vehicle_preset_list;
                if (_colour >= presetList->count && _colour != 255 && _colour != 0)
                {
                    LOG_ERROR("Unknown vehicle colour preset. colour = %d", _colour);
                    return Result(Status::invalidParameters, errTitle, STR_ERR_INVALID_COLOUR);
                }
                break;
            }

            default:
                LOG_ERROR("Invalid ride vehicle setting %d", _type);
                return Result(Status::invalidParameters, errTitle, STR_ERR_VALUE_OUT_OF_RANGE);
        }

        return Result();
    }

    Result RideSetVehicleAction::Execute(GameState_t& gameState, Park::ParkData& park) const
    {
        auto errTitle = kSetVehicleTypeErrorTitle[EnumValue(_type)];
        auto ride = GetRide(_rideIndex);
        if (ride == nullptr)
        {
            LOG_ERROR("Ride not found for rideIndex %u", _rideIndex.ToUnderlying());
            return Result(Status::invalidParameters, errTitle, STR_ERR_RIDE_NOT_FOUND);
        }

        // Execute can be called directly by engine owners as well as through
        // the synchronous dispatcher. Reject a stale/missing current entry
        // before any branch clears vehicles, guests, seats, or queue links.
        if (GetRideEntryByIndex(ride->subtype) == nullptr)
        {
            LOG_ERROR("Ride entry not found for index %d", ride->subtype);
            return Result(Status::invalidParameters, errTitle, STR_ERR_VALUE_OUT_OF_RANGE);
        }

        switch (_type)
        {
            case RideSetVehicleType::numTrains:
                RideClearForConstruction(*ride);
                ride->removePeeps();
                ride->vehicleChangeTimeout = 100;

                if (ride->numCircuits > 1)
                {
                    InvalidateTestResults(*ride);
                }
                ride->proposedNumTrains = _value;
                break;
            case RideSetVehicleType::numCarsPerTrain:
            {
                RideClearForConstruction(*ride);
                ride->removePeeps();
                ride->vehicleChangeTimeout = 100;

                InvalidateTestResults(*ride);
                auto rideEntry = GetRideEntryByIndex(ride->subtype);
                if (rideEntry == nullptr)
                {
                    LOG_ERROR("Ride entry not found for index %d", ride->subtype);
                    return Result(Status::invalidParameters, errTitle, kStringIdNone);
                }
                uint8_t clampValue = _value;
                static_assert(sizeof(clampValue) == sizeof(ride->proposedNumCarsPerTrain));
                if (!gameState.cheats.disableTrainLengthLimit)
                {
                    clampValue = std::clamp(clampValue, rideEntry->min_cars_in_train, rideEntry->max_cars_in_train);
                }
                ride->proposedNumCarsPerTrain = clampValue;
                break;
            }
            case RideSetVehicleType::rideEntry:
            {
                RideClearForConstruction(*ride);
                ride->removePeeps();
                ride->vehicleChangeTimeout = 100;

                InvalidateTestResults(*ride);
                ride->subtype = _value;
                auto rideEntry = GetRideEntryByIndex(ride->subtype);
                if (rideEntry == nullptr)
                {
                    LOG_ERROR("Ride entry not found for index %d", ride->subtype);
                    return Result(Status::invalidParameters, errTitle, kStringIdNone);
                }

                RideSetVehicleColoursToRandomPreset(*ride, _colour);
                if (!gameState.cheats.disableTrainLengthLimit)
                {
                    ride->proposedNumCarsPerTrain = std::clamp(
                        ride->proposedNumCarsPerTrain, rideEntry->min_cars_in_train, rideEntry->max_cars_in_train);
                }
                break;
            }
            case RideSetVehicleType::trainsReversed:
            {
                RideClearForConstruction(*ride);
                ride->removePeeps();
                ride->vehicleChangeTimeout = 100;

                ride->flags.set(RideFlag::reversedTrains, static_cast<bool>(_value));
                break;
            }

            default:
                LOG_ERROR("Invalid ride vehicle setting %d", _type);
                return Result(Status::invalidParameters, errTitle, kStringIdNone);
        }

        ride->updateMaxVehicles();
        if (ride->numTrains > 1)
        {
            ride->numCircuits = 1;
        }

        auto res = Result();
        if (!ride->overallView.IsNull())
        {
            auto location = ride->overallView.ToTileCentre();
            res.position = { location, TileElementHeight(res.position) };
        }

        auto intent = Intent(INTENT_ACTION_RIDE_PAINT_RESET_VEHICLE);
        intent.PutExtra(INTENT_EXTRA_RIDE_ID, _rideIndex.ToUnderlying());
        ContextBroadcastIntent(&intent);

        GfxInvalidateScreen();
        return res;
    }

    bool RideSetVehicleAction::RideIsVehicleTypeValid(GameState_t& gameState, const Ride& ride) const
    {
        bool selectionShouldBeExpanded;
        ride_type_t rideTypeIterator, rideTypeIteratorMax;

        {
            const auto& rtd = ride.getRideTypeDescriptor();
            if (gameState.cheats.showVehiclesFromOtherTrackTypes
                && !(
                    ride.getRideTypeDescriptor().flags.has(RtdFlag::isFlatRide) || rtd.specialType == RtdSpecialType::maze
                    || rtd.specialType == RtdSpecialType::miniGolf))
            {
                selectionShouldBeExpanded = true;
                rideTypeIterator = 0;
                rideTypeIteratorMax = RIDE_TYPE_COUNT - 1;
            }
            else
            {
                selectionShouldBeExpanded = false;
                rideTypeIterator = ride.type;
                rideTypeIteratorMax = ride.type;
            }
        }

        for (; rideTypeIterator <= rideTypeIteratorMax; rideTypeIterator++)
        {
            if (selectionShouldBeExpanded)
            {
                if (GetRideTypeDescriptor(rideTypeIterator).flags.has(RtdFlag::isFlatRide))
                    continue;

                const auto& rtd = GetRideTypeDescriptor(rideTypeIterator);
                if (rtd.specialType == RtdSpecialType::maze || rtd.specialType == RtdSpecialType::miniGolf)
                    continue;
            }

            auto& objManager = GetContext()->GetObjectManager();
            auto& rideEntries = objManager.GetAllRideEntries(rideTypeIterator);
            for (auto rideEntryIndex : rideEntries)
            {
                if (rideEntryIndex == _value)
                {
                    if (!RideEntryIsInvented(rideEntryIndex) && !gameState.cheats.ignoreResearchStatus)
                    {
                        return false;
                    }

                    return true;
                }
            }
        }

        return false;
    }
} // namespace OpenRCT2::GameActions
