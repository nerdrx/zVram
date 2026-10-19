#pragma once

#include "gdeflate_envelope.hpp"

#include <libdeflate.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
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

namespace detail {
struct JoinThreads {
    std::vector<std::thread>& threads;
    void join() noexcept {
        for (auto& thread : threads) if (thread.joinable()) thread.join();
    }
    ~JoinThreads() { join(); }
};

inline bool encodeParallel(const std::uint8_t* raw, std::size_t rawSize,
                           std::size_t count, std::size_t headerBytes,
                           std::vector<std::uint8_t>& encoded,
                           unsigned workers) noexcept {
    try {
        std::unique_ptr<libdeflate_gdeflate_compressor, CompressorDeleter> boundCompressor(
            libdeflate_alloc_gdeflate_compressor(1));
        if (!boundCompressor) return false;
        std::vector<std::size_t> bounds(count), offsets(count), lengths(count);
        std::size_t boundTotal = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto amount = std::min<std::size_t>(TileBytes, rawSize - i * TileBytes);
            std::size_t pages = 0;
            const auto bound = libdeflate_gdeflate_compress_bound(boundCompressor.get(), amount, &pages);
            if (!bound || pages != 1 || headerBytes > MaxEncodedBytes ||
                bound > MaxEncodedBytes - headerBytes - boundTotal) return false;
            bounds[i] = bound;
            offsets[i] = boundTotal;
            boundTotal += bound;
        }
        boundCompressor.reset();

        encoded.reserve(headerBytes + boundTotal);
        encoded.resize(headerBytes + boundTotal, 0);
        encoded[0] = static_cast<std::uint8_t>(CodecId);
        encoded[1] = static_cast<std::uint8_t>(Magic);
        storeLe16(encoded.data() + 2, static_cast<std::uint16_t>(count));
        const auto tail = static_cast<std::uint32_t>(rawSize % TileBytes);
        storeLe32(encoded.data() + 4, 1u | (tail << 2));

        const auto threadCount = std::min<std::size_t>(workers, count);
        std::atomic<bool> failed{false};
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        JoinThreads joiner{threads};
        for (std::size_t worker = 0; worker < threadCount; ++worker) {
            const auto begin = count * worker / threadCount;
            const auto end = count * (worker + 1) / threadCount;
            threads.emplace_back([&, begin, end] {
                std::unique_ptr<libdeflate_gdeflate_compressor, CompressorDeleter> compressor(
                    libdeflate_alloc_gdeflate_compressor(1));
                if (!compressor) { failed.store(true, std::memory_order_relaxed); return; }
                for (std::size_t i = begin; i < end; ++i) {
                    if (failed.load(std::memory_order_relaxed)) return;
                    const auto amount = std::min<std::size_t>(TileBytes, rawSize - i * TileBytes);
                    libdeflate_gdeflate_out_page page{
                        encoded.data() + headerBytes + offsets[i], bounds[i]};
                    const auto compressed = libdeflate_gdeflate_compress(
                        compressor.get(), raw + i * TileBytes, amount, &page, 1);
                    if (!compressed || compressed != page.nbytes || page.nbytes > bounds[i] ||
                        page.nbytes < 4 || (page.nbytes & 3u)) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                    lengths[i] = page.nbytes;
                }
            });
        }
        joiner.join();
        if (failed.load(std::memory_order_relaxed)) { encoded.clear(); return false; }

        std::size_t payloadBytes = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (i) storeLe32(encoded.data() + 8 + i * 4, static_cast<std::uint32_t>(payloadBytes));
            std::memmove(encoded.data() + headerBytes + payloadBytes,
                         encoded.data() + headerBytes + offsets[i], lengths[i]);
            payloadBytes += lengths[i];
        }
        storeLe32(encoded.data() + 8, static_cast<std::uint32_t>(lengths.back()));
        encoded.resize(headerBytes + payloadBytes);
        Limits limits{MaxEncodedBytes, MaxRawBytes, rawSize, 0, 0, MaxTiles};
        if (!validateEnvelope(encoded.data(), encoded.size(), limits)) { encoded.clear(); return false; }
        return true;
    } catch (...) {
        encoded.clear();
        return false;
    }
}
} // namespace detail

// Emits the pinned DirectStorage TileStream envelope. The default serial path
// uses one level-1 compressor and one reusable page buffer; optional workers
// compress independent pages into bounded disjoint output slices. Input and
// output storage must not overlap. Output capacity survives calls; every
// failure clears its logical size.
inline bool encode(const std::uint8_t* raw, std::size_t rawSize,
                   std::vector<std::uint8_t>& encoded, unsigned workers = 1) noexcept {
    encoded.clear();
    if (!raw || !rawSize || rawSize > MaxRawBytes || workers < 1 || workers > 32) return false;
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

        if (workers > 1 && count > 16) {
            compressor.reset();
            return detail::encodeParallel(raw, rawSize, count, headerBytes, encoded, workers);
        }

        auto& result = encoded;
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
                page.nbytes < 4 || (page.nbytes & 3u)) {
                encoded.clear();
                return false;
            }
            if (i + 1 == count)
                storeLe32(result.data() + 8, static_cast<std::uint32_t>(page.nbytes));
            result.insert(result.end(), pageScratch.begin(), pageScratch.begin() + page.nbytes);
            offset += amount;
        }
        Limits limits{MaxEncodedBytes, MaxRawBytes, rawSize, 0, 0, MaxTiles};
        if (!validateEnvelope(result.data(), result.size(), limits)) {
            encoded.clear();
            return false;
        }
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
