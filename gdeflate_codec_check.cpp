#include "gdeflate_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::size_t Guard = 32;
constexpr std::size_t Tile = zvram::gdeflate::TileBytes;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

std::vector<std::uint8_t> pattern(std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<std::uint8_t>(((i % 251u) * 13u + (i / Tile) * 29u) & 0xffu);
    return bytes;
}

bool hasCanaries(const std::vector<std::uint8_t>& bytes) {
    return std::all_of(bytes.begin(), bytes.begin() + Guard,
                       [](auto x) { return x == 0xa7; }) &&
           std::all_of(bytes.end() - Guard, bytes.end(),
                       [](auto x) { return x == 0xa7; });
}

void roundtrip(const std::vector<std::uint8_t>& input) {
    std::vector<std::uint8_t> encoded;
    require(zvram::gdeflate::encode(input.data(), input.size(), encoded), "GDeflate encode failed");
    zvram::gdeflate::Limits limits{zvram::gdeflate::MaxEncodedBytes,
        zvram::gdeflate::MaxRawBytes, input.size(), 0, 0, zvram::gdeflate::MaxTiles};
    zvram::gdeflate::Info info{};
    require(zvram::gdeflate::validateEnvelope(encoded.data(), encoded.size(), limits, &info),
            "encoder emitted invalid TileStream envelope");
    require(info.decodedBytes == input.size(), "encoder raw-size metadata mismatch");
    std::vector<std::uint8_t> guarded(input.size() + 2 * Guard, 0xa7);
    require(zvram::gdeflate::decode(encoded.data(), encoded.size(), guarded.data() + Guard, input.size()),
            "GDeflate decode failed");
    require(std::equal(input.begin(), input.end(), guarded.begin() + Guard),
            "GDeflate roundtrip differs");
    require(hasCanaries(guarded), "GDeflate decode wrote past output bounds");

    auto malformed = encoded;
    malformed[0] ^= 0xff;
    std::fill(guarded.begin(), guarded.end(), 0xa7);
    require(!zvram::gdeflate::decode(malformed.data(), malformed.size(), guarded.data() + Guard, input.size()),
            "bad codec id accepted");
    require(hasCanaries(guarded), "invalid envelope touched output canary");
    require(!zvram::gdeflate::decode(encoded.data(), encoded.size() - 1,
                                    guarded.data() + Guard, input.size()), "truncated envelope accepted");
    require(hasCanaries(guarded), "truncated envelope touched output canary");

    if (info.tileCount > 1) {
        malformed = encoded;
        malformed[12] ^= 1; // First cumulative tile offset must be dword aligned.
        require(!zvram::gdeflate::decode(malformed.data(), malformed.size(),
                                         guarded.data() + Guard, input.size()),
                "unaligned tile table offset accepted");
        require(hasCanaries(guarded), "bad tile table touched output canary");
    }

    malformed = encoded;
    malformed[info.payloadOffset] = static_cast<std::uint8_t>(
        (malformed[info.payloadOffset] & ~0x06u) | 0x06u); // Reserved DEFLATE block type.
    require(!zvram::gdeflate::decode(malformed.data(), malformed.size(),
                                    guarded.data() + Guard, input.size()),
            "corrupted compressed payload accepted");
    require(hasCanaries(guarded), "corrupt payload damaged output canary");
}

std::filesystem::path fixtureDirectory(const char* requested) {
    if (requested) {
        const std::filesystem::path path(requested);
        if (std::filesystem::exists(path / "synthetic-64k.gdeflate")) return path;
        throw std::runtime_error("fixture directory lacks synthetic-64k.gdeflate: " + path.string());
    }
    for (const auto* candidate : {"research/gdeflate/fixtures", "../research/gdeflate/fixtures",
                                  "../../research/gdeflate/fixtures"})
        if (std::filesystem::exists(std::filesystem::path(candidate) / "synthetic-64k.gdeflate"))
            return candidate;
    throw std::runtime_error("cannot locate committed GDeflate fixtures");
}

std::vector<std::uint8_t> readFixture(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open fixture: " + path.string());
    const auto end = file.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) > zvram::gdeflate::MaxEncodedBytes)
        throw std::runtime_error("fixture exceeds codec bounds: " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot read fixture: " + path.string());
    return bytes;
}

void checkFixture(const std::filesystem::path& path, std::size_t rawSize) {
    const auto encoded = readFixture(path);
    const auto expected = pattern(rawSize);
    std::vector<std::uint8_t> decoded(rawSize + 2 * Guard, 0xa7);
    require(zvram::gdeflate::decode(encoded.data(), encoded.size(), decoded.data() + Guard, rawSize),
            "committed GDeflate fixture failed decode");
    require(std::equal(expected.begin(), expected.end(), decoded.begin() + Guard),
            "committed GDeflate fixture differs from its deterministic pattern");
    require(hasCanaries(decoded), "fixture decode damaged output canary");
}
} // namespace

int main(int argc, char** argv) try {
    if (argc > 2) {
        std::cerr << "usage: gdeflate_codec_check [FIXTURE_DIRECTORY]\n";
        return 2;
    }
    roundtrip(pattern(1));
    roundtrip(pattern(Tile));
    roundtrip(pattern(2 * Tile + 123));

    std::mt19937 rng(0x4a17u);
    std::vector<std::uint8_t> entropy(Tile + 19);
    for (auto& byte : entropy) byte = static_cast<std::uint8_t>(rng());
    roundtrip(entropy);

    std::uint8_t oneByte = 7;
    std::vector<std::uint8_t> encoded{1, 2, 3};
    require(!zvram::gdeflate::encode(&oneByte, zvram::gdeflate::MaxRawBytes + 1, encoded) && encoded.empty(),
            "oversized encode did not refuse before reading/allocating");
    require(!zvram::gdeflate::decode(nullptr, 0, &oneByte,
                                    zvram::gdeflate::MaxRawBytes + 1),
            "oversized decode request accepted");

    const auto fixtures = fixtureDirectory(argc == 2 ? argv[1] : nullptr);
    checkFixture(fixtures / "synthetic-64k.gdeflate", Tile);
    checkFixture(fixtures / "synthetic-multi-tail.gdeflate", 2 * Tile + 123);
    std::cout << "PASS: bounded GDeflate encode/decode, exact sizes, canaries, malformed input, fixtures\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
