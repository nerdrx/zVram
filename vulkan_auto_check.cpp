#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
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
constexpr VkDeviceSize BudgetFirstBytes = 256 * MiB;
constexpr VkDeviceSize BudgetSecondBytes = 192 * MiB;
constexpr VkDeviceSize FirstChildBytes = 256 * MiB;
constexpr VkDeviceSize SecondChildBytes = TotalBytes - FirstChildBytes;
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
using ArmRestoreFailure = VkResult (VKAPI_PTR *)(VkDevice, std::uint32_t);

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
    VkQueue secondQueue{};
    VkCommandPool commands{};
    VkCommandPool secondCommands{};
    VkSemaphore chainSemaphores[2]{};
    VkSemaphore pendingTimeline{};
    VkDescriptorSetLayout descriptorLayout{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSet descriptor{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};
    GetSnapshotStats getStats{};
    ArmRestoreFailure armRestoreFailure{};
    VkPhysicalDevice physical{};
    std::uint32_t family{};
    std::uint32_t virtualType{UINT32_MAX};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceProperties properties{};
    bool bdaMode{};
    bool nativeAllocation{};
    bool twoQueues{};
    bool twoFamilies{};
    bool exclusiveFamilies{};
    bool pendingWait{};
    bool pendingBind{};
    std::uint32_t secondFamily{};
    std::uint32_t chainIndex{};
    bool chainStarted{};
    VkDeviceAddress bufferAddress{};

    ~Context() {
        if (device) vkDeviceWaitIdle(device);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        if (pendingTimeline) vkDestroySemaphore(device, pendingTimeline, nullptr);
        for (auto semaphore : chainSemaphores) if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
        if (secondCommands && secondCommands != commands) vkDestroyCommandPool(device, secondCommands, nullptr);
        if (commands) vkDestroyCommandPool(device, commands, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    void initialize(bool enableBda, bool native, bool twoSame, bool twoSeparate, bool exclusive,
                    bool pending, bool bindWhilePending) {
        twoSeparate = twoSeparate || exclusive;
        bdaMode = enableBda;
        nativeAllocation = native; twoQueues = twoSame; twoFamilies = twoSeparate;
        exclusiveFamilies = exclusive; pendingWait = pending; pendingBind = bindWhilePending;
        if (exclusive) twoFamilies = true;
        require(!(twoSame && twoSeparate), "choose only one of --two-queues and --two-families");
        require(!(pending && bindWhilePending), "choose only one pending queue test");
        require(!(pending || bindWhilePending) || twoSame || twoSeparate || exclusive,
                "pending queue tests require a multi-queue mode");
        require(!(pending || bindWhilePending) || native, "pending queue tests require --native-allocation");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = (enableBda || pending || bindWhilePending) ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
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
            VkPhysicalDeviceBufferDeviceAddressFeatures bdaFeatures{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
            VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
            if (enableBda || pending || bindWhilePending) {
                VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                if (enableBda) {
                    features2.pNext = &bdaFeatures;
                    if (pending || bindWhilePending) bdaFeatures.pNext = &timelineFeatures;
                } else {
                    features2.pNext = &timelineFeatures;
                }
                vkGetPhysicalDeviceFeatures2(candidate, &features2);
                features = features2.features;
                if (enableBda && (!bdaFeatures.bufferDeviceAddress || !features.shaderInt64)) continue;
                if ((pending || bindWhilePending) && !timelineFeatures.timelineSemaphore) continue;
            } else {
                vkGetPhysicalDeviceFeatures(candidate, &features);
            }
            if (!features.sparseBinding) continue;
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                const auto sparseTransfer = VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_TRANSFER_BIT;
                const auto all = sparseTransfer | VK_QUEUE_COMPUTE_BIT;
                if (!twoSeparate && (families[i].queueFlags & all) == all &&
                    families[i].queueCount >= (twoSame ? 2u : 1u)) {
                    physical = candidate; family = i; properties = props;
                    if (twoSame) secondFamily = i;
                    break;
                }
                if (twoSeparate && families[i].queueCount &&
                    (families[i].queueFlags & (sparseTransfer | VK_QUEUE_GRAPHICS_BIT)) ==
                                       (sparseTransfer | VK_QUEUE_GRAPHICS_BIT)) {
                    for (std::uint32_t j = 0; j < familyCount; ++j) {
                        if (j != i && families[j].queueCount && (families[j].queueFlags &
                            (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) ==
                            (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) {
                            physical = candidate; family = i; secondFamily = j; properties = props;
                            break;
                        }
                    }
                    if (physical) break;
                }
            }
            if (physical) break;
        }
        require(physical != VK_NULL_HANDLE, "no discrete AMD RADV GPU has the requested sparse/compute queue setup");
        std::cout << "GPU: " << properties.deviceName << '\n';
        require(properties.limits.maxStorageBufferRange >= ChunkBytes &&
                properties.limits.minStorageBufferOffsetAlignment != 0 &&
                ChunkBytes % properties.limits.minStorageBufferOffsetAlignment == 0 &&
                properties.limits.maxComputeWorkGroupInvocations >= 256 &&
                properties.limits.maxComputeWorkGroupSize[0] >= 256 &&
                properties.limits.maxComputeWorkGroupCount[0] >= ChunkWords / 256,
                "device limits do not support 32 MiB storage chunks and compute dispatch");

        float priorities[2]{1.0f, 1.0f};
        VkDeviceQueueCreateInfo qci[2]{};
        qci[0] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci[0].queueFamilyIndex = family; qci[0].queueCount = twoSame ? 2u : 1u;
        qci[0].pQueuePriorities = priorities;
        std::uint32_t qciCount = 1;
        if (twoSeparate) {
            qci[1] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            qci[1].queueFamilyIndex = secondFamily; qci[1].queueCount = 1;
            qci[1].pQueuePriorities = priorities;
            qciCount = 2;
        }
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = qciCount; dci.pQueueCreateInfos = qci;
        VkPhysicalDeviceFeatures enabled{}; enabled.sparseBinding = VK_TRUE;
        VkPhysicalDeviceBufferDeviceAddressFeatures enabledBda{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        VkPhysicalDeviceTimelineSemaphoreFeatures enabledTimeline{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 enabled2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if (enableBda) {
            enabledBda.bufferDeviceAddress = VK_TRUE;
            enabled2.features.sparseBinding = VK_TRUE;
            enabled2.features.shaderInt64 = VK_TRUE;
            enabled2.pNext = &enabledBda;
            if (pendingWait || pendingBind) {
                enabledTimeline.timelineSemaphore = VK_TRUE;
                enabledBda.pNext = &enabledTimeline;
            }
            dci.pNext = &enabled2;
        } else {
            dci.pEnabledFeatures = &enabled;
            if (pendingWait || pendingBind) {
                enabledTimeline.timelineSemaphore = VK_TRUE;
                dci.pNext = &enabledTimeline;
            }
        }
        check(vkCreateDevice(physical, &dci, nullptr, &device), "create device");
        vkGetDeviceQueue(device, family, 0, &queue);
        if (twoSame) vkGetDeviceQueue(device, family, 1, &secondQueue);
        if (twoSeparate) vkGetDeviceQueue(device, secondFamily, 0, &secondQueue);

        getStats = reinterpret_cast<GetSnapshotStats>(vkGetDeviceProcAddr(device, "vkZVramGetSnapshotStatsNX"));
        require(getStats != nullptr, "automatic Vulkan snapshot stats interface unavailable");
        armRestoreFailure = reinterpret_cast<ArmRestoreFailure>(
            vkGetDeviceProcAddr(device, "vkZVramArmRestoreFailureNX"));
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
        if (twoSeparate) {
            cp.queueFamilyIndex = secondFamily;
            check(vkCreateCommandPool(device, &cp, nullptr, &secondCommands), "create second command pool");
        } else if (twoSame) {
            secondCommands = commands;
        }
        if (twoQueues || twoFamilies) {
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(device, &sci, nullptr, &chainSemaphores[0]), "create queue-chain semaphore");
            check(vkCreateSemaphore(device, &sci, nullptr, &chainSemaphores[1]), "create queue-chain semaphore");
        }
        if (pendingWait || pendingBind) {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext = &type;
            check(vkCreateSemaphore(device, &sci, nullptr, &pendingTimeline), "create pending-wait timeline");
        }
        createComputePipeline();
    }

    void createComputePipeline() {
        if (bdaMode) {
            VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
            check(vkCreatePipelineLayout(device, &pl, nullptr, &pipelineLayout), "create BDA pipeline layout");
            createShaderPipeline(ZVRAM_BDA_SHADER_PATH);
            return;
        }
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

        createShaderPipeline(ZVRAM_CHECK_SHADER_PATH);
    }

    void createShaderPipeline(const char* path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
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
    VkResult submit(Record&& record, std::uint32_t queueIndex = 0,
                    VkResult expectedFailure = VK_SUCCESS) {
        VkQueue targetQueue = queueIndex ? secondQueue : queue;
        VkCommandPool targetPool = queueIndex ? secondCommands : commands;
        require(targetQueue != VK_NULL_HANDLE && targetPool != VK_NULL_HANDLE, "requested queue is unavailable");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = targetPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
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
            std::uint32_t signalIndex = chainIndex ^ 1u;
            if (twoQueues || twoFamilies) {
                if (chainStarted) {
                    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &chainSemaphores[chainIndex];
                    static constexpr VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                    si.pWaitDstStageMask = &waitStage;
                }
                si.signalSemaphoreCount = 1; si.pSignalSemaphores = &chainSemaphores[signalIndex];
            }
            const VkResult submitResult = vkQueueSubmit(targetQueue, 1, &si, VK_NULL_HANDLE);
            if (expectedFailure != VK_SUCCESS && submitResult == expectedFailure) {
                vkFreeCommandBuffers(device, targetPool, 1, &command);
                return submitResult;
            }
            check(submitResult, "submit command buffer");
            check(vkQueueWaitIdle(targetQueue), "wait for queue");
            if (twoQueues || twoFamilies) { chainIndex = signalIndex; chainStarted = true; }
        } catch (...) {
            vkFreeCommandBuffers(device, targetPool, 1, &command);
            throw;
        }
        vkFreeCommandBuffers(device, targetPool, 1, &command);
        return VK_SUCCESS;
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

ZvramSnapshotStatsNX waitPartialCold(const Context& context, const ZvramSnapshotStatsNX& before) {
    const auto deadline = std::chrono::steady_clock::now() + ColdTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto current = context.stats();
        if (current.coldLogicalBytes == FirstChildBytes &&
            current.residentBytes == SecondChildBytes && current.coldStoredBytes > 0 &&
            current.freezes > before.freezes) return current;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto final = context.stats();
    throw std::runtime_error("partial freeze state not reached: cold=" +
        std::to_string(final.coldLogicalBytes) + " resident=" + std::to_string(final.residentBytes) +
        " stored=" + std::to_string(final.coldStoredBytes) + " freezes=" +
        std::to_string(final.freezes) + " failures=" + std::to_string(final.failures) +
        " lastError=" + std::to_string(final.lastError));
}

void metadataWhileCold(Context& context, VkBuffer buffer,
                       const ZvramSnapshotStatsNX& cold,
                       std::uint64_t expectedColdBytes = TotalBytes,
                       VkDeviceSize minimumBufferBytes = TotalBytes) {
    if (context.bdaMode) {
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = buffer;
        require(vkGetBufferDeviceAddress(context.device, &addressInfo) == context.bufferAddress,
                "buffer device address changed while allocation was cold");
    }
    for (int i = 0; i < 3; ++i) {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(context.device, buffer, &requirements);
        require(requirements.size >= minimumBufferBytes, "buffer requirements changed while cold");
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(context.physical, &memory);
        require(memory.memoryTypeCount > context.virtualType, "virtual memory type disappeared while cold");
        const auto current = context.stats();
        require(current.restores == cold.restores && current.coldLogicalBytes == expectedColdBytes,
                "metadata query woke the cold allocation");
    }
}

std::uint32_t gpuOnlyNativeType(const Context& context, std::uint32_t bits) {
    for (std::uint32_t i = 0; i < context.memory.memoryTypeCount; ++i) {
        const auto flags = context.memory.memoryTypes[i].propertyFlags;
        if (i != context.virtualType && (bits & (1u << i)) &&
            (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return i;
    }
    return UINT32_MAX;
}

void probeNativeTokenLifetimes(Context& context, VkDeviceMemory original,
                               const ZvramSnapshotStatsNX& cold) {
    struct Probe {
        VkDevice device{};
        VkBuffer buffer{};
        VkDeviceMemory bufferMemory{};
        VkImage image{};
        VkDeviceMemory imageMemory{};
        void cleanup() {
            if (buffer) { vkDestroyBuffer(device, buffer, nullptr); buffer = VK_NULL_HANDLE; }
            if (bufferMemory) { vkFreeMemory(device, bufferMemory, nullptr); bufferMemory = VK_NULL_HANDLE; }
            if (image) { vkDestroyImage(device, image, nullptr); image = VK_NULL_HANDLE; }
            if (imageMemory) { vkFreeMemory(device, imageMemory, nullptr); imageMemory = VK_NULL_HANDLE; }
        }
        ~Probe() { cleanup(); }
    } probe;
    probe.device = context.device;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = 256;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(context.device, &bufferInfo, nullptr, &probe.buffer), "create native probe buffer");
    VkMemoryRequirements bufferRequirements{};
    vkGetBufferMemoryRequirements(context.device, probe.buffer, &bufferRequirements);
    const auto bufferType = gpuOnlyNativeType(context, bufferRequirements.memoryTypeBits);
    require(bufferType != UINT32_MAX, "probe buffer has no compatible native GPU-only type");
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &flags; allocation.allocationSize = bufferRequirements.size;
    allocation.memoryTypeIndex = bufferType;
    check(vkAllocateMemory(context.device, &allocation, nullptr, &probe.bufferMemory),
          "allocate native probe buffer memory");
    VkBindBufferMemoryInfo bufferBind{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};
    bufferBind.buffer = probe.buffer; bufferBind.memory = probe.bufferMemory;
    check(vkBindBufferMemory2(context.device, 1, &bufferBind), "bind native probe buffer memory2");
    require(probe.bufferMemory != original, "small native buffer reused original cold allocation token");

    VkImageFormatProperties imageProperties{};
    check(vkGetPhysicalDeviceImageFormatProperties(context.physical, VK_FORMAT_R8G8B8A8_UNORM,
          VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &imageProperties),
          "query native probe image format");
    require(imageProperties.maxExtent.width >= 2 && imageProperties.maxExtent.height >= 2,
            "native probe image extent is unsupported");
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {2, 2, 1}; imageInfo.mipLevels = 1; imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(context.device, &imageInfo, nullptr, &probe.image), "create native probe image");
    VkMemoryRequirements imageRequirements{};
    vkGetImageMemoryRequirements(context.device, probe.image, &imageRequirements);
    const auto imageType = gpuOnlyNativeType(context, imageRequirements.memoryTypeBits);
    require(imageType != UINT32_MAX, "probe image has no compatible native GPU-only type");
    allocation.allocationSize = imageRequirements.size; allocation.memoryTypeIndex = imageType;
    check(vkAllocateMemory(context.device, &allocation, nullptr, &probe.imageMemory),
          "allocate native probe image memory");
    VkBindImageMemoryInfo imageBind{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    imageBind.image = probe.image; imageBind.memory = probe.imageMemory;
    check(vkBindImageMemory2(context.device, 1, &imageBind), "bind native probe image memory2");
    require(probe.imageMemory != original, "tiny native image reused original cold allocation token");

    const auto during = context.stats();
    require(during.coldLogicalBytes == cold.coldLogicalBytes &&
            during.coldStoredBytes == cold.coldStoredBytes &&
            during.residentBytes == cold.residentBytes && during.freezes == cold.freezes &&
            during.restores == cold.restores && during.failures == cold.failures,
            "unadopted native buffer/image changed cold allocation accounting");
    probe.cleanup();
    const auto afterFree = context.stats();
    require(afterFree.coldLogicalBytes == cold.coldLogicalBytes &&
            afterFree.coldStoredBytes == cold.coldStoredBytes &&
            afterFree.residentBytes == cold.residentBytes && afterFree.freezes == cold.freezes &&
            afterFree.restores == cold.restores && afterFree.failures == cold.failures,
            "freeing unadopted native buffer/image changed cold allocation accounting");
    std::cout << "native token probes bound with distinct allocations; original cold token unchanged" << std::endl;
}

void upload(Context& context, VkBuffer buffer, Staging& staging,
            VkDeviceSize byteSize = TotalBytes) {
    auto* words = static_cast<std::uint32_t*>(staging.mapped);
    require(byteSize % ChunkBytes == 0, "upload size must contain whole chunks");
    const std::uint32_t chunkCount = static_cast<std::uint32_t>(byteSize / ChunkBytes);
    for (std::uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
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
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
            if (context.exclusiveFamilies && chunk + 1 == chunkCount) {
                VkBufferMemoryBarrier release{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                release.dstAccessMask = 0;
                release.srcQueueFamilyIndex = context.family;
                release.dstQueueFamilyIndex = context.secondFamily;
                release.buffer = buffer; release.offset = 0; release.size = byteSize;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                     0, nullptr, 1, &release, 0, nullptr);
            }
        });
    }
}

VkResult computeCycle(Context& context, VkBuffer buffer, std::uint32_t cycle,
                      std::uint32_t queueIndex, bool expectFirstFailure = false,
                      VkDeviceSize byteSize = TotalBytes) {
    require(byteSize % ChunkBytes == 0, "compute size must contain whole chunks");
    const std::uint32_t chunkCount = static_cast<std::uint32_t>(byteSize / ChunkBytes);
    for (std::uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
        const VkDeviceSize offset = static_cast<VkDeviceSize>(chunk) * ChunkBytes;
        VkDescriptorBufferInfo info{buffer, offset, ChunkBytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = context.descriptor; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &info;
        if (!context.bdaMode) vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
        struct BdaPush { VkDeviceAddress address; std::uint32_t count, salt; };
        const BdaPush bdaPush{context.bufferAddress + offset, ChunkWords, cycleSalt(cycle, chunk)};
        const std::uint32_t push[3]{ChunkWords, cycleSalt(cycle, chunk), 1};
        const VkResult result = context.submit([&](VkCommandBuffer command) {
            if (context.exclusiveFamilies && chunk == 0) {
                VkBufferMemoryBarrier acquire{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                acquire.srcAccessMask = 0;
                acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                acquire.srcQueueFamilyIndex = context.family;
                acquire.dstQueueFamilyIndex = context.secondFamily;
                acquire.buffer = buffer; acquire.offset = 0; acquire.size = byteSize;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                     0, nullptr, 1, &acquire, 0, nullptr);
            }
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipeline);
            if (context.bdaMode) {
                vkCmdPushConstants(command, context.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(bdaPush), &bdaPush);
            } else {
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipelineLayout,
                                        0, 1, &context.descriptor, 0, nullptr);
                vkCmdPushConstants(command, context.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(push), push);
            }
            vkCmdDispatch(command, ChunkWords / 256, 1, 1);
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
            if (context.exclusiveFamilies && chunk + 1 == chunkCount) {
                VkBufferMemoryBarrier release{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                release.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                release.dstAccessMask = 0;
                release.srcQueueFamilyIndex = context.secondFamily;
                release.dstQueueFamilyIndex = context.family;
                release.buffer = buffer; release.offset = 0; release.size = byteSize;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                     0, nullptr, 1, &release, 0, nullptr);
            }
        }, queueIndex, expectFirstFailure && chunk == 0 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS);
        if (expectFirstFailure && chunk == 0) return result;
    }
    return VK_SUCCESS;
}

void readbackAndVerify(Context& context, VkBuffer buffer, Staging& staging,
                       int cycle, bool releaseForCompute = false,
                       VkDeviceSize byteSize = TotalBytes) {
    auto* actual = static_cast<const std::uint32_t*>(staging.mapped);
    require(byteSize % ChunkBytes == 0, "readback size must contain whole chunks");
    const std::uint32_t chunkCount = static_cast<std::uint32_t>(byteSize / ChunkBytes);
    for (std::uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
        const VkDeviceSize offset = static_cast<VkDeviceSize>(chunk) * ChunkBytes;
        context.submit([&](VkCommandBuffer command) {
            if (context.exclusiveFamilies && chunk == 0) {
                VkBufferMemoryBarrier acquire{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                acquire.srcAccessMask = 0;
                acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                acquire.srcQueueFamilyIndex = context.secondFamily;
                acquire.dstQueueFamilyIndex = context.family;
                acquire.buffer = buffer; acquire.offset = 0; acquire.size = byteSize;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     0, nullptr, 1, &acquire, 0, nullptr);
            }
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            VkBufferCopy copy{offset, 0, ChunkBytes};
            vkCmdCopyBuffer(command, buffer, staging.buffer, 1, &copy);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
            if (context.exclusiveFamilies && releaseForCompute &&
                chunk + 1 == chunkCount) {
                VkBufferMemoryBarrier release{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                release.dstAccessMask = 0;
                release.srcQueueFamilyIndex = context.family;
                release.dstQueueFamilyIndex = context.secondFamily;
                release.buffer = buffer; release.offset = 0; release.size = byteSize;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                     0, nullptr, 1, &release, 0, nullptr);
            }
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
    bool expectBudgetRefusal = false, expectBudgetRelease = false;
    bool expectPartialFreeze = false, expectPartialRestore = false;
    bool bdaMode = false, nativeAllocation = false;
    bool twoQueues = false, twoFamilies = false, exclusiveFamilies = false;
    bool pendingWait = false, pendingBind = false;
    bool concurrentWait = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--expect-budget-refusal") == 0) expectBudgetRefusal = true;
        else if (std::strcmp(argv[i], "--expect-budget-release") == 0) expectBudgetRelease = true;
        else if (std::strcmp(argv[i], "--expect-partial-freeze") == 0) expectPartialFreeze = true;
        else if (std::strcmp(argv[i], "--expect-partial-restore") == 0) expectPartialRestore = true;
        else if (std::strcmp(argv[i], "--bda") == 0) bdaMode = true;
        else if (std::strcmp(argv[i], "--native-allocation") == 0) nativeAllocation = true;
        else if (std::strcmp(argv[i], "--two-queues") == 0) twoQueues = true;
        else if (std::strcmp(argv[i], "--two-families") == 0) twoFamilies = true;
        else if (std::strcmp(argv[i], "--exclusive-families") == 0) exclusiveFamilies = true;
        else if (std::strcmp(argv[i], "--pending-wait") == 0) pendingWait = true;
        else if (std::strcmp(argv[i], "--pending-bind") == 0) pendingBind = true;
        else if (std::strcmp(argv[i], "--concurrent-wait") == 0) concurrentWait = true;
        else throw std::runtime_error("usage: zvram-vulkan-auto-check [--expect-budget-refusal|--expect-budget-release|--expect-partial-freeze|--expect-partial-restore] [--bda] [--native-allocation] [--two-queues|--two-families|--exclusive-families] [--pending-wait|--pending-bind] [--concurrent-wait]");
    }
    require(!(expectBudgetRelease && (expectBudgetRefusal || expectPartialFreeze || expectPartialRestore ||
                                      bdaMode || nativeAllocation || twoQueues || twoFamilies ||
                                      exclusiveFamilies || pendingWait || pendingBind)),
            "budget-release mode uses one synthetic single-queue allocation path");
    require(!(expectBudgetRefusal && bdaMode), "--bda and --expect-budget-refusal cannot be combined");
    require(!(expectBudgetRefusal && nativeAllocation), "budget refusal requires virtual allocation");
    require(!(expectPartialFreeze && (expectBudgetRefusal || nativeAllocation || bdaMode)),
            "partial-freeze mode requires the standard virtual allocation path");
    require(!(expectPartialRestore && (expectBudgetRefusal || expectPartialFreeze || nativeAllocation || bdaMode)),
            "partial-restore mode requires the standard virtual allocation path");
    require(!(twoQueues && (twoFamilies || exclusiveFamilies)), "choose only one multi-queue mode");
    require(!(expectBudgetRefusal && exclusiveFamilies), "budget refusal mode does not use exclusive family transfers");
    require(!(pendingWait && pendingBind), "choose only one pending queue test");
    require(!concurrentWait || (pendingWait && twoQueues),
            "--concurrent-wait requires --pending-wait and --two-queues");
    require(!pendingBind || (nativeAllocation && (twoQueues || twoFamilies || exclusiveFamilies)),
            "--pending-bind requires --native-allocation and a multi-queue mode");
    Context context;
    context.initialize(bdaMode, nativeAllocation, twoQueues, twoFamilies, exclusiveFamilies,
                       pendingWait, pendingBind);
    if (pendingWait || pendingBind) {
        VkTimelineSemaphoreSubmitInfo waitValues{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        const std::uint64_t value = 1;
        waitValues.waitSemaphoreValueCount = 1; waitValues.pWaitSemaphoreValues = &value;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        waitInfo.pNext = &waitValues; waitInfo.waitSemaphoreCount = 1;
        waitInfo.pWaitSemaphores = &context.pendingTimeline; waitInfo.pWaitDstStageMask = &waitStage;
        check(vkQueueSubmit(context.queue, 1, &waitInfo, VK_NULL_HANDLE), "queue pending timeline wait");
        std::cout << (pendingBind ? "pending-bind queued; bind before signal" :
                                   "pending-wait queued; resolving from second queue after 300 ms") << std::endl;
        if (pendingWait) {
            auto signalSecondQueue = [&]() {
                VkTimelineSemaphoreSubmitInfo signalValues{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
                signalValues.signalSemaphoreValueCount = 1; signalValues.pSignalSemaphoreValues = &value;
                VkSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                signalInfo.pNext = &signalValues; signalInfo.signalSemaphoreCount = 1;
                signalInfo.pSignalSemaphores = &context.pendingTimeline;
                const auto submitted = vkQueueSubmit(context.secondQueue, 1, &signalInfo, VK_NULL_HANDLE);
                return submitted == VK_SUCCESS ? vkQueueWaitIdle(context.secondQueue) : submitted;
            };
            if (concurrentWait) {
                std::atomic<VkResult> signalResult{VK_NOT_READY};
                std::thread signalThread([&] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    signalResult.store(signalSecondQueue());
                });
                const auto firstQueueWait = vkQueueWaitIdle(context.queue);
                signalThread.join();
                check(signalResult.load(), "resolve pending wait concurrently from second queue");
                check(firstQueueWait, "finish first queue pending wait");
                std::cout << "PASS: second queue resolved a wait while first queue was blocked" << std::endl;
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            check(signalSecondQueue(), "resolve pending wait from second queue");
            check(vkQueueWaitIdle(context.queue), "finish first queue pending wait");
            std::cout << "PASS: second queue resolved pending wait" << std::endl;
        }
    }
    Buffer resident; resident.device = context.device;
    Buffer extra; extra.device = context.device;
    const VkDeviceSize residentBufferBytes = expectBudgetRelease ? BudgetFirstBytes : TotalBytes;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = residentBufferBytes;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (bdaMode) ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    std::uint32_t sharingFamilies[2]{context.family, context.secondFamily};
    if (twoFamilies && !exclusiveFamilies) {
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2; ci.pQueueFamilyIndices = sharingFamilies;
    } else {
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    check(vkCreateBuffer(context.device, &ci, nullptr, &resident.handle), "create virtual storage buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device, resident.handle, &requirements);
    require(requirements.memoryTypeBits & (1u << context.virtualType),
            "buffer is not eligible for virtual memory type");
    require(requirements.size <= 98304 * MiB, "buffer exceeds configured virtual capacity");
    std::uint32_t allocationType = context.virtualType;
    if (nativeAllocation) {
        allocationType = UINT32_MAX;
        for (std::uint32_t i = 0; i < context.memory.memoryTypeCount; ++i) {
            const auto flags = context.memory.memoryTypes[i].propertyFlags;
            if (i != context.virtualType && (requirements.memoryTypeBits & (1u << i)) &&
                (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { allocationType = i; break; }
        }
        require(allocationType != UINT32_MAX, "no compatible native GPU-only memory type");
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = requirements.size; ai.memoryTypeIndex = allocationType;
    VkMemoryAllocateFlagsInfo addressFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    addressFlags.flags = bdaMode ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;
    ai.pNext = &addressFlags;
    check(vkAllocateMemory(context.device, &ai, nullptr, &resident.memory), "allocate virtual buffer memory");
    const auto bindStart = std::chrono::steady_clock::now();
    check(vkBindBufferMemory(context.device, resident.handle, resident.memory, 0), "bind virtual buffer memory");
    VkDeviceSize residentLogicalBytes = requirements.size;
    VkDeviceSize extraLogicalBytes = 0;
    if (expectBudgetRelease) {
        VkBufferCreateInfo extraInfo = ci;
        extraInfo.size = BudgetSecondBytes;
        check(vkCreateBuffer(context.device, &extraInfo, nullptr, &extra.handle), "create second virtual buffer");
        VkMemoryRequirements extraRequirements{};
        vkGetBufferMemoryRequirements(context.device, extra.handle, &extraRequirements);
        require(extraRequirements.memoryTypeBits & (1u << context.virtualType),
                "second buffer is not eligible for virtual memory type");
        require(extraRequirements.size <= 98304 * MiB, "second buffer exceeds configured virtual capacity");
        VkMemoryAllocateInfo extraAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        extraAlloc.allocationSize = extraRequirements.size;
        extraAlloc.memoryTypeIndex = context.virtualType;
        VkMemoryAllocateFlagsInfo extraAddressFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        extraAddressFlags.flags = 0;
        extraAlloc.pNext = &extraAddressFlags;
        check(vkAllocateMemory(context.device, &extraAlloc, nullptr, &extra.memory),
              "allocate second virtual buffer memory");
        check(vkBindBufferMemory(context.device, extra.handle, extra.memory, 0),
              "bind second virtual buffer memory");
        residentLogicalBytes = requirements.size;
        extraLogicalBytes = extraRequirements.size;
        require(residentLogicalBytes != extraLogicalBytes,
                "budget-release buffers need distinct allocation sizes for cold identity");
        require(residentLogicalBytes <= FirstChildBytes && extraLogicalBytes <= FirstChildBytes,
                "budget-release allocations must each fit one backing child");
    }
    if (pendingBind) {
        const auto bindMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - bindStart).count();
        std::cout << "pending bind returned before timeline signal in " << bindMs << " ms" << std::endl;
        const std::uint64_t value = 1;
        VkTimelineSemaphoreSubmitInfo signalValues{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        signalValues.signalSemaphoreValueCount = 1; signalValues.pSignalSemaphoreValues = &value;
        VkSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        signalInfo.pNext = &signalValues; signalInfo.signalSemaphoreCount = 1;
        signalInfo.pSignalSemaphores = &context.pendingTimeline;
        check(vkQueueSubmit(context.secondQueue, 1, &signalInfo, VK_NULL_HANDLE), "signal pending bind queue wait");
        check(vkQueueWaitIdle(context.secondQueue), "wait for pending bind signal queue");
        check(vkQueueWaitIdle(context.queue), "finish pending bind wait queue");
    }
    if (bdaMode) {
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = resident.handle;
        context.bufferAddress = vkGetBufferDeviceAddress(context.device, &addressInfo);
        require(context.bufferAddress != 0, "virtual buffer has no device address");
        std::cout << "BDA hot address: 0x" << std::hex << context.bufferAddress << std::dec << std::endl;
    }

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

    upload(context, resident.handle, staging, residentBufferBytes);
    if (expectBudgetRelease) {
        upload(context, extra.handle, staging, BudgetSecondBytes);
        readbackAndVerify(context, resident.handle, staging, -1, false, residentBufferBytes);
        readbackAndVerify(context, extra.handle, staging, -1, false, BudgetSecondBytes);
    }
    if (pendingBind) {
        for (std::uint32_t cycle = 0; cycle < 2; ++cycle) {
            computeCycle(context, resident.handle, cycle, 1);
            readbackAndVerify(context, resident.handle, staging, cycle, cycle == 0);
            std::cout << "pending-bind integrity cycle " << (cycle + 1) << ": verified "
                      << TotalBytes << " bytes" << std::endl;
        }
        std::cout << "PASS: pending bind returned before signal; two full integrity cycles completed" << std::endl;
        return 0;
    }
    const auto initial = context.stats();
    if (expectBudgetRelease) {
        const auto deadline = std::chrono::steady_clock::now() + ColdTimeout;
        ZvramSnapshotStatsNX firstCold{};
        do {
            firstCold = context.stats();
            const bool primaryCold = firstCold.coldLogicalBytes == residentLogicalBytes &&
                                     firstCold.residentBytes == extraLogicalBytes;
            const bool extraCold = firstCold.coldLogicalBytes == extraLogicalBytes &&
                                   firstCold.residentBytes == residentLogicalBytes;
            if ((primaryCold || extraCold) && firstCold.coldStoredBytes > 0 &&
                firstCold.failures > initial.failures) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (std::chrono::steady_clock::now() < deadline);

        Buffer* coldBuffer = nullptr;
        Buffer* remainingBuffer = nullptr;
        VkDeviceSize coldBytes = 0;
        VkDeviceSize remainingBytes = 0;
        if (firstCold.coldLogicalBytes == residentLogicalBytes &&
            firstCold.residentBytes == extraLogicalBytes) {
            coldBuffer = &resident; remainingBuffer = &extra;
            coldBytes = residentLogicalBytes; remainingBytes = extraLogicalBytes;
        } else if (firstCold.coldLogicalBytes == extraLogicalBytes &&
                   firstCold.residentBytes == residentLogicalBytes) {
            coldBuffer = &extra; remainingBuffer = &resident;
            coldBytes = extraLogicalBytes; remainingBytes = residentLogicalBytes;
        }
        require(coldBuffer && remainingBuffer && firstCold.coldStoredBytes > 0 &&
                firstCold.failures > initial.failures,
                "cold-budget test did not freeze exactly one identifiable allocation and block the other");
        metadataWhileCold(context, coldBuffer->handle, firstCold, coldBytes, coldBytes);
        std::cout << "budget release: first cold allocation=" << coldBytes
                  << " resident=" << firstCold.residentBytes
                  << " failures=" << firstCold.failures << std::endl;

        const auto beforeFree = context.stats();
        vkDestroyBuffer(context.device, coldBuffer->handle, nullptr);
        coldBuffer->handle = VK_NULL_HANDLE;
        vkFreeMemory(context.device, coldBuffer->memory, nullptr);
        coldBuffer->memory = VK_NULL_HANDLE;
        const auto afterFree = context.stats();
        require(afterFree.restores == beforeFree.restores,
                "freeing the cold allocation caused an unnecessary restore");

        const auto refreezeDeadline = std::chrono::steady_clock::now() + ColdTimeout;
        ZvramSnapshotStatsNX remainingCold{};
        do {
            remainingCold = context.stats();
            if (remainingCold.coldLogicalBytes == remainingBytes &&
                remainingCold.residentBytes == 0 && remainingCold.coldStoredBytes > 0 &&
                remainingCold.freezes > beforeFree.freezes &&
                remainingCold.restores == beforeFree.restores) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (std::chrono::steady_clock::now() < refreezeDeadline);
        require(remainingCold.coldLogicalBytes == remainingBytes &&
                remainingCold.residentBytes == 0 && remainingCold.coldStoredBytes > 0 &&
                remainingCold.freezes > beforeFree.freezes &&
                remainingCold.restores == beforeFree.restores,
                "remaining allocation did not freeze after cold capacity was freed without queue work");
        metadataWhileCold(context, remainingBuffer->handle, remainingCold,
                          remainingBytes, remainingBytes);

        const auto beforeWake = context.stats();
        check(computeCycle(context, remainingBuffer->handle, 0, 0, false, remainingBytes),
              "wake remaining allocation after budget release");
        const auto afterWake = context.stats();
        require(afterWake.restores > beforeWake.restores,
                "remaining allocation did not restore after the capacity-release freeze");
        readbackAndVerify(context, remainingBuffer->handle, staging, 0, false, remainingBytes);
        vkDestroyBuffer(context.device, remainingBuffer->handle, nullptr);
        remainingBuffer->handle = VK_NULL_HANDLE;
        vkFreeMemory(context.device, remainingBuffer->memory, nullptr);
        remainingBuffer->memory = VK_NULL_HANDLE;
        const auto afterCleanup = context.stats();
        require(afterCleanup.coldLogicalBytes == 0 && afterCleanup.residentBytes == 0,
                "budget-release test cleanup left cold or resident allocation bytes");
        std::cout << "PASS: freeing one cold allocation released budget; remaining allocation froze, woke, and verified all bytes\n";
        return 0;
    }
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
        const auto stillIdle = context.stats();
        require(stillIdle.failures == refused.failures &&
                stillIdle.residentBytes == refused.residentBytes &&
                stillIdle.coldLogicalBytes == 0,
                "budget refusal retried without a new app queue submission");
        std::cout << "budget refusal stayed suppressed without queue work: failures="
                  << stillIdle.failures << std::endl;

        computeCycle(context, resident.handle, 0, 0);
        const auto retryDeadline = std::chrono::steady_clock::now() + ColdTimeout;
        ZvramSnapshotStatsNX retried{};
        do {
            retried = context.stats();
            if (retried.failures > stillIdle.failures && retried.residentBytes > 0 &&
                retried.coldLogicalBytes == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (std::chrono::steady_clock::now() < retryDeadline);
        require(retried.failures > stillIdle.failures && retried.residentBytes > 0 &&
                retried.coldLogicalBytes == 0,
                "budget refusal did not retry after new app queue work");
        readbackAndVerify(context, resident.handle, staging, 0);
        std::cout << "PASS: refusal stayed suppressed while idle, retried after queue work, and all 320 MiB remained intact\n";
        return 0;
    }
    if (expectPartialFreeze) {
        const auto cold = waitPartialCold(context, initial);
        require(cold.coldLogicalBytes == FirstChildBytes && cold.residentBytes == SecondChildBytes,
                "partial freeze did not leave exactly the second child resident");
        metadataWhileCold(context, resident.handle, cold, FirstChildBytes);
        std::cout << "partial freeze: cold=" << cold.coldLogicalBytes << " resident="
                  << cold.residentBytes << " stored=" << cold.coldStoredBytes << std::endl;
        const auto beforeWake = context.stats();
        computeCycle(context, resident.handle, 0, 0);
        const auto afterWake = context.stats();
        require(afterWake.restores > beforeWake.restores,
                "partial-cold allocation did not restore on GPU access");
        readbackAndVerify(context, resident.handle, staging, 0);
        std::cout << "PASS: first child released, second stayed resident, and all 320 MiB verified after wake\n";
        return 0;
    }
    if (expectPartialRestore) {
        const auto cold = [&] {
            waitCold(context, initial);
            return context.stats();
        }();
        require(cold.coldLogicalBytes == TotalBytes && cold.residentBytes == 0 &&
                cold.freezes >= initial.freezes + 2,
                "restore-retry test did not cold both backing child groups");
        metadataWhileCold(context, resident.handle, cold);
        std::cout << "fault-retry cold baseline: logical=" << cold.coldLogicalBytes
                  << " stored=" << cold.coldStoredBytes << " freezes=" << cold.freezes << std::endl;
        const auto beforeFault = context.stats();
        require(context.armRestoreFailure != nullptr, "restore fault-arm interface unavailable");
        check(context.armRestoreFailure(context.device, 1), "arm one-shot partial restore failure");
        const VkResult firstAttempt = computeCycle(context, resident.handle, 0, 0, true);
        require(firstAttempt == VK_ERROR_OUT_OF_DEVICE_MEMORY,
                "injected partial restore did not reject the first queue submission with OOM");
        const auto afterFault = context.stats();
        require(afterFault.failures > beforeFault.failures && afterFault.lastError == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                afterFault.restores > beforeFault.restores &&
                afterFault.coldLogicalBytes == SecondChildBytes &&
                afterFault.residentBytes == FirstChildBytes,
                "faulted restore did not retain one restored and one cold child group");
        std::cout << "partial restore fault: resident=" << afterFault.residentBytes
                  << " cold=" << afterFault.coldLogicalBytes << " failures=" << afterFault.failures << std::endl;
        check(computeCycle(context, resident.handle, 0, 0), "retry compute after partial restore");
        readbackAndVerify(context, resident.handle, staging, 0);
        const auto afterRetry = context.stats();
        require(afterRetry.restores > afterFault.restores,
                "retry did not restore the remaining cold child group");
        vkDestroyBuffer(context.device, resident.handle, nullptr); resident.handle = VK_NULL_HANDLE;
        vkFreeMemory(context.device, resident.memory, nullptr); resident.memory = VK_NULL_HANDLE;
        const auto afterFree = context.stats();
        require(afterFree.coldLogicalBytes == 0 && afterFree.residentBytes == 0,
                "partial-restore retry cleanup left cold or resident virtual bytes");
        std::cout << "PASS: injected partial restore returned OOM, retry restored and verified all bytes, cleanup is empty\n";
        return 0;
    }
    waitCold(context, initial);
    auto cold = context.stats();
    metadataWhileCold(context, resident.handle, cold);
    if (nativeAllocation) probeNativeTokenLifetimes(context, resident.memory, cold);
    std::cout << (nativeAllocation ? "native allocation adopted cold: logical=" : "cold snapshot: logical=")
              << cold.coldLogicalBytes << " stored=" << cold.coldStoredBytes
              << " freezes=" << cold.freezes << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (std::uint32_t cycle = 0; cycle < 2; ++cycle) {
        const auto beforeWake = context.stats();
        const std::uint32_t queueIndex = (twoQueues || twoFamilies || exclusiveFamilies) ? 1u : 0u;
        computeCycle(context, resident.handle, cycle, queueIndex);
        const auto afterWake = context.stats();
        require(afterWake.restores > beforeWake.restores,
                "GPU compute did not restore cold virtual allocation");
        if (bdaMode) {
            VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            addressInfo.buffer = resident.handle;
            require(vkGetBufferDeviceAddress(context.device, &addressInfo) == context.bufferAddress,
                    "buffer device address changed after cold restore");
        }
        readbackAndVerify(context, resident.handle, staging, cycle, cycle == 0);
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
