/*****************************************************************************/
/* Copyright (c) 2014-2026 OpenRCT2 developers                              */
/*                                                                           */
/* For a complete list of all authors, please refer to contributors.md       */
/* OpenRCT2 is licensed under the GNU General Public License version 3.      */
/*****************************************************************************/

#include <gtest/gtest.h>

#include <openrct2/paint/RideRenderDiagnostic.h>

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
