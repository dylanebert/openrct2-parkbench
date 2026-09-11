/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "Crypt.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace OpenRCT2::Crypt;

#if defined(DISABLE_NETWORK)
class OpenRCT2SHA256Algorithm final : public Sha256Algorithm
{
private:
    static constexpr uint32_t kRoundConstants[] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25,  0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    uint32_t _state[8]{};
    uint8_t _buffer[64]{};
    size_t _bufferLength{};
    uint64_t _bitLength{};

    static uint32_t RotateRight(uint32_t value, uint32_t amount)
    {
        return (value >> amount) | (value << (32 - amount));
    }

    static uint32_t BigEndian32(const uint8_t* bytes)
    {
        return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16)
            | (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
    }

    void ProcessBlock(const uint8_t* block)
    {
        uint32_t words[64]{};
        for (size_t i = 0; i < 16; i++)
        {
            words[i] = BigEndian32(block + (i * 4));
        }
        for (size_t i = 16; i < 64; i++)
        {
            const auto s0 = RotateRight(words[i - 15], 7) ^ RotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto s1 = RotateRight(words[i - 2], 17) ^ RotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        auto a = _state[0];
        auto b = _state[1];
        auto c = _state[2];
        auto d = _state[3];
        auto e = _state[4];
        auto f = _state[5];
        auto g = _state[6];
        auto h = _state[7];
        for (size_t i = 0; i < 64; i++)
        {
            const auto s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + ch + kRoundConstants[i] + words[i];
            const auto s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        _state[0] += a;
        _state[1] += b;
        _state[2] += c;
        _state[3] += d;
        _state[4] += e;
        _state[5] += f;
        _state[6] += g;
        _state[7] += h;
    }

public:
    OpenRCT2SHA256Algorithm()
    {
        Clear();
    }

    Sha256Algorithm* Clear() override
    {
        _state[0] = 0x6a09e667;
        _state[1] = 0xbb67ae85;
        _state[2] = 0x3c6ef372;
        _state[3] = 0xa54ff53a;
        _state[4] = 0x510e527f;
        _state[5] = 0x9b05688c;
        _state[6] = 0x1f83d9ab;
        _state[7] = 0x5be0cd19;
        _bufferLength = 0;
        _bitLength = 0;
        return this;
    }

    Sha256Algorithm* Update(const void* data, size_t dataLen) override
    {
        auto bytes = static_cast<const uint8_t*>(data);
        _bitLength += static_cast<uint64_t>(dataLen) * 8;
        while (dataLen > 0)
        {
            const auto copyLength = dataLen < sizeof(_buffer) - _bufferLength ? dataLen : sizeof(_buffer) - _bufferLength;
            std::memcpy(_buffer + _bufferLength, bytes, copyLength);
            _bufferLength += copyLength;
            bytes += copyLength;
            dataLen -= copyLength;
            if (_bufferLength == sizeof(_buffer))
            {
                ProcessBlock(_buffer);
                _bufferLength = 0;
            }
        }
        return this;
    }

    Result Finish() override
    {
        const auto messageBitLength = _bitLength;
        _buffer[_bufferLength++] = 0x80;
        if (_bufferLength > 56)
        {
            std::memset(_buffer + _bufferLength, 0, sizeof(_buffer) - _bufferLength);
            ProcessBlock(_buffer);
            _bufferLength = 0;
        }
        std::memset(_buffer + _bufferLength, 0, 56 - _bufferLength);
        _bufferLength = 56;
        for (size_t i = 0; i < sizeof(messageBitLength); i++)
        {
            _buffer[63 - i] = static_cast<uint8_t>(messageBitLength >> (i * 8));
        }
        ProcessBlock(_buffer);

        Result result{};
        for (size_t i = 0; i < 8; i++)
        {
            result[i * 4] = static_cast<uint8_t>(_state[i] >> 24);
            result[i * 4 + 1] = static_cast<uint8_t>(_state[i] >> 16);
            result[i * 4 + 2] = static_cast<uint8_t>(_state[i] >> 8);
            result[i * 4 + 3] = static_cast<uint8_t>(_state[i]);
        }
        return result;
    }
};
#endif

class OpenRCT2FNV1aAlgorithm final : public FNV1aAlgorithm
{
private:
    static constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
    static constexpr uint64_t kPrime = 0x00000100000001B3ULL;

    uint64_t _data = kOffset;
    uint8_t _rem[8]{};
    size_t _remLen{};

    void ProcessRemainder()
    {
        if (_remLen > 0)
        {
            uint64_t temp{};
            std::memcpy(&temp, _rem, _remLen);
            _data ^= temp;
            _data *= kPrime;
            _remLen = 0;
        }
    }

public:
    HashAlgorithm* Clear() override
    {
        _data = kOffset;
        return this;
    }

    HashAlgorithm* Update(const void* data, size_t dataLen) override
    {
        if (dataLen == 0)
            return this;

        auto src = reinterpret_cast<const uint64_t*>(data);
        if (_remLen > 0)
        {
            // We have remainder, so fill rest of it with bytes from src
            auto fillLen = sizeof(uint64_t) - _remLen;
            assert(_remLen + fillLen <= sizeof(uint64_t));
            std::memcpy(_rem + _remLen, src, fillLen);
            src = reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(src) + fillLen);
            _remLen += fillLen;
            dataLen -= fillLen;
            ProcessRemainder();
        }

        // Process every block of 8 bytes
        while (dataLen >= sizeof(uint64_t))
        {
            auto temp = *src++;
            _data ^= temp;
            _data *= kPrime;
            dataLen -= sizeof(uint64_t);
        }

        // Store the remaining data (< 8 bytes)
        if (dataLen > 0)
        {
            _remLen = dataLen;
            std::memcpy(&_rem, src, dataLen);
        }
        return this;
    }

    Result Finish() override
    {
        ProcessRemainder();

        Result res;
        std::memcpy(res.data(), &_data, sizeof(_data));
        return res;
    }
};

namespace OpenRCT2::Crypt
{
    std::unique_ptr<FNV1aAlgorithm> CreateFNV1a()
    {
        return std::make_unique<OpenRCT2FNV1aAlgorithm>();
    }

#if defined(DISABLE_NETWORK)
    std::unique_ptr<Sha256Algorithm> CreateSHA256()
    {
        return std::make_unique<OpenRCT2SHA256Algorithm>();
    }
#endif
} // namespace OpenRCT2::Crypt
