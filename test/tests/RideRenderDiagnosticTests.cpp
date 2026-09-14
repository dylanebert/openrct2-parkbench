/*****************************************************************************/
/* Copyright (c) 2014-2026 OpenRCT2 developers                              */
/*                                                                           */
/* For a complete list of all authors, please refer to contributors.md       */
/* OpenRCT2 is licensed under the GNU General Public License version 3.      */
/*****************************************************************************/

#include <gtest/gtest.h>

#include <array>
#include <openrct2/OpenRCT2.h>
#include <openrct2/drawing/Drawing.Sprite.h>
#include <openrct2/paint/RideRenderDiagnostic.h>
#include <openrct2/SpriteIds.h>

using namespace OpenRCT2;

TEST(RideRenderDiagnostic, RecordsAreBoundedAndReportTruncation)
{
    RideRenderDiagnostic diagnostic;
    RideRenderDiagnosticSource source;

    for (size_t i = 0; i < RideRenderDiagnostic::kMaxRecords + 1; ++i)
    {
        diagnostic.RecordPaint(source, RideRenderDiagnostic::Component::parent, ImageId(100), { 10, 20 });
    }

    EXPECT_EQ(diagnostic.Records().size(), RideRenderDiagnostic::kMaxRecords);
    EXPECT_TRUE(diagnostic.RecordsTruncated());

    diagnostic.Clear();
    EXPECT_TRUE(diagnostic.Records().empty());
    EXPECT_FALSE(diagnostic.RecordsTruncated());
}

TEST(RideRenderDiagnostic, StableIdentityIgnoresProcessLocalIndexAndTracksContent)
{
    const auto oldNoGraphics = gOpenRCT2NoGraphics;
    gOpenRCT2NoGraphics = false;

    std::array<uint8_t, 4> pixels{ 1, 2, 3, 4 };
    G1Element sprite{};
    sprite.offset = pixels.data();
    sprite.width = 2;
    sprite.height = 2;
    const auto firstIndex = static_cast<ImageIndex>(SPR_IMAGE_LIST_BEGIN + 123);
    const auto secondIndex = static_cast<ImageIndex>(SPR_IMAGE_LIST_BEGIN + 456);
    GfxSetG1Element(firstIndex, &sprite);
    GfxSetG1Element(secondIndex, &sprite);

    RideRenderDiagnostic diagnostic;
    RideRenderDiagnosticSource source;
    diagnostic.RecordPaint(source, RideRenderDiagnostic::Component::parent, ImageId(firstIndex), { 10, 20 });
    diagnostic.RecordPaint(source, RideRenderDiagnostic::Component::parent, ImageId(secondIndex), { 10, 20 });

    ASSERT_EQ(diagnostic.Records().size(), 2u);
    EXPECT_NE(firstIndex, secondIndex);
    EXPECT_EQ(diagnostic.Records()[0].image.GetIndex(), firstIndex);
    EXPECT_EQ(diagnostic.Records()[1].image.GetIndex(), secondIndex);
    EXPECT_FALSE(diagnostic.Records()[0].stableIdentity.empty());
    EXPECT_EQ(diagnostic.Records()[0].stableIdentity, diagnostic.Records()[1].stableIdentity);

    pixels[0] = 5;
    diagnostic.RecordPaint(source, RideRenderDiagnostic::Component::parent, ImageId(secondIndex), { 10, 20 });
    ASSERT_EQ(diagnostic.Records().size(), 3u);
    EXPECT_NE(diagnostic.Records()[1].stableIdentity, diagnostic.Records()[2].stableIdentity);

    gOpenRCT2NoGraphics = oldNoGraphics;
}

TEST(RideRenderDiagnostic, CompositeComponentsKeepOneLogicalSource)
{
    RideRenderDiagnostic diagnostic;
    RideRenderDiagnosticSource source;
    source.mapPosition = { 64, 96 };
    source.ride = 7;
    source.station = 0;
    source.elementType = 2;
    source.trackType = 0;
    source.trackSequence = 1;
    source.direction = 2;

    const auto parent = diagnostic.RecordPaint(source, RideRenderDiagnostic::Component::parent, ImageId(100), { 10, 20 });
    const auto child = diagnostic.RecordPaint(source, RideRenderDiagnostic::Component::child, ImageId(101), { 11, 21 });
    diagnostic.RecordDraw(source, RideRenderDiagnostic::Component::parent, parent, ImageId(100), { 10, 20 });
    diagnostic.RecordDraw(source, RideRenderDiagnostic::Component::child, child, ImageId(101), { 11, 21 });

    ASSERT_EQ(diagnostic.Records().size(), 4u);
    EXPECT_EQ(diagnostic.Records()[0].source, diagnostic.Records()[1].source);
    EXPECT_EQ(diagnostic.Records()[0].componentOrdinal, 0u);
    EXPECT_EQ(diagnostic.Records()[1].componentOrdinal, 1u);
    EXPECT_EQ(diagnostic.Records()[2].phase, RideRenderDiagnostic::Phase::draw);
    EXPECT_EQ(diagnostic.Records()[2].componentOrdinal, diagnostic.Records()[0].componentOrdinal);
    EXPECT_EQ(diagnostic.Records()[3].componentOrdinal, diagnostic.Records()[1].componentOrdinal);
}
