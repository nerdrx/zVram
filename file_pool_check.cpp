#include "managed_pool.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::uint64_t MiB = 1024ull * 1024ull;
constexpr std::size_t FileChunk = 8u * 1024u * 1024u;
constexpr VkDeviceSize ResidentBudget = 16u * MiB;
constexpr std::size_t HostBudget = 512u * MiB;
constexpr VkDeviceSize StagingBudget = 1u * MiB;

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Context {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t family{};

    ~Context() {
        if (device) vkDeviceWaitIdle(device);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    void initialize(bool sparse) {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");

        std::uint32_t physicalCount = 0;
        check(vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr), "vkEnumeratePhysicalDevices(count)");
        require(physicalCount != 0, "no Vulkan physical device found");
        std::vector<VkPhysicalDevice> physicals(physicalCount);
        check(vkEnumeratePhysicalDevices(instance, &physicalCount, physicals.data()), "vkEnumeratePhysicalDevices");

        for (VkPhysicalDevice candidate : physicals) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.vendorID != 0x1002 || properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                continue;
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            if (sparse && (!features.sparseBinding || !features.sparseResidencyBuffer)) continue;

            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, queues.data());
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                const VkQueueFlags sparseFlag = sparse
                    ? static_cast<VkQueueFlags>(VK_QUEUE_SPARSE_BINDING_BIT) : VkQueueFlags{0};
                const VkQueueFlags needed = VK_QUEUE_TRANSFER_BIT | sparseFlag;
                if (queues[i].queueCount && (queues[i].queueFlags & needed) == needed) {
                    physical = candidate;
                    family = i;
                    break;
                }
            }
            if (physical) break;
        }
        require(physical, sparse
            ? "no AMD discrete GPU supports sparse buffer residency and sparse transfer queue"
            : "no AMD discrete GPU with a transfer queue found");

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::cout << "GPU: " << properties.deviceName << "\nmode: "
                  << (sparse ? "stable sparse buffers" : "default managed buffers") << '\n';

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabled{};
        if (sparse) {
            enabled.sparseBinding = VK_TRUE;
            enabled.sparseResidencyBuffer = VK_TRUE;
        }
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = sparse ? &enabled : nullptr;
        check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
    }
};

struct Options {
    std::filesystem::path file;
    bool sparse{};
};

Options parse(int argc, char** argv) {
    Options options;
    bool haveFile = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--sparse") {
            options.sparse = true;
        } else if (arg == "--file" && i + 1 < argc && !haveFile) {
            options.file = argv[++i];
            haveFile = true;
        } else {
            throw std::runtime_error("usage: file-pool-check --file PATH [--sparse]");
        }
    }
    require(haveFile && !options.file.empty(), "usage: file-pool-check --file PATH [--sparse]");
    return options;
}

void checkBudgets(const zvram::ManagedBufferPool::Statistics& stats) {
    require(stats.residentAllocationBytes <= ResidentBudget, "resident allocation budget exceeded");
    require(stats.hostStoredBytes <= HostBudget, "compressed host-store budget exceeded");
}
} // namespace

int main(int argc, char** argv) try {
    const Options options = parse(argc, argv);
    std::error_code fileError;
    const std::uintmax_t fileBytes = std::filesystem::file_size(options.file, fileError);
    if (fileError) throw std::runtime_error("cannot stat input file: " + fileError.message());
    require(fileBytes != 0, "input file must be nonempty");
    require(fileBytes <= HostBudget, "input file exceeds the 512 MiB bounded-test limit");
    require(fileBytes <= std::numeric_limits<std::size_t>::max(), "input file exceeds host address space");
    const std::size_t totalBytes = static_cast<std::size_t>(fileBytes);
    std::ifstream source(options.file, std::ios::binary);
    require(bool(source), "cannot open input file");

    Context context;
    context.initialize(options.sparse);
    using Pool = zvram::ManagedBufferPool;
    Pool::Config config{context.physical, context.device, context.queue, context.family,
                       ResidentBudget, HostBudget, StagingBudget, options.sparse};
    Pool pool(config);

    std::vector<Pool::Id> ids;
    std::vector<VkBuffer> originalHandles;
    ids.reserve((totalBytes + FileChunk - 1) / FileChunk);
    if (options.sparse) originalHandles.reserve(ids.capacity());
    std::vector<std::uint8_t> input(FileChunk);
    std::size_t offset = 0;
    std::size_t hostPeak = 0;
    while (offset < totalBytes) {
        const std::size_t bytes = std::min(FileChunk, totalBytes - offset);
        source.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(bytes));
        require(source.gcount() == static_cast<std::streamsize>(bytes), "input file changed or ended during streaming read");
        const Pool::Id id = pool.upload(input.data(), bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        ids.push_back(id);
        if (options.sparse) {
            const auto view = pool.acquire(id);
            originalHandles.push_back(view.buffer);
            pool.release(id);
        }
        auto stats = pool.statistics();
        checkBudgets(stats);
        hostPeak = std::max(hostPeak, stats.hostStoredBytes);
        offset += bytes;
        std::cout << "loaded " << offset << " / " << totalBytes << " bytes\r" << std::flush;
    }
    std::cout << '\n';

    for (Pool::Id id : ids) {
        require(pool.evict(id), "failed to evict an unpinned input chunk");
        const auto stats = pool.statistics();
        checkBudgets(stats);
        hostPeak = std::max(hostPeak, stats.hostStoredBytes);
    }
    const auto coldStats = pool.statistics();
    checkBudgets(coldStats);
    require(coldStats.residentAllocationBytes == 0, "not all chunks became cold");
    require(coldStats.hostStoredBytes != 0, "cold chunks have no retained host snapshots");

    source.clear();
    std::vector<std::uint8_t> expected(FileChunk);
    std::uint64_t verified = 0;
    for (std::size_t i = 0, fileOffset = 0; i < ids.size(); ++i) {
        const auto view = pool.acquire(ids[i]);
        if (options.sparse)
            require(view.buffer == originalHandles[i], "sparse VkBuffer handle changed across restore");
        pool.release(ids[i]);

        const auto actual = pool.readback(ids[i]);
        const std::size_t bytes = actual.size();
        require(bytes != 0 && bytes <= expected.size(), "invalid restored chunk size");
        source.clear();
        source.seekg(static_cast<std::streamoff>(fileOffset), std::ios::beg);
        require(bool(source), "cannot seek input file during verification");
        source.read(reinterpret_cast<char*>(expected.data()), static_cast<std::streamsize>(bytes));
        require(source.gcount() == static_cast<std::streamsize>(bytes), "cannot reread expected input bytes");
        require(std::equal(actual.begin(), actual.end(), expected.begin()), "restored data differs from input file");
        verified += bytes;
        fileOffset += bytes;
        require(pool.evict(ids[i]), "failed to return verified chunk to cold storage");
        const auto stats = pool.statistics();
        checkBudgets(stats);
        hostPeak = std::max(hostPeak, stats.hostStoredBytes);
        if (verified == fileBytes || verified % (64 * MiB) < bytes)
            std::cout << "verified " << verified << " / " << fileBytes << " bytes\n";
    }
    require(verified == fileBytes, "verified byte count differs from input size");

    auto finalStats = pool.statistics();
    checkBudgets(finalStats);
    require(finalStats.residentAllocationBytes == 0, "resident storage remains after final eviction");
    std::cout << "cold-store input_bytes=" << fileBytes
              << " host_stored_bytes=" << coldStats.hostStoredBytes
              << " host_stored_peak_bytes=" << hostPeak
              << " resident_cold_bytes=" << coldStats.residentAllocationBytes
              << " evictions=" << finalStats.evictions
              << " restores=" << finalStats.restores
              << " raw_fallbacks=" << finalStats.rawFallbacks
              << " verified_bytes=" << verified
              << " stable_handles=" << (options.sparse ? "yes" : "no") << '\n';

    for (Pool::Id id : ids) require(pool.erase(id), "failed to erase cold buffer entry");
    finalStats = pool.statistics();
    require(finalStats.residentAllocationBytes == 0 && finalStats.hostStoredBytes == 0,
            "pool cleanup left resident or host-stored data");
    std::cout << "PASS: every input byte restored and compared; pool storage cleaned up\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
