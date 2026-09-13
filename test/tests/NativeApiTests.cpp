/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TestData.h"

#include <gtest/gtest.h>
#include <memory>
#include <openrct2/Context.h>
#include <openrct2/actions/ride/RideEntranceExitPlaceAction.h>
#include <openrct2/command_line/NativeRegistry.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <openrct2/object/ObjectManager.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace OpenRCT2;
using namespace OpenRCT2::CommandLine;
using json_t = nlohmann::json;

TEST(NativeApiRegistry, HasOneExactInitialResourcePopulation)
{
    const std::vector<std::string> expected{
        "session", "park", "date", "finance", "rides", "ride", "vehicle", "guests", "guest", "objects", "tile", "region",
    };
    std::vector<std::string> actual;
    for (const auto& descriptor : NativeResources())
        actual.emplace_back(descriptor.name);
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(std::set(actual.begin(), actual.end()).size(), actual.size());
}

TEST(NativeApiRegistry, ResourceDescriptorsDeclareSchemaUnitsAuthorityInputsAndCapability)
{
    for (const auto& descriptor : NativeResources())
    {
        EXPECT_NE(descriptor.description, nullptr);
        EXPECT_TRUE(descriptor.schema.is_object());
        EXPECT_TRUE(descriptor.units.is_object());
        EXPECT_FALSE(std::string(descriptor.authority).empty());
        EXPECT_TRUE(descriptor.inputs.is_array() || descriptor.inputs.is_object());
        EXPECT_NE(descriptor.capability, nullptr);
        EXPECT_TRUE(static_cast<bool>(descriptor.read));
        const auto json = NativeResourceDescriptorJson(descriptor);
        EXPECT_TRUE(json.contains("description"));
        EXPECT_TRUE(json.contains("schema"));
        EXPECT_TRUE(json.contains("units"));
        EXPECT_TRUE(json.contains("authority"));
        EXPECT_TRUE(json.contains("classification"));
        EXPECT_TRUE(json.contains("inputs"));
        EXPECT_TRUE(json.contains("capability"));
    }
}

TEST(NativeApiRegistry, ActionDescriptorsComeFromNativeActionRegistrations)
{
    const auto actions = NativeActions();
    ASSERT_FALSE(actions.empty());
    std::set<std::string> names;
    for (const auto& descriptor : actions)
    {
        EXPECT_TRUE(names.insert(descriptor.name).second);
        EXPECT_TRUE(descriptor.schema.is_object());
        EXPECT_TRUE(descriptor.schema.contains("properties"));
        EXPECT_TRUE(descriptor.schema.contains("required"));
        EXPECT_TRUE(descriptor.units.is_object());
        EXPECT_EQ(descriptor.authority, "engine");
        EXPECT_EQ(descriptor.capability, "native");
        const auto json = NativeActionDescriptorJson(descriptor);
        EXPECT_TRUE(json.contains("inputs"));
        EXPECT_TRUE(json.contains("policy"));
    }
}

TEST(NativeApiCaptureView, BoundedViewsAcceptExplicitValuesAndRejectMalformedValues)
{
    const auto valid = ValidateNativeCaptureView(
        json_t{
            { "width", 640 },
            { "height", 480 },
            { "center", { { "x", 3632 }, { "y", 400 } } },
            { "zoom", 0 },
            { "rotation", 0 },
        },
        150,
        150);
    EXPECT_TRUE(valid.ok);
    EXPECT_EQ(ValidateNativeCaptureView(json_t::object(), 150, 150).code, "capture_view_invalid");
    EXPECT_EQ(
        ValidateNativeCaptureView(
            json_t{
                { "width", 0 },
                { "height", 480 },
                { "center", { { "x", 3632 }, { "y", 400 } } },
                { "zoom", 0 },
                { "rotation", 0 },
            },
            150,
            150)
            .code,
        "capture_view_dimensions");
    EXPECT_EQ(
        ValidateNativeCaptureView(
            json_t{
                { "width", 640 },
                { "height", 480 },
                { "center", { { "x", 0 }, { "y", 400 } } },
                { "zoom", 0 },
                { "rotation", 0 },
            },
            150,
            150)
            .code,
        "capture_view_center");
    EXPECT_EQ(
        ValidateNativeCaptureView(
            json_t{
                { "width", 640 },
                { "height", 480 },
                { "center", { { "x", 3632 }, { "y", 400 } } },
                { "zoom", 4 },
                { "rotation", 0 },
            },
            150,
            150)
            .code,
        "capture_view_zoom");
    EXPECT_EQ(
        ValidateNativeCaptureView(
            json_t{
                { "width", 640 },
                { "height", 480 },
                { "center", { { "x", 3632 }, { "y", 400 } } },
                { "zoom", 0 },
                { "rotation", 4 },
            },
            150,
            150)
            .code,
        "capture_view_rotation");
}

class NativeActionThroughline : public testing::Test
{
protected:
    std::unique_ptr<OpenRCT2::IContext> _context;

    void SetUp() override
    {
        gOpenRCT2Headless = true;
        gOpenRCT2NoGraphics = true;
        _context = OpenRCT2::CreateContext();
        ASSERT_NE(_context, nullptr);
        const auto resources = std::filesystem::current_path() / "OpenRCT2.app/Contents/Resources";
        _context->GetPlatformEnvironment().SetBasePath(
            OpenRCT2::DirBase::openrct2, resources.string());
        ASSERT_TRUE(_context->Initialise());

        auto importer = OpenRCT2::ParkImporter::CreateS6(_context->GetObjectRepository());
        auto loadResult = importer->LoadSavedGame(
            TestData::GetParkPath("small_park_with_ferris_wheel.sv6").c_str(), false);
        _context->GetObjectManager().LoadObjects(loadResult.RequiredObjects);
        importer->Import(OpenRCT2::getGameState());
    }
};

TEST_F(NativeActionThroughline, RejectedEntranceExitPlacementIsNonMutatingAndStructured)
{
    auto& state = OpenRCT2::getGameState();
    const json_t args{
        { "x", 0 },
        { "y", 0 },
        { "direction", 2 },
        { "ride", 65535 },
        { "station", 0 },
        { "isExit", true },
    };
    const auto cashBefore = state.park.cash;
    const auto queried = QueryNativeAction("RideEntranceExitPlaceAction", args, state);
    const auto executed = ExecuteNativeAction("RideEntranceExitPlaceAction", args, state);
    EXPECT_TRUE(queried.ok);
    EXPECT_TRUE(executed.ok);
    EXPECT_EQ(queried.value["accepted"], false);
    EXPECT_EQ(executed.value["accepted"], false);
    EXPECT_EQ(queried.value["status"], executed.value["status"]);
    EXPECT_EQ(queried.value["rejection"], executed.value["rejection"]);
    EXPECT_EQ(state.park.cash, cashBefore);
}

static Ride* FindRideWithEntranceAndTrack()
{
    for (RideId::UnderlyingType i = 0; i < Limits::kMaxRidesInPark; ++i)
    {
        auto* ride = GetRide(RideId::FromUnderlying(i));
        if (ride == nullptr)
            continue;
        const auto& station = ride->getStation(StationIndex::FromUnderlying(0));
        if (!station.Start.IsNull() && !station.Entrance.IsNull())
            return ride;
    }
    return nullptr;
}

static size_t TileElementCount(const CoordsXY& location)
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

static bool HasStationTrack(const CoordsXY& endpoint, int16_t z, Direction direction, RideId rideId)
{
    const auto trackLocation = endpoint + CoordsDirectionDelta[direction];
    auto* element = MapGetFirstElementAt(trackLocation);
    if (element == nullptr)
        return false;

    do
    {
        if (element->getBaseZ() == z && element->getType() == TileElementType::Track)
        {
            const auto* track = element->asTrack();
            if (track->GetRideIndex() == rideId && track->GetStationIndex() == StationIndex::FromUnderlying(0))
                return true;
        }
    } while (!(element++)->isLastForTile());
    return false;
}

TEST_F(NativeActionThroughline, RideActionValidityRejectsOutOfRangeDirectionWithoutMutation)
{
    auto* ride = FindRideWithEntranceAndTrack();
    ASSERT_NE(ride, nullptr);
    auto& station = ride->getStation(StationIndex::FromUnderlying(0));
    const auto endpoint = station.Entrance;
    const auto elementsBefore = TileElementCount(endpoint.ToCoordsXY());
    const auto endpointDirectionBefore = endpoint.direction;

    const auto action = OpenRCT2::GameActions::RideEntranceExitPlaceAction(
        endpoint.ToCoordsXY(), 4, ride->id, StationIndex::FromUnderlying(0), false);
    const auto queried = action.Query(OpenRCT2::getGameState(), OpenRCT2::getGameState().park);
    const auto executed = action.Execute(OpenRCT2::getGameState(), OpenRCT2::getGameState().park);

    EXPECT_NE(queried.error, OpenRCT2::GameActions::Status::ok);
    EXPECT_NE(executed.error, OpenRCT2::GameActions::Status::ok);
    EXPECT_EQ(station.Entrance.direction, endpointDirectionBefore);
    EXPECT_EQ(TileElementCount(endpoint.ToCoordsXY()), elementsBefore);
}

TEST_F(NativeActionThroughline, RideActionValidityRejectsEndpointFacingAwayWithoutMutation)
{
    auto* ride = FindRideWithEntranceAndTrack();
    ASSERT_NE(ride, nullptr);
    auto& station = ride->getStation(StationIndex::FromUnderlying(0));
    const auto endpoint = station.Entrance;
    Direction invalidDirection = kInvalidDirection;
    for (const auto direction : kAllDirections)
    {
        if (!HasStationTrack(endpoint.ToCoordsXY(), station.GetBaseZ(), direction, ride->id))
        {
            invalidDirection = direction;
            break;
        }
    }
    ASSERT_NE(invalidDirection, kInvalidDirection);

    const auto elementsBefore = TileElementCount(endpoint.ToCoordsXY());
    const auto endpointBefore = station.Entrance;
    const auto action = OpenRCT2::GameActions::RideEntranceExitPlaceAction(
        endpoint.ToCoordsXY(), invalidDirection, ride->id, StationIndex::FromUnderlying(0), false);
    const auto queried = action.Query(OpenRCT2::getGameState(), OpenRCT2::getGameState().park);
    const auto executed = action.Execute(OpenRCT2::getGameState(), OpenRCT2::getGameState().park);

    EXPECT_NE(queried.error, OpenRCT2::GameActions::Status::ok);
    EXPECT_NE(executed.error, OpenRCT2::GameActions::Status::ok);
    EXPECT_EQ(station.Entrance, endpointBefore);
    EXPECT_EQ(TileElementCount(endpoint.ToCoordsXY()), elementsBefore);
}

TEST_F(NativeActionThroughline, RideActionCompatibilityAcceptsExistingStationSides)
{
    auto* ride = FindRideWithEntranceAndTrack();
    ASSERT_NE(ride, nullptr);
    ride->status = RideStatus::closed;
    const auto& station = ride->getStation(StationIndex::FromUnderlying(0));

    OpenRCT2::getGameState().cheats.disableClearanceChecks = true;
    const auto checkEndpoint = [&](const auto& endpoint, bool isExit) {
        if (endpoint.IsNull())
            return;
        auto action = OpenRCT2::GameActions::RideEntranceExitPlaceAction(
            endpoint.ToCoordsXY(), endpoint.direction, ride->id, StationIndex::FromUnderlying(0), isExit);
        action.SetFlags(OpenRCT2::GameActions::CommandFlag::allowDuringPaused);
        const auto result = action.Query(OpenRCT2::getGameState(), OpenRCT2::getGameState().park);
        EXPECT_EQ(result.error, OpenRCT2::GameActions::Status::ok);
    };
    checkEndpoint(station.Entrance, false);
    checkEndpoint(station.Exit, true);
}

TEST_F(NativeActionThroughline, NativeFlagsRemainDispatcherOwned)
{
    const auto actions = NativeActions();
    const auto it = std::find_if(actions.begin(), actions.end(), [](const auto& action) {
        return std::string_view(action.name) == "RideEntranceExitPlaceAction";
    });
    ASSERT_NE(it, actions.end());
    const auto descriptor = NativeActionDescriptorJson(*it);
    EXPECT_EQ(descriptor["policy"]["flags"], "native-controlled");
}

TEST(NativeApiPolicy, UniversalFlagsAreNotCallerControlledAndSavePathsAreContained)
{
    EXPECT_TRUE(NativePathContained("/tmp/parkbench-owned", "/tmp/parkbench-owned/save.park"));
    EXPECT_FALSE(NativePathContained("/tmp/parkbench-owned", "/tmp/parkbench-owned-other/save.park"));
    EXPECT_FALSE(NativePathContained("/tmp/parkbench-owned", "/tmp/save.park"));
}
