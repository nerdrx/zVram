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

namespace zvram::gdeflate::gpu {
struct ImportedHostInputTestAccess {
    using Decoder = zvram::gdeflate::gpu::Decoder;
    using Owner = Decoder::ImportedHostInput;
    using Budget = Decoder::AllocatedHostBudget;

    static std::shared_ptr<Owner> makeBP16Frame(const std::uint8_t* frame, std::size_t size) {
        if (!frame || !size) return {};
        auto* copy = static_cast<std::uint8_t*>(std::malloc(size));
        if (!copy) return {};
        std::memcpy(copy, frame, size);
        auto owner = std::shared_ptr<Owner>(new Owner());
        owner->allocation_ = copy;
        owner->encodedBytes_ = size;
        if (!zvram::bp16::inspect(copy, size, &owner->bp16Info_)) return {};
        return owner;
    }

    static std::shared_ptr<Owner> makeEmpty() {
        return std::shared_ptr<Owner>(new Owner());
    }

    static bool matchesBP16Frame(const Owner& owner, const std::uint8_t* input,
                                 std::size_t encodedSize, std::size_t rawSize) {
        return owner.matchesBP16Frame(input, encodedSize, rawSize);
    }

    static std::shared_ptr<Owner> make(VkDevice device, VkBuffer buffer,
            VkDeviceMemory memory, void* allocation, bool driverAllocated,
            PFN_vkDestroyBuffer destroyBuffer, PFN_vkFreeMemory freeMemory,
            PFN_vkUnmapMemory unmapMemory = nullptr,
            std::shared_ptr<Budget> budget = {}, std::uint64_t reservedBytes = 0,
            std::shared_ptr<std::atomic<bool>> poisoned = {}) {
        auto owner = std::shared_ptr<Owner>(new Owner());
        owner->device_ = device;
        owner->buffer_ = buffer;
        owner->memory_ = memory;
        owner->allocation_ = allocation;
        owner->destroyBuffer_ = destroyBuffer;
        owner->freeMemory_ = freeMemory;
        owner->unmapMemory_ = unmapMemory;
        owner->driverAllocatedHostMemory_ = driverAllocated;
        owner->budget_ = std::move(budget);
        owner->reservedBudgetBytes_ = reservedBytes;
        owner->poisoned_ = std::move(poisoned);
        return owner;
    }
};
} // namespace zvram::gdeflate::gpu

namespace {

using Bytes = std::vector<std::uint8_t>;
using namespace zvram::bp16;
using ImportedInput = zvram::gdeflate::gpu::Decoder::ImportedHostInput;

void require(bool condition, const char* message);

unsigned destroyedImportBuffers{};
unsigned freedImportMemory{};
unsigned unmappedAllocatedMemory{};

void VKAPI_CALL fakeDestroyImportBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {
    ++destroyedImportBuffers;
}
void VKAPI_CALL fakeFreeImportMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {
    ++freedImportMemory;
}
void VKAPI_CALL fakeUnmapAllocatedMemory(VkDevice, VkDeviceMemory) {
    ++unmappedAllocatedMemory;
}

template<class T> T fakeHandle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}

void testImportedHostOwnerAccounting() {
    using Decoder = zvram::gdeflate::gpu::Decoder;
    using Budget = Decoder::AllocatedHostBudget;
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryTypeCount = 3;
    memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory.memoryTypes[1].propertyFlags = memory.memoryTypes[0].propertyFlags |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    memory.memoryTypes[2].propertyFlags = memory.memoryTypes[1].propertyFlags |
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const auto hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    require(Decoder::selectMemoryTypeIndex(memory, 0x7, hostFlags,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) == 1,
            "cached upload preference did not select cached non-device-local memory");
    require(Decoder::selectMemoryTypeIndex(memory, 0x1, hostFlags,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) == 0,
            "cached upload preference did not fall back to compatible coherent memory");
    require(Decoder::selectMemoryTypeIndex(memory, 0x7, hostFlags,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0,
            "default memory selection changed without a preference");
    require(Decoder::selectMemoryTypeIndex(memory, 0x4, hostFlags,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) == UINT32_MAX,
            "memory selector accepted a forbidden device-local type");

    std::uint64_t budgetBytes{};
    require(Decoder::parseAllocatedHostBudgetMiB("0", budgetBytes) && budgetBytes == 0,
            "zero allocated-host budget parse failed");
    require(Decoder::parseAllocatedHostBudgetMiB("8192", budgetBytes) &&
            budgetBytes == Decoder::DefaultAllocatedHostBudgetBytes,
            "default allocated-host budget parse failed");
    require(!Decoder::parseAllocatedHostBudgetMiB("", budgetBytes) &&
            !Decoder::parseAllocatedHostBudgetMiB("-1", budgetBytes) &&
            !Decoder::parseAllocatedHostBudgetMiB("1.5", budgetBytes) &&
            !Decoder::parseAllocatedHostBudgetMiB("17592186044416", budgetBytes),
            "invalid or overflowing allocated-host budget accepted");
    Budget disabledBudget(0);
    require(disabledBudget.limitBytes() == 0 && !disabledBudget.reserve(1),
            "zero allocated-host budget did not disable owner caching");
    Budget exactBudget(16);
    require(exactBudget.reserve(10) && exactBudget.reserve(6) && exactBudget.usedBytes() == 16,
            "allocated-host budget exact fit failed");
    require(!exactBudget.reserve(1) && exactBudget.release(6) && exactBudget.usedBytes() == 10,
            "allocated-host budget overrun/release failed");
    require(exactBudget.release(10) && exactBudget.usedBytes() == 0 && !exactBudget.release(1),
            "allocated-host budget release accounting failed");
    Budget overflowBudget(UINT64_MAX);
    require(overflowBudget.reserve(UINT64_MAX) && !overflowBudget.reserve(1) &&
            overflowBudget.usedBytes() == UINT64_MAX,
            "allocated-host budget overflow accepted");

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

    destroyedImportBuffers = freedImportMemory = unmappedAllocatedMemory = 0;
    void* host{};
    require(posix_memalign(&host, 4096, 4096) == 0, "aligned host owner allocation failed");
    auto owner = zvram::gdeflate::gpu::ImportedHostInputTestAccess::make(
        fakeHandle<VkDevice>(1), fakeHandle<VkBuffer>(2), fakeHandle<VkDeviceMemory>(3),
        host, false, fakeDestroyImportBuffer, fakeFreeImportMemory);
    auto moved = std::move(owner);
    require(!owner && moved->data() == host && reinterpret_cast<std::uintptr_t>(host) % 4096 == 0,
            "owner move/alignment failed");
    moved.reset();
    require(destroyedImportBuffers == 1 && freedImportMemory == 1,
            "owned Vulkan import handles were not released exactly once");

    auto allocatedBudget = std::make_shared<Budget>(4096);
    require(allocatedBudget->reserve(4096), "allocated-host owner budget reserve failed");
    auto allocated = zvram::gdeflate::gpu::ImportedHostInputTestAccess::make(
        fakeHandle<VkDevice>(7), fakeHandle<VkBuffer>(8), fakeHandle<VkDeviceMemory>(9),
        reinterpret_cast<void*>(10), true, fakeDestroyImportBuffer, fakeFreeImportMemory,
        fakeUnmapAllocatedMemory, allocatedBudget, 4096);
    allocated.reset();
    require(unmappedAllocatedMemory == 1 && destroyedImportBuffers == 2 && freedImportMemory == 2,
            "allocated cached host memory was not unmapped and released exactly once");
    require(allocatedBudget->usedBytes() == 0, "normal allocated-host cleanup did not release budget");

    void* poisonedHost{};
    require(posix_memalign(&poisonedHost, 4096, 4096) == 0, "poison owner allocation failed");
    auto poison = std::make_shared<std::atomic<bool>>(true);
    auto poisonedBudget = std::make_shared<Budget>(4096);
    require(poisonedBudget->reserve(4096), "poisoned allocated-host budget reserve failed");
    auto retained = zvram::gdeflate::gpu::ImportedHostInputTestAccess::make(
        fakeHandle<VkDevice>(4), fakeHandle<VkBuffer>(5), fakeHandle<VkDeviceMemory>(6),
        poisonedHost, true, fakeDestroyImportBuffer, fakeFreeImportMemory,
        fakeUnmapAllocatedMemory, poisonedBudget, 4096, poison);
    retained.reset();
    require(destroyedImportBuffers == 2 && freedImportMemory == 2 && unmappedAllocatedMemory == 1,
            "poisoned import owner freed potentially in-flight handles");
    require(poisonedBudget->usedBytes() == 4096,
            "poisoned allocated-host owner released its budget reservation");
    std::free(poisonedHost); // The poisoned owner intentionally leaked this in production.

    Bytes raw(RawBytesPerBlock, 0), encoded;
    require(encodeFast(raw.data(), raw.size(), encoded), "tiny BP16 owner frame encode failed");
    auto frame = zvram::gdeflate::gpu::ImportedHostInputTestAccess::makeBP16Frame(
        encoded.data(), encoded.size());
    require(frame && zvram::gdeflate::gpu::ImportedHostInputTestAccess::matchesBP16Frame(
                *frame, frame->data(), encoded.size(), raw.size()),
            "cached BP16 owner frame rejected its exact tuple");
    Bytes otherPointer = encoded;
    require(!zvram::gdeflate::gpu::ImportedHostInputTestAccess::matchesBP16Frame(
                *frame, frame->data(), encoded.size(), raw.size() + RawBytesPerBlock),
            "cached BP16 owner accepted a wrong aligned raw size");
    require(!zvram::gdeflate::gpu::ImportedHostInputTestAccess::matchesBP16Frame(
                *frame, frame->data(), encoded.size() - 1, raw.size()),
            "cached BP16 owner accepted a wrong encoded size");
    require(!zvram::gdeflate::gpu::ImportedHostInputTestAccess::matchesBP16Frame(
                *frame, otherPointer.data(), encoded.size(), raw.size()),
            "cached BP16 owner accepted a different input pointer");
    auto empty = zvram::gdeflate::gpu::ImportedHostInputTestAccess::makeEmpty();
    require(!zvram::gdeflate::gpu::ImportedHostInputTestAccess::matchesBP16Frame(
                *empty, encoded.data(), encoded.size(), raw.size()),
            "empty BP16 owner matched a valid frame tuple");
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

void testBP16UploadCopy() {
    using Decoder = zvram::gdeflate::gpu::Decoder;
    unsigned workers = 99;
    require(Decoder::parseBP16UploadWorkers("1", workers) && workers == 1,
            "one upload worker parse failed");
    require(Decoder::parseBP16UploadWorkers("8", workers) && workers == 8,
            "eight upload workers parse failed");
    for (const char* invalid : {"", "0", "9", "workers", "4294967296"}) {
        workers = 3;
        require(!Decoder::parseBP16UploadWorkers(invalid, workers) && workers == 3,
                "invalid upload worker count accepted or changed output");
    }

    constexpr std::uint8_t SourceCanary = 0xa7;
    constexpr std::uint8_t DestinationCanary = 0x5c;
    for (const auto bytes : {std::size_t(1024u * 1024u - 3u),
                             std::size_t(1024u * 1024u + 13u),
                             std::size_t(3u * 1024u * 1024u + 61u)}) {
        Bytes source(bytes + 17u, SourceCanary);
        Bytes expected(bytes);
        for (std::size_t i = 0; i < bytes; ++i) {
            source[7u + i] = static_cast<std::uint8_t>((i * 131u + i / 251u) & 0xffu);
            expected[i] = source[7u + i];
        }
        for (unsigned count = 1; count <= 8; ++count) {
            Bytes destination(bytes + 29u, DestinationCanary);
            Decoder::copyBP16UploadBytes(destination.data() + 11u, source.data() + 7u,
                                         bytes, count);
            require(std::equal(expected.begin(), expected.end(), destination.begin() + 11u),
                    "BP16 upload copy differs from source");
            require(std::all_of(destination.begin(), destination.begin() + 11u,
                                [](std::uint8_t value) { return value == DestinationCanary; }) &&
                    std::all_of(destination.begin() + 11u + bytes, destination.end(),
                                [](std::uint8_t value) { return value == DestinationCanary; }),
                    "BP16 upload copy overwrote destination canary");
            require(std::all_of(source.begin(), source.begin() + 7u,
                                [](std::uint8_t value) { return value == SourceCanary; }) &&
                    std::all_of(source.begin() + 7u + bytes, source.end(),
                                [](std::uint8_t value) { return value == SourceCanary; }),
                    "BP16 upload copy overwrote source canary");
        }
    }
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
    std::array<Result, 6> results{{{1,{}},{2,{}},{4,{}},{8,{}},{16,{}},{32,{}}}};
    Bytes encoded;
    encoded.reserve(serial.size());
    for (auto& result : results) {
        for (auto& elapsed : result.ns) {
            const auto start = std::chrono::steady_clock::now();
            require(encodeFast(raw, encoded, result.workers), "32 MiB worker encode failed");
            elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
            require(encoded == serial, "32 MiB parallel frame differs from serial bytes");
            require(decode(encoded.data(), encoded.size(), decoded.data(), raw.size()) && decoded == raw,
                    "32 MiB worker frame round trip failed");
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
        testBP16UploadCopy();
        testImportedHostOwnerAccounting();
        std::cout << "PASS: BP16 codec, upload worker partitions, and imported/allocated-host ownership, alignment, quota, and poison checks\n";
        if (argc == 3) benchmarkWorkers(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
