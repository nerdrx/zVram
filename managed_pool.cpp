#include "managed_pool.hpp"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace zvram {
namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

struct Allocation {
    VkDevice device{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize logicalSize{};
    VkDeviceSize allocationSize{};

    Allocation() = default;
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;
    Allocation(Allocation&& other) noexcept { *this = std::move(other); }
    Allocation& operator=(Allocation&& other) noexcept {
        if (this != &other) {
            reset();
            device = other.device; buffer = other.buffer; memory = other.memory;
            logicalSize = other.logicalSize; allocationSize = other.allocationSize;
            other.buffer = VK_NULL_HANDLE; other.memory = VK_NULL_HANDLE;
            other.logicalSize = other.allocationSize = 0;
        }
        return *this;
    }
    ~Allocation() { reset(); }
    void reset() noexcept {
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        buffer = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
        logicalSize = allocationSize = 0;
    }
};

struct Stored {
    std::vector<std::uint8_t> bytes;
    bool compressed{};
};

struct Snapshot {
    std::vector<Stored> chunks;
    std::size_t storedBytes{};
};

Stored encode(const std::uint8_t* source, std::size_t size, bool& rawFallback) {
    Stored result;
    const std::size_t bound = ZSTD_compressBound(size);
    if (ZSTD_isError(bound)) throw std::runtime_error(ZSTD_getErrorName(bound));
    std::vector<std::uint8_t> compressed(bound);
    const std::size_t written = ZSTD_compress(compressed.data(), compressed.size(), source, size, 3);
    if (ZSTD_isError(written)) throw std::runtime_error(ZSTD_getErrorName(written));
    if (written >= size) {
        result.bytes.assign(source, source + size);
        rawFallback = true;
    } else {
        result.bytes.assign(compressed.begin(), compressed.begin() + written);
        result.compressed = true;
    }
    return result;
}

} // namespace

struct ManagedBufferPool::Impl {
    struct Entry {
        VkDeviceSize size{};
        VkBufferUsageFlags usage{};
        Allocation gpu;
        Snapshot host;
        std::uint32_t pins{};
        std::uint64_t lastUse{};
        bool resident{};
    };

    Config config;
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool commandPool{};
    Allocation staging;
    void* mapped{};
    std::unordered_map<Id, Entry> entries;
    Id nextId{1};
    std::uint64_t clock{};
    Statistics stats;

    explicit Impl(const Config& c) : config(c) {
        if (!c.physicalDevice || !c.device || !c.queue || c.residentBudget == 0 || c.hostBudget == 0 || c.stagingChunkSize == 0)
            throw std::invalid_argument("ManagedBufferPool requires valid Vulkan handles and nonzero budgets");
        vkGetPhysicalDeviceMemoryProperties(c.physicalDevice, &memory);
        config.stagingChunkSize = std::min<VkDeviceSize>(c.stagingChunkSize,
            static_cast<VkDeviceSize>(std::numeric_limits<std::size_t>::max()));
        config.stagingChunkSize &= ~VkDeviceSize{3};
        if (config.stagingChunkSize < 4) throw std::invalid_argument("staging chunk must be at least four bytes");
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = c.queueFamily;
        check(vkCreateCommandPool(c.device, &pci, nullptr, &commandPool), "vkCreateCommandPool");
        try {
            staging = makeBuffer(config.stagingChunkSize,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                 false);
            check(vkMapMemory(c.device, staging.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(staging)");
        } catch (...) {
            vkDestroyCommandPool(c.device, commandPool, nullptr);
            commandPool = VK_NULL_HANDLE;
            throw;
        }
    }

    ~Impl() {
        if (!config.device) return;
        vkQueueWaitIdle(config.queue);
        if (mapped) vkUnmapMemory(config.device, staging.memory);
        entries.clear();
        staging.reset();
        if (commandPool) vkDestroyCommandPool(config.device, commandPool, nullptr);
    }

    Allocation makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags properties, bool deviceLocal) {
        if (!size) throw std::invalid_argument("zero-size Vulkan buffer");
        Allocation result;
        result.device = config.device;
        result.logicalSize = size;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(config.device, &bci, nullptr, &result.buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(config.device, result.buffer, &requirements);
        if (deviceLocal) makeRoom(requirements.size);
        std::uint32_t selected = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            const auto flags = memory.memoryTypes[i].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << i)) && (flags & properties) == properties &&
                (deviceLocal ? (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0
                             : (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 ||
                               (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0)) {
                selected = i;
                if (deviceLocal || (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) break;
            }
        }
        if (selected == UINT32_MAX) throw std::runtime_error("no compatible Vulkan memory type");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = requirements.size;
        ai.memoryTypeIndex = selected;
        check(vkAllocateMemory(config.device, &ai, nullptr, &result.memory), "vkAllocateMemory");
        check(vkBindBufferMemory(config.device, result.buffer, result.memory, 0), "vkBindBufferMemory");
        result.allocationSize = requirements.size;
        return result;
    }

    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize srcOffset,
                    VkDeviceSize dstOffset, VkDeviceSize bytes) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = commandPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        check(vkAllocateCommandBuffers(config.device, &ai, &cmd), "vkAllocateCommandBuffers");
        try {
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            VkBufferCopy region{srcOffset, dstOffset, bytes};
            vkCmdCopyBuffer(cmd, src, dst, 1, &region);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
            check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            check(vkQueueSubmit(config.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
            check(vkQueueWaitIdle(config.queue), "vkQueueWaitIdle");
        } catch (...) {
            vkFreeCommandBuffers(config.device, commandPool, 1, &cmd);
            throw;
        }
        vkFreeCommandBuffers(config.device, commandPool, 1, &cmd);
    }

    void uploadTo(Allocation& target, const std::uint8_t* data, VkDeviceSize size,
                  VkDeviceSize targetOffset = 0) {
        VkDeviceSize offset = 0;
        while (offset < size) {
            const VkDeviceSize chunk = std::min(config.stagingChunkSize, size - offset);
            const VkDeviceSize copied = (chunk + 3) & ~VkDeviceSize{3};
            std::memcpy(mapped, data + static_cast<std::size_t>(offset), static_cast<std::size_t>(chunk));
            if (copied > chunk) std::memset(static_cast<std::uint8_t*>(mapped) + chunk, 0, static_cast<std::size_t>(copied - chunk));
            copyBuffer(staging.buffer, target.buffer, 0, targetOffset + offset, copied);
            offset += chunk;
        }
    }

    std::vector<std::uint8_t> readGpu(const Entry& entry) {
        if (entry.size > std::numeric_limits<std::size_t>::max())
            throw std::length_error("buffer is too large for host address space");
        std::vector<std::uint8_t> result(static_cast<std::size_t>(entry.size));
        VkDeviceSize offset = 0;
        while (offset < entry.size) {
            const VkDeviceSize chunk = std::min(config.stagingChunkSize, entry.size - offset);
            const VkDeviceSize copied = (chunk + 3) & ~VkDeviceSize{3};
            copyBuffer(entry.gpu.buffer, staging.buffer, offset, 0, copied);
            std::memcpy(result.data() + static_cast<std::size_t>(offset), mapped, static_cast<std::size_t>(chunk));
            offset += chunk;
        }
        return result;
    }

    bool hostFits(std::size_t oldSize, std::size_t newSize) const {
        if (stats.hostStoredBytes < oldSize) throw std::logic_error("host store accounting underflow");
        const std::size_t withoutOld = stats.hostStoredBytes - oldSize;
        return newSize <= config.hostBudget && withoutOld <= config.hostBudget - newSize;
    }

    void installHost(Entry& entry, Snapshot&& stored) {
        if (!hostFits(entry.host.storedBytes, stored.storedBytes))
            throw std::runtime_error("compressed host store budget exceeded");
        stats.hostStoredBytes -= entry.host.storedBytes;
        stats.hostStoredBytes += stored.storedBytes;
        entry.host = std::move(stored);
    }

    auto find(Id id) { return entries.find(id); }
    auto find(Id id) const { return entries.find(id); }

    void evictEntry(Entry& entry) {
        if (entry.pins) throw std::logic_error("cannot evict a pinned buffer");
        bool raw = false;
        Snapshot updated;
        // Compression scratch is bounded by the staging chunk, regardless of
        // buffer size. Keep the GPU copy until every chunk is safely retained.
        for (VkDeviceSize offset = 0; offset < entry.size;) {
            const VkDeviceSize chunk = std::min(config.stagingChunkSize, entry.size - offset);
            copyBuffer(entry.gpu.buffer, staging.buffer, offset, 0, (chunk + 3) & ~VkDeviceSize{3});
            Stored stored = encode(static_cast<const std::uint8_t*>(mapped), static_cast<std::size_t>(chunk), raw);
            if (stored.bytes.capacity() > std::numeric_limits<std::size_t>::max() - updated.storedBytes)
                throw std::overflow_error("host store size overflow");
            updated.storedBytes += stored.bytes.capacity();
            if (!hostFits(entry.host.storedBytes, updated.storedBytes))
                throw std::runtime_error("host store budget cannot retain buffer readback");
            updated.chunks.push_back(std::move(stored));
            offset += chunk;
        }
        installHost(entry, std::move(updated));
        // Every readback chunk already waited for the supplied queue.
        stats.residentAllocationBytes -= entry.gpu.allocationSize;
        entry.gpu.reset();
        entry.resident = false;
        ++stats.evictions;
        if (raw) ++stats.rawFallbacks;
    }

    void makeRoom(VkDeviceSize bytes) {
        if (bytes > config.residentBudget) throw std::runtime_error("buffer exceeds resident budget");
        while (stats.residentAllocationBytes > config.residentBudget - bytes) {
            auto candidate = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (!it->second.resident || it->second.pins) continue;
                if (candidate == entries.end() || it->second.lastUse < candidate->second.lastUse) candidate = it;
            }
            if (candidate == entries.end()) throw std::runtime_error("resident budget is pinned or unavailable");
            evictEntry(candidate->second);
        }
    }

    void restore(Entry& entry) {
        if (entry.resident) return;
        if (entry.host.chunks.empty()) throw std::runtime_error("buffer has no retained host snapshot");
        const VkDeviceSize gpuSize = (entry.size + 3) & ~VkDeviceSize{3};
        Allocation gpu = makeBuffer(gpuSize, entry.usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        if (stats.residentAllocationBytes > config.residentBudget - gpu.allocationSize)
            throw std::runtime_error("Vulkan allocation exceeds remaining resident budget");
        VkDeviceSize offset = 0;
        for (const auto& stored : entry.host.chunks) {
            if (offset >= entry.size) throw std::runtime_error("host snapshot chunk count mismatch");
            const std::size_t size = static_cast<std::size_t>(std::min(config.stagingChunkSize, entry.size - offset));
            if (stored.compressed) {
                std::vector<std::uint8_t> decoded(size);
                const std::size_t n = ZSTD_decompress(decoded.data(), decoded.size(), stored.bytes.data(), stored.bytes.size());
                if (ZSTD_isError(n) || n != size) throw std::runtime_error("zstd restore failed");
                uploadTo(gpu, decoded.data(), size, offset);
            } else {
                if (stored.bytes.size() != size) throw std::runtime_error("raw restore size mismatch");
                uploadTo(gpu, stored.bytes.data(), size, offset);
            }
            offset += size;
        }
        if (offset != entry.size) throw std::runtime_error("host snapshot size mismatch");
        entry.gpu = std::move(gpu);
        entry.resident = true;
        stats.residentAllocationBytes += entry.gpu.allocationSize;
        stats.hostStoredBytes -= entry.host.storedBytes;
        entry.host = Snapshot{};
        entry.lastUse = ++clock;
        ++stats.restores;
    }
};

ManagedBufferPool::ManagedBufferPool(const Config& config) : impl_(std::make_unique<Impl>(config)) {}
ManagedBufferPool::~ManagedBufferPool() = default;
ManagedBufferPool::ManagedBufferPool(ManagedBufferPool&&) noexcept = default;
ManagedBufferPool& ManagedBufferPool::operator=(ManagedBufferPool&&) noexcept = default;

ManagedBufferPool::Id ManagedBufferPool::upload(const void* data, std::size_t size, VkBufferUsageFlags usage) {
    if (!impl_) throw std::logic_error("moved-from buffer pool");
    if (!data || !size) throw std::invalid_argument("upload requires nonempty data");
    if (size > std::numeric_limits<VkDeviceSize>::max()) throw std::length_error("upload size overflows VkDeviceSize");
    if (size > std::numeric_limits<VkDeviceSize>::max() - 3) throw std::length_error("aligned upload size overflows VkDeviceSize");
    const VkDeviceSize gpuSize = (static_cast<VkDeviceSize>(size) + 3) & ~VkDeviceSize{3};
    if (gpuSize > impl_->config.residentBudget)
        throw std::length_error("upload exceeds resident budget");
    Allocation gpu = impl_->makeBuffer(gpuSize,
        usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
    impl_->uploadTo(gpu, static_cast<const std::uint8_t*>(data), static_cast<VkDeviceSize>(size));
    if (!impl_->nextId) throw std::overflow_error("buffer ID space exhausted");
    const Id id = impl_->nextId++;
    Impl::Entry entry;
    entry.size = static_cast<VkDeviceSize>(size);
    entry.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    entry.gpu = std::move(gpu);
    entry.resident = true;
    entry.lastUse = ++impl_->clock;
    auto inserted = impl_->entries.emplace(id, std::move(entry));
    if (!inserted.second) throw std::logic_error("duplicate buffer ID");
    impl_->stats.residentAllocationBytes += inserted.first->second.gpu.allocationSize;
    return id;
}

ManagedBufferPool::BufferView ManagedBufferPool::acquire(Id id) {
    if (!impl_) throw std::logic_error("moved-from buffer pool");
    auto it = impl_->find(id);
    if (it == impl_->entries.end()) throw std::out_of_range("unknown buffer ID");
    impl_->restore(it->second);
    if (it->second.pins == UINT32_MAX) throw std::overflow_error("buffer pin count overflow");
    ++it->second.pins;
    it->second.lastUse = ++impl_->clock;
    return {it->second.gpu.buffer, it->second.size};
}

void ManagedBufferPool::release(Id id) {
    if (!impl_) throw std::logic_error("moved-from buffer pool");
    auto it = impl_->find(id);
    if (it == impl_->entries.end()) throw std::out_of_range("unknown buffer ID");
    if (!it->second.pins) throw std::logic_error("buffer is not pinned");
    --it->second.pins;
}

std::vector<std::uint8_t> ManagedBufferPool::readback(Id id) {
    if (!impl_) throw std::logic_error("moved-from buffer pool");
    auto it = impl_->find(id);
    if (it == impl_->entries.end()) throw std::out_of_range("unknown buffer ID");
    if (it->second.pins) throw std::logic_error("release buffer after synchronizing GPU work before readback");
    impl_->restore(it->second);
    auto bytes = impl_->readGpu(it->second);
    it->second.lastUse = ++impl_->clock;
    return bytes;
}

bool ManagedBufferPool::evict(Id id) {
    if (!impl_) throw std::logic_error("moved-from buffer pool");
    auto it = impl_->find(id);
    if (it == impl_->entries.end()) throw std::out_of_range("unknown buffer ID");
    if (it->second.pins) return false;
    if (it->second.resident) impl_->evictEntry(it->second);
    return true;
}

bool ManagedBufferPool::erase(Id id) {
    if (!impl_) throw std::logic_error("moved-from buffer pool");
    auto it = impl_->find(id);
    if (it == impl_->entries.end()) return false;
    if (it->second.pins) return false;
    check(vkQueueWaitIdle(impl_->config.queue), "vkQueueWaitIdle before erase");
    if (it->second.resident) impl_->stats.residentAllocationBytes -= it->second.gpu.allocationSize;
    impl_->stats.hostStoredBytes -= it->second.host.storedBytes;
    impl_->entries.erase(it);
    return true;
}

ManagedBufferPool::Statistics ManagedBufferPool::statistics() const {
    if (!impl_) throw std::logic_error("moved-from buffer pool");
    return impl_->stats;
}

} // namespace zvram
