#include "../../snapshot_decode.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <zstd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t MaxRaw = 32u * 1024u * 1024u;

std::vector<std::uint8_t> readBounded(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error(std::string("cannot open input: ") + path);
    const auto end = file.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) > MaxRaw)
        throw std::runtime_error("raw input must contain 1..33554432 bytes");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot read complete input");
    return bytes;
}

std::uint64_t elapsedNs(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - start).count());
}
} // namespace

int main(int argc, char** argv) try {
    if (argc != 3) {
        std::cerr << "usage: check_codec_model RAW_INPUT OUTPUT.gdeflate\n";
        return 2;
    }
    if (std::filesystem::absolute(argv[1]).lexically_normal() ==
        std::filesystem::absolute(argv[2]).lexically_normal() ||
        (std::filesystem::exists(argv[2]) && std::filesystem::equivalent(argv[1], argv[2])))
        throw std::runtime_error("input and output paths must differ");
    const auto raw = readBounded(argv[1]);
    std::vector<std::uint8_t> gdeflateBytes;
    auto started = Clock::now();
    const bool encoded = zvram::gdeflate::encode(raw.data(), raw.size(), gdeflateBytes);
    const auto gdeflateEncodeNs = elapsedNs(started);
    if (!encoded) throw std::runtime_error("GDeflate encode failed");

    std::vector<std::uint8_t> decoded(raw.size());
    zvram::snapshot::EncodedChunk gdeflateChunk{
        gdeflateBytes.data(), gdeflateBytes.size(), raw.size(), true, 0,
        zvram::snapshot::Codec::GDeflate};
    started = Clock::now();
    const bool gdeflateDecoded = zvram::snapshot::decodeOne(gdeflateChunk, decoded.data());
    const auto gdeflateDecodeNs = elapsedNs(started);
    if (!gdeflateDecoded || decoded != raw)
        throw std::runtime_error("production snapshot::decodeOne GDeflate bytes differ");

    std::vector<std::uint8_t> zstdBytes;
    started = Clock::now();
    zstdBytes.resize(ZSTD_compressBound(raw.size()));
    const auto zstdSize = ZSTD_compress(zstdBytes.data(), zstdBytes.size(),
                                        raw.data(), raw.size(), 1);
    const auto zstdEncodeNs = elapsedNs(started);
    if (ZSTD_isError(zstdSize)) throw std::runtime_error("Zstd level-1 encode failed");
    zstdBytes.resize(zstdSize);
    zvram::snapshot::EncodedChunk zstdChunk{
        zstdBytes.data(), zstdBytes.size(), raw.size(), true, 0,
        zvram::snapshot::Codec::Zstd};
    started = Clock::now();
    const bool zstdDecoded = zvram::snapshot::decodeOne(zstdChunk, decoded.data());
    const auto zstdDecodeNs = elapsedNs(started);
    if (!zstdDecoded || decoded != raw)
        throw std::runtime_error("production snapshot::decodeOne Zstd bytes differ");

    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    if (!output || !output.write(reinterpret_cast<const char*>(gdeflateBytes.data()),
                                 static_cast<std::streamsize>(gdeflateBytes.size())))
        throw std::runtime_error("cannot write verified GDeflate output");
    output.close();
    if (!output) throw std::runtime_error("failed closing verified GDeflate output");

    std::cout << "CPU-only codec comparison; no GPU/Vulkan dispatch\n"
              << "raw_bytes=" << raw.size() << "\n"
              << "gdeflate_encoded_bytes=" << gdeflateBytes.size()
              << " gdeflate_encode_cpu_ns=" << gdeflateEncodeNs
              << " gdeflate_decodeOne_cpu_ns=" << gdeflateDecodeNs << " exact=1\n"
              << "zstd1_encoded_bytes=" << zstdBytes.size()
              << " zstd1_encode_cpu_ns=" << zstdEncodeNs
              << " zstd1_decodeOne_cpu_ns=" << zstdDecodeNs << " exact=1\n"
              << "ratio_gdeflate=" << static_cast<double>(gdeflateBytes.size()) / raw.size()
              << " ratio_zstd1=" << static_cast<double>(zstdBytes.size()) / raw.size() << "\n"
              << "output=" << argv[2] << " status=PASS\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
