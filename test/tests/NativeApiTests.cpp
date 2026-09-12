/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include <gtest/gtest.h>
#include <openrct2/command_line/NativeRegistry.h>

#include <set>
#include <string>
#include <vector>

using namespace OpenRCT2::CommandLine;
using json_t = nlohmann::json;

TEST(NativeApiRegistry, HasOneExactInitialResourcePopulation)
{
    const std::vector<std::string> expected{
        "session", "park", "date", "finance", "rides", "ride", "guests", "guest", "objects", "tile", "region",
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
        EXPECT_TRUE(NativeActionDescriptorJson(descriptor).contains("policy"));
    }
}

TEST(NativeApiPolicy, UniversalFlagsAreNotCallerControlledAndSavePathsAreContained)
{
    EXPECT_TRUE(NativePathContained("/tmp/parkbench-owned", "/tmp/parkbench-owned/save.park"));
    EXPECT_FALSE(NativePathContained("/tmp/parkbench-owned", "/tmp/parkbench-owned-other/save.park"));
    EXPECT_FALSE(NativePathContained("/tmp/parkbench-owned", "/tmp/save.park"));
}
