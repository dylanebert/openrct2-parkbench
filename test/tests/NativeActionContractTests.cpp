/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TestData.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/command_line/NativeRegistry.h>
#include <openrct2/object/Object.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/ride/RideColour.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/ted/TrackElemType.h>
#include <string>
#include <vector>

using namespace OpenRCT2;
using namespace OpenRCT2::CommandLine;
using json_t = nlohmann::json;

namespace
{
    class NativeActionContract : public testing::Test
    {
    protected:
        std::unique_ptr<IContext> _context;

        void LoadPark()
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
            auto loadResult = importer->LoadSavedGame(TestData::GetParkPath("small_park_with_ferris_wheel.sv6").c_str(), false);
            _context->GetObjectManager().LoadObjects(loadResult.RequiredObjects);
            importer->Import(OpenRCT2::getGameState());
        }

        void LoadEnterprisePark()
        {
            LoadPark();
            auto& objectManager = GetContext()->GetObjectManager();
            std::vector<ObjectEntryDescriptor> unload;
            if (auto* object = objectManager.GetLoadedObject(ObjectType::ride, 10); object != nullptr)
                unload.push_back(object->GetDescriptor());
            const auto enterpriseSlot = objectManager.GetLoadedObjectEntryIndex("rct2.ride.enterp");
            if (enterpriseSlot != kObjectEntryIndexNull && enterpriseSlot != 10)
            {
                if (auto* object = objectManager.GetLoadedObject(ObjectType::ride, enterpriseSlot); object != nullptr)
                    unload.push_back(object->GetDescriptor());
            }
            if (!unload.empty())
                objectManager.UnloadObjects(unload);
            ASSERT_NE(objectManager.LoadObject(ObjectEntryDescriptor("rct2.ride.enterp"), 10), nullptr);
            ASSERT_NE(GetRideEntryByIndex(static_cast<ObjectEntryIndex>(10)), nullptr);
            ASSERT_EQ(
                GetRideEntryByIndex(static_cast<ObjectEntryIndex>(10))->GetFirstNonNullRideType(), static_cast<ride_type_t>(81));
        }
    };

    json_t RideCreateArgs(
        ride_type_t rideType = static_cast<ride_type_t>(81),
        ObjectEntryIndex rideObject = static_cast<ObjectEntryIndex>(10))
    {
        return {
            { "rideType", rideType },
            { "rideObject", rideObject },
            { "entranceObject", static_cast<ObjectEntryIndex>(10) },
            { "colour1", 0 },
            { "colour2", 0 },
            { "inspectionInterval", static_cast<uint8_t>(RideInspection::never) },
        };
    }

    json_t RideAllocationProjection()
    {
        return {
            { "nextFree", GetNextFreeRideId().IsNull() ? json_t(nullptr) : json_t(GetNextFreeRideId().ToUnderlying()) },
            { "count", RideGetCount() },
        };
    }

    json_t TrackPlaceArgs(RideId ride)
    {
        return {
            { "x", 320 },
            { "y", 320 },
            { "z", 16 },
            { "direction", 0 },
            { "ride", ride.ToUnderlying() },
            { "trackType", static_cast<uint16_t>(TrackElemType::flatTrack4x4) },
            { "rideType", static_cast<ride_type_t>(81) },
            { "brakeSpeed", 0 },
            { "colour", 2 },
            { "seatRotation", 4 },
            { "trackPlaceFlags", 0 },
            { "isFromTrackDesign", false },
        };
    }

    RideId CreateEnterpriseRide(GameState_t& state)
    {
        state.cheats.sandboxMode = true;
        state.cheats.allowArbitraryRideTypeChanges = true;
        state.cheats.disableClearanceChecks = true;
        const auto rideId = GetNextFreeRideId();
        const auto created = ExecuteNativeAction("RideCreateAction", RideCreateArgs(), state);
        if (!created.ok || !created.value.value("accepted", false))
        {
            ADD_FAILURE() << (created.ok ? created.value.dump() : created.message);
            return RideId::GetNull();
        }

        auto* ride = GetRide(rideId);
        if (ride == nullptr || ride->type != static_cast<ride_type_t>(81)
            || ride->subtype != static_cast<ObjectEntryIndex>(10))
        {
            ADD_FAILURE() << "Enterprise ride was not created in the expected slot, type, and object";
            return RideId::GetNull();
        }
        ride->status = RideStatus::closed;
        ride->overallView = {};
        return rideId;
    }
}

TEST_F(NativeActionContract, RejectsIncompatibleRideCreateObjectBeforeAllocation)
{
    LoadEnterprisePark();
    auto& state = OpenRCT2::getGameState();
    const auto* entry = GetRideEntryByIndex(static_cast<ObjectEntryIndex>(10));
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(std::ranges::find(entry->ride_type, static_cast<ride_type_t>(81)) != std::end(entry->ride_type));
    ASSERT_TRUE(std::ranges::find(entry->ride_type, static_cast<ride_type_t>(33)) == std::end(entry->ride_type));

    const auto args = RideCreateArgs(static_cast<ride_type_t>(33));
    const auto before = RideAllocationProjection();
    const auto queried = QueryNativeAction("RideCreateAction", args, state);
    ASSERT_TRUE(queried.ok) << queried.message;
    EXPECT_FALSE(queried.value["accepted"]);
    EXPECT_EQ(queried.value["status"], static_cast<uint16_t>(GameActions::Status::invalidParameters));
    EXPECT_EQ(queried.value["rejection"]["code"], "invalid_parameters");
    EXPECT_EQ(RideAllocationProjection(), before);

    const auto executed = ExecuteNativeAction("RideCreateAction", args, state);
    ASSERT_TRUE(executed.ok) << executed.message;
    EXPECT_FALSE(executed.value["accepted"]);
    EXPECT_EQ(executed.value["status"], static_cast<uint16_t>(GameActions::Status::invalidParameters));
    EXPECT_EQ(executed.value["rejection"]["code"], "invalid_parameters");
    EXPECT_EQ(RideAllocationProjection(), before);
}

TEST_F(NativeActionContract, RejectsOutOfRangeTrackPlacePublicArguments)
{
    struct InvalidArgument
    {
        const char* name;
        int32_t value;
    };
    const std::array invalidArguments{
        InvalidArgument{ "rideType", static_cast<int32_t>(RIDE_TYPE_COUNT) },
        InvalidArgument{ "trackType", static_cast<int32_t>(TrackElemType::count) },
        InvalidArgument{ "colour", -1 },
        InvalidArgument{ "colour", kNumRideColourSchemes },
        InvalidArgument{ "seatRotation", -1 },
        InvalidArgument{ "seatRotation", 16 },
        InvalidArgument{ "trackPlaceFlags", 4 },
    };

    for (const auto& invalid : invalidArguments)
    {
        LoadEnterprisePark();
        auto& state = OpenRCT2::getGameState();
        const auto rideId = CreateEnterpriseRide(state);
        ASSERT_FALSE(rideId.IsNull());
        auto args = TrackPlaceArgs(rideId);
        args[invalid.name] = invalid.value;

        const auto queried = QueryNativeAction("TrackPlaceAction", args, state);
        ASSERT_TRUE(queried.ok) << queried.message;
        EXPECT_FALSE(queried.value["accepted"]) << invalid.name;
        EXPECT_EQ(queried.value["status"], static_cast<uint16_t>(GameActions::Status::invalidParameters));
        EXPECT_EQ(queried.value["rejection"]["code"], "invalid_parameters") << invalid.name;

        const auto executed = ExecuteNativeAction("TrackPlaceAction", args, state);
        ASSERT_TRUE(executed.ok) << executed.message;
        EXPECT_FALSE(executed.value["accepted"]) << invalid.name;
        EXPECT_EQ(executed.value["status"], static_cast<uint16_t>(GameActions::Status::invalidParameters));
        EXPECT_EQ(executed.value["rejection"]["code"], "invalid_parameters") << invalid.name;
    }
}
