/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>

namespace OpenRCT2::GameActions
{
    class GameAction;
}

namespace OpenRCT2
{
    static constexpr uint32_t k_MaxReplayTicks = 0xFFFFFFFF;

    struct ReplayRecordInfo
    {
        uint16_t Version;
        uint32_t Ticks;
        uint32_t TickStart;
        uint32_t TickEnd;
        uint64_t TimeRecorded;
        uint32_t NumCommands;
        uint32_t NumChecksums;
        std::string Name;
        std::string FilePath;
    };

    struct ReplayPlaybackProgress
    {
        uint32_t TicksPlayed;
        uint32_t TotalTicks;
        uint32_t CommandsPlayed;
        uint32_t TotalCommands;
        uint32_t Tick;
    };

    struct IReplayManager
    {
    public:
        enum class RecordType
        {
            NORMAL,
            SILENT,
        };

        virtual ~IReplayManager() = default;

        virtual void Update() = 0;

        virtual bool IsReplaying() const = 0;
        virtual bool IsRecording() const = 0;
        virtual bool IsNormalising() const = 0;
        virtual bool ShouldDisplayNotice() const = 0;

        virtual void AddGameAction(uint32_t tick, const GameActions::GameAction* action) = 0;

        virtual bool StartRecording(
            const std::string& name, uint32_t maxTicks = k_MaxReplayTicks, RecordType rt = RecordType::NORMAL)
            = 0;
        virtual bool StopRecording(bool discard = false) = 0;
        virtual bool GetCurrentReplayInfo(ReplayRecordInfo& info) const = 0;

        virtual void StartPlayback(const std::string& file) = 0;
        virtual bool IsPlaybackStateMismatching() const = 0;
        virtual bool StopPlayback() = 0;
        // Progress of the playback in progress; false when not replaying.
        virtual bool GetPlaybackProgress(ReplayPlaybackProgress& progress) const = 0;
        // Progress at the moment the last normal playback ended; false before any playback has ended.
        virtual bool GetPlaybackEnd(ReplayPlaybackProgress& progress) const = 0;
        // Drops the end snapshot so it does not outlive the replayed park.
        virtual void ClearPlaybackEnd() = 0;

        virtual bool NormaliseReplay(const std::string& inputFile, const std::string& outputFile) = 0;
    };

    [[nodiscard]] std::unique_ptr<IReplayManager> CreateReplayManager();

} // namespace OpenRCT2
