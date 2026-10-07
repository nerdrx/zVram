#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define ZVRAM_BP16_X86_BMI2 1
#else
#define ZVRAM_BP16_X86_BMI2 0
#endif

namespace zvram::bp16 {

constexpr std::uint32_t Magic = 0x36315042u;
constexpr std::uint32_t Version = 1;
constexpr std::size_t HeaderBytes = 16;
constexpr std::size_t DescriptorBytes = 8;
constexpr std::size_t WordsPerBlock = 128;
constexpr std::size_t RawBytesPerBlock = WordsPerBlock * sizeof(std::uint16_t);
constexpr std::size_t MaxRawBytes = 32u * 1024u * 1024u;

struct FrameInfo {
    std::uint32_t rawBytes{};
    std::uint32_t blockCount{};
    std::uint32_t payloadBegin{};
};

inline std::uint16_t load16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

inline std::uint32_t load32(const std::uint8_t* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

inline void store16(std::uint8_t* p, std::uint16_t value) noexcept {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

inline void store32(std::uint8_t* p, std::uint32_t value) noexcept {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
    p[2] = static_cast<std::uint8_t>(value >> 16);
    p[3] = static_cast<std::uint8_t>(value >> 24);
}

inline unsigned popcount16(std::uint16_t value) noexcept {
    std::uint32_t x = value;
    x -= (x >> 1) & 0x5555u;
    x = (x & 0x3333u) + ((x >> 2) & 0x3333u);
    x = (x + (x >> 4)) & 0x0f0fu;
    x += x >> 8;
    return x & 0x1fu;
}

namespace detail {

inline std::uint16_t gatherPortable(std::uint16_t value,
                                    std::uint16_t mask) noexcept {
    std::uint16_t gathered = 0;
    unsigned destinationBit = 0;
    while (mask) {
        const auto lowest = static_cast<std::uint16_t>(mask & -mask);
        if (value & lowest) gathered |= static_cast<std::uint16_t>(1u << destinationBit);
        mask ^= lowest;
        ++destinationBit;
    }
    return gathered;
}

#if ZVRAM_BP16_X86_BMI2
__attribute__((target("bmi2")))
inline std::uint16_t gatherBmi2(std::uint16_t value,
                                std::uint16_t mask) noexcept {
    return static_cast<std::uint16_t>(_pext_u32(value, mask));
}

inline bool cpuHasBmi2() noexcept {
    __builtin_cpu_init();
    return __builtin_cpu_supports("bmi2");
}
#else
inline bool cpuHasBmi2() noexcept { return false; }
#endif

template<std::uint16_t (*Gather)(std::uint16_t, std::uint16_t)>
inline void packBlock(std::uint8_t* payload, const std::uint8_t* raw,
                      std::uint16_t mask, unsigned bitsPerWord) noexcept {
    if (!bitsPerWord) return;
    if (bitsPerWord == 16) {
        std::memcpy(payload, raw, RawBytesPerBlock);
        return;
    }
    std::uint64_t accumulator = 0;
    unsigned filled = 0;
    std::size_t outputWord = 0;
    for (std::size_t i = 0; i < WordsPerBlock; ++i) {
        const auto compact = Gather(load16(raw + i * 2), mask);
        accumulator |= std::uint64_t(compact) << filled;
        filled += bitsPerWord;
        if (filled >= 32) {
            store32(payload + outputWord * 4, static_cast<std::uint32_t>(accumulator));
            ++outputWord;
            accumulator >>= 32;
            filled -= 32;
        }
    }
}

struct JoinThreads {
    std::vector<std::thread>& threads;
    void join() noexcept {
        for (auto& thread : threads) if (thread.joinable()) thread.join();
    }
    ~JoinThreads() { join(); }
};

} // namespace detail

inline bool inspect(const std::uint8_t* encoded, std::size_t encodedBytes,
                    FrameInfo* info = nullptr) noexcept {
    if (!encoded || encodedBytes < HeaderBytes || load32(encoded) != Magic ||
        load32(encoded + 4) != Version) {
        return false;
    }
    const auto rawBytes = load32(encoded + 8);
    const auto blockCount = load32(encoded + 12);
    if (!rawBytes || rawBytes > MaxRawBytes || rawBytes % RawBytesPerBlock ||
        blockCount != rawBytes / RawBytesPerBlock) {
        return false;
    }
    const std::size_t tableBytes = std::size_t(blockCount) * DescriptorBytes;
    if (tableBytes > encodedBytes - HeaderBytes) return false;
    const std::size_t payloadBegin = HeaderBytes + tableBytes;
    std::size_t expectedOffset = payloadBegin;
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        const auto* descriptor = encoded + HeaderBytes + std::size_t(i) * DescriptorBytes;
        const auto offset = load32(descriptor);
        const auto packed = load32(descriptor + 4);
        const auto base = static_cast<std::uint16_t>(packed);
        const auto mask = static_cast<std::uint16_t>(packed >> 16);
        if ((base & mask) || offset != expectedOffset) return false;
        const std::size_t payloadBytes = WordsPerBlock * popcount16(mask) / 8;
        if (payloadBytes > encodedBytes - expectedOffset) return false;
        expectedOffset += payloadBytes;
    }
    if (expectedOffset != encodedBytes) return false;
    if (info) *info = {rawBytes, blockCount, static_cast<std::uint32_t>(payloadBegin)};
    return true;
}

inline bool validate(const std::uint8_t* encoded, std::size_t encodedBytes,
                     std::size_t expectedRawBytes,
                     FrameInfo* info = nullptr) noexcept {
    FrameInfo parsed{};
    if (!inspect(encoded, encodedBytes, &parsed) ||
        (expectedRawBytes && (expectedRawBytes > MaxRawBytes ||
                              parsed.rawBytes != expectedRawBytes))) {
        return false;
    }
    if (info) *info = parsed;
    return true;
}

namespace detail {

inline bool encodeImpl(const std::uint8_t* raw, std::size_t rawBytes,
                       std::vector<std::uint8_t>& encoded,
                       bool useBmi2, unsigned workers = 1) {
    if (!raw || !rawBytes || rawBytes > MaxRawBytes ||
        rawBytes % RawBytesPerBlock || workers < 1 || workers > 32) {
        return false;
    }
    const auto blockCount = static_cast<std::uint32_t>(rawBytes / RawBytesPerBlock);
    std::size_t totalBytes = HeaderBytes + std::size_t(blockCount) * DescriptorBytes;
    std::vector<std::uint32_t> blockMeta(blockCount);
    for (std::uint32_t block = 0; block < blockCount; ++block) {
        const auto* src = raw + std::size_t(block) * RawBytesPerBlock;
        std::uint16_t allAnd = 0xffffu;
        std::uint16_t allOr = 0;
        for (std::size_t i = 0; i < WordsPerBlock; ++i) {
            const auto word = load16(src + i * 2);
            allAnd &= word;
            allOr |= word;
        }
        const auto mask = static_cast<std::uint16_t>(allOr ^ allAnd);
        blockMeta[block] = std::uint32_t(allAnd) | (std::uint32_t(mask) << 16);
        totalBytes += WordsPerBlock * popcount16(mask) / 8;
    }
    if (totalBytes > UINT32_MAX) return false;
    encoded.assign(totalBytes, 0);
    store32(encoded.data(), Magic);
    store32(encoded.data() + 4, Version);
    store32(encoded.data() + 8, static_cast<std::uint32_t>(rawBytes));
    store32(encoded.data() + 12, blockCount);

    std::size_t nextPayload = HeaderBytes + std::size_t(blockCount) * DescriptorBytes;
    std::vector<std::uint32_t> payloadOffsets(blockCount);
    for (std::uint32_t block = 0; block < blockCount; ++block) {
        const auto packedMeta = blockMeta[block];
        const auto allAnd = static_cast<std::uint16_t>(packedMeta);
        const auto mask = static_cast<std::uint16_t>(packedMeta >> 16);
        auto* descriptor = encoded.data() + HeaderBytes + std::size_t(block) * DescriptorBytes;
        payloadOffsets[block] = static_cast<std::uint32_t>(nextPayload);
        store32(descriptor, static_cast<std::uint32_t>(nextPayload));
        store32(descriptor + 4, std::uint32_t(allAnd) | (std::uint32_t(mask) << 16));
        nextPayload += WordsPerBlock * popcount16(mask) / 8;
    }
    auto packRange = [&](std::uint32_t begin, std::uint32_t end) {
        for (std::uint32_t block = begin; block < end; ++block) {
            const auto* src = raw + std::size_t(block) * RawBytesPerBlock;
            const auto packedMeta = blockMeta[block];
            const auto mask = static_cast<std::uint16_t>(packedMeta >> 16);
            auto* payload = encoded.data() + payloadOffsets[block];
            const unsigned bitsPerWord = popcount16(mask);
            if (useBmi2) {
#if ZVRAM_BP16_X86_BMI2
                packBlock<gatherBmi2>(payload, src, mask, bitsPerWord);
#else
                packBlock<gatherPortable>(payload, src, mask, bitsPerWord);
#endif
            } else {
                packBlock<gatherPortable>(payload, src, mask, bitsPerWord);
            }
        }
    };
    // ponytail: local bounded fanout; below 1 MiB thread startup costs more than packing.
    if (workers > 1 && rawBytes >= 1024u * 1024u) {
        const auto threadCount = std::min<unsigned>(workers, blockCount);
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        JoinThreads joiner{threads};
        for (unsigned worker = 0; worker < threadCount; ++worker) {
            const auto begin = blockCount * worker / threadCount;
            const auto end = blockCount * (worker + 1) / threadCount;
            threads.emplace_back(packRange, begin, end);
        }
        joiner.join();
    } else {
        packRange(0, blockCount);
    }
    return nextPayload == encoded.size();
}

} // namespace detail

inline bool encode(const std::uint8_t* raw, std::size_t rawBytes,
                   std::vector<std::uint8_t>& encoded) {
    const bool useBmi2 = detail::cpuHasBmi2();
    return detail::encodeImpl(raw, rawBytes, encoded, useBmi2);
}

inline bool encode(const std::vector<std::uint8_t>& raw,
                   std::vector<std::uint8_t>& encoded) {
    if (&raw == &encoded) return false;
    return encode(raw.data(), raw.size(), encoded);
}

inline bool encodeFast(const std::uint8_t* raw, std::size_t rawBytes,
                       std::vector<std::uint8_t>& encoded, unsigned workers) {
    return detail::encodeImpl(raw, rawBytes, encoded, detail::cpuHasBmi2(), workers);
}

inline bool encodeFast(const std::uint8_t* raw, std::size_t rawBytes,
                       std::vector<std::uint8_t>& encoded) {
    return encodeFast(raw, rawBytes, encoded, 1);
}

inline bool encodeFast(const std::vector<std::uint8_t>& raw,
                       std::vector<std::uint8_t>& encoded, unsigned workers) {
    if (&raw == &encoded) return false;
    return encodeFast(raw.data(), raw.size(), encoded, workers);
}

inline bool encodeFast(const std::vector<std::uint8_t>& raw,
                       std::vector<std::uint8_t>& encoded) {
    return encodeFast(raw, encoded, 1);
}

inline bool rangesOverlap(const void* left, std::size_t leftBytes,
                          const void* right, std::size_t rightBytes) noexcept {
    const auto l = reinterpret_cast<std::uintptr_t>(left);
    const auto r = reinterpret_cast<std::uintptr_t>(right);
    if (!leftBytes || !rightBytes) return false;
    if (l > UINTPTR_MAX - leftBytes || r > UINTPTR_MAX - rightBytes) return true;
    return l < r + rightBytes && r < l + leftBytes;
}

// The caller provides an output buffer of expectedRawBytes; malformed frames
// are rejected before any output bytes are written.
inline bool decode(const std::uint8_t* encoded, std::size_t encodedBytes,
                   std::uint8_t* output, std::size_t expectedRawBytes) noexcept {
    FrameInfo info{};
    if (!output || !expectedRawBytes || expectedRawBytes > MaxRawBytes ||
        !validate(encoded, encodedBytes, expectedRawBytes, &info) ||
        rangesOverlap(encoded, encodedBytes, output, expectedRawBytes)) {
        return false;
    }
    for (std::uint32_t block = 0; block < info.blockCount; ++block) {
        const auto* descriptor = encoded + HeaderBytes + std::size_t(block) * DescriptorBytes;
        const auto payloadOffset = load32(descriptor);
        const auto packed = load32(descriptor + 4);
        const auto base = static_cast<std::uint16_t>(packed);
        const auto mask = static_cast<std::uint16_t>(packed >> 16);
        const unsigned bitsPerWord = popcount16(mask);
        const auto* payload = encoded + payloadOffset;
        auto* dst = output + std::size_t(block) * RawBytesPerBlock;
        std::size_t bitOffset = 0;
        for (std::size_t i = 0; i < WordsPerBlock; ++i) {
            std::uint16_t word = base;
            unsigned compactBit = 0;
            for (unsigned destinationBit = 0; destinationBit < 16; ++destinationBit) {
                if ((mask & (std::uint16_t(1u) << destinationBit)) == 0) continue;
                if ((payload[(bitOffset + compactBit) / 8] >>
                     ((bitOffset + compactBit) % 8)) & 1u) {
                    word |= std::uint16_t(1u << destinationBit);
                }
                ++compactBit;
            }
            store16(dst + i * 2, word);
            bitOffset += bitsPerWord;
        }
    }
    return true;
}

inline bool decode(const std::uint8_t* encoded, std::size_t encodedBytes,
                   std::vector<std::uint8_t>& raw,
                   std::uint32_t expectedRawBytes = 0) noexcept {
    if (raw.data() == encoded) return false;
    FrameInfo info{};
    if (!validate(encoded, encodedBytes, expectedRawBytes, &info)) return false;
    if (rangesOverlap(encoded, encodedBytes, raw.data(), info.rawBytes)) return false;
    try {
        raw.resize(info.rawBytes);
    } catch (...) {
        return false;
    }
    return decode(encoded, encodedBytes, raw.data(), info.rawBytes);
}

inline bool decode(const std::vector<std::uint8_t>& encoded,
                   std::vector<std::uint8_t>& raw,
                   std::uint32_t expectedRawBytes = 0) {
    if (&encoded == &raw) return false;
    return decode(encoded.data(), encoded.size(), raw, expectedRawBytes);
}

} // namespace zvram::bp16
