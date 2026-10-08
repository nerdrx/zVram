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
#include <vector>

namespace zvram::gdeflate::gpu {

enum class Format { GDeflate, BP16 };

class Decoder {
public:
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

    struct ImportedHostInput {
        ImportedHostInput() = default;
        ImportedHostInput(const ImportedHostInput&) = delete;
        ImportedHostInput& operator=(const ImportedHostInput&) = delete;
        VkDevice device{};
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        void* allocation{};
        std::size_t encodedBytes{};
        VkDeviceSize allocationBytes{};
        PFN_vkDestroyBuffer destroyBuffer{};
        PFN_vkFreeMemory freeMemory{};
        std::shared_ptr<std::atomic<bool>> poisoned;

        ~ImportedHostInput() {
            if (poisoned && poisoned->load(std::memory_order_acquire)) return;
            if (device && buffer && destroyBuffer) destroyBuffer(device, buffer, nullptr);
            if (device && memory && freeMemory) freeMemory(device, memory, nullptr);
            std::free(allocation);
        }
        const std::uint8_t* data() const noexcept {
            return static_cast<const std::uint8_t*>(allocation);
        }
    };
    using ImportedHostInputPtr = std::shared_ptr<ImportedHostInput>;

    struct Profile {
        std::uint64_t calls{};
        std::uint64_t validationNs{};
        std::uint64_t inputPrepareNs{};
        std::uint64_t submitWaitNs{};
        std::uint64_t gpuTransferNs{};
        std::uint64_t gpuDecodeNs{};
        std::uint64_t gpuFinishNs{};
        std::uint64_t gpuSamples{};
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
    bool importedHostInputEnabled() const noexcept { return bp16ImportHostInput_; }
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
                        VkDeviceSize importHostAlignment = 0) {
        const char* profileEnv = std::getenv("ZVRAM_VULKAN_GPU_PROFILE");
        profileEnabled_ = profileEnv && std::strcmp(profileEnv, "1") == 0;
        profile_ = {};
        if (initialized_ || poisoned_ || !device || !nextGdpa || !queue || !setLoaderData || !shaderPath)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (format != Format::GDeflate && format != Format::BP16)
            return VK_ERROR_VALIDATION_FAILED_EXT;
        bp16ImportHostInput_ = format == Format::BP16 && importHostInput;
        const char* hostInputEnv = std::getenv("ZVRAM_VULKAN_BP16_HOST_INPUT");
        bp16HostInput_ = format == Format::BP16 && !bp16ImportHostInput_ && hostInputEnv &&
                         std::strcmp(hostInputEnv, "1") == 0;
        if (bp16ImportHostInput_ && (!importHostAlignment || importHostAlignment > 65536 ||
            (importHostAlignment & (importHostAlignment - 1))))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        importHostAlignment_ = importHostAlignment;
        if (bp16ImportHostInput_) {
            try { poisonState_ = std::make_shared<std::atomic<bool>>(false); }
            catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
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
            uploadForbidden, upload_);
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
        return VK_SUCCESS;
    }

    VkResult importHostInput(const std::uint8_t* encoded, std::size_t encodedSize,
                             ImportedHostInputPtr& out) {
        out.reset();
        if (!initialized_ || poisoned_ || !bp16ImportHostInput_ || !encoded || !encodedSize ||
            encodedSize > MaxBP16InputBytes) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (!bp16::inspect(encoded, encodedSize)) return VK_ERROR_VALIDATION_FAILED_EXT;
        std::size_t hostBytes{};
        if (!importedHostAllocationSize(encodedSize, importHostAlignment_, hostBytes))
            return VK_ERROR_VALIDATION_FAILED_EXT;
        void* allocation{};
        if (posix_memalign(&allocation, static_cast<std::size_t>(importHostAlignment_), hostBytes) != 0)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        std::memcpy(allocation, encoded, encodedSize);
        std::memset(static_cast<std::uint8_t*>(allocation) + encodedSize, 0, hostBytes - encodedSize);
        ImportedHostInputPtr owner;
        try { owner = std::make_shared<ImportedHostInput>(); }
        catch (const std::bad_alloc&) { std::free(allocation); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        owner->device = device_; owner->allocation = allocation;
        owner->encodedBytes = encodedSize; owner->allocationBytes = hostBytes;
        owner->destroyBuffer = api_.destroyBuffer; owner->freeMemory = api_.freeMemory;
        owner->poisoned = poisonState_;
        VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.pNext = &external; create.size = hostBytes; create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkResult result = api_.createBuffer(device_, &create, nullptr, &owner->buffer);
        if (result != VK_SUCCESS) return checked(result);
        VkBufferMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
        reqInfo.buffer = owner->buffer;
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated;
        api_.getBufferMemoryRequirements2(device_, &reqInfo, &requirements);
        if (dedicated.requiresDedicatedAllocation) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkMemoryHostPointerPropertiesEXT pointerProperties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        result = api_.getMemoryHostPointerProperties(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                      allocation, &pointerProperties);
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
        import.pHostPointer = allocation;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &import; allocate.allocationSize = hostBytes;
        allocate.memoryTypeIndex = type;
        result = api_.allocateMemory(device_, &allocate, nullptr, &owner->memory);
        if (result != VK_SUCCESS) return checked(result);
        result = api_.bindBufferMemory(device_, owner->buffer, owner->memory, 0);
        if (result != VK_SUCCESS) return checked(result);
        out = std::move(owner);
        return VK_SUCCESS;
    }

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
            if (!bp16ImportHostInput_ || imported->device != device_ ||
                imported->buffer == VK_NULL_HANDLE || imported->memory == VK_NULL_HANDLE ||
                imported->poisoned != poisonState_ ||
                imported->encodedBytes != encodedSize || imported->data() != encoded)
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
            if (!bp16::validate(encoded, encodedSize, static_cast<std::uint32_t>(rawSize)))
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
        if (imported && inputBytes > imported->allocationBytes)
            return finishValidation(VK_ERROR_VALIDATION_FAILED_EXT);
        (void)finishValidation(VK_SUCCESS);

        const auto inputPrepareStarted = profileEnabled_ ? Clock::now() : Clock::time_point{};
        VkResult result = imported
            ? updateDescriptors(output, offset, static_cast<VkDeviceSize>(inputBytes),
                                static_cast<VkDeviceSize>(paddedRaw), imported->buffer)
            : checked(ensureInputBuffers(inputBytes, output, offset,
                                         static_cast<VkDeviceSize>(paddedRaw)));
        if (result != VK_SUCCESS) {
            if (profileEnabled_)
                profile_.inputPrepareNs += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - inputPrepareStarted).count());
            return result;
        }

        if (!imported) {
            auto* upload = static_cast<std::uint8_t*>(upload_.mapped);
            std::memcpy(upload, encoded, encodedSize);
            std::memset(upload + encodedSize, 0, inputBytes - encodedSize);
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
        if (result != VK_SUCCESS) {
            // Submission failure leaves completion uncertain. Retain private
            // resources and any imported owners instead of overwriting in flight.
            markPoisoned();
            if (profileEnabled_)
                profile_.submitWaitNs += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - submitWaitStarted).count());
            return result;
        }
        result = api_.waitForFences(device_, 1, &fence_, VK_TRUE, WaitNanoseconds);
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
        if (gpuProfileEnabled_) {
            std::uint64_t timestamps[4]{};
            result = api_.getQueryPoolResults(device_, queryPool_, 0, 4, sizeof(timestamps),
                                               timestamps, sizeof(std::uint64_t),
                                               VK_QUERY_RESULT_64_BIT);
            if (result == VK_ERROR_DEVICE_LOST) return checked(result);
            if (result == VK_SUCCESS) {
                profile_.gpuTransferNs += timestampNs(timestampDelta(timestamps[0], timestamps[1]));
                profile_.gpuDecodeNs += timestampNs(timestampDelta(timestamps[1], timestamps[2]));
                profile_.gpuFinishNs += timestampNs(timestampDelta(timestamps[2], timestamps[3]));
                ++profile_.gpuSamples;
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
                         Buffer& out) {
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size = size;
        create.usage = usage;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = api_.createBuffer(device_, &create, nullptr, &out.buffer);
        if (result != VK_SUCCESS) return result;
        VkMemoryRequirements requirements{};
        api_.getBufferMemoryRequirements(device_, out.buffer, &requirements);
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
            if (!(requirements.memoryTypeBits & (1u << i))) continue;
            const auto flags = memory_.memoryTypes[i].propertyFlags;
            if ((flags & required) == required && !(flags & forbidden)) { type = i; break; }
        }
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
            uploadForbidden, newUpload);
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

    VkResult createDescriptors() {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        VkResult result = api_.createDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_);
        if (result != VK_SUCCESS) return result;
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &setLayout_;
        return api_.allocateDescriptorSets(device_, &allocate, &descriptorSet_);
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
        bp16HostInput_ = false;
        bp16ImportHostInput_ = false;
        importHostAlignment_ = 0;
        if (!poisoned_) poisonState_.reset();
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
    Buffer upload_{}, input_{}, control_{}, scratch_{}, errorReadback_{};
    std::size_t inputCapacity_{};
    VkDescriptorSetLayout setLayout_{};
    VkPipelineLayout pipelineLayout_{};
    VkShaderModule shader_{};
    VkPipeline pipeline_{};
    VkDescriptorPool descriptorPool_{};
    VkDescriptorSet descriptorSet_{};
    VkCommandPool commandPool_{};
    VkCommandBuffer commandBuffer_{};
    VkFence fence_{};
    VkQueryPool queryPool_{};
    bool initialized_{};
    bool poisoned_{};
    bool profileEnabled_{};
    bool gpuProfileEnabled_{};
    bool bp16HostInput_{};
    bool bp16ImportHostInput_{};
    VkDeviceSize importHostAlignment_{};
    std::shared_ptr<std::atomic<bool>> poisonState_;
    std::uint32_t timestampValidBits_{};
    double timestampPeriod_{};
    Profile profile_{};
};

} // namespace zvram::gdeflate::gpu
