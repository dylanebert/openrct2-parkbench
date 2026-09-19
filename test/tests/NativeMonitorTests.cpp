/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include <gtest/gtest.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/command_line/NativeMonitor.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <thread>

#ifndef _WIN32
    #include <sys/socket.h>
    #include <unistd.h>
#endif

using namespace OpenRCT2::CommandLine;
using json_t = nlohmann::json;

TEST(NativeMonitorClock, PausedZeroStepDoesNotAdvanceAndUnpausedStepRefuses)
{
    const auto originalPaused = gGamePaused;
    const auto originalTick = OpenRCT2::getGameState().currentTicks;
    gGamePaused = GAME_PAUSED_NORMAL;
    EXPECT_TRUE(OpenRCT2::gameStateAdvancePausedNativeMonitor(0));
    EXPECT_EQ(OpenRCT2::getGameState().currentTicks, originalTick);
    gGamePaused = 0;
    EXPECT_FALSE(OpenRCT2::gameStateAdvancePausedNativeMonitor(1));
    EXPECT_EQ(OpenRCT2::getGameState().currentTicks, originalTick);
    gGamePaused = originalPaused;
}

#ifndef _WIN32
TEST(NativeMonitorProtocol, ResponsesFollowAcceptedRequestOrderAndEofIsLoss)
{
    int descriptors[2]{};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
    NativeMonitor monitor(descriptors[0]);
    ASSERT_TRUE(monitor.Start(0, true));

    auto readFrame = [](int descriptor) -> json_t {
        uint8_t header[4]{};
        const auto headerSize = read(descriptor, header, sizeof(header));
        EXPECT_EQ(headerSize, 4);
        if (headerSize != 4)
            return json_t::object();
        const uint32_t length = (static_cast<uint32_t>(header[0]) << 24)
            | (static_cast<uint32_t>(header[1]) << 16) | (static_cast<uint32_t>(header[2]) << 8) | header[3];
        std::string payload(length, '\0');
        const auto payloadSize = read(descriptor, payload.data(), payload.size());
        EXPECT_EQ(payloadSize, static_cast<ssize_t>(payload.size()));
        if (payloadSize != static_cast<ssize_t>(payload.size()))
            return json_t::object();
        return json_t::parse(payload);
    };

    const auto greeting = readFrame(descriptors[1]);
    EXPECT_EQ(greeting["type"], "greeting");

    const auto requestOne = NativeMonitor::EncodeFrame(
        R"({"type":"request","id":9,"method":"ping","params":{}})");
    const auto requestTwo = NativeMonitor::EncodeFrame(
        R"({"type":"request","id":10,"method":"status","params":{}})");
    ASSERT_EQ(write(descriptors[1], requestOne.data(), requestOne.size()), static_cast<ssize_t>(requestOne.size()));

    std::optional<NativeMonitorRequest> received;
    for (int i = 0; i < 100 && !received.has_value(); ++i)
    {
        received = monitor.TakeRequest();
        if (!received.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->id, 9u);
    ASSERT_TRUE(monitor.SendSuccess(9, 1, 0, true, json_t{ { "pong", true } }));
    EXPECT_EQ(readFrame(descriptors[1])["id"], 9);

    ASSERT_EQ(write(descriptors[1], requestTwo.data(), requestTwo.size()), static_cast<ssize_t>(requestTwo.size()));
    received.reset();
    for (int i = 0; i < 100 && !received.has_value(); ++i)
    {
        received = monitor.TakeRequest();
        if (!received.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->id, 10u);
    ASSERT_TRUE(monitor.SendError(10, 2, 0, true, "rejected", "synthetic rejection", json_t::object()));
    const auto error = readFrame(descriptors[1]);
    EXPECT_FALSE(error["ok"]);
    EXPECT_EQ(error["error"]["code"], "rejected");
    EXPECT_EQ(error["sequence"], 2);

    close(descriptors[1]);
    for (int i = 0; i < 100 && !monitor.Lost(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(monitor.Lost());
}
#endif
