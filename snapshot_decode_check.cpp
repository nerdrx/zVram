#include "snapshot_decode.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
using zvram::snapshot::EncodedChunk;
constexpr std::size_t MiB = 1024u * 1024u;
constexpr std::size_t ChunkLimit = 4u * MiB;
constexpr std::array<std::size_t, 4> Sizes{4u * MiB, 3u * MiB, 2u * MiB, MiB + MiB / 4};
constexpr std::size_t Total = Sizes[0] + Sizes[1] + Sizes[2] + Sizes[3];

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::array<std::vector<std::uint8_t>, 4> source;
    std::array<std::vector<std::uint8_t>, 4> compressed;
    std::array<EncodedChunk, 4> mixed{};

    Fixture() {
        std::mt19937 rng(0x4a17u);
        for (std::size_t i = 0; i < source.size(); ++i) {
            source[i].resize(Sizes[i]);
            for (std::size_t j = 0; j < Sizes[i]; ++j)
                source[i][j] = i == 1 ? static_cast<std::uint8_t>(rng())
                                       : static_cast<std::uint8_t>((j * 13u + j / 97u + i * 41u) % 251u);
            if (i == 1) {
                mixed[i] = {source[i].data(), source[i].size(), source[i].size(), false};
                continue;
            }
            compressed[i].resize(ZSTD_compressBound(source[i].size()));
            const auto n = ZSTD_compress(compressed[i].data(), compressed[i].size(),
                                         source[i].data(), source[i].size(), 1);
            require(!ZSTD_isError(n), "fixture compression failed");
            compressed[i].resize(n);
            mixed[i] = {compressed[i].data(), compressed[i].size(), source[i].size(), true};
        }
    }

    bool matches(const std::vector<std::uint8_t>& output) const {
        if (output.size() < Total) return false;
        std::size_t offset = 0;
        for (const auto& bytes : source) {
            if (!std::equal(bytes.begin(), bytes.end(), output.begin() + offset)) return false;
            offset += bytes.size();
        }
        return true;
    }
};

void expectFailure(const EncodedChunk* chunks, std::size_t count,
                   std::size_t capacity, std::size_t chunkLimit,
                   const char* message) {
    std::vector<std::uint8_t> output(Total, 0xa5);
    require(!zvram::snapshot::decodeBatch(chunks, count, output.data(), capacity, chunkLimit), message);
}
} // namespace

int main() try {
    Fixture fixture;
    std::vector<std::uint8_t> serial(Total), parallel(Total);
    require(zvram::snapshot::decodeBatch(fixture.mixed.data(), fixture.mixed.size(),
                                          serial.data(), serial.size(), ChunkLimit, false),
            "serial mixed-frame decode failed");
    require(zvram::snapshot::decodeBatch(fixture.mixed.data(), fixture.mixed.size(),
                                          parallel.data(), parallel.size(), ChunkLimit),
            "parallel mixed-frame decode failed");
    require(fixture.matches(serial) && fixture.matches(parallel) && serial == parallel,
            "serial and parallel mixed-frame outputs differ");

    std::array<EncodedChunk, 4> allRaw{};
    for (std::size_t i = 0; i < allRaw.size(); ++i)
        allRaw[i] = {fixture.source[i].data(), fixture.source[i].size(), fixture.source[i].size(), false};
    std::fill(serial.begin(), serial.end(), 0);
    require(zvram::snapshot::decodeBatch(allRaw.data(), allRaw.size(), serial.data(),
                                          serial.size(), ChunkLimit), "all-RAW decode failed");
    require(fixture.matches(serial), "all-RAW output differs");

    std::fill(serial.begin(), serial.end(), 0);
    require(zvram::snapshot::decodeBatch(&fixture.mixed[0], 1, serial.data(),
                                          Sizes[0], ChunkLimit), "single-frame decode failed");
    require(std::equal(fixture.source[0].begin(), fixture.source[0].end(), serial.begin()),
            "single-frame output differs");

    auto corrupt = fixture.mixed;
    corrupt[0].data = nullptr;
    expectFailure(corrupt.data(), corrupt.size(), Total, ChunkLimit, "null encoded pointer accepted");

    auto badMagic = fixture.mixed;
    auto corruptedBytes = fixture.compressed[0];
    corruptedBytes[0] ^= 0xff;
    badMagic[0].data = corruptedBytes.data();
    expectFailure(badMagic.data(), badMagic.size(), Total, ChunkLimit, "corrupt frame accepted");

    auto workerBadMagic = fixture.mixed;
    auto workerCorruptedBytes = fixture.compressed[2];
    workerCorruptedBytes[0] ^= 0xff;
    workerBadMagic[2].data = workerCorruptedBytes.data();
    expectFailure(workerBadMagic.data(), workerBadMagic.size(), Total, ChunkLimit,
                  "corrupt worker frame accepted");

    auto truncated = fixture.mixed;
    truncated[0].storedSize--;
    expectFailure(truncated.data(), truncated.size(), Total, ChunkLimit, "truncated frame accepted");

    auto workerTruncated = fixture.mixed;
    workerTruncated[2].storedSize--;
    expectFailure(workerTruncated.data(), workerTruncated.size(), Total, ChunkLimit,
                  "truncated worker frame accepted");

    auto oversized = fixture.mixed;
    oversized[0].rawSize = ChunkLimit + 1;
    expectFailure(oversized.data(), oversized.size(), Total, ChunkLimit, "oversized raw chunk accepted");
    expectFailure(fixture.mixed.data(), fixture.mixed.size(), Total - 1, ChunkLimit,
                  "insufficient staging capacity accepted");

    auto rawLengthMismatch = allRaw;
    rawLengthMismatch[1].storedSize--;
    expectFailure(rawLengthMismatch.data(), rawLengthMismatch.size(), Total, ChunkLimit,
                  "RAW stored-length mismatch accepted");
    expectFailure(fixture.mixed.data(), 5, Total, ChunkLimit, "more than four frames accepted");

    std::cout << "PASS: serial/parallel mixed frames, RAW, single frame, corruption, truncation, and bounds\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
