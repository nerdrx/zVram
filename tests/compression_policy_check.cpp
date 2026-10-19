#include "compression_policy.hpp"
#include <cstdint>
#include <iostream>
#include <limits>

static_assert(retainCompression(100, 99, 0));
static_assert(retainCompression(100, 95, 5));
static_assert(!retainCompression(100, 96, 5));
static_assert(retainCompression(101, 95, 5));
static_assert(!retainCompression(101, 96, 5));
static_assert(!retainCompression(100, 1, 100));
static_assert(!retainCompression(0, 0, 0));
static_assert(!retainCompression(100, 101, 0));
constexpr auto maxBytes = std::numeric_limits<std::uint64_t>::max();
constexpr auto required = (maxBytes / 100) * 5 + ((maxBytes % 100) * 5 + 99) / 100;
static_assert(retainCompression(maxBytes, maxBytes - required, 5));
static_assert(!retainCompression(maxBytes, maxBytes - required + 1, 5));

int main() {
    // Compare against direct arithmetic where multiplication is known to fit.
    for (std::uint64_t raw = 1; raw <= 512; ++raw)
        for (std::uint64_t stored = 0; stored <= raw + 1; ++stored)
            for (unsigned percent = 0; percent <= 100; ++percent) {
                const bool expected = stored < raw && percent < 100 &&
                    (raw - stored) * 100 >= raw * percent;
                if (retainCompression(raw, stored, percent) != expected) return 1;
            }
    std::cout << "COMPRESSION_POLICY_OK including uint64 boundary checks\n";
}
