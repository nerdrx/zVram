// Bounded RX 7900 staging probe. Build manually with:
// g++ -O2 -std=c++17 validation/vulkan-staging-copy-probe.cpp -lvulkan -lzstd -pthread -o build/vulkan-staging-copy-probe
// Run with the raw F16 model file path; reads exactly one 128 MiB slice.
#include "../snapshot_decode.hpp"

#include <vulkan/vulkan.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr VkDeviceSize ChunkBytes = 32ull * 1024 * 1024;
constexpr VkDeviceSize TotalBytes = 4 * ChunkBytes;
constexpr std::uint64_t ModelOffset = 9898557440ull;
constexpr unsigned Repeats = 5;

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed: " + std::to_string(result));
}
struct Buffer {
    VkDevice device{};
    VkBuffer handle{};
    VkDeviceMemory memory{};
    void* mapped{};
    ~Buffer() { reset(); }
    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept
        : device(other.device), handle(other.handle), memory(other.memory), mapped(other.mapped) {
        other.handle = {}; other.memory = {}; other.mapped = nullptr;
    }
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            reset(); device = other.device; handle = other.handle; memory = other.memory; mapped = other.mapped;
            other.handle = {}; other.memory = {}; other.mapped = nullptr;
        }
        return *this;
    }
    void reset() {
        if (mapped) vkUnmapMemory(device, memory);
        if (handle) vkDestroyBuffer(device, handle, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        mapped = nullptr; handle = {}; memory = {};
    }
};
struct Sample { std::vector<double> decode, cpuCopy, cachedGpu, uncachedGpu, cachedE2e, uncachedE2e; };

} // namespace

int main(int argc, char** argv) try {
    if (argc != 2) throw std::runtime_error("usage: vulkan-staging-copy-probe RAW_F16_MODEL");

    std::vector<std::uint8_t> original(static_cast<std::size_t>(TotalBytes));
    {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) throw std::runtime_error("cannot open model file");
        file.seekg(static_cast<std::streamoff>(ModelOffset));
        file.read(reinterpret_cast<char*>(original.data()), static_cast<std::streamsize>(original.size()));
        if (file.gcount() != static_cast<std::streamsize>(original.size()))
            throw std::runtime_error("model file is shorter than requested 128 MiB slice");
    }
    std::array<std::vector<std::uint8_t>, 4> encoded;
    std::array<zvram::snapshot::EncodedChunk, 4> chunks{};
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        encoded[i].resize(ZSTD_compressBound(static_cast<std::size_t>(ChunkBytes)));
        const auto size = ZSTD_compress(encoded[i].data(), encoded[i].size(),
            original.data() + i * static_cast<std::size_t>(ChunkBytes), static_cast<std::size_t>(ChunkBytes), 1);
        if (ZSTD_isError(size)) throw std::runtime_error(ZSTD_getErrorName(size));
        encoded[i].resize(size);
        chunks[i] = {encoded[i].data(), encoded[i].size(), static_cast<std::size_t>(ChunkBytes), true};
    }

    VkInstance instance{};
    VkDevice device{};
    VkCommandPool commandPool{};
    auto cleanup = [&] {
        if (device) vkDeviceWaitIdle(device);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    };
    try {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
        check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
        VkPhysicalDevice physical{};
        std::uint32_t family = UINT32_MAX;
        VkPhysicalDeviceProperties props{};
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(candidate, &p);
            const std::string name = p.deviceName;
            if (p.vendorID != 0x1002 || p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
                (name.find("RX 7900") == std::string::npos && name.find("RX7900") == std::string::npos)) continue;
            std::uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
            std::vector<VkQueueFamilyProperties> families(n);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, families.data());
            for (unsigned pass = 0; pass < 2 && !physical; ++pass) {
                for (std::uint32_t i = 0; i < n; ++i) {
                    const auto flags = families[i].queueFlags;
                    if (!families[i].queueCount || !(flags & VK_QUEUE_TRANSFER_BIT)) continue;
                    if (!pass && (!(flags & VK_QUEUE_COMPUTE_BIT) || (flags & VK_QUEUE_GRAPHICS_BIT))) continue;
                    physical = candidate; family = i; props = p; break;
                }
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("unsupported: no RX 7900 Vulkan transfer queue");
        VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        VkQueue queue{}; vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = family; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &pci, nullptr, &commandPool), "vkCreateCommandPool");

        auto makeBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, int memoryClass) {
            Buffer b; b.device = device;
            VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = usage;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateBuffer(device, &ci, nullptr, &b.handle), "vkCreateBuffer");
            VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, b.handle, &req);
            std::uint32_t type = UINT32_MAX;
            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                if (!(req.memoryTypeBits & (1u << i))) continue;
                const auto flags = memory.memoryTypes[i].propertyFlags;
                const bool visible = flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
                const bool coherent = flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                const bool cached = flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
                const bool local = flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                const bool match = memoryClass == 0 ? (visible && coherent && cached && !local) :
                                   memoryClass == 1 ? (visible && coherent && !cached && !local) : local;
                if (match) { type = i; break; }
            }
            if (type == UINT32_MAX) {
                if (memoryClass < 2) throw std::runtime_error(memoryClass == 0 ?
                    "unsupported: no HOST_CACHED|HOST_COHERENT non-device-local memory" :
                    "unsupported: no uncached HOST_COHERENT non-device-local memory");
                throw std::runtime_error("unsupported: no compatible device-local memory");
            }
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size; ai.memoryTypeIndex = type;
            check(vkAllocateMemory(device, &ai, nullptr, &b.memory), "vkAllocateMemory");
            check(vkBindBufferMemory(device, b.handle, b.memory, 0), "vkBindBufferMemory");
            if (memoryClass < 2) check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
            std::cout << (memoryClass == 0 ? "cached" : memoryClass == 1 ? "uncached" : "device-local")
                      << " type=" << type << " flags=0x" << std::hex << memory.memoryTypes[type].propertyFlags << std::dec << '\n';
            return b;
        };
        const auto usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Buffer cached = makeBuffer(TotalBytes, usage, 0);
        Buffer uncached = makeBuffer(TotalBytes, usage, 1);
        Buffer target = makeBuffer(TotalBytes, usage, 2);

        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = commandPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &cai, &command), "allocate command buffer");
        auto copy = [&](VkBuffer src, VkBuffer dst) {
            check(vkResetCommandBuffer(command, 0), "reset command buffer");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(command, &bi), "begin command buffer");
            VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
            VkBufferCopy region{0, 0, TotalBytes}; vkCmdCopyBuffer(command, src, dst, 1, &region);
            VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 1, &after, 0, nullptr, 0, nullptr);
            check(vkEndCommandBuffer(command), "end command buffer");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &command;
            check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "queue submit");
            check(vkQueueWaitIdle(queue), "queue wait idle");
        };
        auto elapsedMs = [](auto start, auto end) {
            return std::chrono::duration<double, std::milli>(end - start).count();
        };
        auto verify = [&](const char* mode) {
            if (std::memcmp(cached.mapped, original.data(), original.size()) != 0)
                throw std::runtime_error(std::string(mode) + " GPU readback mismatch");
        };
        Sample times;
        auto run = [&](bool useUncached) {
            auto start = Clock::now();
            if (!zvram::snapshot::decodeBatch(chunks.data(), chunks.size(),
                    static_cast<std::uint8_t*>(cached.mapped), static_cast<std::size_t>(TotalBytes),
                    static_cast<std::size_t>(ChunkBytes)))
                throw std::runtime_error("frame decode failed");
            const auto decoded = Clock::now();
            if (useUncached) std::memcpy(uncached.mapped, cached.mapped, static_cast<std::size_t>(TotalBytes));
            const auto memcpyDone = Clock::now();
            copy(useUncached ? uncached.handle : cached.handle, target.handle);
            const auto done = Clock::now();
            copy(target.handle, cached.handle); verify(useUncached ? "uncached" : "cached");
            times.decode.push_back(elapsedMs(start, decoded));
            if (useUncached) {
                times.cpuCopy.push_back(elapsedMs(decoded, memcpyDone));
                times.uncachedGpu.push_back(elapsedMs(memcpyDone, done));
                times.uncachedE2e.push_back(elapsedMs(start, done));
            } else {
                times.cachedGpu.push_back(elapsedMs(memcpyDone, done));
                times.cachedE2e.push_back(elapsedMs(start, done));
            }
        };
        run(false); run(true); // warm-up, excluded
        times = {};
        for (unsigned i = 0; i < Repeats; ++i) { run(i % 2); run(!(i % 2)); }
        auto median = [](std::vector<double> values) {
            std::sort(values.begin(), values.end()); return values[values.size() / 2];
        };
        std::cout << "GPU: " << props.deviceName << " queue-family=" << family << " model-slice-offset=" << ModelOffset
                  << " bytes=" << TotalBytes << " repeats=" << Repeats << '\n'
                  << "median_ms decode=" << median(times.decode)
                  << " cpu_memcpy_cached_to_uncached=" << median(times.cpuCopy)
                  << " gpu_copy_cached=" << median(times.cachedGpu)
                  << " gpu_copy_uncached=" << median(times.uncachedGpu)
                  << " end_to_end_cached=" << median(times.cachedE2e)
                  << " end_to_end_uncached=" << median(times.uncachedE2e) << '\n';
        vkFreeCommandBuffers(device, commandPool, 1, &command);
    } catch (...) { cleanup(); throw; }
    cleanup();
    return 0;
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
}
