#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace zvram {

// Experimental generic Vulkan buffer pool; it is not a transparent application
// layer. Queue access is externally synchronized: callers must not use the
// supplied queue concurrently with pool calls. Call release only after all GPU
// work using the returned VkBuffer has completed on that queue.
class ManagedBufferPool {
public:
    using Id = std::uint64_t;

    struct Config {
        VkPhysicalDevice physicalDevice{};
        VkDevice device{};
        VkQueue queue{};
        std::uint32_t queueFamily{};
        VkDeviceSize residentBudget{};
        std::size_t hostBudget{};
        VkDeviceSize stagingChunkSize{8u * 1024u * 1024u};
    };

    struct BufferView { VkBuffer buffer{}; VkDeviceSize size{}; };
    struct Statistics {
        VkDeviceSize residentAllocationBytes{};
        std::size_t hostStoredBytes{};
        std::uint64_t rawFallbacks{};
        std::uint64_t evictions{};
        std::uint64_t restores{};
    };

    explicit ManagedBufferPool(const Config& config);
    ~ManagedBufferPool();
    ManagedBufferPool(const ManagedBufferPool&) = delete;
    ManagedBufferPool& operator=(const ManagedBufferPool&) = delete;
    ManagedBufferPool(ManagedBufferPool&&) noexcept;
    ManagedBufferPool& operator=(ManagedBufferPool&&) noexcept;

    // A buffer owns either resident GPU data or a compressed host snapshot.
    // Failed uploads do not lose existing data, but may evict other buffers.
    Id upload(const void* bytes, std::size_t size, VkBufferUsageFlags usage);
    BufferView acquire(Id id); // pins and restores when evicted
    void release(Id id);       // caller has synchronized all uses before this
    std::vector<std::uint8_t> readback(Id id); // returned CPU copy belongs to caller
    bool evict(Id id);         // false when pinned; waits for supplied queue
    bool erase(Id id);         // false when pinned; releases host and GPU storage
    Statistics statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zvram
