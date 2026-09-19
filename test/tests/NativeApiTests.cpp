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
#include <openrct2/actions/ride/RideEntranceExitRemoveAction.h>
#include <openrct2/actions/track/TrackDesignAction.h>
#include <openrct2/actions/CommandFlag.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/ResultWithMessage.h>
#include <openrct2/command_line/NativeRegistry.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/ReplayManager.h>
#include <openrct2/scenes/SceneManager.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/TrackDesign.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <openrct2/object/ObjectManager.h>

#include <algorithm>
#include <array>
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
        LoadPark("small_park_with_ferris_wheel.sv6");
    }

    void LoadPark(const std::string& name)
    {
        _context.reset();
        gOpenRCT2Headless = true;
        gOpenRCT2NoGraphics = true;
        _context = OpenRCT2::CreateContext();
        ASSERT_NE(_context, nullptr);
        const auto resources = std::filesystem::current_path() / "OpenRCT2.app/Contents/Resources";
        _context->GetPlatformEnvironment().SetBasePath(
            OpenRCT2::DirBase::openrct2, resources.string());
        ASSERT_TRUE(_context->Initialise());

        auto importer = OpenRCT2::ParkImporter::CreateS6(_context->GetObjectRepository());
        auto loadResult = importer->LoadSavedGame(TestData::GetParkPath(name).c_str(), false);
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
    ASSERT_TRUE(queried.value["rejection"].contains("code"));
    ASSERT_TRUE(queried.value["rejection"].contains("title"));
    ASSERT_TRUE(queried.value["rejection"].contains("message"));
    ASSERT_TRUE(queried.value["rejection"].contains("detail"));
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

TEST_F(NativeActionThroughline, AcceptedPlacementChargesAndMatchesOrdinaryRunner)
{
    auto* ordinaryRide = FindRideWithEntranceAndTrack();
    ASSERT_NE(ordinaryRide, nullptr);
    const auto ordinaryStation = StationIndex::FromUnderlying(0);
    const auto ordinaryRideId = ordinaryRide->id;
    const auto ordinaryEndpoint = ordinaryRide->getStation(ordinaryStation).Entrance;
    ASSERT_FALSE(ordinaryEndpoint.IsNull());
    const json_t args{
        { "x", ordinaryEndpoint.ToCoordsXY().x },
        { "y", ordinaryEndpoint.ToCoordsXY().y },
        { "direction", static_cast<uint8_t>(ordinaryEndpoint.direction) },
        { "ride", ordinaryRideId.ToUnderlying() },
        { "station", ordinaryStation.ToUnderlying() },
        { "isExit", false },
    };

    auto& ordinaryState = OpenRCT2::getGameState();
    ordinaryState.cheats.disableClearanceChecks = true;
    ordinaryState.cheats.sandboxMode = true;
    auto removeOrdinary = OpenRCT2::GameActions::RideEntranceExitRemoveAction(
        ordinaryEndpoint.ToCoordsXY(), ordinaryRideId, ordinaryStation, false);
    removeOrdinary.SetFlags({
        OpenRCT2::GameActions::CommandFlag::apply,
        OpenRCT2::GameActions::CommandFlag::allowDuringPaused,
    });
    const auto oldInUpdateCode = gInUpdateCode;
    gInUpdateCode = true;
    ASSERT_EQ(
        OpenRCT2::GameActions::Execute(&removeOrdinary, ordinaryState).error,
        OpenRCT2::GameActions::Status::ok);
    gInUpdateCode = oldInUpdateCode;
    const auto ordinaryCashBefore = ordinaryState.park.cash;
    const auto ordinaryElementsBefore = TileElementCount(ordinaryEndpoint.ToCoordsXY());
    auto ordinaryAction = OpenRCT2::GameActions::RideEntranceExitPlaceAction(
        ordinaryEndpoint.ToCoordsXY(), ordinaryEndpoint.direction, ordinaryRideId, ordinaryStation, false);
    ordinaryAction.SetFlags({
        OpenRCT2::GameActions::CommandFlag::apply,
        OpenRCT2::GameActions::CommandFlag::allowDuringPaused,
    });
    gInUpdateCode = true;
    const auto ordinaryResult = OpenRCT2::GameActions::Execute(&ordinaryAction, ordinaryState);
    gInUpdateCode = oldInUpdateCode;

    ASSERT_EQ(ordinaryResult.error, OpenRCT2::GameActions::Status::ok);
    const auto ordinaryCashDelta = ordinaryState.park.cash - ordinaryCashBefore;
    const auto ordinaryElementsAfter = TileElementCount(ordinaryEndpoint.ToCoordsXY());

    LoadPark("small_park_with_ferris_wheel.sv6");
    auto& publicState = OpenRCT2::getGameState();
    publicState.cheats.disableClearanceChecks = true;
    publicState.cheats.sandboxMode = true;
    auto* publicRide = GetRide(ordinaryRideId);
    ASSERT_NE(publicRide, nullptr);
    auto removePublic = OpenRCT2::GameActions::RideEntranceExitRemoveAction(
        ordinaryEndpoint.ToCoordsXY(), ordinaryRideId, ordinaryStation, false);
    removePublic.SetFlags({
        OpenRCT2::GameActions::CommandFlag::apply,
        OpenRCT2::GameActions::CommandFlag::allowDuringPaused,
    });
    gInUpdateCode = true;
    ASSERT_EQ(
        OpenRCT2::GameActions::Execute(&removePublic, publicState).error,
        OpenRCT2::GameActions::Status::ok);
    gInUpdateCode = oldInUpdateCode;
    const auto publicCashBefore = publicState.park.cash;
    const auto publicElementsBefore = TileElementCount(ordinaryEndpoint.ToCoordsXY());
    const auto queried = QueryNativeAction("RideEntranceExitPlaceAction", args, publicState);
    ASSERT_TRUE(queried.ok) << queried.message;
    ASSERT_TRUE(queried.value["accepted"]) << queried.value.dump();
    const auto executed = ExecuteNativeAction("RideEntranceExitPlaceAction", args, publicState);

    ASSERT_TRUE(executed.ok);
    ASSERT_TRUE(executed.value["accepted"]) << executed.value.dump();
    EXPECT_EQ(executed.value["status"], static_cast<uint16_t>(ordinaryResult.error));
    EXPECT_EQ(executed.value["cost"], ordinaryResult.cost);
    EXPECT_EQ(publicState.park.cash - publicCashBefore, ordinaryCashDelta);
    EXPECT_EQ(publicElementsBefore, ordinaryElementsBefore);
    EXPECT_EQ(TileElementCount(ordinaryEndpoint.ToCoordsXY()), ordinaryElementsAfter);
}

static bool HasStationTrack(
    const CoordsXY& endpoint, int16_t z, Direction direction, RideId rideId, StationIndex stationNum)
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
            if (track->GetRideIndex() == rideId && track->GetStationIndex() == stationNum)
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
        if (!HasStationTrack(
                endpoint.ToCoordsXY(), station.GetBaseZ(), direction, ride->id, StationIndex::FromUnderlying(0)))
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

TEST_F(NativeActionThroughline, RideActionCompatibilityCoversDeclaredCorpus)
{
    constexpr std::array kCorpus{
        "small_park_with_ferris_wheel.sv6",
        "small_park_car_ride_one_car.sv6",
        "bpb.sv6",
        "BigMapTest.sv6",
    };
    bool sawFlat = false;
    bool sawTracked = false;
    bool sawMultiStation = false;
    size_t endpointCases = 0;

    for (const auto* fixture : kCorpus)
    {
        SCOPED_TRACE(fixture);
        LoadPark(fixture);
        auto& state = OpenRCT2::getGameState();
        state.cheats.disableClearanceChecks = true;
        state.cheats.sandboxMode = true;
        const auto executableEndpointCorpus = std::string_view(fixture).starts_with("small_park_");

        for (RideId::UnderlyingType rideValue = 0; rideValue < Limits::kMaxRidesInPark; ++rideValue)
        {
            auto* ride = GetRide(RideId::FromUnderlying(rideValue));
            if (ride == nullptr)
                continue;
            ride->status = RideStatus::closed;
            const auto hasTrack = ride->getRideTypeDescriptor().flags.has(RtdFlag::hasTrack);
            sawFlat |= !hasTrack;
            sawTracked |= hasTrack;
            sawMultiStation |= ride->numStations > 1;

            for (StationIndex::UnderlyingType stationValue = 0; stationValue < Limits::kMaxStationsPerRide;
                 ++stationValue)
            {
                const auto stationNum = StationIndex::FromUnderlying(stationValue);
                const auto& station = ride->getStation(stationNum);
                const std::array endpoints{ std::pair{ station.Entrance, false }, std::pair{ station.Exit, true } };
                for (const auto& [endpoint, isExit] : endpoints)
                {
                    if (endpoint.IsNull())
                        continue;
                    ++endpointCases;
                    const auto endpointXY = endpoint.ToCoordsXY();

                    for (uint8_t rawDirection = 0; rawDirection < 4; ++rawDirection)
                    {
                        const auto direction = static_cast<Direction>(rawDirection);
                        const json_t args{
                            { "x", endpointXY.x },
                            { "y", endpointXY.y },
                            { "direction", static_cast<uint32_t>(rawDirection) },
                            { "ride", ride->id.ToUnderlying() },
                            { "station", static_cast<uint32_t>(stationValue) },
                            { "isExit", isExit },
                        };
                        const auto queried = QueryNativeAction("RideEntranceExitPlaceAction", args, state);
                        ASSERT_TRUE(queried.ok) << fixture << " descriptor direction " << rawDirection;
                        EXPECT_EQ(
                            queried.value["accepted"],
                            HasStationTrack(endpoint.ToCoordsXY(), station.GetBaseZ(), direction, ride->id, stationNum))
                            << fixture << " ride " << ride->id.ToUnderlying() << " station " << stationValue << " endpoint "
                            << (isExit ? "exit" : "entrance") << " direction " << rawDirection << " result "
                            << queried.value.dump();
                    }

                    if (!executableEndpointCorpus)
                        continue;

                    const json_t validArgs{
                        { "x", endpointXY.x },
                        { "y", endpointXY.y },
                        { "direction", static_cast<uint32_t>(endpoint.direction) },
                        { "ride", ride->id.ToUnderlying() },
                        { "station", static_cast<uint32_t>(stationValue) },
                        { "isExit", isExit },
                    };
                    const auto executed = ExecuteNativeAction("RideEntranceExitPlaceAction", validArgs, state);
                    ASSERT_TRUE(executed.ok) << fixture << " public endpoint execution";
                    EXPECT_TRUE(executed.value["accepted"]) << fixture << " result " << executed.value.dump();

                    auto ghostAction = OpenRCT2::GameActions::RideEntranceExitPlaceAction(
                        endpointXY, endpoint.direction, ride->id, stationNum, isExit);
                    ghostAction.SetFlags(static_cast<OpenRCT2::GameActions::CommandFlag>(
                        static_cast<uint32_t>(OpenRCT2::GameActions::CommandFlag::ghost)
                        | static_cast<uint32_t>(OpenRCT2::GameActions::CommandFlag::allowDuringPaused)));
                    const auto ghostResult = ghostAction.Query(state, state.park);
                    EXPECT_EQ(ghostResult.error, OpenRCT2::GameActions::Status::ok)
                        << fixture << " ghost endpoint " << endpointXY.x << "," << endpointXY.y << " status "
                        << static_cast<int>(ghostResult.error);

                    auto replayAction = OpenRCT2::GameActions::RideEntranceExitPlaceAction(
                        endpointXY, endpoint.direction, ride->id, stationNum, isExit);
                    replayAction.SetFlags(static_cast<OpenRCT2::GameActions::CommandFlag>(
                        static_cast<uint32_t>(OpenRCT2::GameActions::CommandFlag::replay)
                        | static_cast<uint32_t>(OpenRCT2::GameActions::CommandFlag::allowDuringPaused)));
                    EXPECT_EQ(replayAction.Query(state, state.park).error, OpenRCT2::GameActions::Status::ok)
                        << fixture << " replay endpoint " << endpointXY.x << "," << endpointXY.y;
                }
            }
        }
    }

    EXPECT_GT(endpointCases, 0u);
    EXPECT_TRUE(sawFlat);
    EXPECT_TRUE(sawTracked);
    EXPECT_TRUE(sawMultiStation);
}

TEST_F(NativeActionThroughline, RideActionCompatibilityCoversTrackDesignPlacement)
{
    LoadPark("small_park_car_ride_one_car.sv6");
    auto& state = OpenRCT2::getGameState();
    state.cheats.disableClearanceChecks = true;
    state.cheats.sandboxMode = true;

    auto* ride = GetRide(RideId::FromUnderlying(0));
    ASSERT_NE(ride, nullptr);
    ride->status = RideStatus::closed;

    TrackDesign trackDesign;
    TrackDesignState designState{};
    ASSERT_TRUE(trackDesign.CreateTrackDesign(designState, *ride).Successful);
    ASSERT_FALSE(trackDesign.trackElements.empty());
    ASSERT_FALSE(trackDesign.entranceElements.empty());

    const OpenRCT2::GameActions::CommandFlags flags{
        OpenRCT2::GameActions::CommandFlag::apply,
        OpenRCT2::GameActions::CommandFlag::allowDuringPaused,
        OpenRCT2::GameActions::CommandFlag::noSpend,
        OpenRCT2::GameActions::CommandFlag::replay,
    };
    bool placed = false;
    for (int32_t tileY = 8; tileY < 120 && !placed; tileY += 8)
    {
        for (int32_t tileX = 8; tileX < 120 && !placed; tileX += 8)
        {
            const CoordsXY mapCoords{ tileX * kCoordsXYStep, tileY * kCoordsXYStep };
            const auto* surface = MapGetSurfaceElementAt(mapCoords);
            if (surface == nullptr)
                continue;
            const CoordsXYZD probe{ mapCoords, surface->getBaseZ(), 0 };
            const auto placeZ = TrackDesignGetZPlacement(trackDesign, *ride, probe);
            const CoordsXYZD origin{ mapCoords, surface->getBaseZ() + placeZ, 0 };
            OpenRCT2::GameActions::TrackDesignAction action(
                origin, trackDesign, false, RideInspection::every30Minutes);
            action.SetFlags(flags);
            const auto result = action.Execute(state, state.park);
            if (result.error == OpenRCT2::GameActions::Status::ok)
                placed = true;
        }
    }
    EXPECT_TRUE(placed) << "track-design replay placement did not find a legal origin";
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

TEST_F(NativeActionThroughline, SynchronousExecuteIsRecordedIntoActiveReplay)
{
    auto& state = OpenRCT2::getGameState();
    auto* replayManager = _context->GetReplayManager();
    ASSERT_NE(replayManager, nullptr);
    ASSERT_TRUE(replayManager->StartRecording("native-sync-replay", OpenRCT2::k_MaxReplayTicks));

    const auto executed = ExecuteNativeAction("ParkSetNameAction", json_t{ { "name", "Replay Park" } }, state);
    ASSERT_TRUE(executed.ok) << executed.message;
    ASSERT_TRUE(executed.value["accepted"]) << executed.value.dump();

    OpenRCT2::ReplayRecordInfo info;
    ASSERT_TRUE(replayManager->GetCurrentReplayInfo(info));
    EXPECT_EQ(info.NumCommands, 1u);
    replayManager->StopRecording(true);
}

TEST_F(NativeActionThroughline, RecordingStopReportsEngineCommandCountAndTickSpan)
{
    auto& state = OpenRCT2::getGameState();
    const auto root = std::filesystem::temp_directory_path() / "parkbench-native-recording-stop";
    std::filesystem::remove_all(root);
    SetNativeRecordingRoot(root.string());

    const auto emptyPath = (root / "empty.parkrep").string();
    ASSERT_TRUE(StartNativeRecording(emptyPath).ok);
    const auto emptyStart = state.currentTicks;
    state.currentTicks += 2;
    const auto empty = StopNativeRecording();
    ASSERT_TRUE(empty.ok) << empty.message;
    EXPECT_EQ(empty.value["commandCount"], 0u) << empty.value.dump();
    EXPECT_EQ(empty.value["tickStart"], emptyStart) << empty.value.dump();
    EXPECT_EQ(empty.value["tickEnd"], emptyStart + 2) << empty.value.dump();

    const auto scriptedPath = (root / "scripted.parkrep").string();
    ASSERT_TRUE(StartNativeRecording(scriptedPath).ok);
    const auto scriptedStart = state.currentTicks;
    const auto executed = ExecuteNativeAction("ParkSetNameAction", json_t{ { "name", "Recorded Park" } }, state);
    ASSERT_TRUE(executed.ok) << executed.message;
    state.currentTicks += 5;
    const auto scripted = StopNativeRecording();
    ASSERT_TRUE(scripted.ok) << scripted.message;
    EXPECT_EQ(scripted.value["commandCount"], 1u) << scripted.value.dump();
    EXPECT_EQ(scripted.value["tickStart"], scriptedStart) << scripted.value.dump();
    EXPECT_EQ(scripted.value["tickEnd"], scriptedStart + 5) << scripted.value.dump();

    auto* replayManager = _context->GetReplayManager();
    OpenRCT2::ReplayPlaybackProgress progress{};
    EXPECT_FALSE(replayManager->GetPlaybackProgress(progress));
    ASSERT_NO_THROW(replayManager->StartPlayback(scriptedPath));
    ASSERT_TRUE(replayManager->GetPlaybackProgress(progress));
    EXPECT_EQ(progress.TicksPlayed, 0u);
    EXPECT_EQ(progress.TotalTicks, 5u);
    EXPECT_EQ(progress.CommandsPlayed, 0u);
    EXPECT_EQ(progress.TotalCommands, 1u);
    EXPECT_FALSE(replayManager->GetPlaybackEnd(progress));
    for (int step = 0; step <= 10 && replayManager->IsReplaying(); step++)
    {
        replayManager->Update();
        if (replayManager->IsReplaying())
            state.currentTicks++;
    }
    EXPECT_FALSE(replayManager->IsReplaying());
    ASSERT_TRUE(replayManager->GetPlaybackEnd(progress));
    EXPECT_EQ(progress.Tick, scriptedStart + 5);
    EXPECT_EQ(progress.CommandsPlayed, 1u);
    EXPECT_EQ(progress.TotalCommands, 1u);
    EXPECT_EQ(progress.TicksPlayed, 5u);
    EXPECT_EQ(progress.TotalTicks, 5u);

    auto* sceneManager = _context->GetSceneManager();
    sceneManager->setActiveScene(sceneManager->getGameScene());
    EXPECT_FALSE(replayManager->GetPlaybackEnd(progress));
    std::filesystem::remove_all(root);
}

TEST(NativeApiPolicy, UniversalFlagsAreNotCallerControlledAndSavePathsAreContained)
{
    EXPECT_TRUE(NativePathContained("/tmp/parkbench-owned", "/tmp/parkbench-owned/save.park"));
    EXPECT_FALSE(NativePathContained("/tmp/parkbench-owned", "/tmp/parkbench-owned-other/save.park"));
    EXPECT_FALSE(NativePathContained("/tmp/parkbench-owned", "/tmp/save.park"));
}
