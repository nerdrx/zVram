#include "bp16.hpp"

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

std::uint16_t maskFor(unsigned k, unsigned block) {
    std::uint16_t mask = 0;
    for (unsigned bit = 0; bit < k; ++bit)
        mask |= static_cast<std::uint16_t>(1u << ((bit * 5u + block * 3u) & 15u));
    return mask;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::runtime_error("usage: bp16-gpu-fixture [output-dir]");
        const std::filesystem::path outputDir = argc == 2 ? argv[1] : "build/bp16-research";
        Bytes raw(17u * zvram::bp16::RawBytesPerBlock);
        for (unsigned block = 0; block <= 16; ++block) {
            const auto mask = maskFor(block, block);
            const auto base = static_cast<std::uint16_t>(0xa55au & ~mask);
            auto* output = raw.data() + std::size_t(block) * zvram::bp16::RawBytesPerBlock;
            for (unsigned word = 0; word < zvram::bp16::WordsPerBlock; ++word) {
                const auto varying = static_cast<std::uint16_t>(
                    ((word * 0x9e37u) ^ (word >> 1)) & mask);
                zvram::bp16::store16(output + word * 2, static_cast<std::uint16_t>(base | varying));
            }
            for (unsigned bit = 0; bit < block; ++bit) {
                const auto position = (bit * 5u + block * 3u) & 15u;
                zvram::bp16::store16(output + bit * 2,
                    static_cast<std::uint16_t>(base | (1u << position)));
            }
            zvram::bp16::store16(output + 126u * 2u, base);
        }

        Bytes encoded, decoded;
        if (!zvram::bp16::encodeFast(raw, encoded) ||
            !zvram::bp16::validate(encoded.data(), encoded.size(), raw.size()) ||
            !zvram::bp16::decode(encoded, decoded, raw.size()) || decoded != raw)
            throw std::runtime_error("mixed-pattern fixture failed CPU round-trip validation");
        for (unsigned block = 0; block <= 16; ++block) {
            const auto descriptor = zvram::bp16::HeaderBytes + block * zvram::bp16::DescriptorBytes;
            const auto packed = zvram::bp16::load32(encoded.data() + descriptor + 4);
            const auto mask = static_cast<std::uint16_t>(packed >> 16);
            const auto base = static_cast<std::uint16_t>(packed);
            const auto expectedMask = maskFor(block, block);
            const auto expectedBase = static_cast<std::uint16_t>(0xa55au & ~expectedMask);
            if (zvram::bp16::popcount16(mask) != block || mask != expectedMask || base != expectedBase)
                throw std::runtime_error("fixture descriptor does not cover the requested k/base case");
        }
        const auto lastDescriptor = zvram::bp16::HeaderBytes + 16u * zvram::bp16::DescriptorBytes;
        if (zvram::bp16::load32(encoded.data() + lastDescriptor) + 256u != encoded.size())
            throw std::runtime_error("last fixture payload does not end at the frame boundary");
        std::filesystem::create_directories(outputDir);
        writeFile(outputDir / "gpu-mixed-pattern.raw", raw);
        writeFile(outputDir / "gpu-mixed-pattern.bp16", encoded);
        std::cout << "BP16 mixed fixture blocks=17 raw-bytes=" << raw.size()
                  << " frame-bytes=" << encoded.size()
                  << " k=0..16 base-and-sparse-mask=checked last-payload-end=checked\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
