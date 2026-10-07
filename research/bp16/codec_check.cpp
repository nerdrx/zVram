#include "bp16.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    zvram::bp16::store16(bytes.data() + offset, value);
}

void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    zvram::bp16::store32(bytes.data() + offset, value);
}

std::uint32_t get32(const Bytes& bytes, std::size_t offset) {
    return zvram::bp16::load32(bytes.data() + offset);
}

bool encodeReference(const Bytes& raw, Bytes& encoded) {
    using namespace zvram::bp16;
    if (raw.empty() || raw.size() > MaxRawBytes || raw.size() % RawBytesPerBlock) return false;
    const auto blockCount = static_cast<std::uint32_t>(raw.size() / RawBytesPerBlock);
    std::vector<std::uint32_t> descriptors(blockCount);
    std::size_t total = HeaderBytes + std::size_t(blockCount) * DescriptorBytes;
    for (std::uint32_t block = 0; block < blockCount; ++block) {
        const auto* src = raw.data() + std::size_t(block) * RawBytesPerBlock;
        std::uint16_t allAnd = 0xffffu, allOr = 0;
        for (std::size_t i = 0; i < WordsPerBlock; ++i) {
            const auto word = load16(src + i * 2);
            allAnd &= word;
            allOr |= word;
        }
        const auto mask = static_cast<std::uint16_t>(allOr ^ allAnd);
        descriptors[block] = std::uint32_t(allAnd) | (std::uint32_t(mask) << 16);
        total += 16u * popcount16(mask);
    }
    encoded.assign(total, 0);
    store32(encoded.data(), Magic);
    store32(encoded.data() + 4, Version);
    store32(encoded.data() + 8, static_cast<std::uint32_t>(raw.size()));
    store32(encoded.data() + 12, blockCount);
    std::size_t nextPayload = HeaderBytes + std::size_t(blockCount) * DescriptorBytes;
    for (std::uint32_t block = 0; block < blockCount; ++block) {
        const auto* src = raw.data() + std::size_t(block) * RawBytesPerBlock;
        const auto packed = descriptors[block];
        const auto mask = static_cast<std::uint16_t>(packed >> 16);
        auto* descriptor = encoded.data() + HeaderBytes + std::size_t(block) * DescriptorBytes;
        store32(descriptor, static_cast<std::uint32_t>(nextPayload));
        store32(descriptor + 4, packed);
        std::size_t bitOffset = 0;
        for (std::size_t i = 0; i < WordsPerBlock; ++i) {
            const auto word = load16(src + i * 2);
            unsigned compactBit = 0;
            for (unsigned sourceBit = 0; sourceBit < 16; ++sourceBit) {
                if (!(mask & (std::uint16_t(1u) << sourceBit))) continue;
                if (word & (std::uint16_t(1u) << sourceBit))
                    encoded[nextPayload + (bitOffset + compactBit) / 8] |=
                        std::uint8_t(1u << ((bitOffset + compactBit) % 8));
                ++compactBit;
            }
            bitOffset += compactBit;
        }
        nextPayload += 16u * popcount16(mask);
    }
    return nextPayload == encoded.size();
}

Bytes makeKBlock(unsigned k) {
    const auto mask = static_cast<std::uint16_t>(k == 16 ? 0xffffu : ((1u << k) - 1u));
    const auto base = static_cast<std::uint16_t>(0xa55au & ~mask);
    Bytes raw(zvram::bp16::RawBytesPerBlock);
    for (std::size_t i = 0; i < zvram::bp16::WordsPerBlock; ++i) {
        std::uint16_t variable = static_cast<std::uint16_t>((i * 0x9e37u) & mask);
        if (i < k) variable |= static_cast<std::uint16_t>(1u << i);
        put16(raw, i * 2, static_cast<std::uint16_t>(base | (variable & mask)));
    }
    return raw;
}

void roundTrip(const Bytes& source) {
    Bytes encoded;
    Bytes portableFast;
    Bytes runtimeFast;
    Bytes decoded;
    require(encodeReference(source, encoded), "reference encode rejected valid raw bytes");
    require(zvram::bp16::detail::encodeImpl(source.data(), source.size(), portableFast, false),
            "portable fast encoder rejected valid raw bytes");
    require(zvram::bp16::encodeFast(source, runtimeFast),
            "runtime fast encoder rejected valid raw bytes");
    require(encoded == portableFast, "portable accumulator stream differs from reference");
    require(encoded == runtimeFast, "runtime-selected stream differs from reference");
    require(zvram::bp16::validate(encoded.data(), encoded.size(),
                                  static_cast<std::uint32_t>(source.size())),
            "encoder emitted invalid frame");
    require(zvram::bp16::decode(encoded, decoded,
                                static_cast<std::uint32_t>(source.size())),
            "decoder rejected encoded frame");
    require(decoded == source, "round-trip bytes differ");
}

void testAllK() {
    for (unsigned k = 0; k <= 16; ++k) {
        const auto raw = makeKBlock(k);
        Bytes encoded;
        Bytes portableFast;
        Bytes runtimeFast;
        Bytes decoded;
        require(encodeReference(raw, encoded), "reference encode rejected k-pattern");
        require(zvram::bp16::detail::encodeImpl(raw.data(), raw.size(), portableFast, false),
                "portable fast encode rejected k-pattern");
        require(zvram::bp16::encodeFast(raw, runtimeFast),
                "runtime fast encode rejected k-pattern");
        require(encoded == portableFast && encoded == runtimeFast,
                "optimized k-pattern stream differs from reference");
        const auto packed = get32(encoded, zvram::bp16::HeaderBytes + 4);
        const auto mask = static_cast<std::uint16_t>(packed >> 16);
        require(zvram::bp16::popcount16(mask) == k, "unexpected varying-mask width");
        require((packed & 0xffffu) == (static_cast<std::uint16_t>(0xa55au) & ~mask),
                "unexpected fixed base");
        require(encoded.size() == zvram::bp16::HeaderBytes +
                zvram::bp16::DescriptorBytes + 16u * k,
                "unexpected k-pattern frame size");
        require(zvram::bp16::decode(encoded, decoded), "decode rejected k-pattern");
        require(decoded == raw, "k-pattern round-trip differs");
    }
}

void testPatterns() {
    Bytes constant(3 * zvram::bp16::RawBytesPerBlock);
    for (std::size_t i = 0; i < constant.size(); i += 2) put16(constant, i, 0xa55au);
    roundTrip(constant);

    std::mt19937 rng(0x42503136u);
    std::uniform_int_distribution<unsigned> anyWord(0, 0xffffu);
    Bytes random(8 * zvram::bp16::RawBytesPerBlock);
    for (std::size_t i = 0; i < random.size(); i += 2)
        put16(random, i, static_cast<std::uint16_t>(anyWord(rng)));
    roundTrip(random);

    Bytes sparse(4 * zvram::bp16::RawBytesPerBlock);
    const std::uint16_t masks[] = {0x0005u, 0x0180u, 0x4500u, 0x7fffu};
    const std::uint16_t bases[] = {0xaaaau, 0x2401u, 0x80a5u, 0x8000u};
    for (std::size_t block = 0; block < 4; ++block) {
        const auto mask = masks[block];
        const auto base = static_cast<std::uint16_t>(bases[block] & ~mask);
        for (std::size_t i = 0; i < zvram::bp16::WordsPerBlock; ++i) {
            const auto variable = static_cast<std::uint16_t>((rng() ^ (i * 0x1021u)) & mask);
            put16(sparse, block * zvram::bp16::RawBytesPerBlock + i * 2,
                  static_cast<std::uint16_t>(base | variable));
        }
    }
    roundTrip(sparse);

    Bytes partial;
    require(!zvram::bp16::encode(random.data(), random.size() - 2, partial),
            "encoder accepted a non-block-aligned input");
    require(!zvram::bp16::encode(random.data(), 0, partial), "encoder accepted empty input");
}

void testMalformedFrames() {
    const auto raw = makeKBlock(7);
    Bytes valid;
    require(encodeReference(raw, valid), "valid fixture encode failed");
    auto rejects = [&](Bytes damaged, const char* message) {
        Bytes output{0x55u};
        require(!zvram::bp16::decode(damaged.data(), damaged.size(), output), message);
        require(output == Bytes{0x55u}, "invalid frame changed output");
    };

    rejects(Bytes(valid.begin(), valid.begin() + 15), "accepted truncated header");
    auto bad = valid; bad[0] ^= 1u; rejects(std::move(bad), "accepted bad magic");
    bad = valid; put32(bad, 4, 2); rejects(std::move(bad), "accepted bad version");
    bad = valid; put32(bad, 8, 0); rejects(std::move(bad), "accepted empty raw size");
    bad = valid; put32(bad, 8, 257); rejects(std::move(bad), "accepted partial block");
    bad = valid; put32(bad, 8, zvram::bp16::MaxRawBytes + 256);
    rejects(std::move(bad), "accepted oversized frame");
    bad = valid; put32(bad, 12, 2); rejects(std::move(bad), "accepted bad block count");
    bad = valid; put32(bad, zvram::bp16::HeaderBytes, get32(valid, zvram::bp16::HeaderBytes) + 1);
    rejects(std::move(bad), "accepted noncanonical first payload offset");
    bad = valid;
    const auto desc = zvram::bp16::HeaderBytes;
    const auto originalPacked = get32(bad, desc + 4);
    put32(bad, desc + 4, originalPacked | 0x00010001u);
    rejects(std::move(bad), "accepted overlapping base and mask");
    bad = valid; bad.pop_back(); rejects(std::move(bad), "accepted truncated payload");
    bad = valid; bad.push_back(0); rejects(std::move(bad), "accepted trailing data");

    Bytes output;
    require(!zvram::bp16::decode(valid, output, 2 * zvram::bp16::RawBytesPerBlock),
            "accepted mismatched expected raw size");
}

void writeFile(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("failed writing " + path.string());
    }
}

Bytes readSample(const std::filesystem::path& path, std::uint64_t offset,
                 std::size_t bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open sample model: " + path.string());
    file.seekg(static_cast<std::streamoff>(offset));
    Bytes raw(bytes);
    if (!file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(bytes)))
        throw std::runtime_error("sample range is unavailable in " + path.string());
    return raw;
}

void testRealSample(const std::filesystem::path& model,
                    const std::filesystem::path& outputDir) {
    constexpr std::uint64_t offset = 64ull * 1024u * 1024u;
    constexpr std::size_t bytes = 32u * 1024u * 1024u;
    const auto raw = readSample(model, offset, bytes);
    Bytes reference;
    Bytes portableFast;
    Bytes encoded;
    Bytes decoded;
    const auto encodeStart = std::chrono::steady_clock::now();
    require(encodeReference(raw, reference), "real sample reference encode failed");
    const auto referenceEncodeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - encodeStart).count();
    const auto portableStart = std::chrono::steady_clock::now();
    require(zvram::bp16::detail::encodeImpl(raw.data(), raw.size(), portableFast, false),
            "real sample portable fast encode failed");
    const auto portableEncodeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - portableStart).count();
    require(reference == portableFast, "real sample portable stream differs from reference");
    portableFast.clear();
    portableFast.shrink_to_fit();
    const auto fastStart = std::chrono::steady_clock::now();
    require(zvram::bp16::encodeFast(raw, encoded), "real sample runtime fast encode failed");
    const auto fastEncodeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - fastStart).count();
    require(reference == encoded, "real sample runtime stream differs from reference");
    reference.clear();
    reference.shrink_to_fit();
    const auto decodeStart = std::chrono::steady_clock::now();
    require(zvram::bp16::decode(encoded, decoded, bytes), "real sample decode failed");
    const auto decodeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - decodeStart).count();
    require(decoded == raw, "real sample round-trip differs");

    std::filesystem::create_directories(outputDir);
    writeFile(outputDir / "internlm2_5-20b-chat-fp16-64MiB.raw", raw);
    writeFile(outputDir / "internlm2_5-20b-chat-fp16-64MiB.bp16", encoded);
    std::ofstream timing(outputDir / "timing.json", std::ios::trunc);
    if (!timing) throw std::runtime_error("failed writing timing.json");
    timing << "{\n"
           << "  \"scope\": \"CPU component only; no GPU timing\",\n"
           << "  \"bmi2_selected\": " << (zvram::bp16::detail::cpuHasBmi2() ? "true" : "false") << ",\n"
           << "  \"sample_offset_bytes\": " << offset << ",\n"
           << "  \"raw_bytes\": " << raw.size() << ",\n"
           << "  \"encoded_bytes\": " << encoded.size() << ",\n"
           << "  \"ratio\": " << static_cast<double>(encoded.size()) / raw.size() << ",\n"
           << "  \"reference_encode_nanoseconds\": " << referenceEncodeNs << ",\n"
           << "  \"portable_fast_encode_nanoseconds\": " << portableEncodeNs << ",\n"
           << "  \"runtime_fast_encode_nanoseconds\": " << fastEncodeNs << ",\n"
           << "  \"decode_nanoseconds\": " << decodeNs << ",\n"
           << "  \"round_trip_equal\": true\n"
           << "}\n";
    if (!timing) throw std::runtime_error("failed writing timing.json");
    std::cout << "BP16 sample raw=" << raw.size() << " encoded=" << encoded.size()
              << " ratio=" << static_cast<double>(encoded.size()) / raw.size()
              << " bmi2=" << (zvram::bp16::detail::cpuHasBmi2() ? "yes" : "no")
              << " reference-encode-ns=" << referenceEncodeNs
              << " portable-fast-encode-ns=" << portableEncodeNs
              << " runtime-fast-encode-ns=" << fastEncodeNs
              << " decode-ns=" << decodeNs << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 3) throw std::runtime_error("usage: bp16-codec-check [model.gguf [output-dir]]");
        testAllK();
        testPatterns();
        testMalformedFrames();
        std::cout << "PASS: BP16 k=0..16, constant, random, sparse, malformed, and round-trip checks\n";
        const std::filesystem::path model = argc > 1 ? argv[1] :
            "build/third-party/models/internlm2_5-20b-chat-fp16.gguf";
        const std::filesystem::path outputDir = argc > 2 ? argv[2] : "build/bp16-research";
        testRealSample(model, outputDir);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
