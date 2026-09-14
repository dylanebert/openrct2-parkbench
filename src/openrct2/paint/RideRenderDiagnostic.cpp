/*****************************************************************************/
/* Copyright (c) 2014-2026 OpenRCT2 developers                              */
/*                                                                           */
/* For a complete list of all authors, please refer to contributors.md       */
/* OpenRCT2 is licensed under the GNU General Public License version 3.      */
/*****************************************************************************/

#include "RideRenderDiagnostic.h"

namespace OpenRCT2
{
    uint32_t RideRenderDiagnostic::NextComponentOrdinal(const RideRenderDiagnosticSource& source) const
    {
        uint32_t ordinal = 0;
        for (const auto& record : _records)
        {
            if (record.phase == Phase::paint && record.source == source)
            {
                ++ordinal;
            }
        }
        return ordinal;
    }

    uint32_t RideRenderDiagnostic::RecordPaint(
        const RideRenderDiagnosticSource source, const Component component, const ImageId image,
        const ScreenCoordsXY& screenPosition)
    {
        const auto ordinal = NextComponentOrdinal(source);
        if (_records.size() >= kMaxRecords)
        {
            _recordsTruncated = true;
            return ordinal;
        }
        _records.push_back({ Phase::paint, component, source, ordinal, image, screenPosition });
        return ordinal;
    }

    void RideRenderDiagnostic::RecordDraw(
        const RideRenderDiagnosticSource source, const Component component, const uint32_t componentOrdinal,
        const ImageId image, const ScreenCoordsXY& screenPosition)
    {
        if (_records.size() >= kMaxRecords)
        {
            _recordsTruncated = true;
            return;
        }
        _records.push_back({ Phase::draw, component, source, componentOrdinal, image, screenPosition });
    }
} // namespace OpenRCT2
