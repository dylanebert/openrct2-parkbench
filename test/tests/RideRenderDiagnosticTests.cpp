/*****************************************************************************/
/* Copyright (c) 2014-2026 OpenRCT2 developers                              */
/*                                                                           */
/* For a complete list of all authors, please refer to contributors.md       */
/* OpenRCT2 is licensed under the GNU General Public License version 3.      */
/*****************************************************************************/

#include <gtest/gtest.h>

#include <array>
#include <utility>
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

TEST(RideRenderDiagnostic, EnterpriseBoundariesExposeAdjacentSelectionAndRejectCorruptJoins)
{
    const auto oldNoGraphics = gOpenRCT2NoGraphics;
    gOpenRCT2NoGraphics = false;

    std::array<uint8_t, 4> pixels{ 1, 2, 3, 4 };
    G1Element sprite{};
    sprite.offset = pixels.data();
    sprite.width = 2;
    sprite.height = 2;
    const auto base = static_cast<ImageIndex>(SPR_IMAGE_LIST_BEGIN + 1000);
    for (uint32_t offset : { 40u, 48u, 52u, 60u })
        GfxSetG1Element(static_cast<ImageIndex>(base + offset), &sprite);

    const auto body = static_cast<Drawing::Colour>(28);
    const auto trim = static_cast<Drawing::Colour>(2);
    RideRenderDiagnostic diagnostic;
    RideRenderDiagnosticSource source;
    source.ride = 0;
    source.entity = 1;

    auto add = [&](const uint8_t frame, const int16_t currentTime) {
        const auto offset = (static_cast<uint32_t>(frame) << 2) + 0;
        const auto image = ImageId(static_cast<ImageIndex>(base + offset), body, trim);
        const auto ordinal = diagnostic.RecordPaint(source, RideRenderDiagnostic::Component::parent, image, { 10, 20 });
        EnterpriseSpriteSelection selection;
        selection.source = source;
        selection.componentOrdinal = ordinal;
        selection.flatRideAnimationFrame = frame;
        selection.currentTime = currentTime;
        selection.status = 12;
        selection.orientation = 0;
        selection.bodyColour = static_cast<uint8_t>(body);
        selection.trimColour = static_cast<uint8_t>(trim);
        selection.imagePrimary = image.GetRemap();
        selection.imageSecondary = static_cast<uint8_t>(image.GetSecondary());
        selection.baseImageIndex = base;
        selection.imageOffset = offset;
        selection.selectedImageIndex = image.GetIndex();
        selection.stableIdentity = RideRenderDiagnostic::StableSpriteIdentity(image);
        return std::pair<EnterpriseSpriteSelection, uint32_t>{ selection, ordinal };
    };

    const auto frame12 = add(12, 239);
    const auto frame13 = add(13, 240);
    const auto frame15 = add(15, 227);
    const auto frame10 = add(10, 228);

    EXPECT_TRUE(diagnostic.RecordEnterpriseSelection(frame12.first));
    EXPECT_TRUE(diagnostic.RecordEnterpriseSelection(frame13.first));
    EXPECT_TRUE(diagnostic.RecordEnterpriseSelection(frame15.first));
    EXPECT_TRUE(diagnostic.RecordEnterpriseSelection(frame10.first));
    ASSERT_EQ(diagnostic.EnterpriseSelections().size(), 4u);
    EXPECT_EQ(diagnostic.EnterpriseSelections()[1].imageOffset - diagnostic.EnterpriseSelections()[0].imageOffset, 4u);
    EXPECT_EQ(diagnostic.EnterpriseSelections()[2].imageOffset - diagnostic.EnterpriseSelections()[3].imageOffset, 20u);
    EXPECT_EQ(diagnostic.EnterpriseSelections()[0].source, diagnostic.EnterpriseSelections()[1].source);
    EXPECT_EQ(diagnostic.EnterpriseSelections()[0].bodyColour, diagnostic.EnterpriseSelections()[1].bodyColour);
    EXPECT_EQ(diagnostic.EnterpriseSelections()[0].trimColour, diagnostic.EnterpriseSelections()[1].trimColour);

    auto missingAnimation = frame12.first;
    missingAnimation.imageOffset = 0;
    EXPECT_FALSE(diagnostic.RecordEnterpriseSelection(missingAnimation));
    auto mutatedRemap = frame12.first;
    mutatedRemap.imagePrimary++;
    EXPECT_FALSE(diagnostic.RecordEnterpriseSelection(mutatedRemap));
    auto mutatedIdentity = frame12.first;
    mutatedIdentity.stableIdentity[0] = mutatedIdentity.stableIdentity[0] == '0' ? '1' : '0';
    EXPECT_FALSE(diagnostic.RecordEnterpriseSelection(mutatedIdentity));
    auto duplicateComponent = frame12.first;
    EXPECT_FALSE(diagnostic.RecordEnterpriseSelection(duplicateComponent));
    auto wrongSource = frame12.first;
    wrongSource.source.entity = 2;
    EXPECT_FALSE(diagnostic.RecordEnterpriseSelection(wrongSource));

    gOpenRCT2NoGraphics = oldNoGraphics;
}
