/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "NativeMonitor.h"

#include "../Version.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

#ifdef _WIN32
    #include <io.h>
#else
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace OpenRCT2::CommandLine
{
    namespace
    {
        using json_t = nlohmann::json;

        int ReadDescriptor(int descriptor, void* buffer, size_t size)
        {
#ifdef _WIN32
            return _read(descriptor, buffer, static_cast<unsigned int>(size));
#else
            return static_cast<int>(read(descriptor, buffer, size));
#endif
        }

        int WriteDescriptor(int descriptor, const void* buffer, size_t size)
        {
#ifdef _WIN32
            return _write(descriptor, buffer, static_cast<unsigned int>(size));
#else
            return static_cast<int>(write(descriptor, buffer, size));
#endif
        }

        void CloseDescriptor(int descriptor)
        {
#ifdef _WIN32
            _close(descriptor);
#else
            close(descriptor);
#endif
        }

        bool ReadExact(int descriptor, void* destination, size_t size)
        {
            auto* bytes = static_cast<uint8_t*>(destination);
            size_t offset = 0;
            while (offset < size)
            {
                const int result = ReadDescriptor(descriptor, bytes + offset, size - offset);
                if (result == 0)
                    return false;
                if (result < 0)
                {
#ifndef _WIN32
                    if (errno == EINTR)
                        continue;
#endif
                    return false;
                }
                offset += static_cast<size_t>(result);
            }
            return true;
        }

        bool WriteAll(int descriptor, const void* source, size_t size)
        {
            const auto* bytes = static_cast<const uint8_t*>(source);
            size_t offset = 0;
            while (offset < size)
            {
                const int result = WriteDescriptor(descriptor, bytes + offset, size - offset);
                if (result < 0)
                {
#ifndef _WIN32
                    if (errno == EINTR)
                        continue;
#endif
                    return false;
                }
                if (result == 0)
                    return false;
                offset += static_cast<size_t>(result);
            }
            return true;
        }

        json_t StateFields(uint64_t sequence, uint32_t tick, bool paused)
        {
            return {
                { "sequence", sequence },
                { "tick", tick },
                { "paused", paused },
            };
        }

        std::string ErrorMessage(std::string_view code, std::string_view message)
        {
            return std::string(code) + ": " + std::string(message);
        }
    }

    NativeMonitor::NativeMonitor(int descriptor)
        : _descriptor(descriptor)
        , _waitingForResponse(false)
        , _closing(false)
        , _lost(false)
    {
    }

    NativeMonitor::~NativeMonitor()
    {
        {
            std::lock_guard lock(_stateMutex);
            _closing = true;
        }
        _responseCondition.notify_all();
        if (_descriptor >= 0)
        {
#ifndef _WIN32
            // The launch descriptor is a private Unix socket. Shutdown wakes
            // a reader blocked in read before the descriptor is closed.
            shutdown(_descriptor, SHUT_RDWR);
#endif
        }
        if (_reader.joinable())
            _reader.join();
        if (_descriptor >= 0)
        {
            CloseDescriptor(_descriptor);
            _descriptor = -1;
        }
    }

    bool NativeMonitor::Start(uint32_t tick, bool paused)
    {
        if (_descriptor < 0)
            return false;
        if (!SendPayload(Greeting(tick, paused)))
            return false;
        _reader = std::thread(&NativeMonitor::ReadLoop, this);
        return true;
    }

    std::optional<NativeMonitorRequest> NativeMonitor::TakeRequest()
    {
        std::lock_guard lock(_stateMutex);
        if (_requests.empty())
            return std::nullopt;
        auto request = std::move(_requests.front());
        _requests.pop_front();
        return request;
    }

    bool NativeMonitor::Lost() const
    {
        std::lock_guard lock(_stateMutex);
        return _lost;
    }

    std::vector<uint8_t> NativeMonitor::EncodeFrame(std::string_view payload)
    {
        if (payload.empty() || payload.size() > kMaxFrameBytes)
            return {};
        const auto length = static_cast<uint32_t>(payload.size());
        std::vector<uint8_t> frame(4 + payload.size());
        frame[0] = static_cast<uint8_t>((length >> 24) & 0xff);
        frame[1] = static_cast<uint8_t>((length >> 16) & 0xff);
        frame[2] = static_cast<uint8_t>((length >> 8) & 0xff);
        frame[3] = static_cast<uint8_t>(length & 0xff);
        std::memcpy(frame.data() + 4, payload.data(), payload.size());
        return frame;
    }

    std::string NativeMonitor::Greeting(uint32_t tick, bool paused)
    {
        json_t greeting = json_t::object();
        greeting["type"] = "greeting";
        greeting["schema"] = "park/native-monitor/v1";
        greeting["version"] = kProtocolVersion;
        greeting["maxFrameBytes"] = kMaxFrameBytes;
        greeting["capabilities"] = json_t{
            "ping", "status", "step", "stop", "resource.list", "resource.describe", "resource.read",
            "action.list", "action.describe", "action.query", "action.execute", "save",
            "record.start", "record.status", "record.stop", "capture",
        };
        greeting["engine"] = json_t::object();
        greeting["engine"]["version"] = std::string(gVersionInfoFull);
        greeting["sequence"] = 0;
        greeting["tick"] = tick;
        greeting["paused"] = paused;
        return greeting.dump();
    }

    bool NativeMonitor::ParseRequest(
        std::string_view payload, NativeMonitorRequest& request, std::string& code, std::string& message)
    {
        request = {};
        if (payload.empty() || payload.size() > kMaxFrameBytes)
        {
            code = "frame_bounds";
            message = "request frame is empty or exceeds the bounded payload size";
            return false;
        }

        json_t value;
        try
        {
            value = json_t::parse(payload);
        }
        catch (const json_t::exception& error)
        {
            code = "invalid_json";
            message = ErrorMessage(code, error.what());
            return false;
        }
        if (!value.is_object())
        {
            code = "invalid_request";
            message = "request must be a JSON object";
            return false;
        }
        const auto id = value.find("id");
        if (id != value.end() && id->is_number_unsigned() && id->get<uint64_t>() > 0)
            request.id = id->get<uint64_t>();
        if (value.value("type", "") != "request")
        {
            code = "invalid_request";
            message = "request type must be request";
            return false;
        }
        if (request.id == 0)
        {
            code = "invalid_request";
            message = "request id must be a positive integer";
            return false;
        }
        const auto method = value.find("method");
        if (method == value.end() || !method->is_string())
        {
            code = "invalid_request";
            message = "request method must be a string";
            return false;
        }
        request.method = method->get<std::string>();
        if (request.method != "ping" && request.method != "status" && request.method != "step"
            && request.method != "stop" && request.method != "resource.list" && request.method != "resource.describe"
            && request.method != "resource.read" && request.method != "action.list" && request.method != "action.describe"
            && request.method != "action.query" && request.method != "action.execute" && request.method != "save"
            && request.method != "record.start" && request.method != "record.status" && request.method != "record.stop"
            && request.method != "capture")
        {
            code = "unknown_method";
            message = "monitor method is not advertised";
            return false;
        }

        const auto params = value.find("params");
        if (params != value.end() && !params->is_object())
        {
            code = "invalid_request";
            message = "request params must be an object";
            return false;
        }
        request.params = params == value.end() ? json_t::object() : *params;
        if (request.method == "step")
        {
            if (params == value.end() || !params->is_object())
            {
                code = "invalid_request";
                message = "step requires an object params value";
                return false;
            }
            const auto ticks = params->find("ticks");
            if (ticks == params->end() || !ticks->is_number_unsigned())
            {
                code = "invalid_ticks";
                message = "step ticks must be a non-negative integer";
                return false;
            }
            const auto requested = ticks->get<uint64_t>();
            if (requested > kMaxStepTicks)
            {
                code = "invalid_ticks";
                message = "step ticks exceed the bounded request limit";
                return false;
            }
            request.ticks = static_cast<uint32_t>(requested);
        }
        return true;
    }

    bool NativeMonitor::SendSuccess(uint64_t id, uint64_t sequence, uint32_t tick, bool paused, const json_t& result)
    {
        json_t response = StateFields(sequence, tick, paused);
        response["type"] = "response";
        response["id"] = id;
        response["ok"] = true;
        response["result"] = result;
        return SendPayload(response.dump());
    }

    bool NativeMonitor::SendError(
        uint64_t id, uint64_t sequence, uint32_t tick, bool paused, std::string_view code,
        std::string_view message, const json_t& detail)
    {
        json_t response = StateFields(sequence, tick, paused);
        response["type"] = "response";
        response["id"] = id == 0 ? json_t(nullptr) : json_t(id);
        response["ok"] = false;
        response["error"] = {
            { "code", code },
            { "message", message },
            { "detail", detail },
        };
        return SendPayload(response.dump());
    }

    bool NativeMonitor::SendPayload(std::string_view payload)
    {
        const auto frame = EncodeFrame(payload);
        if (frame.empty())
        {
            MarkLost();
            return false;
        }
        std::lock_guard writeLock(_writeMutex);
        if (_descriptor < 0 || !WriteAll(_descriptor, frame.data(), frame.size()))
        {
            MarkLost();
            return false;
        }
        {
            std::lock_guard stateLock(_stateMutex);
            _waitingForResponse = false;
        }
        _responseCondition.notify_all();
        return true;
    }

    void NativeMonitor::MarkLost()
    {
        {
            std::lock_guard lock(_stateMutex);
            _lost = true;
            _waitingForResponse = false;
        }
        _responseCondition.notify_all();
    }

    void NativeMonitor::ReadLoop()
    {
        for (;;)
        {
            uint8_t header[4]{};
            if (!ReadExact(_descriptor, header, sizeof(header)))
                break;
            const uint32_t length = (static_cast<uint32_t>(header[0]) << 24)
                | (static_cast<uint32_t>(header[1]) << 16) | (static_cast<uint32_t>(header[2]) << 8)
                | static_cast<uint32_t>(header[3]);
            if (length == 0 || length > kMaxFrameBytes)
            {
                SendError(0, 0, 0, true, "frame_bounds", "request frame exceeds the bounded payload size");
                break;
            }
            std::string payload(length, '\0');
            if (!ReadExact(_descriptor, payload.data(), payload.size()))
                break;

            NativeMonitorRequest request;
            std::string code;
            std::string message;
            if (!ParseRequest(payload, request, code, message))
            {
                SendError(request.id, 0, 0, true, code, message);
                break;
            }

            std::unique_lock stateLock(_stateMutex);
            _responseCondition.wait(stateLock, [this] { return !_waitingForResponse || _closing || _lost; });
            if (_closing || _lost)
                break;
            _requests.push_back(std::move(request));
            _waitingForResponse = true;
            stateLock.unlock();
            _responseCondition.notify_all();
        }
        MarkLost();
    }
}
