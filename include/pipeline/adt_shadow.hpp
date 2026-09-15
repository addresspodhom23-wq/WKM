#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wowee::pipeline {

inline constexpr std::size_t kMCSHPackedBytes = 512;
inline constexpr std::size_t kMCSHTexels = 64 * 64;

/// Decode the Vanilla 1.12 MCSH channel exactly as the retail lookup table:
/// bit order is LSB-first, an unset bit is 0xff (full direct light), and a set
/// bit is 0x00 (baked shadow). Missing/truncated input stays fully lit.
inline bool decodeMCSHShadowChannel(std::span<const uint8_t> packed,
                                    std::array<uint8_t, kMCSHTexels>& out) {
    out.fill(0xff);
    if (packed.size() < kMCSHPackedBytes) return false;

    for (std::size_t i = 0; i < kMCSHTexels; ++i) {
        const uint8_t bit =
            static_cast<uint8_t>((packed[i >> 3] >> (i & 7u)) & 1u);
        out[i] = bit ? 0x00 : 0xff;
    }
    return true;
}

} // namespace wowee::pipeline
