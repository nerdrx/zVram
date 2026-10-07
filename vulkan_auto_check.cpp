#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr VkDeviceSize MiB = 1024ull * 1024ull;
constexpr VkDeviceSize TotalBytes = 320 * MiB;
constexpr VkDeviceSize ChunkBytes = 32 * MiB;
constexpr std::uint32_t ChunkWords = static_cast<std::uint32_t>(ChunkBytes / 4);
constexpr std::uint32_t TotalWords = static_cast<std::uint32_t>(TotalBytes / 4);
constexpr auto ColdTimeout = std::chrono::seconds(45);

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::uint32_t mix32(std::uint32_t value) {
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
std::uint32_t initialWord(std::uint32_t index) {
    return (index & 31u) == 0 ? mix32(index ^ 0x7a565241u) : 0xa5a5a5a5u;
}
std::uint32_t cycleSalt(std::uint32_t cycle, std::uint32_t chunk) {
    return 0x85ebca6bu * (cycle + 1) ^ (chunk + 17u);
}

struct ZvramSnapshotStatsNX {
    std::uint32_t structSize{};
    std::uint32_t version{};
    std::uint64_t coldLogicalBytes{};
    std::uint64_t coldStoredBytes{};
    std::uint64_t coldBudgetBytes{};
    std::uint64_t residentBytes{};
    std::uint64_t freezes{};
    std::uint64_t restores{};
    std::uint64_t failures{};
    std::int32_t lastError{};
};
using GetSnapshotStats = VkResult (VKAPI_PTR *)(VkDevice, ZvramSnapshotStatsNX*);

struct Buffer {
    VkDevice device{};
    VkBuffer handle{};
    VkDeviceMemory memory{};
    ~Buffer() {
        if (handle) vkDestroyBuffer(device, handle, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
};
struct Context {
    VkInstance instance{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool commands{};
    VkDescriptorSetLayout descriptorLayout{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSet descriptor{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};
    GetSnapshotStats getStats{};
    VkPhysicalDevice physical{};
    std::uint32_t family{};
    std::uint32_t virtualType{UINT32_MAX};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceProperties properties{};

    ~Context() {
        if (device) vkDeviceWaitIdle(device);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        if (commands) vkDestroyCommandPool(device, commands, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    void initialize() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        check(vkCreateInstance(&ici, nullptr, &instance), "create instance");

        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate physical devices");
        require(count != 0, "no Vulkan physical devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate physical devices");
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.vendorID != 0x1002 || props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
                std::strstr(props.deviceName, "RADV") == nullptr) continue;
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            if (!features.sparseBinding) continue;
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                const auto needed = VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT;
                if (families[i].queueCount && (families[i].queueFlags & needed) == needed) {
                    physical = candidate; family = i; properties = props; break;
                }
            }
            if (physical) break;
        }
        require(physical != VK_NULL_HANDLE,
                "no discrete AMD RADV GPU has one sparse, transfer, and compute queue family");
        std::cout << "GPU: " << properties.deviceName << '\n';
        require(properties.limits.maxStorageBufferRange >= ChunkBytes &&
                properties.limits.minStorageBufferOffsetAlignment != 0 &&
                ChunkBytes % properties.limits.minStorageBufferOffsetAlignment == 0 &&
                properties.limits.maxComputeWorkGroupInvocations >= 256 &&
                properties.limits.maxComputeWorkGroupSize[0] >= 256 &&
                properties.limits.maxComputeWorkGroupCount[0] >= ChunkWords / 256,
                "device limits do not support 32 MiB storage chunks and compute dispatch");

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabled{}; enabled.sparseBinding = VK_TRUE;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.pEnabledFeatures = &enabled;
        check(vkCreateDevice(physical, &dci, nullptr, &device), "create device");
        vkGetDeviceQueue(device, family, 0, &queue);

        getStats = reinterpret_cast<GetSnapshotStats>(vkGetDeviceProcAddr(device, "vkZVramGetSnapshotStatsNX"));
        require(getStats != nullptr, "automatic Vulkan snapshot stats interface unavailable");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        require(memory.memoryTypeCount != 0 && memory.memoryHeapCount != 0,
                "no Vulkan memory types or heaps");
        virtualType = memory.memoryTypeCount - 1;
        require(memory.memoryTypes[virtualType].heapIndex == memory.memoryHeapCount - 1 &&
                memory.memoryTypes[virtualType].propertyFlags == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                "last exposed memory type is not zVram virtual device-local type");

        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.queueFamilyIndex = family; cp.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        check(vkCreateCommandPool(device, &cp, nullptr, &commands), "create command pool");
        createComputePipeline();
    }

    void createComputePipeline() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0; binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1; binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 1; dl.pBindings = &binding;
        check(vkCreateDescriptorSetLayout(device, &dl, nullptr, &descriptorLayout), "create descriptor layout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1; dp.poolSizeCount = 1; dp.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(device, &dp, nullptr, &descriptorPool), "create descriptor pool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptorPool; da.descriptorSetCount = 1; da.pSetLayouts = &descriptorLayout;
        check(vkAllocateDescriptorSets(device, &da, &descriptor), "allocate descriptor set");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &descriptorLayout;
        pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &pl, nullptr, &pipelineLayout), "create pipeline layout");

        std::ifstream file(ZVRAM_CHECK_SHADER_PATH, std::ios::binary | std::ios::ate);
        require(bool(file), "cannot open compute shader");
        const auto length = file.tellg();
        require(length > 0 && static_cast<std::size_t>(length) % 4 == 0, "invalid compute shader size");
        std::vector<std::uint32_t> code(static_cast<std::size_t>(length) / 4);
        file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), length);
        require(bool(file), "cannot read compute shader");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = code.size() * sizeof(code[0]); sm.pCode = code.data();
        VkShaderModule module{};
        check(vkCreateShaderModule(device, &sm, nullptr, &module), "create shader module");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.layout = pipelineLayout;
        pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pi.stage.module = module; pi.stage.pName = "main";
        const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        check(result, "create compute pipeline");
    }

    ZvramSnapshotStatsNX stats() const {
        ZvramSnapshotStatsNX result{}; result.structSize = sizeof(result); result.version = 1;
        check(getStats(device, &result), "query snapshot stats");
        require(result.version == 1 && result.structSize == sizeof(result), "snapshot stats ABI mismatch");
        return result;
    }

    template<class Record>
    void submit(Record&& record) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = commands; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer command{};
        check(vkAllocateCommandBuffers(device, &ai, &command), "allocate command buffer");
        try {
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(command, &bi), "begin command buffer");
            record(command);
            check(vkEndCommandBuffer(command), "end command buffer");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1; si.pCommandBuffers = &command;
            check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "submit command buffer");
            check(vkQueueWaitIdle(queue), "wait for queue");
        } catch (...) {
            vkFreeCommandBuffers(device, commands, 1, &command);
            throw;
        }
        vkFreeCommandBuffers(device, commands, 1, &command);
    }
};

struct Staging {
    VkDevice device{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
    ~Staging() {
        if (mapped) vkUnmapMemory(device, memory);
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
};

std::uint32_t hostCoherentType(const Context& context, std::uint32_t bits) {
    std::uint32_t selected = UINT32_MAX;
    int bestRank = 5;
    for (std::uint32_t i = 0; i < context.memory.memoryTypeCount; ++i) {
        const auto flags = context.memory.memoryTypes[i].propertyFlags;
        if (i == context.virtualType || !(bits & (1u << i)) ||
            (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
        const bool cached = (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
        const bool local = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        const int rank = cached ? (local ? 1 : 0) : (local ? 3 : 2);
        if (rank < bestRank) { selected = i; bestRank = rank; }
    }
    return selected;
}

void waitCold(const Context& context, const ZvramSnapshotStatsNX& before) {
    const auto deadline = std::chrono::steady_clock::now() + ColdTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto current = context.stats();
        if (current.coldLogicalBytes >= TotalBytes && current.coldStoredBytes > 0 &&
            current.freezes > before.freezes) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto final = context.stats();
    throw std::runtime_error("buffer did not reach cold snapshot before timeout: cold=" +
        std::to_string(final.coldLogicalBytes) + " stored=" + std::to_string(final.coldStoredBytes) +
        " freezes=" + std::to_string(final.freezes) + " failures=" + std::to_string(final.failures) +
        " lastError=" + std::to_string(final.lastError));
}

void metadataWhileCold(Context& context, VkBuffer buffer,
                       const ZvramSnapshotStatsNX& cold) {
    for (int i = 0; i < 3; ++i) {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(context.device, buffer, &requirements);
        require(requirements.size >= TotalBytes, "buffer requirements changed while cold");
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(context.physical, &memory);
        require(memory.memoryTypeCount > context.virtualType, "virtual memory type disappeared while cold");
        const auto current = context.stats();
        require(current.restores == cold.restores && current.coldLogicalBytes >= TotalBytes,
                "metadata query woke the cold allocation");
    }
}

void upload(Context& context, VkBuffer buffer, Staging& staging) {
    auto* words = static_cast<std::uint32_t*>(staging.mapped);
    for (std::uint32_t chunk = 0; chunk < TotalBytes / ChunkBytes; ++chunk) {
        const std::uint32_t base = chunk * ChunkWords;
        for (std::uint32_t i = 0; i < ChunkWords; ++i) words[i] = initialWord(base + i);
        context.submit([&](VkCommandBuffer command) {
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
            VkBufferCopy copy{0, static_cast<VkDeviceSize>(base) * 4, ChunkBytes};
            vkCmdCopyBuffer(command, staging.buffer, buffer, 1, &copy);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
        });
    }
}

void computeCycle(Context& context, VkBuffer buffer, std::uint32_t cycle) {
    for (std::uint32_t chunk = 0; chunk < TotalBytes / ChunkBytes; ++chunk) {
        const VkDeviceSize offset = static_cast<VkDeviceSize>(chunk) * ChunkBytes;
        VkDescriptorBufferInfo info{buffer, offset, ChunkBytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = context.descriptor; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &info;
        vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
        const std::uint32_t push[3]{ChunkWords, cycleSalt(cycle, chunk), 1};
        context.submit([&](VkCommandBuffer command) {
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipelineLayout,
                                    0, 1, &context.descriptor, 0, nullptr);
            vkCmdPushConstants(command, context.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), push);
            vkCmdDispatch(command, ChunkWords / 256, 1, 1);
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
        });
    }
}

void readbackAndVerify(Context& context, VkBuffer buffer, Staging& staging,
                       int cycle) {
    auto* actual = static_cast<const std::uint32_t*>(staging.mapped);
    for (std::uint32_t chunk = 0; chunk < TotalBytes / ChunkBytes; ++chunk) {
        const VkDeviceSize offset = static_cast<VkDeviceSize>(chunk) * ChunkBytes;
        context.submit([&](VkCommandBuffer command) {
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            VkBufferCopy copy{offset, 0, ChunkBytes};
            vkCmdCopyBuffer(command, buffer, staging.buffer, 1, &copy);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
        });
        const std::uint32_t base = chunk * ChunkWords;
        for (std::uint32_t i = 0; i < ChunkWords; ++i) {
            std::uint32_t expected = initialWord(base + i);
            for (int completed = 0; completed <= cycle; ++completed)
                expected ^= mix32(i ^ cycleSalt(static_cast<std::uint32_t>(completed), chunk));
            if (actual[i] != expected)
                throw std::runtime_error("readback mismatch at byte " +
                    std::to_string((static_cast<std::uint64_t>(base) + i) * 4));
        }
    }
}
} // namespace

int main(int argc, char** argv) try {
    const bool expectBudgetRefusal = argc == 2 && std::strcmp(argv[1], "--expect-budget-refusal") == 0;
    require(argc == 1 || expectBudgetRefusal, "usage: zvram-vulkan-auto-check [--expect-budget-refusal]");
    Context context;
    context.initialize();
    Buffer resident; resident.device = context.device;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = TotalBytes;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(context.device, &ci, nullptr, &resident.handle), "create virtual storage buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device, resident.handle, &requirements);
    require(requirements.memoryTypeBits & (1u << context.virtualType),
            "buffer is not eligible for virtual memory type");
    require(requirements.size <= 98304 * MiB, "buffer exceeds configured virtual capacity");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = requirements.size; ai.memoryTypeIndex = context.virtualType;
    check(vkAllocateMemory(context.device, &ai, nullptr, &resident.memory), "allocate virtual buffer memory");
    check(vkBindBufferMemory(context.device, resident.handle, resident.memory, 0), "bind virtual buffer memory");

    Staging staging; staging.device = context.device;
    VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stagingInfo.size = ChunkBytes;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(context.device, &stagingInfo, nullptr, &staging.buffer), "create native staging buffer");
    VkMemoryRequirements stagingReq{};
    vkGetBufferMemoryRequirements(context.device, staging.buffer, &stagingReq);
    const std::uint32_t stagingType = hostCoherentType(context, stagingReq.memoryTypeBits);
    require(stagingType != UINT32_MAX, "no native host-coherent staging memory type");
    VkMemoryAllocateInfo stagingAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    stagingAlloc.allocationSize = stagingReq.size; stagingAlloc.memoryTypeIndex = stagingType;
    check(vkAllocateMemory(context.device, &stagingAlloc, nullptr, &staging.memory), "allocate staging memory");
    check(vkBindBufferMemory(context.device, staging.buffer, staging.memory, 0), "bind staging memory");
    check(vkMapMemory(context.device, staging.memory, 0, ChunkBytes, 0, &staging.mapped), "map staging memory");

    upload(context, resident.handle, staging);
    const auto initial = context.stats();
    if (expectBudgetRefusal) {
        const auto deadline = std::chrono::steady_clock::now() + ColdTimeout;
        ZvramSnapshotStatsNX refused{};
        do {
            refused = context.stats();
            if (refused.failures > initial.failures && refused.residentBytes > 0 &&
                refused.coldLogicalBytes == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (std::chrono::steady_clock::now() < deadline);
        require(refused.failures > initial.failures && refused.residentBytes > 0 &&
                refused.coldLogicalBytes == 0,
                "over-budget snapshot did not fail while retaining resident allocation");
        std::cout << "budget refusal: failures=" << refused.failures << " resident="
                  << refused.residentBytes << " cold=" << refused.coldLogicalBytes << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        readbackAndVerify(context, resident.handle, staging, -1);
        std::cout << "PASS: over-budget snapshot refused; original 320 MiB contents remain intact\n";
        return 0;
    }
    waitCold(context, initial);
    auto cold = context.stats();
    metadataWhileCold(context, resident.handle, cold);
    std::cout << "cold snapshot: logical=" << cold.coldLogicalBytes << " stored="
              << cold.coldStoredBytes << " freezes=" << cold.freezes << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (std::uint32_t cycle = 0; cycle < 2; ++cycle) {
        const auto beforeWake = context.stats();
        computeCycle(context, resident.handle, cycle);
        const auto afterWake = context.stats();
        require(afterWake.restores > beforeWake.restores,
                "GPU compute did not restore cold virtual allocation");
        readbackAndVerify(context, resident.handle, staging, cycle);
        std::cout << "wake cycle " << (cycle + 1) << ": verified " << TotalBytes << " bytes" << std::endl;
        if (cycle == 0) {
            const auto beforeSleep = context.stats();
            waitCold(context, beforeSleep);
            cold = context.stats();
            metadataWhileCold(context, resident.handle, cold);
        }
    }

    const auto beforeFinalSleep = context.stats();
    waitCold(context, beforeFinalSleep);
    const auto beforeColdFree = context.stats();
    vkDestroyBuffer(context.device, resident.handle, nullptr); resident.handle = VK_NULL_HANDLE;
    vkFreeMemory(context.device, resident.memory, nullptr); resident.memory = VK_NULL_HANDLE;
    const auto afterColdFree = context.stats();
    require(afterColdFree.restores == beforeColdFree.restores,
            "freeing cold virtual allocation caused a restore");
    require(afterColdFree.coldLogicalBytes < beforeColdFree.coldLogicalBytes,
            "cold allocation remained accounted after free");
    check(vkQueueWaitIdle(context.queue), "finish cold-free cleanup");
    std::cout << "PASS: two cold/wake compute cycles, full readback, metadata stayed cold, cold free did not restore\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
