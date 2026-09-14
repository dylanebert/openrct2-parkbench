/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../core/Json.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace OpenRCT2::CommandLine
{
    struct NativeMonitorRequest
    {
        uint64_t id{};
        std::string method;
        uint32_t ticks{};
        json_t params = json_t::object();
    };

    class NativeMonitor final
    {
    public:
        static constexpr uint32_t kProtocolVersion = 1;
        // Diagnostic capture responses contain bounded paint/draw records. Keep the
        // wire frame bounded, but large enough to carry the complete bounded
        // diagnostic response without turning a successful capture into EOF.
        static constexpr uint32_t kMaxFrameBytes = 4 * 1024 * 1024;
        static constexpr uint32_t kMaxStepTicks = 1'000'000;

        explicit NativeMonitor(int descriptor);
        ~NativeMonitor();

        NativeMonitor(const NativeMonitor&) = delete;
        NativeMonitor& operator=(const NativeMonitor&) = delete;

        bool Start(uint32_t tick, bool paused);
        [[nodiscard]] std::optional<NativeMonitorRequest> TakeRequest();
        [[nodiscard]] bool Lost() const;

        bool SendSuccess(uint64_t id, uint64_t sequence, uint32_t tick, bool paused, const json_t& result);
        bool SendError(
            uint64_t id, uint64_t sequence, uint32_t tick, bool paused, std::string_view code, std::string_view message,
            const json_t& detail = nullptr);

        // These helpers are intentionally public so the focused tests exercise the
        // wire boundary without starting an engine.
        [[nodiscard]] static std::vector<uint8_t> EncodeFrame(std::string_view payload);
        [[nodiscard]] static bool ParseRequest(
            std::string_view payload, NativeMonitorRequest& request, std::string& code, std::string& message);
        [[nodiscard]] static std::string Greeting(uint32_t tick, bool paused);

    private:
        void ReadLoop();
        bool SendPayload(std::string_view payload);
        void MarkLost();

        int _descriptor;
        mutable std::mutex _stateMutex;
        std::condition_variable _responseCondition;
        std::mutex _writeMutex;
        std::deque<NativeMonitorRequest> _requests;
        bool _waitingForResponse;
        bool _closing;
        bool _lost;
        std::thread _reader;
    };
} // namespace OpenRCT2::CommandLine
