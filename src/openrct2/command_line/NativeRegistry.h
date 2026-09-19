#pragma once

#include "../core/Json.hpp"

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenRCT2
{
    struct GameState_t;

    namespace CommandLine
    {
        struct NativeResourceDescriptor
        {
            const char* name;
            const char* description;
            json_t schema;
            json_t units;
            const char* authority;
            std::function<json_t(const json_t&, GameState_t&)> read;
        };

        struct NativeActionDescriptor
        {
            uint32_t id;
            const char* name;
            std::string description;
            json_t schema;
            json_t units;
        };

        struct NativeDispatchResult
        {
            bool ok = false;
            json_t value = json_t::object();
            std::string code;
            std::string message;
            json_t detail = json_t::object();
        };

        const std::array<NativeResourceDescriptor, 11>& NativeResources();
        std::vector<NativeActionDescriptor> NativeActions();
        json_t NativeResourceDescriptorJson(const NativeResourceDescriptor& descriptor);
        json_t NativeActionDescriptorJson(const NativeActionDescriptor& descriptor);

        NativeDispatchResult ReadNativeResource(std::string_view name, const json_t& args, GameState_t& state);
        NativeDispatchResult QueryNativeAction(
            std::string_view name, const json_t& args, GameState_t& state);
        NativeDispatchResult ExecuteNativeAction(
            std::string_view name, const json_t& args, GameState_t& state);

        NativeDispatchResult SaveNativeGame(std::string_view path, GameState_t& state);
        bool NativePathContained(std::string_view root, std::string_view path);
        void SetNativeSaveRoot(std::string root);
        void SetNativeRecordingRoot(std::string root);
        void SetNativeCaptureRoot(std::string root);
        NativeDispatchResult StartNativeRecording(std::string_view path);
        NativeDispatchResult StopNativeRecording();
        NativeDispatchResult CaptureNativeFrame(
            std::string_view path, const json_t& view = json_t(nullptr));
        NativeDispatchResult ValidateNativeCaptureView(const json_t& view, int32_t mapWidth, int32_t mapHeight);
        NativeDispatchResult ValidateNativeCaptureView(const json_t& view, const GameState_t& state);
    }
}
