#include "gdeflate_envelope.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using zvram::gdeflate::Info;
using zvram::gdeflate::Limits;
using zvram::gdeflate::TileBytes;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::vector<std::uint8_t> envelope(std::size_t decoded,
                                   const std::vector<std::uint32_t>& tileBytes) {
    const auto count = tileBytes.size();
    std::vector<std::uint8_t> data(8 + 4 * count, 0);
    data[0] = 4;
    data[1] = 0xfb;
    data[2] = static_cast<std::uint8_t>(count);
    data[3] = static_cast<std::uint8_t>(count >> 8);
    const auto tail = static_cast<std::uint32_t>(decoded % TileBytes);
    put32(data, 4, (1u | (tail << 2)));
    put32(data, 8, tileBytes.back());
    std::uint32_t offset = 0;
    for (std::size_t i = 1; i < count; ++i) {
        offset += tileBytes[i - 1];
        put32(data, 8 + i * 4, offset);
    }
    for (std::size_t i = 0; i < count; ++i)
        data.insert(data.end(), tileBytes[i], static_cast<std::uint8_t>(0x30 + i));
    return data;
}

Limits limitsFor(std::size_t encoded, std::size_t decoded) {
    return {encoded, decoded, decoded, 0, 0, zvram::gdeflate::MaxTiles};
}

void rejects(const std::vector<std::uint8_t>& bytes, Limits limits, const char* message) {
    require(!zvram::gdeflate::validateEnvelope(bytes.data(), bytes.size(), limits), message);
}

int inspect(const char* path, const char* decodedText) {
    constexpr std::size_t MaxEncoded = 64u * 1024u * 1024u;
    constexpr std::size_t MaxDecoded = 32u * 1024u * 1024u;
    std::size_t decoded{};
    try {
        std::size_t used{};
        const auto value = std::stoull(decodedText, &used, 10);
        if (!value || used != std::string(decodedText).size() || value > MaxDecoded) {
            std::cerr << "FAIL: decoded byte count must be 1..33554432\n";
            return 2;
        }
        decoded = static_cast<std::size_t>(value);
    } catch (...) {
        std::cerr << "FAIL: invalid decoded byte count\n";
        return 2;
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) { std::cerr << "FAIL: cannot open input\n"; return 2; }
    const auto end = file.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) > MaxEncoded) {
        std::cerr << "FAIL: encoded file must contain 1..67108864 bytes\n";
        return 2;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        std::cerr << "FAIL: cannot read input\n";
        return 2;
    }
    Limits limits{MaxEncoded, MaxDecoded, decoded, 0, 0, zvram::gdeflate::MaxTiles};
    Info info{};
    if (!zvram::gdeflate::validateEnvelope(data.data(), data.size(), limits, &info)) {
        std::cerr << "FAIL: invalid GDeflate envelope\n";
        return 1;
    }
    std::cout << "{\"valid\":true,\"encoded_bytes\":" << data.size()
              << ",\"decoded_bytes\":" << info.decodedBytes
              << ",\"tile_count\":" << info.tileCount
              << ",\"last_tile_bytes\":" << info.lastTileBytes
              << ",\"payload_offset\":" << info.payloadOffset
              << ",\"payload_bytes\":" << info.payloadBytes << "}\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) try {
    if (argc == 4 && std::string(argv[1]) == "--inspect")
        return inspect(argv[2], argv[3]);
    if (argc != 1) {
        std::cerr << "usage: gdeflate_envelope_check [--inspect ENCODED DECODED_BYTES]\n";
        return 2;
    }
    auto single = envelope(TileBytes, {8});
    Info info{};
    require(zvram::gdeflate::validateEnvelope(single.data(), single.size(),
                                               limitsFor(single.size(), TileBytes), &info),
            "valid one-tile envelope rejected");
    require(info.tileCount == 1 && info.lastTileBytes == TileBytes &&
            info.decodedBytes == TileBytes && info.payloadOffset == 12 && info.payloadBytes == 8,
            "single-tile metadata mismatch");

    constexpr std::size_t tailBytes = 123;
    const auto decoded = 2u * std::size_t(TileBytes) + tailBytes;
    auto multi = envelope(decoded, {8, 12, 16});
    require(zvram::gdeflate::validateEnvelope(multi.data(), multi.size(),
                                               limitsFor(multi.size(), decoded), &info),
            "valid multi-tile tail envelope rejected");
    require(info.tileCount == 3 && info.lastTileBytes == tailBytes &&
            info.decodedBytes == decoded && info.payloadOffset == 20 && info.payloadBytes == 36,
            "multi-tile metadata mismatch");
    auto alignedLimits = limitsFor(multi.size(), decoded);
    alignedLimits.inputBufferOffset = 4;
    alignedLimits.outputBufferOffset = 8;
    require(zvram::gdeflate::validateEnvelope(multi.data(), multi.size(), alignedLimits),
            "valid aligned buffer offsets rejected");

    auto bad = multi;
    bad[0] = 5;
    rejects(bad, limitsFor(multi.size(), decoded), "wrong codec id accepted");
    bad = multi; bad[1] ^= 1;
    rejects(bad, limitsFor(multi.size(), decoded), "bad magic accepted");
    bad = multi; put32(bad, 4, 1u | (tailBytes << 2) | (1u << 20));
    rejects(bad, limitsFor(multi.size(), decoded), "reserved header bits accepted");
    bad = multi; put32(bad, 4, 0u | (tailBytes << 2));
    rejects(bad, limitsFor(multi.size(), decoded), "wrong tile-size index accepted");
    bad = multi; bad[2] = bad[3] = 0;
    rejects(bad, limitsFor(multi.size(), decoded), "zero tile count accepted");
    bad = multi; put32(bad, 4, 1u | (0x3ffffu << 2));
    rejects(bad, limitsFor(multi.size(), decoded), "noncanonical full-tile tail accepted");

    rejects(multi, limitsFor(multi.size() - 1, decoded), "encoded-size limit ignored");
    rejects(multi, limitsFor(multi.size(), decoded - 1), "decoded-size limit ignored");
    rejects(multi, limitsFor(multi.size(), decoded + 1), "expected decoded-size mismatch accepted");
    auto constrained = limitsFor(multi.size(), decoded);
    constrained.maxTiles = 2;
    rejects(multi, constrained, "tile-count limit ignored");
    constrained = limitsFor(multi.size(), decoded); constrained.inputBufferOffset = 2;
    rejects(multi, constrained, "unaligned input buffer offset accepted");
    constrained = limitsFor(multi.size(), decoded); constrained.outputBufferOffset = 2;
    rejects(multi, constrained, "unaligned output buffer offset accepted");
    constrained = limitsFor(multi.size(), decoded); constrained.inputBufferOffset = UINT32_MAX - 3;
    rejects(multi, constrained, "input offset overflow accepted");
    constrained = limitsFor(multi.size(), decoded); constrained.outputBufferOffset = UINT32_MAX - 1;
    rejects(multi, constrained, "output offset overflow accepted");
    const auto oneByte = envelope(1, {8});
    constrained = limitsFor(oneByte.size(), 1); constrained.outputBufferOffset = UINT32_MAX - 3;
    rejects(oneByte, constrained, "padded final output word overflow accepted");

    bad = multi; bad.resize(7);
    rejects(bad, limitsFor(multi.size(), decoded), "truncated header accepted");
    bad = multi; bad.resize(8 + 4 * 2);
    rejects(bad, limitsFor(multi.size(), decoded), "truncated tile table accepted");
    bad = multi; put32(bad, 12, 9);
    rejects(bad, limitsFor(multi.size(), decoded), "unaligned tile offset accepted");
    bad = multi; put32(bad, 12, 0);
    rejects(bad, limitsFor(multi.size(), decoded), "duplicate tile offset accepted");
    bad = multi; put32(bad, 16, 8);
    rejects(bad, limitsFor(multi.size(), decoded), "unordered tile offsets accepted");
    bad = multi; put32(bad, 16, 0xfffffffcu);
    rejects(bad, limitsFor(multi.size(), decoded), "out-of-payload tile offset accepted");
    bad = multi; put32(bad, 8, 15);
    rejects(bad, limitsFor(multi.size(), decoded), "unaligned last tile length accepted");
    bad = multi; put32(bad, 8, 12);
    rejects(bad, limitsFor(multi.size(), decoded), "short last tile length accepted");
    bad = multi; bad.push_back(0);
    rejects(bad, limitsFor(bad.size(), decoded), "trailing payload bytes accepted");
    bad = multi; bad.resize(bad.size() - 4);
    rejects(bad, limitsFor(multi.size(), decoded), "truncated final tile accepted");

    bad = multi; bad[2] = 0xff; bad[3] = 0xff; // maximal count, but no corresponding table
    rejects(bad, limitsFor(bad.size(), decoded), "truncated maximal tile table accepted");

    std::vector<std::uint32_t> maximumTiles(zvram::gdeflate::MaxTiles, 4);
    const auto maximumDecoded = std::size_t(zvram::gdeflate::MaxTiles) * TileBytes;
    const auto maximumCount = envelope(maximumDecoded, maximumTiles);
    require(zvram::gdeflate::validateEnvelope(maximumCount.data(), maximumCount.size(),
                limitsFor(maximumCount.size(), maximumDecoded)),
            "maximum valid tile count rejected");
    auto overDecoded = limitsFor(maximumCount.size(), maximumDecoded);
    overDecoded.expectedDecodedBytes = std::size_t(UINT32_MAX) + 1;
    rejects(maximumCount, overDecoded, "decoded size beyond shader uint32 range accepted");

    std::cout << "PASS: synthetic GDeflate envelope headers, metadata, bounds, alignment, and tile tables\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
