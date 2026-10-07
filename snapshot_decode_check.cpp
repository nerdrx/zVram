#include "snapshot_decode.hpp"
#include "byte_shuffle.hpp"

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
constexpr std::size_t Guard = 32;
constexpr std::array<std::size_t, 4> Sizes{4u * MiB - 1, 3u * MiB + 5,
                                            2u * MiB - 3, MiB + 1};
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
                mixed[i] = {source[i].data(), source[i].size(), source[i].size(), false, 0};
                continue;
            }
            std::vector<std::uint8_t> shuffled;
            const unsigned stride = i == 2 ? 2 : (i == 3 ? 4 : 0);
            const auto* input = source[i].data();
            if (stride) {
                shuffled.resize(source[i].size());
                require(zvram::byte_shuffle(input, shuffled.data(), shuffled.size(), stride),
                        "fixture byte shuffle failed");
                input = shuffled.data();
            }
            compressed[i].resize(ZSTD_compressBound(source[i].size()));
            const auto n = ZSTD_compress(compressed[i].data(), compressed[i].size(),
                                         input, source[i].size(), 1);
            require(!ZSTD_isError(n), "fixture compression failed");
            compressed[i].resize(n);
            mixed[i] = {compressed[i].data(), compressed[i].size(), source[i].size(), true, stride};
        }
    }

    bool matches(const std::uint8_t* output) const {
        std::size_t offset = 0;
        for (const auto& bytes : source) {
            if (!std::equal(bytes.begin(), bytes.end(), output + offset)) return false;
            offset += bytes.size();
        }
        return true;
    }
};

std::vector<std::uint8_t> guardedOutput() {
    return std::vector<std::uint8_t>(Total + 2 * Guard, 0xa5);
}

bool guardsIntact(const std::vector<std::uint8_t>& output) {
    return std::all_of(output.begin(), output.begin() + Guard,
                       [](auto value) { return value == 0xa5; }) &&
           std::all_of(output.end() - Guard, output.end(),
                       [](auto value) { return value == 0xa5; });
}

void expectFailure(const EncodedChunk* chunks, std::size_t count,
                   std::size_t capacity, std::size_t chunkLimit,
                   const char* message) {
    auto output = guardedOutput();
    require(!zvram::snapshot::decodeBatch(chunks, count, output.data() + Guard,
                                           capacity, chunkLimit), message);
    require(guardsIntact(output), "failed decode damaged staging canary");
}
} // namespace

int main() try {
    Fixture fixture;
    auto serial = guardedOutput();
    auto parallel = guardedOutput();
    require(zvram::snapshot::decodeBatch(fixture.mixed.data(), fixture.mixed.size(),
                                          serial.data() + Guard, Total, ChunkLimit, false),
            "serial mixed/shuffled-frame decode failed");
    require(zvram::snapshot::decodeBatch(fixture.mixed.data(), fixture.mixed.size(),
                                          parallel.data() + Guard, Total, ChunkLimit),
            "parallel mixed/shuffled-frame decode failed");
    require(fixture.matches(serial.data() + Guard) && fixture.matches(parallel.data() + Guard),
            "mixed/shuffled output differs from source");
    require(std::equal(serial.begin() + Guard, serial.end() - Guard,
                       parallel.begin() + Guard), "serial and parallel outputs differ");
    require(guardsIntact(serial) && guardsIntact(parallel), "successful decode damaged staging canary");

    std::array<EncodedChunk, 4> allRaw{};
    for (std::size_t i = 0; i < allRaw.size(); ++i)
        allRaw[i] = {fixture.source[i].data(), fixture.source[i].size(),
                     fixture.source[i].size(), false, 0};
    auto rawOutput = guardedOutput();
    require(zvram::snapshot::decodeBatch(allRaw.data(), allRaw.size(), rawOutput.data() + Guard,
                                          Total, ChunkLimit), "all-RAW decode failed");
    require(fixture.matches(rawOutput.data() + Guard) && guardsIntact(rawOutput),
            "all-RAW output differs or damaged canary");

    auto one = guardedOutput();
    require(zvram::snapshot::decodeBatch(&fixture.mixed[0], 1, one.data() + Guard,
                                          Sizes[0], ChunkLimit), "single-frame decode failed");
    require(std::equal(fixture.source[0].begin(), fixture.source[0].end(), one.begin() + Guard) &&
            guardsIntact(one), "single-frame output differs or damaged canary");

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

    auto invalidStride = fixture.mixed;
    invalidStride[2].byteShuffle = 3;
    expectFailure(invalidStride.data(), invalidStride.size(), Total, ChunkLimit,
                  "invalid byte-shuffle stride accepted");
    auto rawFiltered = allRaw;
    rawFiltered[0].byteShuffle = 2;
    expectFailure(rawFiltered.data(), rawFiltered.size(), Total, ChunkLimit,
                  "RAW frame with byte-shuffle metadata accepted");

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

    std::cout << "PASS: serial/parallel mixed Zstd+RAW+stride2/4, odd tails, canaries, "
                 "corruption, truncation, metadata, and bounds\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
