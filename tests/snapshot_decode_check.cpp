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

struct Bp16Fixture {
    std::vector<std::uint8_t> source = std::vector<std::uint8_t>(4 * 256);
    std::vector<std::uint8_t> encoded;
    EncodedChunk chunk{};

    Bp16Fixture() {
        std::mt19937 rng(0xb016u);
        for (std::size_t word = 0; word < 4 * 128; ++word) {
            std::uint16_t value{};
            const auto block = word / 128;
            if (block == 0) value = 0x3c3cu;
            else if (block == 1) value = static_cast<std::uint16_t>(
                0x2400u | (rng() & 0x0185u));
            else if (block == 2) value = static_cast<std::uint16_t>(rng());
            else value = static_cast<std::uint16_t>(0x8010u | (rng() & 0x0204u));
            zvram::bp16::store16(source.data() + word * 2, value);
        }
        // Ensure the sparse masks' every variable bit occurs in the block.
        const auto forceBits = [&](std::size_t block, std::uint16_t base,
                                   std::uint16_t mask) {
            for (unsigned bit = 0; bit < 16; ++bit) {
                if (mask & (std::uint16_t(1u) << bit))
                    zvram::bp16::store16(source.data() + block * 256 + bit * 2,
                        static_cast<std::uint16_t>(base | (1u << bit)));
            }
        };
        forceBits(1, 0x2400u, 0x0185u);
        forceBits(3, 0x8010u, 0x0204u);
        require(zvram::bp16::encodeFast(source.data(), source.size(), encoded),
                "BP16 fixture encode failed");
        chunk = {encoded.data(), encoded.size(), source.size(), true, 0,
                 zvram::snapshot::Codec::BP16};
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

void testBp16Batch(Fixture& fixture) {
    Bp16Fixture bp16;
    const std::array<EncodedChunk, 3> mixed{
        fixture.mixed[0], bp16.chunk, fixture.mixed[2]};
    const auto expected = Sizes[0] + bp16.source.size() + Sizes[2];
    auto matches = [&](const std::uint8_t* output) {
        return std::equal(fixture.source[0].begin(), fixture.source[0].end(), output) &&
            std::equal(bp16.source.begin(), bp16.source.end(), output + Sizes[0]) &&
            std::equal(fixture.source[2].begin(), fixture.source[2].end(),
                       output + Sizes[0] + bp16.source.size());
    };
    for (bool parallel : {false, true}) {
        std::vector<std::uint8_t> output(expected + 2 * Guard, 0xa5);
        require(zvram::snapshot::decodeBatch(mixed.data(), mixed.size(),
                output.data() + Guard, expected, ChunkLimit, parallel),
                "mixed Zstd/BP16 decode failed");
        require(matches(output.data() + Guard) && guardsIntact(output),
                "mixed Zstd/BP16 output differs or damaged canaries");
    }

    const auto expectBp16Failure = [&](const EncodedChunk& invalid, const char* message) {
        std::vector<std::uint8_t> output(bp16.source.size() + 2 * Guard, 0xa5);
        require(!zvram::snapshot::decodeBatch(&invalid, 1, output.data() + Guard,
                bp16.source.size(), ChunkLimit, false), message);
        require(std::all_of(output.begin(), output.end(), [](auto byte) { return byte == 0xa5; }),
                "BP16 preflight failure changed staging bytes");
    };

    auto bad = bp16.chunk;
    bad.byteShuffle = 2;
    expectBp16Failure(bad, "BP16 with byte-shuffle metadata accepted");
    bad = bp16.chunk;
    bad.compressed = false;
    expectBp16Failure(bad, "RAW-tagged BP16 frame accepted");
    bad = bp16.chunk;
    bad.rawSize += 256;
    expectBp16Failure(bad, "BP16 expected-size mismatch accepted");
    bad = bp16.chunk;
    bad.storedSize--;
    expectBp16Failure(bad, "truncated BP16 frame accepted");
    auto malformed = bp16.encoded;
    const auto desc = zvram::bp16::HeaderBytes;
    auto packed = zvram::bp16::load32(malformed.data() + desc + 4);
    zvram::bp16::store32(malformed.data() + desc + 4, packed | 0x00010001u);
    bad.data = malformed.data();
    bad.storedSize = malformed.size();
    expectBp16Failure(bad, "malformed BP16 base/mask accepted");
}
} // namespace

int main() try {
    Fixture fixture;
    testBp16Batch(fixture);
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
    auto unknownCodec = fixture.mixed;
    unknownCodec[0].codec = static_cast<zvram::snapshot::Codec>(999);
    expectFailure(unknownCodec.data(), unknownCodec.size(), Total, ChunkLimit,
                  "unknown snapshot codec accepted");
#ifndef ZVRAM_HAVE_GDEFLATE
    auto unavailableCodec = fixture.mixed;
    unavailableCodec[0].codec = zvram::snapshot::Codec::GDeflate;
    expectFailure(unavailableCodec.data(), unavailableCodec.size(), Total, ChunkLimit,
                  "unbuilt GDeflate codec accepted");
#endif

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

    std::cout << "PASS: serial/parallel mixed Zstd+BP16+RAW+stride2/4, odd tails, canaries, "
                 "corruption, truncation, metadata, and bounds\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
