/*****************************************************************************/
/* Copyright (c) 2014-2026 OpenRCT2 developers                              */
/*                                                                           */
/* For a complete list of all authors, please refer to contributors.md       */
/* OpenRCT2 is licensed under the GNU General Public License version 3.      */
/*****************************************************************************/

#pragma once

#include "../Identifiers.h"
#include "../drawing/ImageId.hpp"
#include "../world/Location.hpp"

#include <cstdint>
#include <vector>

namespace OpenRCT2
{
    /**
     * Renderer-only evidence for joining a paint request to the draw request
     * that consumed it. It deliberately contains no validity or duplication
     * verdict. A logical source may emit several components for one frame.
     */
    struct RideRenderDiagnosticSource
    {
        static constexpr uint16_t kNull = UINT16_MAX;

        CoordsXY mapPosition{};
        uint16_t ride = kNull;
        uint16_t station = kNull;
        uint16_t trackType = kNull;
        uint16_t elementType = kNull;
        uint16_t entity = kNull;
        uint8_t trackSequence = 0;
        uint8_t direction = 0;
        uint8_t entityType = 0;

        bool operator==(const RideRenderDiagnosticSource&) const = default;
    };

    class RideRenderDiagnostic
    {
    public:
        enum class Phase : uint8_t
        {
            paint,
            draw,
        };

        enum class Component : uint8_t
        {
            parent,
            child,
            attached,
        };

        struct Record
        {
            Phase phase;
            Component component;
            RideRenderDiagnosticSource source;
            uint32_t componentOrdinal;
            ImageId image;
            ScreenCoordsXY screenPosition;
        };

        uint32_t RecordPaint(
            RideRenderDiagnosticSource source, Component component, ImageId image, const ScreenCoordsXY& screenPosition);
        void RecordDraw(
            RideRenderDiagnosticSource source, Component component, uint32_t componentOrdinal, ImageId image,
            const ScreenCoordsXY& screenPosition);

        const std::vector<Record>& Records() const
        {
            return _records;
        }

        void Clear()
        {
            _records.clear();
        }

    private:
        uint32_t NextComponentOrdinal(const RideRenderDiagnosticSource& source) const;
        std::vector<Record> _records;
    };
} // namespace OpenRCT2
