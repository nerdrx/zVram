#include "bp16_codec.hpp"
#include "gdeflate_gpu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <type_traits>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
using namespace zvram::bp16;
using ImportedInput = zvram::gdeflate::gpu::Decoder::ImportedHostInput;

void require(bool condition, const char* message);

unsigned destroyedImportBuffers{};
unsigned freedImportMemory{};

void VKAPI_CALL fakeDestroyImportBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {
    ++destroyedImportBuffers;
}
void VKAPI_CALL fakeFreeImportMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {
    ++freedImportMemory;
}

template<class T> T fakeHandle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}

void testImportedHostOwnerAccounting() {
    using Decoder = zvram::gdeflate::gpu::Decoder;
    std::size_t padded{};
    require(Decoder::importedHostAllocationSize(12345, 4096, padded) && padded == 16384,
            "host frame padding calculation failed");
    require(!Decoder::importedHostAllocationSize(12345, 3, padded), "non-power-of-two alignment accepted");
    require(!Decoder::importedHostAllocationSize(12345, 131072, padded), "unbounded alignment accepted");
    constexpr auto maxFrame = zvram::bp16::HeaderBytes +
        (zvram::bp16::MaxRawBytes / zvram::bp16::RawBytesPerBlock) * zvram::bp16::DescriptorBytes +
        zvram::bp16::MaxRawBytes;
    require(!Decoder::importedHostAllocationSize(maxFrame + 1u, 4096, padded),
            "oversized imported frame accepted");
    require(Decoder::importedHostFitsBudget(100, 50, 10, 160), "padded cache quota rejected exact fit");
    require(!Decoder::importedHostFitsBudget(100, 50, 11, 160), "padded cache quota exceeded budget");
    require(!Decoder::importedHostFitsBudget(UINT64_MAX, 1, 0, UINT64_MAX), "quota overflow accepted");

    destroyedImportBuffers = freedImportMemory = 0;
    void* host{};
    require(posix_memalign(&host, 4096, 4096) == 0, "aligned host owner allocation failed");
    auto owner = std::make_shared<ImportedInput>();
    owner->device = fakeHandle<VkDevice>(1); owner->buffer = fakeHandle<VkBuffer>(2);
    owner->memory = fakeHandle<VkDeviceMemory>(3); owner->allocation = host;
    owner->destroyBuffer = fakeDestroyImportBuffer; owner->freeMemory = fakeFreeImportMemory;
    auto moved = std::move(owner);
    require(!owner && moved->data() == host && reinterpret_cast<std::uintptr_t>(host) % 4096 == 0,
            "owner move/alignment failed");
    moved.reset();
    require(destroyedImportBuffers == 1 && freedImportMemory == 1,
            "owned Vulkan import handles were not released exactly once");

    void* poisonedHost{};
    require(posix_memalign(&poisonedHost, 4096, 4096) == 0, "poison owner allocation failed");
    auto poison = std::make_shared<std::atomic<bool>>(true);
    auto retained = std::make_shared<ImportedInput>();
    retained->device = fakeHandle<VkDevice>(4); retained->buffer = fakeHandle<VkBuffer>(5);
    retained->memory = fakeHandle<VkDeviceMemory>(6); retained->allocation = poisonedHost;
    retained->destroyBuffer = fakeDestroyImportBuffer; retained->freeMemory = fakeFreeImportMemory;
    retained->poisoned = poison;
    retained.reset();
    require(destroyedImportBuffers == 1 && freedImportMemory == 1,
            "poisoned import owner freed potentially in-flight handles");
    std::free(poisonedHost); // The poisoned owner intentionally leaked this in production.
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    store16(bytes.data() + offset, value);
}

void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    store32(bytes.data() + offset, value);
}

std::uint32_t get32(const Bytes& bytes, std::size_t offset) {
    return load32(bytes.data() + offset);
}

Bytes pattern(unsigned k, std::uint16_t base = 0xa55au) {
    const auto mask = static_cast<std::uint16_t>(k == 16 ? 0xffffu : ((1u << k) - 1u));
    base = static_cast<std::uint16_t>(base & ~mask);
    Bytes raw(RawBytesPerBlock);
    for (std::size_t i = 0; i < WordsPerBlock; ++i) {
        auto variable = static_cast<std::uint16_t>((i * 0x9e37u) & mask);
        if (i < k) variable |= static_cast<std::uint16_t>(1u << i);
        put16(raw, i * 2, static_cast<std::uint16_t>(base | (variable & mask)));
    }
    return raw;
}

bool encodeReference(const Bytes& raw, Bytes& encoded) {
    if (raw.empty() || raw.size() > MaxRawBytes || raw.size() % RawBytesPerBlock) return false;
    const auto blocks = static_cast<std::uint32_t>(raw.size() / RawBytesPerBlock);
    std::vector<std::uint32_t> meta(blocks);
    std::size_t total = HeaderBytes + std::size_t(blocks) * DescriptorBytes;
    for (std::uint32_t block = 0; block < blocks; ++block) {
        const auto* src = raw.data() + std::size_t(block) * RawBytesPerBlock;
        std::uint16_t allAnd = 0xffffu, allOr = 0;
        for (std::size_t i = 0; i < WordsPerBlock; ++i) {
            const auto word = load16(src + 2 * i);
            allAnd &= word;
            allOr |= word;
        }
        const auto mask = static_cast<std::uint16_t>(allOr ^ allAnd);
        meta[block] = std::uint32_t(allAnd) | (std::uint32_t(mask) << 16);
        total += 16u * popcount16(mask);
    }
    encoded.assign(total, 0);
    store32(encoded.data(), Magic);
    store32(encoded.data() + 4, Version);
    store32(encoded.data() + 8, static_cast<std::uint32_t>(raw.size()));
    store32(encoded.data() + 12, blocks);
    std::size_t next = HeaderBytes + std::size_t(blocks) * DescriptorBytes;
    for (std::uint32_t block = 0; block < blocks; ++block) {
        const auto* src = raw.data() + std::size_t(block) * RawBytesPerBlock;
        const auto packed = meta[block];
        const auto mask = static_cast<std::uint16_t>(packed >> 16);
        auto* desc = encoded.data() + HeaderBytes + std::size_t(block) * DescriptorBytes;
        store32(desc, static_cast<std::uint32_t>(next));
        store32(desc + 4, packed);
        std::size_t bit = 0;
        for (std::size_t i = 0; i < WordsPerBlock; ++i) {
            const auto word = load16(src + 2 * i);
            unsigned outBit = 0;
            for (unsigned inBit = 0; inBit < 16; ++inBit) {
                if (!(mask & (std::uint16_t(1u) << inBit))) continue;
                if (word & (std::uint16_t(1u) << inBit))
                    encoded[next + (bit + outBit) / 8] |=
                        std::uint8_t(1u << ((bit + outBit) % 8));
                ++outBit;
            }
            bit += outBit;
        }
        next += 16u * popcount16(mask);
    }
    return next == encoded.size();
}

void roundTrip(const Bytes& raw) {
    Bytes ref, portable, fast, decoded(raw.size());
    require(encodeReference(raw, ref), "reference encode failed");
    require(detail::encodeImpl(raw.data(), raw.size(), portable, false), "portable encode failed");
    require(encodeFast(raw.data(), raw.size(), fast), "fast encode failed");
    require(ref == portable && ref == fast, "frame differs from reference bytes");
    require(validate(fast.data(), fast.size(), raw.size()), "frame validation failed");
    require(decode(fast.data(), fast.size(), decoded.data(), raw.size()), "pointer decode failed");
    require(decoded == raw, "decoded bytes differ");
    Bytes decodedVector;
    require(decode(fast, decodedVector, static_cast<std::uint32_t>(raw.size())),
            "vector decode failed");
    require(decodedVector == raw, "vector-decoded bytes differ");
}

void testPatterns() {
    for (unsigned k = 0; k <= 16; ++k) roundTrip(pattern(k));
    roundTrip(Bytes(4 * RawBytesPerBlock, 0));
    roundTrip(pattern(0, 0x0f0fu));

    std::mt19937 rng(0x42503136u);
    Bytes random(12 * RawBytesPerBlock);
    for (std::size_t i = 0; i < random.size(); i += 2)
        put16(random, i, static_cast<std::uint16_t>(rng()));
    roundTrip(random);

    Bytes sparse(4 * RawBytesPerBlock);
    const std::uint16_t masks[] = {0x0005u, 0x0180u, 0x4500u, 0x7fffu};
    const std::uint16_t bases[] = {0xaaaau, 0x2401u, 0x80a5u, 0x8000u};
    for (std::size_t block = 0; block < 4; ++block) {
        const auto mask = masks[block];
        const auto base = static_cast<std::uint16_t>(bases[block] & ~mask);
        for (std::size_t i = 0; i < WordsPerBlock; ++i)
            put16(sparse, block * RawBytesPerBlock + i * 2,
                  static_cast<std::uint16_t>(base | (rng() & mask)));
    }
    roundTrip(sparse);

    Bytes output;
    require(!encodeFast(nullptr, RawBytesPerBlock, output), "accepted null input");
    require(!encodeFast(random.data(), random.size() - 2, output), "accepted partial block");
    require(!encodeFast(random.data(), MaxRawBytes + RawBytesPerBlock, output),
            "accepted oversized input");
}

void testMalformed() {
    Bytes encoded;
    require(encodeFast(pattern(7), encoded), "fixture encode failed");
    auto reject = [&](Bytes bad) {
        Bytes out(RawBytesPerBlock, 0x5a);
        require(!validate(bad.data(), bad.size(), RawBytesPerBlock), "accepted malformed frame");
        require(!decode(bad.data(), bad.size(), out.data(), out.size()),
                "decoded malformed frame");
        require(out == Bytes(RawBytesPerBlock, 0x5a), "invalid decode modified output");
    };
    reject(Bytes(encoded.begin(), encoded.begin() + 15));
    auto bad = encoded; bad[0] ^= 1; reject(std::move(bad));
    bad = encoded; put32(bad, 4, 2); reject(std::move(bad));
    bad = encoded; put32(bad, 8, 0); reject(std::move(bad));
    bad = encoded; put32(bad, 8, RawBytesPerBlock + 2); reject(std::move(bad));
    bad = encoded; put32(bad, 8, MaxRawBytes + RawBytesPerBlock); reject(std::move(bad));
    bad = encoded; put32(bad, 12, 2); reject(std::move(bad));
    bad = encoded; put32(bad, HeaderBytes, get32(encoded, HeaderBytes) + 1); reject(std::move(bad));
    bad = encoded;
    const auto packed = load32(bad.data() + HeaderBytes + 4);
    put32(bad, HeaderBytes + 4, packed | 0x00010001u);
    reject(std::move(bad));
    bad = encoded; bad.pop_back(); reject(std::move(bad));
    bad = encoded; bad.push_back(0); reject(std::move(bad));

    Bytes output(RawBytesPerBlock);
    require(!decode(encoded.data(), encoded.size(), output.data(), output.size() + 256),
            "accepted wrong output size");
    require(!decode(encoded.data(), encoded.size(), encoded.data(), RawBytesPerBlock),
            "accepted overlapping input/output");
}

void testWorkerEncoding() {
    Bytes raw(1024u * 1024u);
    for (std::size_t block = 0; block < raw.size() / RawBytesPerBlock; ++block) {
        const unsigned k = static_cast<unsigned>(block % 17);
        const auto mask = static_cast<std::uint16_t>(k == 16 ? 0xffffu : ((1u << k) - 1u));
        const auto base = static_cast<std::uint16_t>(0xa55au & ~mask);
        for (std::size_t i = 0; i < WordsPerBlock; ++i) {
            const auto variable = static_cast<std::uint16_t>((block * 0x0765u + i * 0x1234u) & mask);
            put16(raw, block * RawBytesPerBlock + i * 2,
                  static_cast<std::uint16_t>(base | variable));
        }
    }
    Bytes serial;
    require(encodeFast(raw, serial, 1), "serial 1 MiB encode failed");
    Bytes portableSerial;
    require(detail::encodeImpl(raw.data(), raw.size(), portableSerial, false, 1),
            "portable serial 1 MiB encode failed");
    require(portableSerial == serial, "serial portable/BMI2 frame bytes differ");
    for (const auto workers : {2u, 4u, 8u}) {
        Bytes parallel;
        require(encodeFast(raw, parallel, workers), "parallel 1 MiB encode failed");
        require(parallel == serial, "parallel frame differs from serial bytes");
        Bytes portableParallel;
        require(detail::encodeImpl(raw.data(), raw.size(), portableParallel, false, workers),
                "parallel portable 1 MiB encode failed");
        require(portableParallel == portableSerial,
                "parallel portable frame differs from serial portable bytes");
        require(validate(parallel.data(), parallel.size(), raw.size()), "parallel frame invalid");
    }
    Bytes decoded(raw.size());
    require(decode(serial.data(), serial.size(), decoded.data(), raw.size()),
            "parallel regression frame decode failed");
    require(decoded == raw, "parallel regression decoded bytes differ");

    Bytes unchanged{0x12, 0x34};
    require(!encodeFast(raw, unchanged, 0) && unchanged == Bytes({0x12, 0x34}),
            "zero workers changed output or succeeded");
    require(!encodeFast(raw, unchanged, 33) && unchanged == Bytes({0x12, 0x34}),
            "excess workers changed output or succeeded");
}

Bytes readRawFixture(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open 32 MiB BP16 raw fixture");
    if (file.tellg() != static_cast<std::streamoff>(MaxRawBytes))
        throw std::runtime_error("BP16 benchmark fixture must be exactly 32 MiB");
    file.seekg(0);
    Bytes raw(MaxRawBytes);
    if (!file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size())))
        throw std::runtime_error("failed reading BP16 benchmark fixture");
    return raw;
}

void benchmarkWorkers(const std::string& rawPath, const std::string& outputPath) {
    const auto raw = readRawFixture(rawPath);
    Bytes serial;
    require(encodeFast(raw, serial, 1), "32 MiB serial encode failed");
    require(validate(serial.data(), serial.size(), raw.size()), "32 MiB serial frame invalid");
    Bytes decoded(raw.size());
    require(decode(serial.data(), serial.size(), decoded.data(), raw.size()) && decoded == raw,
            "32 MiB serial round trip failed");

    struct Result { unsigned workers; std::array<std::uint64_t, 3> ns; };
    std::array<Result, 4> results{{{1,{}},{2,{}},{4,{}},{8,{}}}};
    Bytes encoded;
    for (auto& result : results) {
        for (auto& elapsed : result.ns) {
            const auto start = std::chrono::steady_clock::now();
            require(encodeFast(raw, encoded, result.workers), "32 MiB worker encode failed");
            elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
            require(encoded == serial, "32 MiB parallel frame differs from serial bytes");
        }
        std::sort(result.ns.begin(), result.ns.end());
    }
    std::ofstream report(outputPath, std::ios::trunc);
    if (!report) throw std::runtime_error("failed opening BP16 worker benchmark output");
    report << "{\n  \"raw_bytes\": " << raw.size()
           << ",\n  \"encoded_bytes\": " << serial.size()
           << ",\n  \"bmi2_selected\": " << (detail::cpuHasBmi2() ? "true" : "false")
           << ",\n  \"round_trip_equal\": true,\n  \"runs\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        report << "    {\"workers\": " << result.workers << ", \"median_ns\": "
               << result.ns[1] << ", \"runs_ns\": [" << result.ns[0] << ", "
               << result.ns[1] << ", " << result.ns[2] << "]}"
               << (i + 1 == results.size() ? "\n" : ",\n");
        std::cout << "BP16 workers=" << result.workers << " median-ns=" << result.ns[1]
                  << " runs-ns=" << result.ns[0] << ',' << result.ns[1] << ','
                  << result.ns[2] << '\n';
    }
    report << "  ]\n}\n";
    if (!report) throw std::runtime_error("failed writing BP16 worker benchmark output");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 1 && argc != 3)
            throw std::runtime_error("usage: bp16_codec_check [32MiB-raw-fixture timing.json]");
        testPatterns();
        testMalformed();
        testWorkerEncoding();
        testImportedHostOwnerAccounting();
        std::cout << "PASS: BP16 codec and imported-host ownership, alignment, quota, and poison checks\n";
        if (argc == 3) benchmarkWorkers(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
