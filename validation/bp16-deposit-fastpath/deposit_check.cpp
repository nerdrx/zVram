#include <cstdint>
#include <iostream>

static unsigned popcount16(std::uint16_t value) {
    unsigned count = 0;
    for (; value; value &= static_cast<std::uint16_t>(value - 1)) ++count;
    return count;
}

static unsigned firstbitlow(std::uint16_t value) {
    unsigned bit = 0;
    while (bit < 16 && !(value & (1u << bit))) ++bit;
    return bit;
}

static std::uint16_t oldDeposit(std::uint32_t gathered, std::uint16_t mask) {
    std::uint16_t value = 0;
    unsigned sourceBit = 0;
    for (unsigned bit = 0; bit < 16; ++bit) {
        if (mask & (1u << bit)) {
            value |= static_cast<std::uint16_t>(((gathered >> sourceBit) & 1u) << bit);
            ++sourceBit;
        }
    }
    return value;
}

static std::uint16_t fastDeposit(std::uint32_t gathered, std::uint16_t mask) {
    const unsigned bits = popcount16(mask);
    if (mask) {
        const unsigned shift = firstbitlow(mask);
        const std::uint32_t compactMask = (1u << bits) - 1u;
        if (mask == static_cast<std::uint16_t>(compactMask << shift))
            return static_cast<std::uint16_t>(gathered << shift);

        const std::uint16_t lowMask = mask & 0x7fffu;
        if ((mask & 0x8000u) && lowMask) {
            const unsigned lowBits = popcount16(lowMask);
            const unsigned lowShift = firstbitlow(lowMask);
            const std::uint32_t lowCompactMask = (1u << lowBits) - 1u;
            if (lowMask == static_cast<std::uint16_t>(lowCompactMask << lowShift))
                return static_cast<std::uint16_t>(
                    ((gathered & lowCompactMask) << lowShift) |
                    (((gathered >> lowBits) & 1u) << 15u));
        }
    }

    return oldDeposit(gathered, mask);
}

int main() {
    std::uint32_t random = 0x6d2b79f5u;
    unsigned contiguousMasks = 0;
    unsigned splitMasks = 0;
    for (std::uint32_t rawMask = 0; rawMask <= 0xffffu; ++rawMask) {
        const auto mask = static_cast<std::uint16_t>(rawMask);
        const unsigned bits = popcount16(mask);
        const std::uint32_t gatheredMask = bits == 16 ? 0xffffu : ((1u << bits) - 1u);
        const unsigned shift = firstbitlow(mask);
        const bool contiguous = mask && mask == static_cast<std::uint16_t>(gatheredMask << shift);
        const std::uint16_t lowMask = mask & 0x7fffu;
        const unsigned lowBits = popcount16(lowMask);
        const unsigned lowShift = firstbitlow(lowMask);
        const std::uint32_t lowCompactMask = (1u << lowBits) - 1u;
        const bool split = !contiguous && (mask & 0x8000u) && lowMask &&
            lowMask == static_cast<std::uint16_t>(lowCompactMask << lowShift);
        contiguousMasks += contiguous;
        splitMasks += split;

        const std::uint32_t fixed[] = {0u, gatheredMask, gatheredMask / 3u,
            0xa55au & gatheredMask, bits ? (1u << (bits - 1u)) : 0u};
        for (std::uint32_t gathered : fixed) {
            if (fastDeposit(gathered, mask) != oldDeposit(gathered, mask)) {
                std::cerr << "mismatch mask=0x" << std::hex << rawMask
                          << " gathered=0x" << gathered << std::dec << '\n';
                return 1;
            }
        }
        for (unsigned i = 0; i < 8; ++i) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            const auto gathered = random & gatheredMask;
            if (fastDeposit(gathered, mask) != oldDeposit(gathered, mask)) {
                std::cerr << "random mismatch mask=0x" << std::hex << rawMask
                          << " gathered=0x" << gathered << std::dec << '\n';
                return 1;
            }
        }
    }
    std::cout << "PASS: 65,536 masks x 13 gathered values; contiguous="
              << contiguousMasks << " split-sign=" << splitMasks << '\n';
}
