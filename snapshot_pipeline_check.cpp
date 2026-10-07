#include "snapshot_pipeline.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
using Bytes = std::vector<std::uint8_t>;

struct Frames {
    std::array<Bytes, 4> stored;
    std::array<zvram::snapshot::EncodedChunk, 4> chunks{};
    Bytes expected;
    std::size_t count{};
};

Frames fixture(bool mixed) {
    Frames f;
    const std::array<std::size_t, 4> sizes{1u << 20, 128u << 10, 7777, 0};
    f.count = mixed ? 3 : 2;
    for (std::size_t i = 0; i < f.count; ++i) {
        Bytes raw(sizes[i]);
        for (std::size_t j = 0; j < raw.size(); ++j)
            raw[j] = static_cast<std::uint8_t>((j / 64 + i * 31) % 251);
        f.expected.insert(f.expected.end(), raw.begin(), raw.end());
        const bool compressed = !(mixed && i == 1);
        if (compressed) {
            f.stored[i].resize(ZSTD_compressBound(raw.size()));
            const auto size = ZSTD_compress(f.stored[i].data(), f.stored[i].size(), raw.data(), raw.size(), 1);
            require(!ZSTD_isError(size), "fixture compression failed");
            f.stored[i].resize(size);
        } else f.stored[i] = raw;
        f.chunks[i] = {f.stored[i].data(), f.stored[i].size(), raw.size(), compressed};
    }
    return f;
}

void checkBytes(const Bytes& output, const Bytes& expected) {
    require(output.size() >= expected.size(), "output too small");
    require(std::memcmp(output.data(), expected.data(), expected.size()) == 0, "decoded bytes differ");
}
}

int main() try {
    auto f = fixture(true); // compressed + RAW + non-32-MiB tail
    Bytes first(f.expected.size()), second(f.expected.size());
    zvram::snapshot::DecodeAhead decoder;
    const bool launched = decoder.start(f.chunks, f.count, first.data(), first.size(), 1u << 20);
    const auto result = decoder.take();
    require(result.ok && result.bytes == f.expected.size() && result.nanoseconds > 0, "mixed async decode failed");
    checkBytes(first, f.expected);

    require(!decoder.start(f.chunks, f.count, second.data(), second.size(), 1u << 20, false), "forced inline reported async");
    const auto inlineResult = decoder.take();
    require(inlineResult.ok && inlineResult.bytes == f.expected.size(), "forced inline decode failed");
    checkBytes(second, f.expected);

    Bytes replacedFirst(f.expected.size()), replacedSecond(f.expected.size());
    (void)decoder.start(f.chunks, f.count, replacedFirst.data(), replacedFirst.size(), 1u << 20);
    (void)decoder.start(f.chunks, f.count, replacedSecond.data(), replacedSecond.size(), 1u << 20);
    const auto replaced = decoder.take();
    require(replaced.ok, "replacement decode failed");
    checkBytes(replacedFirst, f.expected); checkBytes(replacedSecond, f.expected);

    Bytes destructorOutput(f.expected.size());
    { zvram::snapshot::DecodeAhead scoped; (void)scoped.start(f.chunks, f.count, destructorOutput.data(), destructorOutput.size(), 1u << 20); }
    checkBytes(destructorOutput, f.expected);

    auto corrupt = fixture(false);
    corrupt.stored[1][0] ^= 0xff;
    corrupt.chunks[1].data = corrupt.stored[1].data();
    Bytes bad(corrupt.expected.size());
    (void)decoder.start(corrupt.chunks, corrupt.count, bad.data(), bad.size(), 1u << 20);
    require(!decoder.take().ok, "corrupt second frame accepted");

    auto truncated = fixture(false);
    --truncated.chunks[1].storedSize;
    (void)decoder.start(truncated.chunks, truncated.count, bad.data(), bad.size(), 1u << 20);
    require(!decoder.take().ok, "truncated second frame accepted");

    auto badRaw = fixture(true);
    --badRaw.chunks[1].storedSize;
    (void)decoder.start(badRaw.chunks, badRaw.count, bad.data(), bad.size(), 1u << 20);
    require(!decoder.take().ok, "incorrect RAW stored size accepted");

    Bytes tooSmall(f.expected.size() - 1);
    (void)decoder.start(f.chunks, f.count, tooSmall.data(), tooSmall.size(), 1u << 20);
    require(!decoder.take().ok, "insufficient output capacity accepted");

    (void)decoder.start(f.chunks, 5, second.data(), second.size(), 1u << 20);
    require(!decoder.take().ok, "oversized frame count accepted");

    std::cout << "PASS: bounded decode-ahead (" << (launched ? "async" : "inline fallback")
              << "), replacement, join, corruption and capacity checks\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
