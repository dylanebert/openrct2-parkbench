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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace OpenRCT2
{
    struct G1Element;

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

    struct FlatRideSpriteSelection
    {
        RideRenderDiagnosticSource source;
        uint32_t componentOrdinal = 0;
        uint8_t flatRideAnimationFrame = 0;
        int16_t currentTime = 0;
        uint8_t substate = 0;
        uint8_t status = 0;
        uint8_t orientation = 0;
        uint8_t bodyColour = 0;
        uint8_t trimColour = 0;
        uint8_t imagePrimary = 0;
        uint8_t imageSecondary = 0;
        uint8_t orientationQuarter = 0;
        uint32_t baseImageIndex = 0;
        uint32_t imageOffset = 0;
        uint32_t selectedImageIndex = 0;
        std::string stableIdentity;
        uint16_t rideType = RideRenderDiagnosticSource::kNull;
        uint16_t rideEntry = RideRenderDiagnosticSource::kNull;
        uint16_t trackStyle = RideRenderDiagnosticSource::kNull;
        uint16_t trackType = RideRenderDiagnosticSource::kNull;
    };

    enum class FlatRideSelectionRecordResult : uint8_t
    {
        recorded,
        sourceComponentMissing,
        duplicateComponent,
    };

    using EnterpriseSpriteSelection = FlatRideSpriteSelection;
    using EnterpriseSelectionRecordResult = FlatRideSelectionRecordResult;

    enum class FlatRideSelectionAttemptReason : uint8_t
    {
        recorded,
        rideEntryMissing,
        notOnTrack,
        vehicleMissing,
        diagnosticUnavailable,
        paintStructMissing,
        sourceComponentMissing,
        recordRejected,
    };

    struct FlatRideSelectionAttempt
    {
        FlatRideSelectionAttemptReason reason = FlatRideSelectionAttemptReason::recordRejected;
        uint16_t rideType = RideRenderDiagnosticSource::kNull;
        uint16_t rideEntry = RideRenderDiagnosticSource::kNull;
        uint16_t trackStyle = RideRenderDiagnosticSource::kNull;
        uint16_t trackType = RideRenderDiagnosticSource::kNull;
        bool dispatcherReached = false;
        bool rideEntryFound = false;
        bool onTrack = false;
        bool vehicleFound = false;
        bool diagnosticAvailable = false;
        bool paintStructFound = false;
        bool sourceComponentJoined = false;
        bool recordCalled = false;
        bool recordAccepted = false;
        FlatRideSpriteSelection selection;
    };

    class RideRenderDiagnostic
    {
    public:
        static constexpr size_t kMaxRecords = 4096;
        static constexpr size_t kMaxFlatRideSelectionAttempts = 4096;
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
            // Empty only when the engine cannot resolve the image to sprite content.
            std::string stableIdentity;
            ScreenCoordsXY screenPosition;
        };

        // Hashes authoritative sprite metadata and encoded content, never the process-local image index.
        static std::string StableSpriteIdentity(const G1Element& sprite);
        static std::string StableSpriteIdentity(ImageId image);

        uint32_t RecordPaint(
            RideRenderDiagnosticSource source, Component component, ImageId image, const ScreenCoordsXY& screenPosition);
        void RecordDraw(
            RideRenderDiagnosticSource source, Component component, uint32_t componentOrdinal, ImageId image,
            const ScreenCoordsXY& screenPosition);
        // Records raw flat-ride selection only when it joins an emitted parent
        // paint record. This is evidence, not a renderer verdict.
        FlatRideSelectionRecordResult TryRecordFlatRideSelection(FlatRideSpriteSelection selection);
        EnterpriseSelectionRecordResult TryRecordEnterpriseSelection(EnterpriseSpriteSelection selection);
        bool RecordFlatRideSelection(FlatRideSpriteSelection selection);
        bool RecordEnterpriseSelection(EnterpriseSpriteSelection selection);
        bool HasPaintRecord(const RideRenderDiagnosticSource& source, uint32_t componentOrdinal) const;
        void RecordFlatRideSelectionAttempt(FlatRideSelectionAttempt attempt);
        const std::vector<FlatRideSpriteSelection>& FlatRideSelections() const
        {
            return _flatRideSelections;
        }

        const std::vector<FlatRideSelectionAttempt>& FlatRideSelectionAttempts() const
        {
            return _flatRideSelectionAttempts;
        }

        bool FlatRideAttemptsTruncated() const
        {
            return _flatRideAttemptsTruncated;
        }

        const std::vector<Record>& Records() const
        {
            return _records;
        }

        bool RecordsTruncated() const
        {
            return _recordsTruncated;
        }

        void Clear()
        {
            _records.clear();
            _flatRideSelections.clear();
            _flatRideSelectionAttempts.clear();
            _recordsTruncated = false;
            _flatRideAttemptsTruncated = false;
        }

    private:
        uint32_t NextComponentOrdinal(const RideRenderDiagnosticSource& source) const;
        std::vector<Record> _records;
        std::vector<FlatRideSpriteSelection> _flatRideSelections;
        std::vector<FlatRideSelectionAttempt> _flatRideSelectionAttempts;
        bool _recordsTruncated = false;
        bool _flatRideAttemptsTruncated = false;
    };
} // namespace OpenRCT2
