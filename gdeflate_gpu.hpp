#pragma once

#include "gdeflate_envelope.hpp"
#include "bp16_codec.hpp"

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace zvram::gdeflate::gpu {

enum class Format { GDeflate, BP16 };

class Decoder {
public:
    static constexpr std::uint64_t DefaultAllocatedHostBudgetBytes = 8ull * 1024u * 1024u * 1024u;

    static std::uint32_t selectMemoryTypeIndex(
        const VkPhysicalDeviceMemoryProperties& memory, std::uint32_t typeBits,
        VkMemoryPropertyFlags required, VkMemoryPropertyFlags forbidden,
        VkMemoryPropertyFlags preferred = 0) noexcept {
        std::uint32_t fallback = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if (!(typeBits & (1u << i))) continue;
            const auto flags = memory.memoryTypes[i].propertyFlags;
            if ((flags & required) != required || (flags & forbidden)) continue;
            if (fallback == UINT32_MAX) fallback = i;
            if ((flags & preferred) == preferred) return i;
        }
        return fallback;
    }

    static bool importedHostAllocationSize(std::size_t encodedBytes, VkDeviceSize alignment,
                                          std::size_t& allocationBytes) noexcept {
        if (!encodedBytes || encodedBytes > MaxBP16InputBytes || alignment < sizeof(void*) ||
            alignment > 65536 || (alignment & (alignment - 1)) ||
            alignment > std::numeric_limits<std::size_t>::max()) return false;
        const auto a = static_cast<std::size_t>(alignment);
        if (encodedBytes > std::numeric_limits<std::size_t>::max() - (a - 1)) return false;
        allocationBytes = (encodedBytes + a - 1) & ~(a - 1);
        return allocationBytes >= encodedBytes && allocationBytes - encodedBytes < a &&
               allocationBytes <= MaxBP16InputBytes + 65536u;
    }
    static bool importedHostFitsBudget(std::uint64_t coldBytes, std::uint64_t cacheBytes,
                                      std::uint64_t paddingBytes, std::uint64_t budgetBytes) noexcept {
        if (coldBytes > budgetBytes || cacheBytes > budgetBytes - coldBytes) return false;
        return paddingBytes <= budgetBytes - coldBytes - cacheBytes;
    }

    static bool localOwnerFitsLiveBudget(std::uint64_t usedBytes, std::uint64_t requestedBytes,
                                         std::uint64_t configuredLimitBytes,
                                         std::uint64_t liveLimitBytes) noexcept {
        const auto limit = std::min(configuredLimitBytes, liveLimitBytes);
        return usedBytes <= limit && requestedBytes <= limit - usedBytes;
    }

    static bool parseAllocatedHostBudgetMiB(const char* text, std::uint64_t& bytes) noexcept {
        constexpr std::uint64_t MiB = 1024ull * 1024ull;
        constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max() / MiB;
        if (!text || !*text) return false;
        std::uint64_t value = 0;
        for (; *text; ++text) {
            if (*text < '0' || *text > '9') return false;
            const auto digit = static_cast<std::uint64_t>(*text - '0');
            if (value > (maximum - digit) / 10u) return false;
            value = value * 10u + digit;
        }
        bytes = value * MiB;
        return true;
    }

    static bool parseLocalOwnerBudgetMiB(const char* text, std::uint64_t& bytes) noexcept {
        return parseAllocatedHostBudgetMiB(text, bytes);
    }

    static bool parseBP16UploadWorkers(const char* text, unsigned& workers) noexcept {
        if (!text || !*text) return false;
        unsigned value = 0;
        for (; *text; ++text) {
            if (*text < '0' || *text > '9') return false;
            const auto digit = static_cast<unsigned>(*text - '0');
            if (digit > 8u || value > (8u - digit) / 10u) return false;
            value = value * 10u + digit;
        }
        if (!value) return false;
        workers = value;
        return true;
    }

    static void copyBP16UploadBytes(std::uint8_t* destination, const std::uint8_t* source,
                                    std::size_t bytes, unsigned workers) noexcept {
        constexpr std::size_t MinimumParallelBytes = 1024u * 1024u;
        constexpr std::size_t PartitionAlignment = 64;
        if (!bytes || destination == source) return;
        if (!destination || !source || workers < 2 || workers > 8 ||
            bytes < MinimumParallelBytes) {
            if (destination && source) std::memcpy(destination, source, bytes);
            return;
        }
        const auto partitionBytes = (bytes / workers) & ~(PartitionAlignment - 1u);
        if (!partitionBytes) {
            std::memcpy(destination, source, bytes);
            return;
        }
        std::vector<std::thread> threads;
        try { threads.reserve(workers - 1u); }
        catch (...) { std::memcpy(destination, source, bytes); return; }
        bp16::detail::JoinThreads joiner{threads};
        try {
            for (unsigned i = 1; i < workers; ++i) {
                const auto begin = std::size_t(i) * partitionBytes;
                const auto end = i + 1u == workers ? bytes : begin + partitionBytes;
                threads.emplace_back([=] {
                    std::memcpy(destination + begin, source + begin, end - begin);
                });
            }
        } catch (...) {
            joiner.join();
            std::memcpy(destination, source, bytes);
            return;
        }
        std::memcpy(destination, source, partitionBytes);
        joiner.join();
    }

    class AllocatedHostBudget {
    public:
        explicit AllocatedHostBudget(std::uint64_t limitBytes) noexcept : limitBytes_(limitBytes) {}
        bool reserve(std::uint64_t bytes) noexcept {
            auto used = usedBytes_.load(std::memory_order_relaxed);
            while (used <= limitBytes_ && bytes <= limitBytes_ - used) {
                if (usedBytes_.compare_exchange_weak(used, used + bytes,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
            }
            return false;
        }
        bool reserve(std::uint64_t bytes, std::uint64_t liveLimitBytes) noexcept {
            auto used = usedBytes_.load(std::memory_order_relaxed);
            while (localOwnerFitsLiveBudget(used, bytes, limitBytes_, liveLimitBytes)) {
                if (usedBytes_.compare_exchange_weak(used, used + bytes,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
            }
            return false;
        }
        bool release(std::uint64_t bytes) noexcept {
            auto used = usedBytes_.load(std::memory_order_relaxed);
            while (used >= bytes) {
                if (usedBytes_.compare_exchange_weak(used, used - bytes,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
            }
            return false;
        }
        std::uint64_t usedBytes() const noexcept {
            return usedBytes_.load(std::memory_order_acquire);
        }
        std::uint64_t limitBytes() const noexcept { return limitBytes_; }
    private:
        const std::uint64_t limitBytes_;
        std::atomic<std::uint64_t> usedBytes_{};
    };

    class ImportedHostInput {
        friend class Decoder;
        friend class ImportedHostInputTestAccess;
    public:
        ImportedHostInput(const ImportedHostInput&) = delete;
        ImportedHostInput& operator=(const ImportedHostInput&) = delete;
        std::size_t encodedBytes() const noexcept { return encodedBytes_; }
        VkDeviceSize allocationBytes() const noexcept { return allocationBytes_; }
        bool deviceLocal() const noexcept { return deviceLocal_; }
        const std::uint8_t* data() const noexcept {
            return static_cast<const std::uint8_t*>(allocation_);
        }

        ~ImportedHostInput() {
            if (poisoned_ && poisoned_->load(std::memory_order_acquire)) return;
            if (driverAllocatedHostMemory_ && device_ && memory_ && allocation_ && unmapMemory_)
                unmapMemory_(device_, memory_);
            if (device_ && buffer_ && destroyBuffer_) destroyBuffer_(device_, buffer_, nullptr);
            if (device_ && memory_ && freeMemory_) freeMemory_(device_, memory_, nullptr);
            if (!driverAllocatedHostMemory_) std::free(allocation_);
            if (budget_ && reservedBudgetBytes_) budget_->release(reservedBudgetBytes_);
            if (localBudget_ && reservedLocalBudgetBytes_) localBudget_->release(reservedLocalBudgetBytes_);
        }

    private:
        ImportedHostInput() = default;
        bool matchesBP16Frame(const std::uint8_t* input, std::size_t encodedSize,
                              std::size_t rawSize) const noexcept {
            return input && encodedSize && rawSize && input == data() &&
                   encodedSize == encodedBytes_ && bp16Info_.rawBytes == rawSize;
        }
        VkDevice device_{};
        VkBuffer buffer_{};
        VkDeviceMemory memory_{};
        void* allocation_{};
        std::size_t encodedBytes_{};
        VkDeviceSize allocationBytes_{};
        bp16::FrameInfo bp16Info_{};
        PFN_vkDestroyBuffer destroyBuffer_{};
        PFN_vkFreeMemory freeMemory_{};
        PFN_vkUnmapMemory unmapMemory_{};
        bool driverAllocatedHostMemory_{};
        bool deviceLocal_{};
        std::shared_ptr<AllocatedHostBudget> budget_;
        std::uint64_t reservedBudgetBytes_{};
        std::shared_ptr<AllocatedHostBudget> localBudget_;
        std::uint64_t reservedLocalBudgetBytes_{};
        std::shared_ptr<std::atomic<bool>> poisoned_;
    };
    using ImportedHostInputPtr = std::shared_ptr<ImportedHostInput>;

    struct Profile {
        std::uint64_t calls{};
        std::uint64_t validationNs{};
        std::uint64_t inputPrepareNs{};
        std::uint64_t bp16BufferPrepareNs{};
        std::uint64_t bp16DirectCopyNs{};
        std::uint64_t submitWaitNs{};
        std::uint64_t queueSubmitNs{};
        std::uint64_t fenceWaitNs{};
        std::uint64_t gpuTransferNs{};
        std::uint64_t gpuDecodeNs{};
        std::uint64_t gpuFinishNs{};
        std::uint64_t gpuSamples{};
        std::uint64_t bp16InputBytes{};
        std::uint64_t bp16LocalOwnerInputBytes{};
        std::uint64_t bp16OtherInputBytes{};
        std::uint64_t bp16LocalOwnerDecodeNs{};
        std::uint64_t bp16OtherDecodeNs{};
        std::uint64_t bp16LocalOwnerSamples{};
        std::uint64_t bp16OtherSamples{};
    };

    Decoder() = default;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;
    ~Decoder() { destroy(); }
    bool unsafe() const noexcept { return poisoned_; }
    bool profilingEnabled() const noexcept { return profileEnabled_; }
    bool hostInputEnabled() const noexcept { return bp16HostInput_; }
    bool cachedHostUploadPreferenceEnabled() const noexcept { return bp16CachedUploadPreference_; }
    bool importedHostInputEnabled() const noexcept { return bp16ImportHostInput_; }
    bool allocatedHostInputEnabled() const noexcept { return bp16AllocatedHostInput_; }
    bool bp16EncoderEnabled() const noexcept { return bp16EncoderEnabled_; }
    unsigned uploadWorkers() const noexcept { return bp16UploadWorkers_; }
    std::uint64_t allocatedHostInputUsedBytes() const noexcept {
        return allocatedHostBudget_ ? allocatedHostBudget_->usedBytes() : 0;
    }
    std::uint64_t allocatedHostInputLimitBytes() const noexcept {
        return allocatedHostBudget_ ? allocatedHostBudget_->limitBytes() : 0;
    }
    std::uint64_t localOwnerUsedBytes() const noexcept {
        return localOwnerBudget_ ? localOwnerBudget_->usedBytes() : 0;
    }
    std::uint64_t localOwnerLimitBytes() const noexcept {
        return localOwnerBudget_ ? localOwnerBudget_->limitBytes() : 0;
    }
    Profile profile() const noexcept { return profile_; }

    // GDeflate requires shaderInt64, subgroup-size-control, computeFullSubgroups,
    // and a compute-capable private queue. BP16 needs the supplied device limits
    // for its plain 256-thread compute pipeline.
    VkResult initialize(VkDevice device, PFN_vkGetDeviceProcAddr nextGdpa,
                        const VkPhysicalDeviceMemoryProperties& memory,
                        VkQueue queue, std::uint32_t family,
                        PFN_vkSetDeviceLoaderData setLoaderData,
                        const char* shaderPath,
                        const VkPhysicalDeviceProperties* properties = nullptr,
                        Format format = Format::GDeflate,
                        std::uint32_t timestampValidBits = 0,
                        bool importHostInput = false,
                        VkDeviceSize importHostAlignment = 0,
                        bool allocatedHostInput = false,
                        std::uint64_t allocatedHostBudgetBytes = DefaultAllocatedHostBudgetBytes,
                        const char* bp16EncodeAnalyzeShaderPath = nullptr,
                        const char* bp16EncodePackShaderPath = nullptr,
                        std::uint64_t localOwnerBudgetBytes = 0,
                        std::uint32_t localOwnerHeapIndex = UINT32_MAX) {
        const char* profileEnv = std::getenv("ZVRAM_VULKAN_GPU_PROFILE");
        profileEnabled_ = profileEnv && std::strcmp(profileEnv, "1") == 0;
        profile_ = {};
        localOwnerHeapIndex_ = localOwnerHeapIndex < memory.memoryHeapCount
            ? localOwnerHeapIndex : UINT32_MAX;
        if (initialized_ || poisoned_ || !device || !nextGdpa || !queue || !setLoaderData || !shaderPath)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (format != Format::GDeflate && format != Format::BP16)
            return VK_ERROR_VALIDATION_FAILED_EXT;
        bp16UploadWorkers_ = 1;
        if (format == Format::BP16) {
            if (const char* uploadWorkers = std::getenv("ZVRAM_VULKAN_BP16_UPLOAD_WORKERS"))
                if (!parseBP16UploadWorkers(uploadWorkers, bp16UploadWorkers_))
                    return VK_ERROR_VALIDATION_FAILED_EXT;
        }
        bp16ImportHostInput_ = format == Format::BP16 && importHostInput;
        bp16AllocatedHostInput_ = format == Format::BP16 && allocatedHostInput;
        if (bp16ImportHostInput_ && bp16AllocatedHostInput_)
            return VK_ERROR_VALIDATION_FAILED_EXT;
        const char* hostInputEnv = std::getenv("ZVRAM_VULKAN_BP16_HOST_INPUT");
        bp16HostInput_ = format == Format::BP16 && !bp16ImportHostInput_ &&
                         (bp16AllocatedHostInput_ || (hostInputEnv &&
                          std::strcmp(hostInputEnv, "1") == 0));
        const char* cachedUploadEnv = std::getenv("ZVRAM_VULKAN_BP16_CACHED_UPLOAD");
        bp16CachedUploadPreference_ = format == Format::BP16 && bp16HostInput_ &&
            cachedUploadEnv && std::strcmp(cachedUploadEnv, "1") == 0;
        if (bp16ImportHostInput_ && (!importHostAlignment || importHostAlignment > 65536 ||
            (importHostAlignment & (importHostAlignment - 1))))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        importHostAlignment_ = importHostAlignment;
        if (bp16ImportHostInput_ || bp16AllocatedHostInput_) {
            try { poisonState_ = std::make_shared<std::atomic<bool>>(false); }
            catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        }
        if (bp16AllocatedHostInput_) {
            try { allocatedHostBudget_ = std::make_shared<AllocatedHostBudget>(allocatedHostBudgetBytes); }
            catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
            if (localOwnerBudgetBytes) {
                try { localOwnerBudget_ = std::make_shared<AllocatedHostBudget>(
                    std::min(localOwnerBudgetBytes, allocatedHostBudgetBytes)); }
                catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
            }
        }
        if (format == Format::BP16 && (!properties ||
            properties->limits.maxComputeWorkGroupInvocations < 256 ||
            properties->limits.maxComputeWorkGroupSize[0] < 256 ||
            !properties->limits.maxComputeWorkGroupCount[0] ||
            !properties->limits.maxStorageBufferRange))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        std::vector<std::uint8_t> code;
        if (!readShader(shaderPath, code)) return VK_ERROR_INITIALIZATION_FAILED;

        device_ = device;
        memory_ = memory;
        queue_ = queue;
        family_ = family;
        setLoaderData_ = setLoaderData;
        format_ = format;
        if (properties) {
            maxStorageBufferRange_ = properties->limits.maxStorageBufferRange;
            maxDispatchGroupsX_ = properties->limits.maxComputeWorkGroupCount[0];
            minStorageBufferOffsetAlignment_ = properties->limits.minStorageBufferOffsetAlignment;
            timestampPeriod_ = properties->limits.timestampPeriod;
        }
        VkResult result = loadFunctions(nextGdpa);
        if (result == VK_SUCCESS && profileEnabled_ && timestampValidBits &&
            timestampValidBits <= 64 && timestampPeriod_ > 0.0 &&
            loadTimestampFunctions(nextGdpa)) {
            VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 4;
            result = api_.createQueryPool(device_, &queryInfo, nullptr, &queryPool_);
            if (result == VK_ERROR_DEVICE_LOST) markPoisoned();
            else if (result != VK_SUCCESS) result = VK_SUCCESS;
            else {
                timestampValidBits_ = timestampValidBits;
                gpuProfileEnabled_ = true;
            }
        }
        const auto uploadUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            (bp16HostInput_ ? VkBufferUsageFlags(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
                            : VkBufferUsageFlags(0));
        const auto uploadForbidden = bp16HostInput_
            ? VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            : VkMemoryPropertyFlags(0);
        if (result == VK_SUCCESS) result = createBuffer(4, uploadUsage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            uploadForbidden, upload_, bp16CachedUploadPreference_
                ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
        if (result == VK_SUCCESS && !bp16HostInput_) result = createBuffer(4,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, input_);
        if (result == VK_SUCCESS) result = createBuffer(12, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, control_);
        if (result == VK_SUCCESS) result = createBuffer(4,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, scratch_);
        if (result == VK_SUCCESS) result = createBuffer(4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, errorReadback_);
        if (result == VK_SUCCESS) result = createPipeline(code, format);
        if (result == VK_SUCCESS && format == Format::BP16 && bp16AllocatedHostInput_ &&
            bp16EncodeAnalyzeShaderPath && bp16EncodePackShaderPath) {
            const auto encoderResult = createBP16Encoder(bp16EncodeAnalyzeShaderPath,
                                                         bp16EncodePackShaderPath);
            if (encoderResult == VK_ERROR_DEVICE_LOST) result = checked(encoderResult);
            else if (encoderResult != VK_SUCCESS) cleanupBP16Encoder();
            else bp16EncoderRequested_ = true;
        }
        if (result == VK_SUCCESS) result = createDescriptors();
        if (result == VK_SUCCESS) result = createCommands();
        if (result == VK_SUCCESS) result = createFence();
        if (result != VK_SUCCESS) {
            if (result == VK_ERROR_DEVICE_LOST) markPoisoned();
            else cleanup();
            return result;
        }
        inputCapacity_ = 4;
        initialized_ = true;
        bp16EncoderEnabled_ = bp16EncoderRequested_;
        return VK_SUCCESS;
    }

    VkResult importHostInput(const std::uint8_t* encoded, std::size_t encodedSize,
                             ImportedHostInputPtr& out) {
        out.reset();
        if (!initialized_ || poisoned_ || !bp16ImportHostInput_ || !encoded || !encodedSize ||
            encodedSize > MaxBP16InputBytes) return VK_ERROR_FEATURE_NOT_PRESENT;
        std::size_t hostBytes{};
        if (!importedHostAllocationSize(encodedSize, importHostAlignment_, hostBytes))
            return VK_ERROR_VALIDATION_FAILED_EXT;
        void* allocation{};
        if (posix_memalign(&allocation, static_cast<std::size_t>(importHostAlignment_), hostBytes) != 0)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        copyBP16UploadBytes(static_cast<std::uint8_t*>(allocation), encoded,
                            encodedSize, bp16UploadWorkers_);
        std::memset(static_cast<std::uint8_t*>(allocation) + encodedSize, 0, hostBytes - encodedSize);
        bp16::FrameInfo bp16Info{};
        if (!bp16::inspect(static_cast<const std::uint8_t*>(allocation), encodedSize, &bp16Info)) {
            std::free(allocation);
            return VK_ERROR_VALIDATION_FAILED_EXT;
        }
        ImportedHostInputPtr owner;
        try { owner.reset(new ImportedHostInput()); }
        catch (const std::bad_alloc&) { std::free(allocation); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        owner->device_ = device_; owner->allocation_ = allocation;
        owner->encodedBytes_ = encodedSize; owner->allocationBytes_ = hostBytes;
        owner->bp16Info_ = bp16Info;
        owner->destroyBuffer_ = api_.destroyBuffer; owner->freeMemory_ = api_.freeMemory;
        owner->poisoned_ = poisonState_;
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.pNext = &external; create.size = hostBytes; create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkResult result = api_.createBuffer(device_, &create, nullptr, &owner->buffer_);
        if (result != VK_SUCCESS) return checked(result);
        VkBufferMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
        reqInfo.buffer = owner->buffer_;
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated;
        api_.getBufferMemoryRequirements2(device_, &reqInfo, &requirements);
        if (dedicated.requiresDedicatedAllocation) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkMemoryHostPointerPropertiesEXT pointerProperties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        result = api_.getMemoryHostPointerProperties(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                      owner->allocation_, &pointerProperties);
        if (result != VK_SUCCESS) return checked(result);
        std::uint32_t type = UINT32_MAX;
        const auto bits = requirements.memoryRequirements.memoryTypeBits & pointerProperties.memoryTypeBits;
        for (std::uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
            if (!(bits & (1u << i))) continue;
            const auto flags = memory_.memoryTypes[i].propertyFlags;
            if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
                !(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
        }
        if (type == UINT32_MAX || requirements.memoryRequirements.size > hostBytes)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import.pHostPointer = owner->allocation_;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &import; allocate.allocationSize = hostBytes;
        allocate.memoryTypeIndex = type;
        result = api_.allocateMemory(device_, &allocate, nullptr, &owner->memory_);
        if (result != VK_SUCCESS) return checked(result);
        result = api_.bindBufferMemory(device_, owner->buffer_, owner->memory_, 0);
        if (result != VK_SUCCESS) return checked(result);
        out = std::move(owner);
        return VK_SUCCESS;
    }

    // Experimental synchronous BP16 encoder. Source must be storage-buffer
    // readable and stable until this call returns; uncertain GPU completion
    // poisons the decoder so the caller retains the sparse backing and owner.
    VkResult encodeBP16(VkBuffer rawBuffer, VkDeviceSize rawOffset, std::size_t rawBytes,
                        std::size_t maxEncodedBytes, ImportedHostInputPtr& out,
                        std::uint64_t localOwnerLiveLimitBytes = UINT64_MAX) {
        out.reset();
        if (!initialized_ || poisoned_) return VK_ERROR_DEVICE_LOST;
        if (!allocatedHostBudget_ || !allocatedHostBudget_->limitBytes() ||
            allocatedHostBudget_->usedBytes() >= allocatedHostBudget_->limitBytes())
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (!bp16EncoderEnabled_ || !rawBuffer || !rawBytes ||
            rawBytes > MaxRawBytes || rawBytes % bp16::RawBytesPerBlock ||
            rawBytes > maxStorageBufferRange_ ||
            rawOffset > std::numeric_limits<VkDeviceSize>::max() - rawBytes ||
            (rawOffset & 3u) ||
            (minStorageBufferOffsetAlignment_ && rawOffset % minStorageBufferOffsetAlignment_))
            return VK_ERROR_VALIDATION_FAILED_EXT;
        const auto blockCount = rawBytes / bp16::RawBytesPerBlock;
        const auto analyzeGroups = static_cast<std::uint32_t>((blockCount + 255u) / 256u);
        const auto packedWords = blockCount * 64u;
        const auto packGroups = static_cast<std::uint32_t>((packedWords + 255u) / 256u);
        const auto metadataBytes = blockCount * sizeof(std::uint32_t);
        if (!analyzeGroups || analyzeGroups > maxDispatchGroupsX_ ||
            !packGroups || packGroups > maxDispatchGroupsX_ ||
            metadataBytes > maxStorageBufferRange_ || !maxEncodedBytes)
            return VK_ERROR_VALIDATION_FAILED_EXT;
        maxEncodedBytes = std::min(maxEncodedBytes, MaxBP16InputBytes);
        VkResult result = updateEncoderDescriptors(rawBuffer, rawOffset, rawBytes,
            VK_NULL_HANDLE, 0, true);
        if (result != VK_SUCCESS) return result;
        result = recordAndSubmitBP16Encode(rawBuffer, rawOffset, rawBytes,
            VK_NULL_HANDLE, 0, analyzeGroups, true);
        if (result != VK_SUCCESS) return result;

        const auto tableBytes = blockCount * bp16::DescriptorBytes;
        const auto payloadBegin = bp16::HeaderBytes + tableBytes;
        if (payloadBegin > maxEncodedBytes || payloadBegin > UINT32_MAX)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        std::vector<std::uint8_t> prefix;
        try { prefix.resize(payloadBegin); }
        catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        bp16::store32(prefix.data(), bp16::Magic);
        bp16::store32(prefix.data() + 4, bp16::Version);
        bp16::store32(prefix.data() + 8, static_cast<std::uint32_t>(rawBytes));
        bp16::store32(prefix.data() + 12, static_cast<std::uint32_t>(blockCount));
        std::size_t nextPayload = payloadBegin;
        const auto* metadata = static_cast<const std::uint8_t*>(encodeMetadata_.mapped);
        for (std::size_t block = 0; block < blockCount; ++block) {
            const auto packed = bp16::load32(metadata + block * sizeof(std::uint32_t));
            const auto mask = packed >> 16;
            const auto bits = static_cast<unsigned>(__builtin_popcount(mask));
            if (nextPayload > maxEncodedBytes || nextPayload > UINT32_MAX ||
                bits * 16u > maxEncodedBytes - nextPayload)
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            auto* descriptor = prefix.data() + bp16::HeaderBytes + block * bp16::DescriptorBytes;
            bp16::store32(descriptor, static_cast<std::uint32_t>(nextPayload));
            bp16::store32(descriptor + 4, packed);
            nextPayload += bits * 16u;
        }
        if (nextPayload > maxStorageBufferRange_ || nextPayload > MaxBP16InputBytes)
            return VK_ERROR_VALIDATION_FAILED_EXT;

        ImportedHostInputPtr owner;
        result = allocateHostFrameBuffer(nextPayload, owner, maxEncodedBytes, true,
                                         localOwnerLiveLimitBytes);
        if (result != VK_SUCCESS) return result;
        std::memcpy(owner->allocation_, prefix.data(), prefix.size());
        std::memset(static_cast<std::uint8_t*>(owner->allocation_) + nextPayload, 0,
                    static_cast<std::size_t>(owner->allocationBytes_ - nextPayload));
        result = updateEncoderDescriptors(rawBuffer, rawOffset, rawBytes,
            owner->buffer_, static_cast<VkDeviceSize>(nextPayload), false);
        if (result != VK_SUCCESS) return result;
        result = recordAndSubmitBP16Encode(rawBuffer, rawOffset, rawBytes,
            owner->buffer_, static_cast<VkDeviceSize>(nextPayload), packGroups, false);
        if (result != VK_SUCCESS) return result;
        if (!bp16::inspectMetadata(prefix.data(), prefix.size(), nextPayload, &owner->bp16Info_))
            return VK_ERROR_UNKNOWN;
        out = std::move(owner);
        return VK_SUCCESS;
    }

    VkResult allocateHostInput(const std::uint8_t* encoded, std::size_t encodedSize,
                               ImportedHostInputPtr& out) {
        out.reset();
        if (!initialized_ || poisoned_ || !bp16AllocatedHostInput_ || !encoded || !encodedSize ||
            encodedSize > MaxBP16InputBytes) return VK_ERROR_FEATURE_NOT_PRESENT;
        ImportedHostInputPtr owner;
        VkResult result = allocateHostFrameBuffer(encodedSize, owner);
        if (result != VK_SUCCESS) return result;
        copyBP16UploadBytes(static_cast<std::uint8_t*>(owner->allocation_), encoded,
                            encodedSize, bp16UploadWorkers_);
        std::memset(static_cast<std::uint8_t*>(owner->allocation_) + encodedSize, 0,
                    static_cast<std::size_t>(owner->allocationBytes_) - encodedSize);
        if (!bp16::inspect(owner->data(), encodedSize, &owner->bp16Info_))
            return VK_ERROR_VALIDATION_FAILED_EXT;
        out = std::move(owner);
        return VK_SUCCESS;
    }

private:
    VkResult allocateHostFrameBuffer(std::size_t encodedSize, ImportedHostInputPtr& out,
                                     std::uint64_t maxAllocationBytes = MaxBP16InputBytes + 65536u,
                                     bool preferLocal = false,
                                     std::uint64_t localOwnerLiveLimitBytes = UINT64_MAX) {
        out.reset();
        if (!allocatedHostBudget_ || !allocatedHostBudget_->limitBytes())
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (!api_.getBufferMemoryRequirements2) return VK_ERROR_FEATURE_NOT_PRESENT;
        const auto bufferBytes = (encodedSize + 3u) & ~std::size_t(3u);
        if (!encodedSize || bufferBytes > maxStorageBufferRange_ || bufferBytes < encodedSize)
            return VK_ERROR_VALIDATION_FAILED_EXT;
        ImportedHostInputPtr owner;
        try { owner.reset(new ImportedHostInput()); }
        catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        owner->device_ = device_;
        owner->encodedBytes_ = encodedSize;
        owner->destroyBuffer_ = api_.destroyBuffer;
        owner->freeMemory_ = api_.freeMemory;
        owner->unmapMemory_ = api_.unmapMemory;
        owner->driverAllocatedHostMemory_ = true;
        owner->budget_ = allocatedHostBudget_;
        owner->poisoned_ = poisonState_;

        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size = static_cast<VkDeviceSize>(bufferBytes);
        create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = checked(api_.createBuffer(device_, &create, nullptr, &owner->buffer_));
        if (result != VK_SUCCESS) return result;

        VkBufferMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
        reqInfo.buffer = owner->buffer_;
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated;
        api_.getBufferMemoryRequirements2(device_, &reqInfo, &requirements);
        const auto& memoryRequirements = requirements.memoryRequirements;
        if (memoryRequirements.size < bufferBytes ||
            memoryRequirements.size > MaxBP16InputBytes + 65536u ||
            memoryRequirements.size > maxAllocationBytes)
            return VK_ERROR_FEATURE_NOT_PRESENT;

        constexpr VkMemoryPropertyFlags gttRequired = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        constexpr VkMemoryPropertyFlags localRequired = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
#ifdef VK_AMD_device_coherent_memory
        constexpr VkMemoryPropertyFlags localForbidden = VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
#else
        constexpr VkMemoryPropertyFlags localForbidden = 0;
#endif
        std::uint32_t localType = UINT32_MAX, gttType = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
            if (!(memoryRequirements.memoryTypeBits & (1u << i))) continue;
            const auto flags = memory_.memoryTypes[i].propertyFlags;
            if ((flags & localRequired) == localRequired && !(flags & localForbidden) &&
                memory_.memoryTypes[i].heapIndex == localOwnerHeapIndex_)
                localType = i;
            if (gttType == UINT32_MAX && (flags & gttRequired) == gttRequired &&
                !(flags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | localForbidden)))
                gttType = i;
        }
        const bool useLocal = preferLocal && localOwnerBudget_ && localType != UINT32_MAX &&
            localOwnerBudget_->reserve(memoryRequirements.size, localOwnerLiveLimitBytes);
        if (!useLocal && gttType == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (!allocatedHostBudget_->reserve(memoryRequirements.size))
        {
            if (useLocal) localOwnerBudget_->release(memoryRequirements.size);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        owner->reservedBudgetBytes_ = memoryRequirements.size;
        if (useLocal) {
            owner->localBudget_ = localOwnerBudget_;
            owner->reservedLocalBudgetBytes_ = memoryRequirements.size;
            owner->deviceLocal_ = true;
        }

        VkMemoryDedicatedAllocateInfo dedicatedAllocate{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedAllocate.buffer = owner->buffer_;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = dedicated.requiresDedicatedAllocation ? &dedicatedAllocate : nullptr;
        allocate.allocationSize = memoryRequirements.size;
        allocate.memoryTypeIndex = useLocal ? localType : gttType;
        result = checked(api_.allocateMemory(device_, &allocate, nullptr, &owner->memory_));
        if (result != VK_SUCCESS) {
            if (useLocal && result != VK_ERROR_DEVICE_LOST) {
                owner.reset();
                return allocateHostFrameBuffer(encodedSize, out, maxAllocationBytes, false);
            }
            return result;
        }
        owner->allocationBytes_ = memoryRequirements.size;
        result = checked(api_.bindBufferMemory(device_, owner->buffer_, owner->memory_, 0));
        if (result != VK_SUCCESS) {
            if (useLocal && result != VK_ERROR_DEVICE_LOST) {
                owner.reset();
                return allocateHostFrameBuffer(encodedSize, out, maxAllocationBytes, false);
            }
            return result;
        }
        result = checked(api_.mapMemory(device_, owner->memory_, 0, owner->allocationBytes_, 0,
                                       &owner->allocation_));
        if (result != VK_SUCCESS || !owner->allocation_) {
            if (useLocal && result != VK_ERROR_DEVICE_LOST) {
                owner.reset();
                return allocateHostFrameBuffer(encodedSize, out, maxAllocationBytes, false);
            }
            return result == VK_SUCCESS ? VK_ERROR_MEMORY_MAP_FAILED : result;
        }
        out = std::move(owner);
        return VK_SUCCESS;
    }

public:

    // `output` must belong to this device, contain [offset, offset+roundUp(raw,4)),
    // and have STORAGE_BUFFER and TRANSFER_DST usage. `offset` must satisfy
    // minStorageBufferOffsetAlignment; the descriptor starts there and is
    // narrowed to the padded range, so shader control[2] remains zero. The
    // caller excludes concurrent use and synchronizes later access across queues.
    VkResult restore(const std::uint8_t* encoded, std::size_t encodedSize,
                     VkBuffer output, VkDeviceSize offset, std::size_t rawSize,
                     const ImportedHostInput* imported = nullptr) {
        using Clock = std::chrono::steady_clock;
        const auto validationStarted = profileEnabled_ ? Clock::now() : Clock::time_point{};
        if (profileEnabled_) ++profile_.calls;
        auto finishValidation = [&](VkResult result) {
            if (profileEnabled_)
                profile_.validationNs += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - validationStarted).count());
            return result;
        };
        if (!initialized_ || poisoned_) return finishValidation(VK_ERROR_DEVICE_LOST);
        if (imported) {
            if ((!bp16ImportHostInput_ && !bp16AllocatedHostInput_) ||
                imported->driverAllocatedHostMemory_ != bp16AllocatedHostInput_ ||
                imported->device_ != device_ ||
                imported->buffer_ == VK_NULL_HANDLE || imported->memory_ == VK_NULL_HANDLE ||
                !imported->allocation_ ||
                imported->poisoned_ != poisonState_ ||
                imported->encodedBytes_ != encodedSize || imported->data() != encoded)
                return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
        }
        const auto maxInputBytes = format_ == Format::BP16 ? MaxBP16InputBytes : MaxInputBytes;
        if (!encoded || !output || !rawSize || rawSize > MaxRawBytes ||
            encodedSize > maxInputBytes || encodedSize > MaxEncodedBytes ||
            (offset & 3u))
            return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
        const auto paddedRaw = (rawSize + 3u) & ~std::size_t(3u);
        if (offset > std::numeric_limits<VkDeviceSize>::max() - paddedRaw)
            return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
        std::uint32_t dispatchGroups{};
        if (format_ == Format::BP16) {
            if (imported) {
                if (!imported->matchesBP16Frame(encoded, encodedSize, rawSize))
                    return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
            } else if (!bp16::validate(encoded, encodedSize, static_cast<std::uint32_t>(rawSize)))
                return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
            if (paddedRaw > maxStorageBufferRange_)
                return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
            dispatchGroups = static_cast<std::uint32_t>((rawSize + 1023u) / 1024u);
            if (!dispatchGroups || dispatchGroups > maxDispatchGroupsX_)
                return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
        } else {
            Info info{};
            const Limits limits{MaxEncodedBytes, MaxRawBytes, rawSize, 0, 0, 512};
            if (!validateEnvelope(encoded, encodedSize, limits, &info))
                return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
            dispatchGroups = info.tileCount;
        }
        const auto inputBytes = (encodedSize + 3u) & ~std::size_t(3u);
        if (format_ == Format::BP16 && inputBytes > maxStorageBufferRange_)
            return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
        if (imported && inputBytes > imported->allocationBytes_)
            return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
        (void)finishValidation(VK_SUCCESS);

        const bool profileBP16Input = profileEnabled_ && format_ == Format::BP16;
        const auto inputPrepareStarted = profileEnabled_ ? Clock::now() : Clock::time_point{};
        const auto bufferPrepareStarted = profileBP16Input ? Clock::now() : Clock::time_point{};
        VkResult result = imported
            ? updateDescriptors(output, offset, static_cast<VkDeviceSize>(inputBytes),
                static_cast<VkDeviceSize>(paddedRaw), imported->buffer_)
            : checked(ensureInputBuffers(inputBytes, output, offset,
                                         static_cast<VkDeviceSize>(paddedRaw)));
        if (profileBP16Input)
            profile_.bp16BufferPrepareNs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - bufferPrepareStarted).count());
        if (result != VK_SUCCESS) {
            if (profileEnabled_)
                profile_.inputPrepareNs += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - inputPrepareStarted).count());
            return result;
        }

        if (!imported) {
            const auto directCopyStarted = profileBP16Input ? Clock::now() : Clock::time_point{};
            auto* upload = static_cast<std::uint8_t*>(upload_.mapped);
            if (format_ == Format::BP16)
                copyBP16UploadBytes(upload, encoded, encodedSize, bp16UploadWorkers_);
            else
                std::memcpy(upload, encoded, encodedSize);
            std::memset(upload + encodedSize, 0, inputBytes - encodedSize);
            if (profileBP16Input)
                profile_.bp16DirectCopyNs += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - directCopyStarted).count());
        }
        const std::uint32_t controlWords[3]{1u, 0u, 0u};
        std::memcpy(control_.mapped, controlWords, sizeof(controlWords));
        if (profileEnabled_)
            profile_.inputPrepareNs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - inputPrepareStarted).count());
        result = checked(api_.resetCommandPool(device_, commandPool_, 0));
        if (result != VK_SUCCESS) return result;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = checked(api_.beginCommandBuffer(commandBuffer_, &begin));
        if (result != VK_SUCCESS) return result;
        if (gpuProfileEnabled_) {
            api_.cmdResetQueryPool(commandBuffer_, queryPool_, 0, 4);
            api_.cmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_, 0);
        }

        VkMemoryBarrier reuse{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        reuse.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                              VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                              VK_ACCESS_TRANSFER_WRITE_BIT;
        reuse.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        api_.cmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuse, 0, nullptr, 0, nullptr);

        const VkBufferCopy uploadRegion{0, 0, static_cast<VkDeviceSize>(inputBytes)};
        if (!bp16HostInput_ && !imported)
            api_.cmdCopyBuffer(commandBuffer_, upload_.buffer, input_.buffer, 1, &uploadRegion);
        api_.cmdFillBuffer(commandBuffer_, scratch_.buffer, 0, 4, 0);
        // Canonical BP16 frames cover every output word. Avoid a redundant
        // full-range transfer write; scratch clearing still detects errors.
        if (format_ != Format::BP16)
            api_.cmdFillBuffer(commandBuffer_, output, offset, static_cast<VkDeviceSize>(paddedRaw), 0);
        if (gpuProfileEnabled_)
            api_.cmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, queryPool_, 1);

        VkMemoryBarrier computeReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        computeReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
        computeReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        api_.cmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT |
            VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &computeReady, 0, nullptr, 0, nullptr);

        api_.cmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        api_.cmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
        api_.cmdDispatch(commandBuffer_, dispatchGroups, 1, 1);
        if (gpuProfileEnabled_)
            api_.cmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queryPool_, 2);

        VkBufferMemoryBarrier outputReady{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        outputReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        outputReady.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        outputReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outputReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outputReady.buffer = output;
        outputReady.offset = offset;
        outputReady.size = static_cast<VkDeviceSize>(paddedRaw);
        VkBufferMemoryBarrier errorReady{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        errorReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        errorReady.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        errorReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        errorReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        errorReady.buffer = scratch_.buffer;
        errorReady.offset = 0;
        errorReady.size = 4;
        VkBufferMemoryBarrier readyBarriers[2]{outputReady, errorReady};
        api_.cmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 2, readyBarriers, 0, nullptr);

        const VkBufferCopy errorCopy{0, 0, 4};
        api_.cmdCopyBuffer(commandBuffer_, scratch_.buffer, errorReadback_.buffer, 1, &errorCopy);
        VkBufferMemoryBarrier hostReady{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostReady.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostReady.buffer = errorReadback_.buffer;
        hostReady.offset = 0;
        hostReady.size = 4;
        api_.cmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostReady, 0, nullptr);
        if (gpuProfileEnabled_)
            api_.cmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, 3);
        result = checked(api_.endCommandBuffer(commandBuffer_));
        if (result != VK_SUCCESS) return result;

        result = checked(api_.resetFences(device_, 1, &fence_));
        if (result != VK_SUCCESS) return result;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        const auto submitWaitStarted = profileEnabled_ ? Clock::now() : Clock::time_point{};
        result = checked(api_.queueSubmit(queue_, 1, &submit, fence_));
        if (profileEnabled_)
            profile_.queueSubmitNs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - submitWaitStarted).count());
        if (result != VK_SUCCESS) {
            // Submission failure leaves completion uncertain. Retain private
            // resources and any imported owners instead of overwriting in flight.
            markPoisoned();
            if (profileEnabled_)
                profile_.submitWaitNs += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - submitWaitStarted).count());
            return result;
        }
        const auto fenceWaitStarted = profileEnabled_ ? Clock::now() : Clock::time_point{};
        result = api_.waitForFences(device_, 1, &fence_, VK_TRUE, WaitNanoseconds);
        if (profileEnabled_)
            profile_.fenceWaitNs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - fenceWaitStarted).count());
        if (result != VK_SUCCESS) {
            markPoisoned(); // Work may still be in flight; never reset/free these objects.
            if (profileEnabled_)
                profile_.submitWaitNs += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - submitWaitStarted).count());
            return result;
        }
        if (profileEnabled_)
            profile_.submitWaitNs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - submitWaitStarted).count());
        if (profileEnabled_ && format_ == Format::BP16) {
            profile_.bp16InputBytes += inputBytes;
            if (imported && imported->deviceLocal_)
                profile_.bp16LocalOwnerInputBytes += inputBytes;
            else
                profile_.bp16OtherInputBytes += inputBytes;
        }
        if (gpuProfileEnabled_) {
            std::uint64_t timestamps[4]{};
            result = api_.getQueryPoolResults(device_, queryPool_, 0, 4, sizeof(timestamps),
                                               timestamps, sizeof(std::uint64_t),
                                               VK_QUERY_RESULT_64_BIT);
            if (result == VK_ERROR_DEVICE_LOST) return checked(result);
            if (result == VK_SUCCESS) {
                profile_.gpuTransferNs += timestampNs(timestampDelta(timestamps[0], timestamps[1]));
                const auto decodeNs=timestampNs(timestampDelta(timestamps[1], timestamps[2]));
                profile_.gpuDecodeNs += decodeNs;
                profile_.gpuFinishNs += timestampNs(timestampDelta(timestamps[2], timestamps[3]));
                ++profile_.gpuSamples;
                if (format_ == Format::BP16) {
                    if (imported && imported->deviceLocal_) {
                        profile_.bp16LocalOwnerDecodeNs += decodeNs;
                        ++profile_.bp16LocalOwnerSamples;
                    } else {
                        profile_.bp16OtherDecodeNs += decodeNs;
                        ++profile_.bp16OtherSamples;
                    }
                }
            } else {
                disableGpuProfiling();
            }
        }
        std::uint32_t errorMask{};
        std::memcpy(&errorMask, errorReadback_.mapped, sizeof(errorMask));
        return errorMask ? VK_ERROR_UNKNOWN : VK_SUCCESS;
    }

    void destroy() noexcept {
        if (poisoned_) return;
        cleanup();
    }

private:
    static constexpr std::size_t MaxInputBytes = 32u * 1024u * 1024u;
    static constexpr std::size_t MaxRawBytes = 32u * 1024u * 1024u;
    static constexpr std::size_t MaxEncodedBytes = 64u * 1024u * 1024u;
    static constexpr std::size_t MaxBP16InputBytes = bp16::HeaderBytes +
        (MaxRawBytes / bp16::RawBytesPerBlock) * bp16::DescriptorBytes + MaxRawBytes;
    static constexpr std::uint64_t WaitNanoseconds = 5'000'000'000ull;

    struct Functions {
        PFN_vkCreateBuffer createBuffer{};
        PFN_vkDestroyBuffer destroyBuffer{};
        PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements{};
        PFN_vkGetBufferMemoryRequirements2 getBufferMemoryRequirements2{};
        PFN_vkGetMemoryHostPointerPropertiesEXT getMemoryHostPointerProperties{};
        PFN_vkAllocateMemory allocateMemory{};
        PFN_vkFreeMemory freeMemory{};
        PFN_vkBindBufferMemory bindBufferMemory{};
        PFN_vkMapMemory mapMemory{};
        PFN_vkUnmapMemory unmapMemory{};
        PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout{};
        PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout{};
        PFN_vkCreatePipelineLayout createPipelineLayout{};
        PFN_vkDestroyPipelineLayout destroyPipelineLayout{};
        PFN_vkCreateShaderModule createShaderModule{};
        PFN_vkDestroyShaderModule destroyShaderModule{};
        PFN_vkCreateComputePipelines createComputePipelines{};
        PFN_vkDestroyPipeline destroyPipeline{};
        PFN_vkCreateDescriptorPool createDescriptorPool{};
        PFN_vkDestroyDescriptorPool destroyDescriptorPool{};
        PFN_vkAllocateDescriptorSets allocateDescriptorSets{};
        PFN_vkUpdateDescriptorSets updateDescriptorSets{};
        PFN_vkCreateCommandPool createCommandPool{};
        PFN_vkDestroyCommandPool destroyCommandPool{};
        PFN_vkResetCommandPool resetCommandPool{};
        PFN_vkAllocateCommandBuffers allocateCommandBuffers{};
        PFN_vkBeginCommandBuffer beginCommandBuffer{};
        PFN_vkEndCommandBuffer endCommandBuffer{};
        PFN_vkCmdPipelineBarrier cmdPipelineBarrier{};
        PFN_vkCmdCopyBuffer cmdCopyBuffer{};
        PFN_vkCmdFillBuffer cmdFillBuffer{};
        PFN_vkCmdBindPipeline cmdBindPipeline{};
        PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets{};
        PFN_vkCmdDispatch cmdDispatch{};
        PFN_vkCreateFence createFence{};
        PFN_vkDestroyFence destroyFence{};
        PFN_vkResetFences resetFences{};
        PFN_vkWaitForFences waitForFences{};
        PFN_vkQueueSubmit queueSubmit{};
        PFN_vkCreateQueryPool createQueryPool{};
        PFN_vkDestroyQueryPool destroyQueryPool{};
        PFN_vkCmdResetQueryPool cmdResetQueryPool{};
        PFN_vkCmdWriteTimestamp cmdWriteTimestamp{};
        PFN_vkGetQueryPoolResults getQueryPoolResults{};
    };

    struct Buffer {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkDeviceSize allocationSize{};
        VkDeviceSize size{};
        void* mapped{};
    };

    static bool readShader(const char* path, std::vector<std::uint8_t>& code) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;
        const auto end = file.tellg();
        if (end < 20 || end > 4 * 1024 * 1024 || (static_cast<std::uint64_t>(end) & 3u))
            return false;
        code.resize(static_cast<std::size_t>(end));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size())))
            return false;
        return loadLe32(code.data()) == 0x07230203u;
    }

    VkResult loadFunctions(PFN_vkGetDeviceProcAddr nextGdpa) {
#define ZVRAM_GDEFLATE_LOAD(member, name) \
        api_.member = reinterpret_cast<PFN_vk##name>(nextGdpa(device_, "vk" #name)); \
        if (!api_.member) return VK_ERROR_FEATURE_NOT_PRESENT
        ZVRAM_GDEFLATE_LOAD(createBuffer, CreateBuffer);
        ZVRAM_GDEFLATE_LOAD(destroyBuffer, DestroyBuffer);
        ZVRAM_GDEFLATE_LOAD(getBufferMemoryRequirements, GetBufferMemoryRequirements);
        api_.getBufferMemoryRequirements2 = reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(
            nextGdpa(device_, "vkGetBufferMemoryRequirements2"));
        if (!api_.getBufferMemoryRequirements2)
            api_.getBufferMemoryRequirements2 = reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(
                nextGdpa(device_, "vkGetBufferMemoryRequirements2KHR"));
        api_.getMemoryHostPointerProperties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
            nextGdpa(device_, "vkGetMemoryHostPointerPropertiesEXT"));
        ZVRAM_GDEFLATE_LOAD(allocateMemory, AllocateMemory);
        ZVRAM_GDEFLATE_LOAD(freeMemory, FreeMemory);
        ZVRAM_GDEFLATE_LOAD(bindBufferMemory, BindBufferMemory);
        ZVRAM_GDEFLATE_LOAD(mapMemory, MapMemory);
        ZVRAM_GDEFLATE_LOAD(unmapMemory, UnmapMemory);
        ZVRAM_GDEFLATE_LOAD(createDescriptorSetLayout, CreateDescriptorSetLayout);
        ZVRAM_GDEFLATE_LOAD(destroyDescriptorSetLayout, DestroyDescriptorSetLayout);
        ZVRAM_GDEFLATE_LOAD(createPipelineLayout, CreatePipelineLayout);
        ZVRAM_GDEFLATE_LOAD(destroyPipelineLayout, DestroyPipelineLayout);
        ZVRAM_GDEFLATE_LOAD(createShaderModule, CreateShaderModule);
        ZVRAM_GDEFLATE_LOAD(destroyShaderModule, DestroyShaderModule);
        ZVRAM_GDEFLATE_LOAD(createComputePipelines, CreateComputePipelines);
        ZVRAM_GDEFLATE_LOAD(destroyPipeline, DestroyPipeline);
        ZVRAM_GDEFLATE_LOAD(createDescriptorPool, CreateDescriptorPool);
        ZVRAM_GDEFLATE_LOAD(destroyDescriptorPool, DestroyDescriptorPool);
        ZVRAM_GDEFLATE_LOAD(allocateDescriptorSets, AllocateDescriptorSets);
        ZVRAM_GDEFLATE_LOAD(updateDescriptorSets, UpdateDescriptorSets);
        ZVRAM_GDEFLATE_LOAD(createCommandPool, CreateCommandPool);
        ZVRAM_GDEFLATE_LOAD(destroyCommandPool, DestroyCommandPool);
        ZVRAM_GDEFLATE_LOAD(resetCommandPool, ResetCommandPool);
        ZVRAM_GDEFLATE_LOAD(allocateCommandBuffers, AllocateCommandBuffers);
        ZVRAM_GDEFLATE_LOAD(beginCommandBuffer, BeginCommandBuffer);
        ZVRAM_GDEFLATE_LOAD(endCommandBuffer, EndCommandBuffer);
        ZVRAM_GDEFLATE_LOAD(cmdPipelineBarrier, CmdPipelineBarrier);
        ZVRAM_GDEFLATE_LOAD(cmdCopyBuffer, CmdCopyBuffer);
        ZVRAM_GDEFLATE_LOAD(cmdFillBuffer, CmdFillBuffer);
        ZVRAM_GDEFLATE_LOAD(cmdBindPipeline, CmdBindPipeline);
        ZVRAM_GDEFLATE_LOAD(cmdBindDescriptorSets, CmdBindDescriptorSets);
        ZVRAM_GDEFLATE_LOAD(cmdDispatch, CmdDispatch);
        ZVRAM_GDEFLATE_LOAD(createFence, CreateFence);
        ZVRAM_GDEFLATE_LOAD(destroyFence, DestroyFence);
        ZVRAM_GDEFLATE_LOAD(resetFences, ResetFences);
        ZVRAM_GDEFLATE_LOAD(waitForFences, WaitForFences);
        ZVRAM_GDEFLATE_LOAD(queueSubmit, QueueSubmit);
#undef ZVRAM_GDEFLATE_LOAD
        if (bp16ImportHostInput_ && (!api_.getBufferMemoryRequirements2 ||
                                    !api_.getMemoryHostPointerProperties))
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        return VK_SUCCESS;
    }

    bool loadTimestampFunctions(PFN_vkGetDeviceProcAddr nextGdpa) {
#define ZVRAM_GDEFLATE_LOAD_TIMESTAMP(member, name) \
        api_.member = reinterpret_cast<PFN_vk##name>(nextGdpa(device_, "vk" #name)); \
        if (!api_.member) return false
        ZVRAM_GDEFLATE_LOAD_TIMESTAMP(createQueryPool, CreateQueryPool);
        ZVRAM_GDEFLATE_LOAD_TIMESTAMP(destroyQueryPool, DestroyQueryPool);
        ZVRAM_GDEFLATE_LOAD_TIMESTAMP(cmdResetQueryPool, CmdResetQueryPool);
        ZVRAM_GDEFLATE_LOAD_TIMESTAMP(cmdWriteTimestamp, CmdWriteTimestamp);
        ZVRAM_GDEFLATE_LOAD_TIMESTAMP(getQueryPoolResults, GetQueryPoolResults);
#undef ZVRAM_GDEFLATE_LOAD_TIMESTAMP
        return true;
    }

    std::uint64_t timestampDelta(std::uint64_t begin, std::uint64_t end) const noexcept {
        const auto delta = end - begin;
        if (timestampValidBits_ >= 64) return delta;
        const auto mask = (std::uint64_t{1} << timestampValidBits_) - 1;
        return delta & mask;
    }

    std::uint64_t timestampNs(std::uint64_t ticks) const noexcept {
        const long double ns = static_cast<long double>(ticks) * timestampPeriod_;
        if (ns >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
            return std::numeric_limits<std::uint64_t>::max();
        return static_cast<std::uint64_t>(ns);
    }

    void disableGpuProfiling() noexcept {
        if (queryPool_) api_.destroyQueryPool(device_, queryPool_, nullptr);
        queryPool_ = VK_NULL_HANDLE;
        gpuProfileEnabled_ = false;
        timestampValidBits_ = 0;
    }

    VkBuffer shaderInputBuffer() const noexcept {
        return bp16HostInput_ ? upload_.buffer : input_.buffer;
    }

    VkResult createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags required, VkMemoryPropertyFlags forbidden,
                         Buffer& out, VkMemoryPropertyFlags preferred = 0) {
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size = size;
        create.usage = usage;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = api_.createBuffer(device_, &create, nullptr, &out.buffer);
        if (result != VK_SUCCESS) return result;
        VkMemoryRequirements requirements{};
        api_.getBufferMemoryRequirements(device_, out.buffer, &requirements);
        const auto type = selectMemoryTypeIndex(memory_, requirements.memoryTypeBits,
                                                required, forbidden, preferred);
        if (type == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        result = api_.allocateMemory(device_, &allocation, nullptr, &out.memory);
        if (result != VK_SUCCESS) return result;
        out.allocationSize = requirements.size;
        out.size = size;
        result = api_.bindBufferMemory(device_, out.buffer, out.memory, 0);
        if (result != VK_SUCCESS) return result;
        if (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            result = api_.mapMemory(device_, out.memory, 0, requirements.size, 0, &out.mapped);
        return result;
    }

    void destroyBuffer(Buffer& buffer) noexcept {
        if (buffer.mapped) api_.unmapMemory(device_, buffer.memory);
        if (buffer.buffer) api_.destroyBuffer(device_, buffer.buffer, nullptr);
        if (buffer.memory) api_.freeMemory(device_, buffer.memory, nullptr);
        buffer = {};
    }

    VkResult ensureInputBuffers(std::size_t bytes, VkBuffer output,
                                VkDeviceSize outputOffset, VkDeviceSize outputRange) {
        if (bytes <= inputCapacity_) {
            if (format_ == Format::BP16) {
                const VkResult result = updateInputDescriptor(static_cast<VkDeviceSize>(bytes));
                if (result != VK_SUCCESS) return result;
            }
            return updateOutputDescriptor(output, outputOffset, outputRange);
        }
        std::size_t capacity = format_ == Format::BP16 ? bytes : 4096;
        while (capacity < bytes) capacity *= 2;
        Buffer newUpload{}, newInput{};
        const auto uploadUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            (bp16HostInput_ ? VkBufferUsageFlags(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
                            : VkBufferUsageFlags(0));
        const auto uploadForbidden = bp16HostInput_
            ? VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            : VkMemoryPropertyFlags(0);
        VkResult result = createBuffer(static_cast<VkDeviceSize>(capacity), uploadUsage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            uploadForbidden, newUpload,
            bp16CachedUploadPreference_ ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
        if (result == VK_SUCCESS && !bp16HostInput_) result = createBuffer(static_cast<VkDeviceSize>(capacity),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, newInput);
        if (result != VK_SUCCESS) {
            destroyBuffer(newUpload);
            destroyBuffer(newInput);
            return result;
        }
        const Buffer oldUpload = upload_;
        const Buffer oldInput = input_;
        upload_ = newUpload;
        input_ = newInput;
        inputCapacity_ = capacity;
        const auto inputRange = format_ == Format::BP16 ? static_cast<VkDeviceSize>(bytes)
                                                        : static_cast<VkDeviceSize>(inputCapacity_);
        result = updateDescriptors(output, outputOffset, inputRange, outputRange);
        if (result != VK_SUCCESS) {
            destroyBuffer(upload_);
            destroyBuffer(input_);
            upload_ = oldUpload;
            input_ = oldInput;
            inputCapacity_ = oldInput.size ? static_cast<std::size_t>(oldInput.size) : 4;
            return result;
        }
        auto oldUploadCopy = oldUpload;
        auto oldInputCopy = oldInput;
        destroyBuffer(oldUploadCopy);
        destroyBuffer(oldInputCopy);
        return VK_SUCCESS;
    }

    VkResult createPipeline(const std::vector<std::uint8_t>& code, Format format) {
        VkDescriptorSetLayoutBinding bindings[4]{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 4;
        setInfo.pBindings = bindings;
        VkResult result = api_.createDescriptorSetLayout(device_, &setInfo, nullptr, &setLayout_);
        if (result != VK_SUCCESS) return result;
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout_;
        result = api_.createPipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_);
        if (result != VK_SUCCESS) return result;
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = code.size();
        shaderInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
        result = api_.createShaderModule(device_, &shaderInfo, nullptr, &shader_);
        if (result != VK_SUCCESS) return result;
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        if (format == Format::GDeflate) {
            subgroup.requiredSubgroupSize = 32;
            pipelineInfo.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
            pipelineInfo.stage.pNext = &subgroup;
        }
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader_;
        pipelineInfo.stage.pName = "CSMain";
        pipelineInfo.layout = pipelineLayout_;
        return api_.createComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    }

    VkResult createBP16Encoder(const char* analyzePath, const char* packPath) {
        if (format_ != Format::BP16 || !bp16AllocatedHostInput_ || !analyzePath || !packPath ||
            !api_.getBufferMemoryRequirements2 || !maxStorageBufferRange_ || !maxDispatchGroupsX_)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        const auto metadataBytes = (MaxRawBytes / bp16::RawBytesPerBlock) * sizeof(std::uint32_t);
        if (metadataBytes > maxStorageBufferRange_) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkResult result = createBuffer(metadataBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, encodeMetadata_);
        if (result != VK_SUCCESS) return result;

        std::vector<std::uint8_t> analyzeCode, packCode;
        try {
            if (!readShader(analyzePath, analyzeCode) || !readShader(packPath, packCode))
                return VK_ERROR_INITIALIZATION_FAILED;
        } catch (const std::bad_alloc&) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        result = createShaderAndPipeline(analyzeCode, "AnalyzeMain",
                                         encodeAnalyzeShader_, encodeAnalyzePipeline_);
        if (result != VK_SUCCESS) return result;
        result = createShaderAndPipeline(packCode, "PackMain",
                                         encodePackShader_, encodePackPipeline_);
        if (result != VK_SUCCESS) return result;
        bp16EncoderRequested_ = true;
        return VK_SUCCESS;
    }

    VkResult createShaderAndPipeline(const std::vector<std::uint8_t>& code, const char* entry,
                                     VkShaderModule& shader, VkPipeline& pipeline) {
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = code.size();
        shaderInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
        VkResult result = api_.createShaderModule(device_, &shaderInfo, nullptr, &shader);
        if (result != VK_SUCCESS) return result;
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader;
        pipelineInfo.stage.pName = entry;
        pipelineInfo.layout = pipelineLayout_;
        return api_.createComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    }

    void cleanupBP16Encoder() noexcept {
        if (!device_) return;
        if (encodeAnalyzePipeline_) api_.destroyPipeline(device_, encodeAnalyzePipeline_, nullptr);
        if (encodePackPipeline_) api_.destroyPipeline(device_, encodePackPipeline_, nullptr);
        if (encodeAnalyzeShader_) api_.destroyShaderModule(device_, encodeAnalyzeShader_, nullptr);
        if (encodePackShader_) api_.destroyShaderModule(device_, encodePackShader_, nullptr);
        destroyBuffer(encodeMetadata_);
        encodeAnalyzePipeline_ = VK_NULL_HANDLE;
        encodePackPipeline_ = VK_NULL_HANDLE;
        encodeAnalyzeShader_ = VK_NULL_HANDLE;
        encodePackShader_ = VK_NULL_HANDLE;
        encodeDescriptorSet_ = VK_NULL_HANDLE;
        bp16EncoderRequested_ = false;
        bp16EncoderEnabled_ = false;
    }

    VkResult updateEncoderDescriptors(VkBuffer raw, VkDeviceSize rawOffset,
                                     VkDeviceSize rawBytes, VkBuffer frame,
                                     VkDeviceSize frameBytes, bool analyze) {
        VkDescriptorBufferInfo infos[4]{};
        infos[0] = {raw, rawOffset, rawBytes};
        infos[1] = {encodeMetadata_.buffer, 0,
            static_cast<VkDeviceSize>((rawBytes / bp16::RawBytesPerBlock) * sizeof(std::uint32_t))};
        infos[2] = {analyze ? control_.buffer : frame, 0,
                    analyze ? VkDeviceSize(12) : frameBytes};
        infos[3] = {scratch_.buffer, 0, 4};
        VkWriteDescriptorSet writes[4]{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = encodeDescriptorSet_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        api_.updateDescriptorSets(device_, 4, writes, 0, nullptr);
        return VK_SUCCESS;
    }

    VkResult recordAndSubmitBP16Encode(VkBuffer raw, VkDeviceSize rawOffset,
                                       VkDeviceSize rawBytes, VkBuffer frame,
                                       VkDeviceSize frameBytes, std::uint32_t groups,
                                       bool analyze) {
        VkResult result = checked(api_.resetCommandPool(device_, commandPool_, 0));
        if (result != VK_SUCCESS) return result;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = checked(api_.beginCommandBuffer(commandBuffer_, &begin));
        if (result != VK_SUCCESS) return result;

        VkBufferMemoryBarrier rawReady{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        rawReady.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        rawReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        rawReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rawReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rawReady.buffer = raw;
        rawReady.offset = rawOffset;
        rawReady.size = rawBytes;
        if (analyze) {
            api_.cmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &rawReady, 0, nullptr);
        } else {
            VkBufferMemoryBarrier hostPrefix{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            hostPrefix.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            hostPrefix.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            hostPrefix.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostPrefix.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostPrefix.buffer = frame;
            hostPrefix.offset = 0;
            hostPrefix.size = frameBytes;
            VkBufferMemoryBarrier metadataReady{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            metadataReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
            metadataReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            metadataReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            metadataReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            metadataReady.buffer = encodeMetadata_.buffer;
            metadataReady.offset = 0;
            metadataReady.size = static_cast<VkDeviceSize>(
                (rawBytes / bp16::RawBytesPerBlock) * sizeof(std::uint32_t));
            VkBufferMemoryBarrier barriers[3]{rawReady, hostPrefix, metadataReady};
            api_.cmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT |
                VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 3, barriers, 0, nullptr);
        }
        api_.cmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
            analyze ? encodeAnalyzePipeline_ : encodePackPipeline_);
        api_.cmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipelineLayout_, 0, 1, &encodeDescriptorSet_, 0, nullptr);
        api_.cmdDispatch(commandBuffer_, groups, 1, 1);
        if (analyze) {
            VkBufferMemoryBarrier metadataHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            metadataHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            metadataHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            metadataHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            metadataHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            metadataHost.buffer = encodeMetadata_.buffer;
            metadataHost.offset = 0;
            metadataHost.size = static_cast<VkDeviceSize>(
                (rawBytes / bp16::RawBytesPerBlock) * sizeof(std::uint32_t));
            api_.cmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &metadataHost, 0, nullptr);
        } else {
            VkBufferMemoryBarrier frameHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            frameHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            frameHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            frameHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            frameHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            frameHost.buffer = frame;
            frameHost.offset = 0;
            frameHost.size = frameBytes;
            api_.cmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &frameHost, 0, nullptr);
        }
        result = checked(api_.endCommandBuffer(commandBuffer_));
        if (result != VK_SUCCESS) return result;
        result = checked(api_.resetFences(device_, 1, &fence_));
        if (result != VK_SUCCESS) return result;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        result = api_.queueSubmit(queue_, 1, &submit, fence_);
        if (result != VK_SUCCESS) {
            markPoisoned();
            return result;
        }
        result = api_.waitForFences(device_, 1, &fence_, VK_TRUE, WaitNanoseconds);
        if (result != VK_SUCCESS) {
            markPoisoned();
            return result;
        }
        return VK_SUCCESS;
    }

    VkResult createDescriptors() {
        const auto descriptorCount = bp16EncoderRequested_ ? 8u : 4u;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptorCount};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = bp16EncoderRequested_ ? 2u : 1u;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        VkResult result = api_.createDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_);
        if (result != VK_SUCCESS && bp16EncoderRequested_ && result != VK_ERROR_DEVICE_LOST) {
            cleanupBP16Encoder();
            size.descriptorCount = 4;
            poolInfo.maxSets = 1;
            result = api_.createDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_);
        }
        if (result != VK_SUCCESS) return result;
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool_;
        VkDescriptorSetLayout layouts[2]{setLayout_, setLayout_};
        VkDescriptorSet sets[2]{};
        allocate.descriptorSetCount = bp16EncoderRequested_ ? 2u : 1u;
        allocate.pSetLayouts = layouts;
        result = api_.allocateDescriptorSets(device_, &allocate, sets);
        if (result != VK_SUCCESS && bp16EncoderRequested_ && result != VK_ERROR_DEVICE_LOST) {
            api_.destroyDescriptorPool(device_, descriptorPool_, nullptr);
            descriptorPool_ = VK_NULL_HANDLE;
            cleanupBP16Encoder();
            size.descriptorCount = 4;
            poolInfo.maxSets = 1;
            result = api_.createDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_);
            if (result != VK_SUCCESS) return result;
            allocate.descriptorPool = descriptorPool_;
            allocate.descriptorSetCount = 1;
            result = api_.allocateDescriptorSets(device_, &allocate, sets);
        }
        if (result == VK_SUCCESS) {
            descriptorSet_ = sets[0];
            if (bp16EncoderRequested_) encodeDescriptorSet_ = sets[1];
        }
        return result;
    }

    VkResult updateDescriptors(VkBuffer output, VkDeviceSize outputOffset,
                               VkDeviceSize inputRange, VkDeviceSize outputRange,
                               VkBuffer suppliedInput = VK_NULL_HANDLE) {
        VkDescriptorBufferInfo infos[4]{};
        infos[0] = {suppliedInput ? suppliedInput : shaderInputBuffer(), 0, inputRange};
        infos[1] = {control_.buffer, 0, 12};
        infos[2] = {output, outputOffset, outputRange};
        infos[3] = {scratch_.buffer, 0, 4};
        VkWriteDescriptorSet writes[4]{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descriptorSet_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        api_.updateDescriptorSets(device_, 4, writes, 0, nullptr);
        return VK_SUCCESS;
    }

    VkResult updateInputDescriptor(VkDeviceSize inputRange) {
        VkDescriptorBufferInfo info{shaderInputBuffer(), 0, inputRange};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSet_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        api_.updateDescriptorSets(device_, 1, &write, 0, nullptr);
        return VK_SUCCESS;
    }

    VkResult updateOutputDescriptor(VkBuffer output, VkDeviceSize outputOffset,
                                    VkDeviceSize outputRange) {
        VkDescriptorBufferInfo info{output, outputOffset, outputRange};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSet_;
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &info;
        api_.updateDescriptorSets(device_, 1, &write, 0, nullptr);
        return VK_SUCCESS;
    }

    VkResult createCommands() {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family_;
        VkResult result = api_.createCommandPool(device_, &pool, nullptr, &commandPool_);
        if (result != VK_SUCCESS) return result;
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commandPool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        result = api_.allocateCommandBuffers(device_, &allocate, &commandBuffer_);
        if (result != VK_SUCCESS) return result;
        return setLoaderData_(device_, commandBuffer_);
    }

    VkResult createFence() {
        VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        return api_.createFence(device_, &create, nullptr, &fence_);
    }

    VkResult checked(VkResult result) noexcept {
        if (result == VK_ERROR_DEVICE_LOST) markPoisoned();
        return result;
    }

    void markPoisoned() noexcept {
        poisoned_ = true;
        if (poisonState_) poisonState_->store(true, std::memory_order_release);
    }

    void cleanup() noexcept {
        if (!device_) return;
        if (queryPool_) api_.destroyQueryPool(device_, queryPool_, nullptr);
        if (fence_) api_.destroyFence(device_, fence_, nullptr);
        if (commandPool_) api_.destroyCommandPool(device_, commandPool_, nullptr);
        cleanupBP16Encoder();
        if (descriptorPool_) api_.destroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (pipeline_) api_.destroyPipeline(device_, pipeline_, nullptr);
        if (shader_) api_.destroyShaderModule(device_, shader_, nullptr);
        if (pipelineLayout_) api_.destroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (setLayout_) api_.destroyDescriptorSetLayout(device_, setLayout_, nullptr);
        destroyBuffer(errorReadback_);
        destroyBuffer(scratch_);
        destroyBuffer(control_);
        destroyBuffer(input_);
        destroyBuffer(upload_);
        api_ = {};
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        setLoaderData_ = nullptr;
        inputCapacity_ = 0;
        poisoned_ = false;
        initialized_ = false;
        fence_ = VK_NULL_HANDLE;
        commandPool_ = VK_NULL_HANDLE;
        commandBuffer_ = VK_NULL_HANDLE;
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSet_ = VK_NULL_HANDLE;
        pipeline_ = VK_NULL_HANDLE;
        shader_ = VK_NULL_HANDLE;
        pipelineLayout_ = VK_NULL_HANDLE;
        setLayout_ = VK_NULL_HANDLE;
        format_ = Format::GDeflate;
        maxDispatchGroupsX_ = 0;
        maxStorageBufferRange_ = 0;
        minStorageBufferOffsetAlignment_ = 0;
        bp16HostInput_ = false;
        bp16CachedUploadPreference_ = false;
        bp16ImportHostInput_ = false;
        bp16AllocatedHostInput_ = false;
        bp16EncoderRequested_ = false;
        bp16EncoderEnabled_ = false;
        importHostAlignment_ = 0;
        if (!poisoned_) {
            poisonState_.reset();
            allocatedHostBudget_.reset();
            localOwnerBudget_.reset();
        }
        queryPool_ = VK_NULL_HANDLE;
        gpuProfileEnabled_ = false;
        timestampValidBits_ = 0;
        timestampPeriod_ = 0.0;
    }

    Functions api_{};
    VkDevice device_{};
    VkPhysicalDeviceMemoryProperties memory_{};
    VkQueue queue_{};
    std::uint32_t family_{};
    PFN_vkSetDeviceLoaderData setLoaderData_{};
    Format format_{Format::GDeflate};
    std::uint32_t maxDispatchGroupsX_{};
    VkDeviceSize maxStorageBufferRange_{};
    VkDeviceSize minStorageBufferOffsetAlignment_{};
    Buffer upload_{}, input_{}, control_{}, scratch_{}, errorReadback_{};
    Buffer encodeMetadata_{};
    std::size_t inputCapacity_{};
    VkDescriptorSetLayout setLayout_{};
    VkPipelineLayout pipelineLayout_{};
    VkShaderModule shader_{};
    VkPipeline pipeline_{};
    VkDescriptorPool descriptorPool_{};
    VkDescriptorSet descriptorSet_{};
    VkDescriptorSet encodeDescriptorSet_{};
    VkShaderModule encodeAnalyzeShader_{};
    VkShaderModule encodePackShader_{};
    VkPipeline encodeAnalyzePipeline_{};
    VkPipeline encodePackPipeline_{};
    VkCommandPool commandPool_{};
    VkCommandBuffer commandBuffer_{};
    VkFence fence_{};
    VkQueryPool queryPool_{};
    bool initialized_{};
    bool poisoned_{};
    bool profileEnabled_{};
    bool gpuProfileEnabled_{};
    bool bp16HostInput_{};
    bool bp16CachedUploadPreference_{};
    bool bp16ImportHostInput_{};
    bool bp16AllocatedHostInput_{};
    bool bp16EncoderRequested_{};
    bool bp16EncoderEnabled_{};
    unsigned bp16UploadWorkers_{1};
    VkDeviceSize importHostAlignment_{};
    std::shared_ptr<std::atomic<bool>> poisonState_;
    std::shared_ptr<AllocatedHostBudget> allocatedHostBudget_;
    std::shared_ptr<AllocatedHostBudget> localOwnerBudget_;
    std::uint32_t localOwnerHeapIndex_{UINT32_MAX};
    std::uint32_t timestampValidBits_{};
    double timestampPeriod_{};
    Profile profile_{};
};

} // namespace zvram::gdeflate::gpu
