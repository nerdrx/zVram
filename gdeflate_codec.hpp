#pragma once

#include "gdeflate_envelope.hpp"

#include <libdeflate.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace zvram::gdeflate {

constexpr std::size_t MaxRawBytes = 32u * 1024u * 1024u;
constexpr std::size_t MaxEncodedBytes = 64u * 1024u * 1024u;

struct CompressorDeleter {
    void operator()(libdeflate_gdeflate_compressor* p) const noexcept {
        libdeflate_free_gdeflate_compressor(p);
    }
};
struct DecompressorDeleter {
    void operator()(libdeflate_gdeflate_decompressor* p) const noexcept {
        libdeflate_free_gdeflate_decompressor(p);
    }
};

inline void storeLe16(std::uint8_t* p, std::uint16_t value) noexcept {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}
inline void storeLe32(std::uint8_t* p, std::uint32_t value) noexcept {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// Emits the pinned DirectStorage TileStream envelope using one level-1
// compressor, one reusable worst-case page buffer, and one bounded output.
inline bool encode(const std::uint8_t* raw, std::size_t rawSize,
                   std::vector<std::uint8_t>& encoded) noexcept {
    encoded.clear();
    if (!raw || !rawSize || rawSize > MaxRawBytes) return false;
    try {
        const auto count = (rawSize + TileBytes - 1) / TileBytes;
        if (!count || count > MaxTiles) return false;
        std::unique_ptr<libdeflate_gdeflate_compressor, CompressorDeleter> compressor(
            libdeflate_alloc_gdeflate_compressor(1));
        if (!compressor) return false;

        const auto headerBytes = std::size_t(8) + count * sizeof(std::uint32_t);
        std::size_t boundTotal = 0, pageScratchSize = 0;
        for (std::size_t offset = 0; offset < rawSize;) {
            const auto amount = std::min<std::size_t>(TileBytes, rawSize - offset);
            std::size_t pages = 0;
            const auto bound = libdeflate_gdeflate_compress_bound(compressor.get(), amount, &pages);
            if (!bound || pages != 1 || bound > MaxEncodedBytes - headerBytes - boundTotal)
                return false;
            boundTotal += bound;
            pageScratchSize = std::max(pageScratchSize, bound);
            offset += amount;
        }

        std::vector<std::uint8_t> result;
        result.reserve(headerBytes + boundTotal);
        result.resize(headerBytes, 0);
        std::vector<std::uint8_t> pageScratch(pageScratchSize);
        result[0] = static_cast<std::uint8_t>(CodecId);
        result[1] = static_cast<std::uint8_t>(Magic);
        storeLe16(result.data() + 2, static_cast<std::uint16_t>(count));
        const auto tail = static_cast<std::uint32_t>(rawSize % TileBytes);
        storeLe32(result.data() + 4, 1u | (tail << 2));

        for (std::size_t i = 0, offset = 0; i < count; ++i) {
            const auto amount = std::min<std::size_t>(TileBytes, rawSize - offset);
            if (i) storeLe32(result.data() + 8 + i * 4,
                             static_cast<std::uint32_t>(result.size() - headerBytes));
            libdeflate_gdeflate_out_page page{pageScratch.data(), pageScratch.size()};
            const auto compressed = libdeflate_gdeflate_compress(
                compressor.get(), raw + offset, amount, &page, 1);
            if (!compressed || compressed != page.nbytes || page.nbytes > pageScratch.size() ||
                page.nbytes < 4 || (page.nbytes & 3u)) return false;
            if (i + 1 == count)
                storeLe32(result.data() + 8, static_cast<std::uint32_t>(page.nbytes));
            result.insert(result.end(), pageScratch.begin(), pageScratch.begin() + page.nbytes);
            offset += amount;
        }
        Limits limits{MaxEncodedBytes, MaxRawBytes, rawSize, 0, 0, MaxTiles};
        if (!validateEnvelope(result.data(), result.size(), limits)) return false;
        encoded.swap(result);
        return true;
    } catch (...) {
        encoded.clear();
        return false;
    }
}

// Validates all envelope offsets and exact raw size before passing any page to
// libdeflate. Output contents are unspecified on compressed-payload failure.
inline bool decode(const std::uint8_t* encoded, std::size_t encodedSize,
                   std::uint8_t* raw, std::size_t rawSize) noexcept {
    if (!raw || !rawSize || rawSize > MaxRawBytes) return false;
    Limits limits{MaxEncodedBytes, MaxRawBytes, rawSize, 0, 0, MaxTiles};
    Info info{};
    if (!validateEnvelope(encoded, encodedSize, limits, &info)) return false;
    std::unique_ptr<libdeflate_gdeflate_decompressor, DecompressorDeleter> decompressor(
        libdeflate_alloc_gdeflate_decompressor());
    if (!decompressor) return false;

    const auto* table = encoded + 8;
    const auto* payload = encoded + info.payloadOffset;
    for (std::uint32_t i = 0; i < info.tileCount; ++i) {
        const std::uint32_t begin = i ? loadLe32(table + std::size_t(i) * 4) : 0;
        const std::uint32_t end = i + 1 < info.tileCount
            ? loadLe32(table + std::size_t(i + 1) * 4)
            : begin + loadLe32(table);
        if (end <= begin || end > info.payloadBytes) return false;
        const auto tileRawSize = i + 1 < info.tileCount ? TileBytes : info.lastTileBytes;
        libdeflate_gdeflate_in_page page{payload + begin, end - begin};
        std::size_t actual = 0;
        const auto result = libdeflate_gdeflate_decompress(
            decompressor.get(), &page, 1, raw + std::size_t(i) * TileBytes,
            tileRawSize, &actual);
        if (result != LIBDEFLATE_SUCCESS || actual != tileRawSize) return false;
    }
    return true;
}

} // namespace zvram::gdeflate
