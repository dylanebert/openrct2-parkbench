/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "../Context.h"
#include "../Game.h"
#include "../GameState.h"
#include "../OpenRCT2.h"
#include "../PlatformEnvironment.h"
#include "../ReplayManager.h"
#include "../Version.h"
#include "../actions/CommandFlag.h"
#include "../actions/GameActionRunner.h"
#include "../actions/footpath/FootpathPlaceAction.h"
#include "../core/Crypt.h"
#include "../core/File.h"
#include "../core/Path.hpp"
#include "../core/String.hpp"
#include "../entity/EntityBase.h"
#include "../scenario/Scenario.h"
#include "../world/Map.h"
#include "../world/TileElementsView.h"
#include "../world/tile_element/PathElement.h"
#include "CommandLine.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#ifndef OPENRCT2_COMMIT_SHA1_FULL
    #define OPENRCT2_COMMIT_SHA1_FULL "unknown"
#endif

namespace OpenRCT2::CommandLine
{
    bool gNativeTransactionProofResult = false;

    namespace
    {
        constexpr uint32_t kNativeTransactionProofUpdates = 256;
        constexpr const char* kNativeTransactionCheckpointSha256 = "26365ce8ac1c12f02128ce811d6c0d1e5858e72c55081a16c79a6873b8d"
                                                                   "24907";
        constexpr const char* kNativeTransactionAction = "footpathplace";
        constexpr int32_t kNativeTransactionTargetX = 113;
        constexpr int32_t kNativeTransactionTargetY = 12;
        constexpr int32_t kNativeTransactionNeighborX = 112;
        constexpr int32_t kNativeTransactionNeighborY = 12;
        constexpr int32_t kNativeTransactionBaseZ = 32;

        struct NativeTransactionPathElement
        {
            int32_t baseZ;
            uint8_t edges;
        };

        struct NativeTransactionState
        {
            uint32_t tick;
            uint32_t scenarioRngS0;
            uint32_t scenarioRngS1;
            money64 cash;
            money64 loan;
            uint16_t guests;
            uint16_t staff;
            uint16_t rides;
            uint16_t vehicles;
            std::vector<NativeTransactionPathElement> path;
        };

        bool proofStep(NativeTransactionProofState& state)
        {
            if (!state.paused)
                return false;
            state.paused = false;
            state.currentTicks++;
            state.paused = true;
            return true;
        }

        bool nativeTransactionEngineStep(NativeTransactionProofState& state)
        {
            if (GameIsNotPaused())
                return false;
            gDoSingleUpdate = true;
            gameStateTick();
            state.paused = GameIsPaused();
            state.currentTicks = getGameState().currentTicks;
            return true;
        }

        bool runExactArm(uint32_t requested, uint32_t expected)
        {
            NativeTransactionProofState state{ true, 0 };
            const bool accepted = gameStateAdvancePausedNativeTransaction(requested, state, proofStep);
            return accepted && state.paused && state.currentTicks == expected;
        }

        bool CheckpointMatches(const char* checkpointPath)
        {
            try
            {
                const auto bytes = File::ReadAllBytes(checkpointPath);
                const auto digest = Crypt::SHA256(bytes.data(), bytes.size());
                return String::StringFromHex(digest) == kNativeTransactionCheckpointSha256;
            }
            catch (const std::exception&)
            {
                return false;
            }
        }

        NativeTransactionState CaptureState()
        {
            const auto target = TileCoordsXY{ kNativeTransactionTargetX, kNativeTransactionTargetY };
            const auto scenarioRng = ScenarioRandState();
            NativeTransactionState state{
                getGameState().currentTicks,
                scenarioRng.s0,
                scenarioRng.s1,
                getGameState().park.cash,
                getGameState().park.bankLoan,
                getGameState().entities.GetEntityListCount(EntityType::guest),
                getGameState().entities.GetEntityListCount(EntityType::staff),
                static_cast<uint16_t>(std::count_if(
                    getGameState().rides.begin(), getGameState().rides.end(),
                    [](const auto& ride) { return ride.id != RideId::GetNull(); })),
                getGameState().entities.GetEntityListCount(EntityType::vehicle),
                {},
            };

            for (const auto* pathElement : TileElementsView<PathElement>(target))
            {
                state.path.push_back({ pathElement->getBaseZ(), pathElement->GetEdges() });
            }
            return state;
        }

        void WriteState(std::ostringstream& output, const NativeTransactionState& state)
        {
            output << "{\"cash\":" << state.cash << ",\"entities\":{";
            output << "\"guests\":" << state.guests << ",\"rides\":" << state.rides;
            output << ",\"staff\":" << state.staff << ",\"vehicles\":" << state.vehicles << "},";
            output << "\"loan\":" << state.loan << ",\"path\":[";
            for (size_t i = 0; i < state.path.size(); i++)
            {
                if (i != 0)
                    output << ',';
                output << "{\"baseZ\":" << state.path[i].baseZ << ",\"edges\":" << static_cast<uint32_t>(state.path[i].edges)
                       << ",\"type\":\"footpath\"}";
            }
            output << "],\"scenarioRng\":{\"s0\":" << state.scenarioRngS0 << ",\"s1\":" << state.scenarioRngS1
                   << "},\"tick\":" << state.tick << '}';
        }

        void WriteResult(
            const NativeTransactionState& before, const NativeTransactionState& after, bool queryAccepted, bool executeAccepted)
        {
            std::ostringstream output;
            output << "{\"after\":";
            WriteState(output, after);
            output << ",\"before\":";
            WriteState(output, before);
            output << ",\"checkpointSha256\":\"" << kNativeTransactionCheckpointSha256 << "\",";
            constexpr char kVersionPrefix[] = "OpenRCT2, ";
            const char* engineVersion = gVersionInfoFull;
            if (std::strncmp(engineVersion, kVersionPrefix, sizeof(kVersionPrefix) - 1) == 0)
                engineVersion += sizeof(kVersionPrefix) - 1;
            output << "\"engine\":{\"commit\":\"" << OPENRCT2_COMMIT_SHA1_FULL << "\",\"version\":\"";
            output << engineVersion << "\"},\"instance\":\"native-transaction-proof\",";
            output << "\"request\":{\"action\":\"" << kNativeTransactionAction << "\",\"baseZ\":" << kNativeTransactionBaseZ
                   << ",\"neighbor\":[" << kNativeTransactionNeighborX << ',' << kNativeTransactionNeighborY << "],\"target\":["
                   << kNativeTransactionTargetX << ',' << kNativeTransactionTargetY
                   << "],\"ticks\":" << kNativeTransactionProofUpdates << "},";
            output << "\"schema\":\"park-native-transaction/v1\",\"verdict\":{\"execute\":"
                   << (executeAccepted ? "true" : "false") << ",\"query\":" << (queryAccepted ? "true" : "false") << "}}\n";
            std::fwrite(output.str().data(), 1, output.str().size(), stdout);
        }

        bool FailNativeTransaction(const char* message)
        {
            std::fprintf(stderr, "native-transaction-proof-result: %s\n", message);
            return false;
        }
    } // namespace

    ExitCode HandleCommandNativeTransactionProof(CommandLineArgEnumerator*)
    {
        const bool exact = runExactArm(kNativeTransactionProofUpdates, kNativeTransactionProofUpdates);
        const bool shortArm = !runExactArm(kNativeTransactionProofUpdates - 1, kNativeTransactionProofUpdates);
        const bool longArm = !runExactArm(kNativeTransactionProofUpdates + 1, kNativeTransactionProofUpdates);

        NativeTransactionProofState unpaused{ false, 0 };
        const bool unpausedRefusal = !gameStateAdvancePausedNativeTransaction(
                                         kNativeTransactionProofUpdates, unpaused, proofStep)
            && !unpaused.paused && unpaused.currentTicks == 0;

        if (!(exact && shortArm && longArm && unpausedRefusal))
        {
            std::fprintf(
                stderr, "native-transaction-proof: FAIL exact=%d n-1=%d n+1=%d unpaused=%d\n", exact, shortArm, longArm,
                unpausedRefusal);
            return ExitCode::fail;
        }

        std::printf("native-transaction-proof: PASS N=256 N-1=red N+1=red unpaused=red\n");
        return ExitCode::ok;
    }

    ExitCode HandleCommandNativeTransactionProofResult(CommandLineArgEnumerator* enumerator)
    {
        if (CommandLine::HandleCommandDefault() != ExitCode::launch)
            return ExitCode::fail;

        const char* checkpointPath = nullptr;
        if (!enumerator->TryPopString(&checkpointPath) || enumerator->TryPop())
        {
            std::fprintf(stderr, "native-transaction-proof-result: expected exactly one checkpoint path\n");
            return ExitCode::fail;
        }

        gNativeTransactionProofResult = true;
        gOpenRCT2StartupAction = StartupAction::open;
        String::set(gOpenRCT2StartupActionPath, sizeof(gOpenRCT2StartupActionPath), checkpointPath);
        return ExitCode::launch;
    }

    bool RunNativeTransactionProofResult(const char* checkpointPath)
    {
        if (!CheckpointMatches(checkpointPath))
            return FailNativeTransaction("checkpoint hash or path refused");
        if (!GameIsPaused())
            return FailNativeTransaction("checkpoint was not loaded paused");

        const auto neighbor = TileCoordsXYZ{ kNativeTransactionNeighborX, kNativeTransactionNeighborY,
                                             kNativeTransactionBaseZ / kCoordsZStep };
        const auto target = CoordsXYZ{ kNativeTransactionTargetX * kCoordsXYStep, kNativeTransactionTargetY * kCoordsXYStep,
                                       kNativeTransactionBaseZ };
        auto* neighborPath = MapGetPathElementAt(neighbor);
        if (neighborPath == nullptr || neighborPath->getBaseZ() != kNativeTransactionBaseZ)
            return FailNativeTransaction("fixed neighbor path was not found");
        auto targetPaths = TileElementsView<PathElement>(TileCoordsXY{ kNativeTransactionTargetX, kNativeTransactionTargetY });
        if (targetPaths.begin() != targetPaths.end())
            return FailNativeTransaction("fixed target tile was not empty");

        auto replayPath = Path::Combine(
            GetContext()->GetPlatformEnvironment().GetDirectoryPath(DirBase::user, DirId::replayRecordings),
            "native-transaction-proof.parkrep");
        auto* replayManager = GetContext()->GetReplayManager();
        if (replayManager == nullptr || !replayManager->StartRecording(replayPath, kNativeTransactionProofUpdates))
            return FailNativeTransaction("native recording did not start");

        const auto before = CaptureState();
        if (!before.path.empty())
        {
            replayManager->StopRecording(true);
            return FailNativeTransaction("fixed target tile was not empty");
        }

        GameActions::FootpathPlaceAction action(
            target, FootpathSlope{ FootpathSlopeType::flat, 0 }, neighborPath->GetSurfaceEntryIndex(),
            neighborPath->GetRailingsEntryIndex());
        action.SetFlags({ GameActions::CommandFlag::apply, GameActions::CommandFlag::allowDuringPaused });

        const auto query = GameActions::Query(&action, getGameState());
        const bool queryAccepted = query.error == GameActions::Status::ok;
        bool executeAccepted = false;
        if (queryAccepted)
        {
            const bool wasInUpdateCode = gInUpdateCode;
            gInUpdateCode = true;
            const auto execute = GameActions::Execute(&action, getGameState());
            gInUpdateCode = wasInUpdateCode;
            executeAccepted = execute.error == GameActions::Status::ok;
        }
        if (!queryAccepted || !executeAccepted)
        {
            replayManager->StopRecording(true);
            return FailNativeTransaction("native footpath query or execute was refused");
        }

        NativeTransactionProofState stepState{ GameIsPaused(), before.tick };
        if (!gameStateAdvancePausedNativeTransaction(kNativeTransactionProofUpdates, stepState, nativeTransactionEngineStep))
        {
            replayManager->StopRecording(true);
            return FailNativeTransaction("native exact-step transaction was refused");
        }

        const auto after = CaptureState();
        if (after.tick - before.tick != kNativeTransactionProofUpdates || after.path.empty())
        {
            replayManager->StopRecording(true);
            return FailNativeTransaction("native transaction state bounds were not met");
        }
        if (!replayManager->StopRecording())
            return FailNativeTransaction("native recording did not flush");

        WriteResult(before, after, queryAccepted, executeAccepted);
        return true;
    }
} // namespace OpenRCT2::CommandLine
