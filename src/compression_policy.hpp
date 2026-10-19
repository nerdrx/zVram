#pragma once
#include <cstdint>

// Require an exact minimum saving without overflowing byte-size arithmetic.
constexpr bool retainCompression(std::uint64_t raw, std::uint64_t stored, unsigned percent) {
    if (!raw || stored >= raw || percent >= 100) return false;
    const auto required = (raw / 100) * percent + ((raw % 100) * percent + 99) / 100;
    return raw - stored >= required;
}
