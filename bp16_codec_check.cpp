#include "bp16_codec.hpp"

#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
using namespace zvram::bp16;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    store16(bytes.data() + offset, value);
}

void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    store32(bytes.data() + offset, value);
}

std::uint32_t get32(const Bytes& bytes, std::size_t offset) {
    return load32(bytes.data() + offset);
}

Bytes pattern(unsigned k, std::uint16_t base = 0xa55au) {
    const auto mask = static_cast<std::uint16_t>(k == 16 ? 0xffffu : ((1u << k) - 1u));
    base = static_cast<std::uint16_t>(base & ~mask);
    Bytes raw(RawBytesPerBlock);
    for (std::size_t i = 0; i < WordsPerBlock; ++i) {
        auto variable = static_cast<std::uint16_t>((i * 0x9e37u) & mask);
        if (i < k) variable |= static_cast<std::uint16_t>(1u << i);
        put16(raw, i * 2, static_cast<std::uint16_t>(base | (variable & mask)));
    }
    return raw;
}

bool encodeReference(const Bytes& raw, Bytes& encoded) {
    if (raw.empty() || raw.size() > MaxRawBytes || raw.size() % RawBytesPerBlock) return false;
    const auto blocks = static_cast<std::uint32_t>(raw.size() / RawBytesPerBlock);
    std::vector<std::uint32_t> meta(blocks);
    std::size_t total = HeaderBytes + std::size_t(blocks) * DescriptorBytes;
    for (std::uint32_t block = 0; block < blocks; ++block) {
        const auto* src = raw.data() + std::size_t(block) * RawBytesPerBlock;
        std::uint16_t allAnd = 0xffffu, allOr = 0;
        for (std::size_t i = 0; i < WordsPerBlock; ++i) {
            const auto word = load16(src + 2 * i);
            allAnd &= word;
            allOr |= word;
        }
        const auto mask = static_cast<std::uint16_t>(allOr ^ allAnd);
        meta[block] = std::uint32_t(allAnd) | (std::uint32_t(mask) << 16);
        total += 16u * popcount16(mask);
    }
    encoded.assign(total, 0);
    store32(encoded.data(), Magic);
    store32(encoded.data() + 4, Version);
    store32(encoded.data() + 8, static_cast<std::uint32_t>(raw.size()));
    store32(encoded.data() + 12, blocks);
    std::size_t next = HeaderBytes + std::size_t(blocks) * DescriptorBytes;
    for (std::uint32_t block = 0; block < blocks; ++block) {
        const auto* src = raw.data() + std::size_t(block) * RawBytesPerBlock;
        const auto packed = meta[block];
        const auto mask = static_cast<std::uint16_t>(packed >> 16);
        auto* desc = encoded.data() + HeaderBytes + std::size_t(block) * DescriptorBytes;
        store32(desc, static_cast<std::uint32_t>(next));
        store32(desc + 4, packed);
        std::size_t bit = 0;
        for (std::size_t i = 0; i < WordsPerBlock; ++i) {
            const auto word = load16(src + 2 * i);
            unsigned outBit = 0;
            for (unsigned inBit = 0; inBit < 16; ++inBit) {
                if (!(mask & (std::uint16_t(1u) << inBit))) continue;
                if (word & (std::uint16_t(1u) << inBit))
                    encoded[next + (bit + outBit) / 8] |=
                        std::uint8_t(1u << ((bit + outBit) % 8));
                ++outBit;
            }
            bit += outBit;
        }
        next += 16u * popcount16(mask);
    }
    return next == encoded.size();
}

void roundTrip(const Bytes& raw) {
    Bytes ref, portable, fast, decoded(raw.size());
    require(encodeReference(raw, ref), "reference encode failed");
    require(detail::encodeImpl(raw.data(), raw.size(), portable, false), "portable encode failed");
    require(encodeFast(raw.data(), raw.size(), fast), "fast encode failed");
    require(ref == portable && ref == fast, "frame differs from reference bytes");
    require(validate(fast.data(), fast.size(), raw.size()), "frame validation failed");
    require(decode(fast.data(), fast.size(), decoded.data(), raw.size()), "pointer decode failed");
    require(decoded == raw, "decoded bytes differ");
    Bytes decodedVector;
    require(decode(fast, decodedVector, static_cast<std::uint32_t>(raw.size())),
            "vector decode failed");
    require(decodedVector == raw, "vector-decoded bytes differ");
}

void testPatterns() {
    for (unsigned k = 0; k <= 16; ++k) roundTrip(pattern(k));
    roundTrip(Bytes(4 * RawBytesPerBlock, 0));
    roundTrip(pattern(0, 0x0f0fu));

    std::mt19937 rng(0x42503136u);
    Bytes random(12 * RawBytesPerBlock);
    for (std::size_t i = 0; i < random.size(); i += 2)
        put16(random, i, static_cast<std::uint16_t>(rng()));
    roundTrip(random);

    Bytes sparse(4 * RawBytesPerBlock);
    const std::uint16_t masks[] = {0x0005u, 0x0180u, 0x4500u, 0x7fffu};
    const std::uint16_t bases[] = {0xaaaau, 0x2401u, 0x80a5u, 0x8000u};
    for (std::size_t block = 0; block < 4; ++block) {
        const auto mask = masks[block];
        const auto base = static_cast<std::uint16_t>(bases[block] & ~mask);
        for (std::size_t i = 0; i < WordsPerBlock; ++i)
            put16(sparse, block * RawBytesPerBlock + i * 2,
                  static_cast<std::uint16_t>(base | (rng() & mask)));
    }
    roundTrip(sparse);

    Bytes output;
    require(!encodeFast(nullptr, RawBytesPerBlock, output), "accepted null input");
    require(!encodeFast(random.data(), random.size() - 2, output), "accepted partial block");
    require(!encodeFast(random.data(), MaxRawBytes + RawBytesPerBlock, output),
            "accepted oversized input");
}

void testMalformed() {
    Bytes encoded;
    require(encodeFast(pattern(7), encoded), "fixture encode failed");
    auto reject = [&](Bytes bad) {
        Bytes out(RawBytesPerBlock, 0x5a);
        require(!validate(bad.data(), bad.size(), RawBytesPerBlock), "accepted malformed frame");
        require(!decode(bad.data(), bad.size(), out.data(), out.size()),
                "decoded malformed frame");
        require(out == Bytes(RawBytesPerBlock, 0x5a), "invalid decode modified output");
    };
    reject(Bytes(encoded.begin(), encoded.begin() + 15));
    auto bad = encoded; bad[0] ^= 1; reject(std::move(bad));
    bad = encoded; put32(bad, 4, 2); reject(std::move(bad));
    bad = encoded; put32(bad, 8, 0); reject(std::move(bad));
    bad = encoded; put32(bad, 8, RawBytesPerBlock + 2); reject(std::move(bad));
    bad = encoded; put32(bad, 8, MaxRawBytes + RawBytesPerBlock); reject(std::move(bad));
    bad = encoded; put32(bad, 12, 2); reject(std::move(bad));
    bad = encoded; put32(bad, HeaderBytes, get32(encoded, HeaderBytes) + 1); reject(std::move(bad));
    bad = encoded;
    const auto packed = load32(bad.data() + HeaderBytes + 4);
    put32(bad, HeaderBytes + 4, packed | 0x00010001u);
    reject(std::move(bad));
    bad = encoded; bad.pop_back(); reject(std::move(bad));
    bad = encoded; bad.push_back(0); reject(std::move(bad));

    Bytes output(RawBytesPerBlock);
    require(!decode(encoded.data(), encoded.size(), output.data(), output.size() + 256),
            "accepted wrong output size");
    require(!decode(encoded.data(), encoded.size(), encoded.data(), RawBytesPerBlock),
            "accepted overlapping input/output");
}

} // namespace

int main() {
    try {
        testPatterns();
        testMalformed();
        std::cout << "PASS: BP16 reference/portable-fast/BMI2 streams, decode, and malformed frames\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
