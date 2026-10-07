#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <zstd.h>
#include "byte_shuffle.hpp"
#ifdef ZVRAM_HAVE_GDEFLATE
#include "gdeflate_codec.hpp"
#endif

namespace zvram::snapshot {

enum class Codec { Zstd, GDeflate };

struct EncodedChunk {
    const std::uint8_t* data{};
    std::size_t storedSize{};
    std::size_t rawSize{};
    bool compressed{};
    unsigned byteShuffle{};
    // RAW chunks retain the default tag; this selects compressed payloads only.
    Codec codec{Codec::Zstd};
};

inline bool decodeOne(const EncodedChunk& chunk, std::uint8_t* output) noexcept {
    if (chunk.codec != Codec::Zstd) {
#ifdef ZVRAM_HAVE_GDEFLATE
        if (chunk.codec == Codec::GDeflate && chunk.compressed && !chunk.byteShuffle)
            return zvram::gdeflate::decode(chunk.data, chunk.storedSize, output, chunk.rawSize);
#endif
        return false;
    }
    if (chunk.byteShuffle) {
        try {
            std::unique_ptr<std::uint8_t[]> shuffled(new std::uint8_t[chunk.rawSize]);
            const auto size = ZSTD_decompress(shuffled.get(), chunk.rawSize, chunk.data, chunk.storedSize);
            return !ZSTD_isError(size) && size == chunk.rawSize &&
                   zvram::byte_unshuffle(shuffled.get(), output, chunk.rawSize, chunk.byteShuffle);
        } catch (...) { return false; }
    }
    if (!chunk.compressed) {
        std::memcpy(output, chunk.data, chunk.rawSize);
        return true;
    }
    const auto size = ZSTD_decompress(output, chunk.rawSize, chunk.data, chunk.storedSize);
    return !ZSTD_isError(size) && size == chunk.rawSize;
}

inline bool decodeBatch(const EncodedChunk* chunks, std::size_t count,
                        std::uint8_t* staging, std::size_t capacity,
                        std::size_t chunkLimit, bool allowParallel = true) noexcept {
    if (count > 4 || (count && (!chunks || !staging || !chunkLimit))) return false;
    std::size_t total = 0, compressedCount = 0, firstCompressed = count;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& chunk = chunks[i];
#ifndef ZVRAM_HAVE_GDEFLATE
        if (chunk.codec == Codec::GDeflate) return false;
#endif
        if (!chunk.rawSize || chunk.rawSize > chunkLimit ||
            chunk.rawSize > capacity - total || !chunk.data || !chunk.storedSize ||
            (chunk.byteShuffle && (!chunk.compressed || (chunk.byteShuffle != 2 && chunk.byteShuffle != 4))) ||
            (chunk.codec != Codec::Zstd &&
             (chunk.codec != Codec::GDeflate || !chunk.compressed || chunk.byteShuffle)) ||
            (!chunk.compressed && chunk.storedSize != chunk.rawSize)) return false;
        total += chunk.rawSize;
        if (chunk.compressed) {
            if (firstCompressed == count) firstCompressed = i;
            ++compressedCount;
        }
    }
    if (compressedCount < 2 || !allowParallel) {
        std::size_t offset = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (!decodeOne(chunks[i], staging + offset)) return false;
            offset += chunks[i].rawSize;
        }
        return true;
    }

    std::array<std::future<bool>, 3> workers;
    std::size_t launched = 0;
    auto waitWorkers = [&]() noexcept {
        for (std::size_t i = 0; i < launched; ++i) {
            try { workers[i].wait(); } catch (...) {}
        }
    };
    auto decodeSerial = [&]() noexcept {
        std::size_t offset = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (!decodeOne(chunks[i], staging + offset)) return false;
            offset += chunks[i].rawSize;
        }
        return true;
    };

    bool launchFailed = false;
    try {
        std::size_t offset = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (i != firstCompressed && chunks[i].compressed) {
                const auto chunk = chunks[i];
                auto* output = staging + offset;
                workers[launched++] = std::async(std::launch::async, [chunk, output]() noexcept {
                    return decodeOne(chunk, output);
                });
            }
            offset += chunks[i].rawSize;
        }
    } catch (...) {
        launchFailed = true;
    }
    if (launchFailed) {
        waitWorkers();
        return decodeSerial();
    }

    bool callerOk = true;
    std::size_t offset = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (i == firstCompressed || !chunks[i].compressed) {
            if (!decodeOne(chunks[i], staging + offset)) callerOk = false;
        }
        offset += chunks[i].rawSize;
    }
    waitWorkers();
    bool workersOk = true;
    for (std::size_t i = 0; i < launched; ++i) {
        try { workersOk = workers[i].get() && workersOk; }
        catch (...) { workersOk = false; }
    }
    if (callerOk && workersOk) return true;
    if (!callerOk && workersOk) return false;
    return decodeSerial();
}

} // namespace zvram::snapshot
