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
#include <openrct2/actions/CommandFlag.h>
#include <openrct2/actions/GameActionRunner.h>
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
#include <openrct2/world/Map.h>
#include <openrct2/object/ObjectManager.h>

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
        "session", "park", "date", "finance", "rides", "ride", "vehicle", "guests", "guest", "objects", "tile",
    };
    std::vector<std::string> actual;
    for (const auto& descriptor : NativeResources())
        actual.emplace_back(descriptor.name);
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(std::set(actual.begin(), actual.end()).size(), actual.size());
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

TEST_F(NativeActionThroughline, NativeSaveMutateLoadRestoresAuthoritativePark)
{
    const auto originalPaused = gGamePaused;
    gGamePaused |= GAME_PAUSED_NORMAL;
    auto& state = OpenRCT2::getGameState();
    const auto root = std::filesystem::temp_directory_path() / "parkbench-native-save-load";
    std::filesystem::remove_all(root);
    SetNativeSaveRoot(root.string());
    const auto path = (root / "round-trip.park").string();

    const auto savedState = ReadNativeResource("park", json_t::object(), state);
    ASSERT_TRUE(savedState.ok) << savedState.message;
    const auto saved = SaveNativeGame(path, state);
    ASSERT_TRUE(saved.ok) << saved.message;
    ASSERT_EQ(saved.value["tick"], state.currentTicks);

    const auto mutated = ExecuteNativeAction("ParkSetNameAction", json_t{ { "name", "Mutated Park" } }, state);
    ASSERT_TRUE(mutated.ok) << mutated.message;
    ASSERT_TRUE(mutated.value["accepted"]) << mutated.value.dump();
    const auto mutatedState = ReadNativeResource("park", json_t::object(), state);
    ASSERT_TRUE(mutatedState.ok) << mutatedState.message;
    EXPECT_EQ(mutatedState.value["name"], "Mutated Park");
    EXPECT_NE(mutatedState.value["name"], savedState.value["name"]);

    const auto restored = LoadNativeGame(path, state);
    ASSERT_TRUE(restored.ok) << restored.message;
    EXPECT_TRUE(GameIsPaused());
    EXPECT_EQ(restored.value["tick"], saved.value["tick"]);
    EXPECT_EQ(state.currentTicks, saved.value["tick"]);

    const auto restoredState = ReadNativeResource("park", json_t::object(), state);
    ASSERT_TRUE(restoredState.ok) << restoredState.message;
    EXPECT_EQ(restoredState.value["name"], savedState.value["name"]);
    EXPECT_EQ(restoredState.value["cash"], savedState.value["cash"]);
    EXPECT_EQ(restoredState.value["entranceFee"], savedState.value["entranceFee"]);

    std::filesystem::remove_all(root);
    SetNativeSaveRoot({});
    gGamePaused = originalPaused;
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

TEST(NativeApiPolicy, SavePathsStayContained)
{
    EXPECT_TRUE(NativePathContained("/tmp/parkbench-owned", "/tmp/parkbench-owned/save.park"));
    EXPECT_FALSE(NativePathContained("/tmp/parkbench-owned", "/tmp/parkbench-owned-other/save.park"));
    EXPECT_FALSE(NativePathContained("/tmp/parkbench-owned", "/tmp/save.park"));
}
