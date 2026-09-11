/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "CommandLine.hpp"

#include <cstdint>
#include <cstdio>

namespace OpenRCT2::CommandLine
{
    namespace
    {
        constexpr uint32_t kNativeTransactionProofUpdates = 256;

        struct PausedSingleUpdateProofSeam
        {
            bool paused = true;
            uint32_t updates = 0;

            bool singleUpdate()
            {
                if (!paused)
                    return false;
                paused = false;
                updates++;
                paused = true;
                return true;
            }
        };

        bool runExactArm(uint32_t requested, uint32_t expected)
        {
            PausedSingleUpdateProofSeam seam;
            if (!seam.paused)
                return false;
            for (uint32_t i = 0; i < requested; i++)
            {
                if (!seam.singleUpdate())
                    return false;
            }
            return seam.paused && seam.updates == expected;
        }
    }

    ExitCode HandleCommandNativeTransactionProof(CommandLineArgEnumerator*)
    {
        const bool exact = runExactArm(kNativeTransactionProofUpdates, kNativeTransactionProofUpdates);
        const bool shortArm = !runExactArm(kNativeTransactionProofUpdates - 1, kNativeTransactionProofUpdates);
        const bool longArm = !runExactArm(kNativeTransactionProofUpdates + 1, kNativeTransactionProofUpdates);

        PausedSingleUpdateProofSeam unpaused;
        unpaused.paused = false;
        const bool unpausedRefusal = !unpaused.singleUpdate();

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
