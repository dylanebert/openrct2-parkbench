/*****************************************************************************/
/* Copyright (c) 2014-2026 OpenRCT2 developers                              */
/*                                                                           */
/* For a complete list of all authors, please refer to contributors.md       */
/* OpenRCT2 is licensed under the GNU General Public License version 3.      */
/*****************************************************************************/

#include "RideRenderDiagnostic.h"

#include "../core/Crypt.h"
#include "../core/String.hpp"
#include "../drawing/Drawing.Sprite.h"

#include <algorithm>
#include <string_view>
#include <type_traits>
#include <vector>

namespace OpenRCT2
{
    namespace
    {
        template<typename T>
        void AppendLittleEndian(std::vector<uint8_t>& bytes, const T value)
        {
            using Unsigned = std::make_unsigned_t<T>;
            auto unsignedValue = static_cast<Unsigned>(value);
            for (size_t i = 0; i < sizeof(T); ++i)
            {
                bytes.push_back(static_cast<uint8_t>(unsignedValue >> (i * 8)));
            }
        }
    } // namespace

    std::string RideRenderDiagnostic::StableSpriteIdentity(const G1Element& sprite)
    {
        const auto dataSize = G1CalculateDataSize(&sprite);
        if (dataSize != 0 && sprite.offset == nullptr)
        {
            return {};
        }

        std::vector<uint8_t> canonical;
        canonical.reserve(32 + dataSize);
        constexpr std::string_view kIdentityVersion = "openrct2-sprite-identity-v1";
        canonical.insert(canonical.end(), kIdentityVersion.begin(), kIdentityVersion.end());
        AppendLittleEndian(canonical, sprite.width);
        AppendLittleEndian(canonical, sprite.height);
        AppendLittleEndian(canonical, sprite.xOffset);
        AppendLittleEndian(canonical, sprite.yOffset);
        AppendLittleEndian(canonical, sprite.flags.holder);
        AppendLittleEndian(canonical, static_cast<uint32_t>(dataSize));
        if (dataSize != 0)
        {
            canonical.insert(canonical.end(), sprite.offset, sprite.offset + dataSize);
        }
        return String::StringFromHex(Crypt::SHA256(canonical.data(), canonical.size()));
    }

    std::string RideRenderDiagnostic::StableSpriteIdentity(const ImageId image)
    {
        const auto* sprite = GfxGetG1Element(image);
        return sprite == nullptr ? std::string{} : StableSpriteIdentity(*sprite);
    }

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
        _records.push_back({ Phase::paint, component, source, ordinal, image, StableSpriteIdentity(image), screenPosition });
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
        _records.push_back(
            { Phase::draw, component, source, componentOrdinal, image, StableSpriteIdentity(image), screenPosition });
    }

    bool RideRenderDiagnostic::RecordEnterpriseSelection(const EnterpriseSpriteSelection selection)
    {
        for (const auto& existing : _enterpriseSelections)
        {
            if (existing.source == selection.source && existing.componentOrdinal == selection.componentOrdinal)
                return false;
        }

        const auto record = std::find_if(_records.begin(), _records.end(), [&selection](const auto& candidate) {
            return candidate.phase == Phase::paint && candidate.source == selection.source
                && candidate.componentOrdinal == selection.componentOrdinal;
        });
        // The paint record is the authoritative source/component join. The
        // selected ImageId is deliberately retained as raw engine evidence:
        // station remaps and palette resolution can differ from the
        // diagnostic image record without changing which paint call emitted
        // the Enterprise parent. Do not silently erase the selection at the
        // exact palette-cliff ticks this diagnostic exists to explain.
        if (record == _records.end())
            return false;

        _enterpriseSelections.push_back(selection);
        return true;
    }
} // namespace OpenRCT2
