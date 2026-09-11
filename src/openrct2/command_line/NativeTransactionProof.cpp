/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "CommandLine.hpp"

#include "../GameState.h"

#include <cstdint>
#include <cstdio>

namespace OpenRCT2::CommandLine
{
    namespace
    {
        constexpr uint32_t kNativeTransactionProofUpdates = 256;

        bool proofStep(NativeTransactionProofState& state)
        {
            if (!state.paused)
                return false;
            state.paused = false;
            state.currentTicks++;
            state.paused = true;
            return true;
        }

        bool runExactArm(uint32_t requested, uint32_t expected)
        {
            NativeTransactionProofState state{ true, 0 };
            const bool accepted = gameStateAdvancePausedNativeTransaction(requested, state, proofStep);
            return accepted && state.paused && state.currentTicks == expected;
        }
    }

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
            std::fprintf(stderr,
                "native-transaction-proof: FAIL exact=%d n-1=%d n+1=%d unpaused=%d\n",
                exact, shortArm, longArm, unpausedRefusal);
            return ExitCode::fail;
        }

        std::printf("native-transaction-proof: PASS N=256 N-1=red N+1=red unpaused=red\n");
        return ExitCode::ok;
    }
} // namespace OpenRCT2::CommandLine
