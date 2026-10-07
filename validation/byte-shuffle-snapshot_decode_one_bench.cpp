#include "snapshot_decode.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool run(const char* label, const std::string& path,
         const std::vector<std::uint8_t>& expected, unsigned shuffle) {
    auto compressed = readFile(path);
    std::vector<std::uint8_t> output(expected.size());
    const zvram::snapshot::EncodedChunk chunk{
        compressed.data(), compressed.size(), expected.size(), true, shuffle};
    std::vector<double> times;
    for (int i = 0; i < 5; ++i) {
        const auto start = Clock::now();
        const bool ok = zvram::snapshot::decodeOne(chunk, output.data());
        const auto stop = Clock::now();
        if (!ok || output != expected) {
            std::cerr << label << " failed exact decode on repetition " << i << '\n';
            return false;
        }
        times.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    std::sort(times.begin(), times.end());
    std::cout << label << " stored=" << compressed.size() << " raw=" << expected.size()
              << " reps_ms=";
    for (std::size_t i = 0; i < times.size(); ++i)
        std::cout << (i ? "," : "") << times[i];
    std::cout << " median_ms=" << times[2] << '\n';
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: snapshot_decode_one_bench RAW PLAIN STRIDE2 STRIDE4\n";
        return 2;
    }
    try {
        const auto expected = readFile(argv[1]);
        if (expected.size() != 32u * 1024u * 1024u) {
            std::cerr << "expected fixture must be exactly 32 MiB\n";
            return 2;
        }
        if (!run("plain", argv[2], expected, 0) ||
            !run("stride2", argv[3], expected, 2) ||
            !run("stride4", argv[4], expected, 4)) return 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
