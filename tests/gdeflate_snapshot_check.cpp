#include "snapshot_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#include <zstd.h>

#ifndef ZVRAM_HAVE_GDEFLATE
#error "Build this check with ZVRAM_HAVE_GDEFLATE"
#endif

namespace {
using Bytes = std::vector<std::uint8_t>;
using zvram::snapshot::Codec;
using zvram::snapshot::EncodedChunk;
constexpr std::size_t Guard = 32;
constexpr std::size_t ChunkLimit = 256u * 1024u;
constexpr std::array<std::size_t, 4> Sizes{65'537, 98'317, 65'536, 131'195};
constexpr std::size_t Total = Sizes[0] + Sizes[1] + Sizes[2] + Sizes[3];
constexpr std::uint8_t Canary = 0xa5;

void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}

struct Fixture {
    std::array<Bytes, 4> raw;
    std::array<Bytes, 4> stored;
    std::array<EncodedChunk, 4> chunks{};
    Bytes expected;

    Fixture() {
        std::mt19937 random(0x61d3u);
        for (std::size_t i = 0; i < raw.size(); ++i) {
            raw[i].resize(Sizes[i]);
            for (std::size_t j = 0; j < raw[i].size(); ++j)
                raw[i][j] = i == 1 ? static_cast<std::uint8_t>(random())
                    : static_cast<std::uint8_t>((j / 17u + j * 13u + i * 37u) % 251u);
            expected.insert(expected.end(), raw[i].begin(), raw[i].end());

            if (i == 0) {
                stored[i] = raw[i];
                chunks[i] = {stored[i].data(), stored[i].size(), raw[i].size(), false, 0};
            } else if (i == 1) {
                Bytes shuffled(raw[i].size());
                require(zvram::byte_shuffle(raw[i].data(), shuffled.data(), shuffled.size(), 2),
                        "Zstd fixture shuffle failed");
                stored[i].resize(ZSTD_compressBound(shuffled.size()));
                const auto size = ZSTD_compress(stored[i].data(), stored[i].size(),
                                                shuffled.data(), shuffled.size(), 1);
                require(!ZSTD_isError(size), "Zstd fixture compression failed");
                stored[i].resize(size);
                chunks[i] = {stored[i].data(), stored[i].size(), raw[i].size(), true, 2};
            } else {
                require(zvram::gdeflate::encode(raw[i].data(), raw[i].size(), stored[i]),
                        "GDeflate fixture compression failed");
                chunks[i] = {stored[i].data(), stored[i].size(), raw[i].size(), true, 0,
                             Codec::GDeflate};
            }
        }
    }
};

Bytes guardedOutput() { return Bytes(Total + Guard * 2, Canary); }

bool guardsIntact(const Bytes& output) {
    return output.size() >= Guard * 2 &&
        std::all_of(output.begin(), output.begin() + Guard,
                    [](std::uint8_t value) { return value == Canary; }) &&
        std::all_of(output.end() - Guard, output.end(),
                    [](std::uint8_t value) { return value == Canary; });
}

bool matches(const Fixture& fixture, const std::uint8_t* output) {
    return std::equal(fixture.expected.begin(), fixture.expected.end(), output);
}

void expectBatchFailure(const std::array<EncodedChunk, 4>& chunks, const char* message) {
    auto output = guardedOutput();
    require(!zvram::snapshot::decodeBatch(chunks.data(), chunks.size(), output.data() + Guard,
                                           Total, ChunkLimit), message);
    require(guardsIntact(output), "rejected metadata/decode damaged output canary");
}
} // namespace

int main() try {
    Fixture fixture;
    auto serial = guardedOutput();
    auto parallel = guardedOutput();
    require(zvram::snapshot::decodeBatch(fixture.chunks.data(), fixture.chunks.size(),
            serial.data() + Guard, Total, ChunkLimit, false), "serial mixed decode failed");
    require(zvram::snapshot::decodeBatch(fixture.chunks.data(), fixture.chunks.size(),
            parallel.data() + Guard, Total, ChunkLimit), "parallel mixed decode failed");
    require(matches(fixture, serial.data() + Guard) && matches(fixture, parallel.data() + Guard),
            "mixed decode differs from source");
    require(std::equal(serial.begin() + Guard, serial.end() - Guard, parallel.begin() + Guard),
            "serial and parallel outputs differ");
    require(guardsIntact(serial) && guardsIntact(parallel), "successful decode damaged canary");

    Bytes aheadOutput = guardedOutput();
    zvram::snapshot::DecodeAhead ahead;
    const bool launched = ahead.start(fixture.chunks, fixture.chunks.size(), aheadOutput.data() + Guard,
                                      Total, ChunkLimit);
    const auto result = ahead.take();
    require(launched && result.ok && result.bytes == Total && result.nanoseconds > 0,
            "DecodeAhead did not complete asynchronously");
    require(matches(fixture, aheadOutput.data() + Guard) && guardsIntact(aheadOutput),
            "DecodeAhead output differs or damaged canary");

    auto malformed = fixture.chunks;
    auto brokenGDeflate = fixture.stored[3];
    brokenGDeflate[1] ^= 1;
    malformed[3].data = brokenGDeflate.data();
    expectBatchFailure(malformed, "malformed GDeflate stream accepted");

    auto unknownTag = fixture.chunks;
    unknownTag[2].codec = static_cast<Codec>(99);
    expectBatchFailure(unknownTag, "unknown codec tag accepted");

    auto rawGDeflate = fixture.chunks;
    rawGDeflate[0].codec = Codec::GDeflate;
    expectBatchFailure(rawGDeflate, "RAW chunk tagged GDeflate accepted");

    auto shuffledGDeflate = fixture.chunks;
    shuffledGDeflate[2].byteShuffle = 2;
    expectBatchFailure(shuffledGDeflate, "byte-shuffle plus GDeflate accepted");

    std::cout << "PASS: serial/parallel/DecodeAhead RAW+shuffled-Zstd+2xGDeflate, "
                 "odd tails, canaries, malformed payload and metadata rejection\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
