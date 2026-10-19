#pragma once

#include <cstddef>
#include <cstdint>

namespace zvram::gdeflate {

// Pinned DirectStorage GDeflate TileStream envelope (8-byte little-endian
// header, uint32 tile table, then dword-aligned tile payloads).
constexpr std::uint32_t TileBytes = 64u * 1024u;
constexpr std::uint32_t MaxTiles = 65535u;
constexpr std::uint32_t CodecId = 4u;
constexpr std::uint32_t Magic = 0xfbu;

struct Limits {
    std::size_t maxEncodedBytes{};
    std::size_t maxDecodedBytes{};
    std::size_t expectedDecodedBytes{};
    std::uint32_t inputBufferOffset{};
    std::uint32_t outputBufferOffset{};
    std::uint32_t maxTiles{MaxTiles};
};

struct Info {
    std::uint32_t tileCount{};
    std::uint32_t lastTileBytes{};
    std::size_t decodedBytes{};
    std::size_t payloadOffset{};
    std::size_t payloadBytes{};
};

inline std::uint32_t loadLe32(const std::uint8_t* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

inline bool validateEnvelope(const std::uint8_t* data, std::size_t size,
                             const Limits& limits, Info* info = nullptr) noexcept {
    if (!data || size < 8 || !limits.maxEncodedBytes || !limits.maxDecodedBytes ||
        !limits.expectedDecodedBytes || limits.expectedDecodedBytes > UINT32_MAX || !limits.maxTiles ||
        limits.maxTiles > MaxTiles || size > limits.maxEncodedBytes ||
        size > UINT32_MAX || (limits.inputBufferOffset & 3u) ||
        (limits.outputBufferOffset & 3u)) return false;
    if (size > std::size_t(UINT32_MAX - limits.inputBufferOffset) ||
        limits.outputBufferOffset > UINT32_MAX - limits.expectedDecodedBytes) return false;

    const auto codec = data[0];
    const auto magic = data[1];
    const auto count = std::uint32_t(data[2]) | (std::uint32_t(data[3]) << 8);
    const auto flags = loadLe32(data + 4);
    const auto tileSizeIndex = flags & 3u;
    const auto lastField = (flags >> 2) & 0x3ffffu;
    if (codec != CodecId || magic != Magic || !count || count > limits.maxTiles ||
        tileSizeIndex != 1 || (flags & 0xfff00000u)) return false;

    const std::uint32_t lastBytes = lastField ? lastField : TileBytes;
    if (lastField >= TileBytes && lastField != 0) return false;
    if (lastBytes > TileBytes) return false;
    const std::uint64_t decoded64 = std::uint64_t(count - 1) * TileBytes + lastBytes;
    const std::uint64_t paddedDecoded64 = (decoded64 + 3u) & ~std::uint64_t(3u);
    if (decoded64 > limits.maxDecodedBytes || decoded64 != limits.expectedDecodedBytes ||
        paddedDecoded64 > UINT32_MAX - limits.outputBufferOffset) return false;

    const std::size_t tableBytes = std::size_t(count) * sizeof(std::uint32_t);
    if (tableBytes > size - 8) return false;
    const std::size_t payloadOffset = 8 + tableBytes;
    const std::size_t payloadBytes = size - payloadOffset;
    const auto lastCompressedBytes = loadLe32(data + 8);
    if (payloadOffset > UINT32_MAX - limits.inputBufferOffset ||
        (payloadOffset & 3u) || !payloadBytes || !lastCompressedBytes ||
        (lastCompressedBytes & 3u) || lastCompressedBytes > payloadBytes) return false;

    std::uint32_t previous = 0;
    for (std::uint32_t i = 1; i < count; ++i) {
        const auto offset = loadLe32(data + 8 + std::size_t(i) * 4);
        if ((offset & 3u) || offset <= previous || offset > payloadBytes ||
            offset - previous < 4) return false;
        previous = offset;
    }
    if (lastCompressedBytes < 4 || payloadBytes - previous != lastCompressedBytes) return false;

    if (info) *info = {count, lastBytes, static_cast<std::size_t>(decoded64),
                       payloadOffset, payloadBytes};
    return true;
}

} // namespace zvram::gdeflate
