#include "bp16_codec.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void writeFile(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("failed writing " + path.string());
}

std::uint32_t nextRandom(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::runtime_error("usage: all_masks_fixture [output-dir]");
        const std::filesystem::path outputDir = argc == 2
            ? argv[1] : "build/bp16-fastpath-research";
        constexpr std::uint32_t blockCount = 1u << 16;
        constexpr std::size_t rawBytes =
            std::size_t(blockCount) * zvram::bp16::RawBytesPerBlock;
        static_assert(rawBytes <= zvram::bp16::MaxRawBytes, "fixture exceeds BP16 limit");

        Bytes raw(rawBytes);
        std::uint32_t random = 0x91e10da5u;
        for (std::uint32_t mask = 0; mask < blockCount; ++mask) {
            auto* block = raw.data() + std::size_t(mask) * zvram::bp16::RawBytesPerBlock;
            for (std::uint32_t word = 0; word < zvram::bp16::WordsPerBlock; ++word) {
                const auto value = word == 0 ? 0u :
                    (word == 1 ? mask : (nextRandom(random) & mask));
                zvram::bp16::store16(block + word * 2u, static_cast<std::uint16_t>(value));
            }
        }

        Bytes encoded, decoded(rawBytes);
        if (!zvram::bp16::encode(raw, encoded) || encoded.size() > zvram::bp16::MaxRawBytes ||
            !zvram::bp16::validate(encoded.data(), encoded.size(), rawBytes) ||
            !zvram::bp16::decode(encoded.data(), encoded.size(), decoded.data(), rawBytes) ||
            decoded != raw)
            throw std::runtime_error("all-mask frame failed BP16 validation or CPU roundtrip");

        for (std::uint32_t mask = 0; mask < blockCount; ++mask) {
            const std::size_t descriptor = zvram::bp16::HeaderBytes +
                std::size_t(mask) * zvram::bp16::DescriptorBytes;
            const auto payloadOffset = zvram::bp16::load32(encoded.data() + descriptor);
            const auto packed = zvram::bp16::load32(encoded.data() + descriptor + 4u);
            if ((packed >> 16) != mask || (packed & 0xffffu) != 0)
                throw std::runtime_error("descriptor did not preserve its requested exact mask/base");
            if (mask + 1u == blockCount &&
                std::size_t(payloadOffset) +
                    16u * zvram::bp16::popcount16(static_cast<std::uint16_t>(mask)) != encoded.size())
                throw std::runtime_error("last block payload does not end at frame boundary");
        }

        std::filesystem::create_directories(outputDir);
        writeFile(outputDir / "all-masks.raw", raw);
        writeFile(outputDir / "all-masks.bp16", encoded);
        std::cout << "PASS: all 65,536 exact masks/base=0; blocks=" << blockCount
                  << " raw-bytes=" << raw.size() << " frame-bytes=" << encoded.size()
                  << " CPU-roundtrip=exact last-payload-end=exact\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
