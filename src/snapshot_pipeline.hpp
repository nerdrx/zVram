#pragma once

#include "snapshot_decode.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>

namespace zvram::snapshot {

struct DecodeResult { bool ok{}; std::size_t bytes{}; std::uint64_t nanoseconds{}; };

class DecodeAhead {
    std::future<DecodeResult> future_;
    DecodeResult result_{};
    bool active_{};

    static DecodeResult decode(const std::array<EncodedChunk, 4>& chunks, std::size_t count,
                               std::uint8_t* mapped, std::size_t capacity,
                               std::size_t chunkLimit) noexcept {
        std::size_t bytes = 0;
        if (!count || count > chunks.size() || !mapped || !chunkLimit) return {};
        for (std::size_t i = 0; i < count; ++i) {
            if (!chunks[i].rawSize || chunks[i].rawSize > chunkLimit ||
                chunks[i].rawSize > capacity - bytes) return {};
            bytes += chunks[i].rawSize;
        }
        const auto start = std::chrono::steady_clock::now();
        const bool ok = decodeBatch(chunks.data(), count, mapped, capacity, chunkLimit);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        return {ok, ok ? bytes : 0, static_cast<std::uint64_t>(ns)};
    }

public:
    DecodeAhead() = default;
    DecodeAhead(const DecodeAhead&) = delete;
    DecodeAhead& operator=(const DecodeAhead&) = delete;
    ~DecodeAhead() { try { if (future_.valid()) future_.wait(); } catch (...) {} }

    // Input bytes stay immutable/alive and mapped output stays writable until take().
    // Returns true only when the decode was actually launched asynchronously.
    bool start(const std::array<EncodedChunk, 4>& chunks, std::size_t count,
               std::uint8_t* mapped, std::size_t capacity, std::size_t chunkLimit,
               bool asynchronous = true) noexcept {
        if (active_) (void)take();
        result_ = {}; active_ = true;
        if (!count || count > chunks.size() || !mapped || !chunkLimit) return false;
        for (std::size_t i = 0, bytes = 0; i < count; ++i) {
            if (!chunks[i].rawSize || chunks[i].rawSize > chunkLimit ||
                chunks[i].rawSize > capacity - bytes) return false;
            bytes += chunks[i].rawSize;
        }
        if (asynchronous) {
            try {
                future_ = std::async(std::launch::async, [chunks, count, mapped, capacity, chunkLimit] {
                    return decode(chunks, count, mapped, capacity, chunkLimit);
                });
                return true;
            } catch (...) {}
        }
        result_ = decode(chunks, count, mapped, capacity, chunkLimit);
        return false;
    }

    DecodeResult take() noexcept {
        if (!active_) return {};
        if (future_.valid()) {
            try { result_ = future_.get(); }
            catch (...) { result_ = {}; }
        }
        active_ = false;
        return result_;
    }
};

} // namespace zvram::snapshot
