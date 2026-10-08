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
#include <utility>
#include <vector>

namespace {
constexpr VkDeviceSize MiB = 1024ull * 1024ull;
constexpr VkDeviceSize TotalBytes = 320 * MiB;
constexpr VkDeviceSize BudgetFirstBytes = 256 * MiB;
constexpr VkDeviceSize BudgetSecondBytes = 192 * MiB;
constexpr VkDeviceSize FirstChildBytes = 256 * MiB;
constexpr VkDeviceSize SecondChildBytes = TotalBytes - FirstChildBytes;
constexpr VkDeviceSize PipelineFirstChildBytes = 128 * MiB;
constexpr VkDeviceSize PipelineColdRemainingBytes = TotalBytes - PipelineFirstChildBytes;
constexpr VkDeviceSize ChunkBytes = 32 * MiB;
constexpr std::uint32_t ChunkWords = static_cast<std::uint32_t>(ChunkBytes / 4);
constexpr std::uint32_t TotalWords = static_cast<std::uint32_t>(TotalBytes / 4);
constexpr auto ColdTimeout = std::chrono::seconds(45);
bool zeroPattern = false;

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
    if (zeroPattern && (index & 31u) == 0) return 0;
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
    bool activeSubmit{};
    bool pressureTimeline{};
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
                    bool pending, bool bindWhilePending, bool active, bool pressureWait = false, bool robustCore = false) {
        twoSeparate = twoSeparate || exclusive;
        bdaMode = enableBda;
        nativeAllocation = native; twoQueues = twoSame; twoFamilies = twoSeparate;
        exclusiveFamilies = exclusive; pendingWait = pending; pendingBind = bindWhilePending;
        activeSubmit = active;
        pressureTimeline = pressureWait;
        if (exclusive) twoFamilies = true;
        require(!(twoSame && twoSeparate), "choose only one of --two-queues and --two-families");
        require(!(pending && bindWhilePending), "choose only one pending queue test");
        require(!(pending || bindWhilePending) || twoSame || twoSeparate || exclusive,
                "pending queue tests require a multi-queue mode");
        require(!(pending || bindWhilePending) || native, "pending queue tests require --native-allocation");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = (enableBda || pending || bindWhilePending || active || pressureWait) ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
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
            if (enableBda || pending || bindWhilePending || active || pressureWait) {
                VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                if (enableBda) {
                    features2.pNext = &bdaFeatures;
                    if (pending || bindWhilePending || active || pressureWait) bdaFeatures.pNext = &timelineFeatures;
                } else {
                    features2.pNext = &timelineFeatures;
                }
                vkGetPhysicalDeviceFeatures2(candidate, &features2);
                features = features2.features;
                if (enableBda && (!bdaFeatures.bufferDeviceAddress || !features.shaderInt64)) continue;
                if ((pending || bindWhilePending || active || pressureWait) && !timelineFeatures.timelineSemaphore) continue;
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
        require(physical != VK_NULL_HANDLE, activeSubmit
            ? "UNSUPPORTED: no discrete AMD RADV GPU exposes two queues in one sparse/compute family"
            : "no discrete AMD RADV GPU has the requested sparse/compute queue setup");
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
        VkPhysicalDeviceFeatures enabled{}; enabled.sparseBinding = VK_TRUE; enabled.robustBufferAccess=robustCore;
        VkPhysicalDeviceBufferDeviceAddressFeatures enabledBda{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        VkPhysicalDeviceTimelineSemaphoreFeatures enabledTimeline{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 enabled2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if (enableBda) {
            enabledBda.bufferDeviceAddress = VK_TRUE;
            enabled2.features.sparseBinding = VK_TRUE;
            enabled2.features.shaderInt64 = VK_TRUE; enabled2.features.robustBufferAccess=robustCore;
            enabled2.pNext = &enabledBda;
            if (pendingWait || pendingBind || activeSubmit || pressureTimeline) {
                enabledTimeline.timelineSemaphore = VK_TRUE;
                enabledBda.pNext = &enabledTimeline;
            }
            dci.pNext = &enabled2;
        } else {
            dci.pEnabledFeatures = &enabled;
            if (pendingWait || pendingBind || activeSubmit || pressureTimeline) {
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
        if (pendingWait || pendingBind || activeSubmit || pressureTimeline) {
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
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 8; dp.poolSizeCount = 1; dp.pPoolSizes = &poolSize;
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

struct NativeImage {
    VkDevice device{};
    VkImage handle{};
    VkDeviceMemory memory{};
    bool unsafe{};
    ~NativeImage() {
        if (unsafe) return;
        if (handle) vkDestroyImage(device, handle, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
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

struct TimelineWatchdog {
    VkDevice device{};
    VkSemaphore semaphore{};
    std::uint64_t value{};
    std::atomic<bool> closed{false};
    std::atomic<bool> fired{false};
    std::atomic<bool> attempted{false};
    std::atomic<VkResult> result{VK_NOT_READY};
    std::thread thread;

    TimelineWatchdog(VkDevice d, VkSemaphore s, std::uint64_t v)
        : device(d), semaphore(s), value(v), thread([this] {
              for (int i = 0; i < 200 && !closed.load(); ++i)
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              if (!closed.load()) { fired = true; signalOnce(); }
          }) {}
    TimelineWatchdog(const TimelineWatchdog&) = delete;
    TimelineWatchdog& operator=(const TimelineWatchdog&) = delete;

    void signalOnce() {
        bool expected = false;
        if (!attempted.compare_exchange_strong(expected, true)) return;
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = semaphore; info.value = value;
        result = vkSignalSemaphore(device, &info);
    }
    void cancelAndJoin() {
        closed = true;
        if (thread.joinable()) thread.join();
    }
    ~TimelineWatchdog() {
        if (!closed.exchange(true)) signalOnce();
        if (thread.joinable()) thread.join();
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
    auto requireStillCold = [&] {
        const auto current = context.stats();
        require(current.restores == cold.restores &&
                current.coldLogicalBytes == expectedColdBytes &&
                current.coldStoredBytes == cold.coldStoredBytes &&
                current.residentBytes == cold.residentBytes,
                "idle wait woke or changed the cold allocation");
    };
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
    requireStillCold();
    check(vkQueueWaitIdle(context.queue), "wait idle on primary queue while cold");
    requireStillCold();
    if (context.secondQueue && context.secondQueue != context.queue) {
        check(vkQueueWaitIdle(context.secondQueue), "wait idle on second queue while cold");
        requireStillCold();
    }
    check(vkDeviceWaitIdle(context.device), "wait idle on device while cold");
    requireStillCold();
}

std::uint32_t gpuOnlyNativeType(const Context& context, std::uint32_t bits) {
    for (std::uint32_t i = 0; i < context.memory.memoryTypeCount; ++i) {
        const auto flags = context.memory.memoryTypes[i].propertyFlags;
        if (i != context.virtualType && (bits & (1u << i)) &&
            (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            !(flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                       VK_MEMORY_PROPERTY_PROTECTED_BIT))) return i;
    }
    return UINT32_MAX;
}

void createUnknownResourceImage(const Context& context, NativeImage& image) {
    image.device = context.device;
    VkImageFormatProperties supported{};
    check(vkGetPhysicalDeviceImageFormatProperties(context.physical, VK_FORMAT_R8G8B8A8_UNORM,
          VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &supported),
          "query unknown-resource image format");
    require(supported.maxExtent.width && supported.maxExtent.height && supported.maxExtent.depth &&
            supported.maxMipLevels && supported.maxArrayLayers && (supported.sampleCounts & VK_SAMPLE_COUNT_1_BIT),
            "unknown-resource image format has no 1x1 transfer support");
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = VK_FORMAT_R8G8B8A8_UNORM;
    create.extent = {1, 1, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(context.device, &create, nullptr, &image.handle), "create unknown-resource image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context.device, image.handle, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = gpuOnlyNativeType(context, requirements.memoryTypeBits);
    require(allocation.memoryTypeIndex != UINT32_MAX,
            "unknown-resource image has no compatible native GPU-only memory type");
    check(vkAllocateMemory(context.device, &allocation, nullptr, &image.memory),
          "allocate unknown-resource image memory");
    check(vkBindImageMemory(context.device, image.handle, image.memory, 0),
          "bind unknown-resource image memory");
}

void recordUnknownResourceClear(VkCommandBuffer command, VkImage image) {
    VkImageMemoryBarrier ready{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ready.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ready.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ready.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.image = image;
    ready.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ready.subresourceRange.levelCount = 1;
    ready.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ready);
    const VkClearColorValue color{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
}

struct PendingSubmission {
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkSemaphore timeline{};
    bool submitted{};
    NativeImage* image{};

    VkResult finish() noexcept {
        if (submitted) {
            std::uint64_t value{};
            VkResult result = vkGetSemaphoreCounterValue(device, timeline, &value);
            if (result != VK_SUCCESS) return result;
            if (value < 1) {
                VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
                signal.semaphore = timeline;
                signal.value = 1;
                result = vkSignalSemaphore(device, &signal);
                if (result != VK_SUCCESS) return result;
            }
            result = vkQueueWaitIdle(queue);
            if (result != VK_SUCCESS) return result;
            submitted = false;
        }
        if (command) {
            vkFreeCommandBuffers(device, pool, 1, &command);
            command = VK_NULL_HANDLE;
        }
        return VK_SUCCESS;
    }

    ~PendingSubmission() {
        if (finish() != VK_SUCCESS && image) image->unsafe = true;
    }
};

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
    allocation.pNext = &flags; allocation.allocationSize = 2 * MiB;
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
    allocation.allocationSize = std::max(imageRequirements.size, VkDeviceSize{2 * MiB}); allocation.memoryTypeIndex = imageType;
    check(vkAllocateMemory(context.device, &allocation, nullptr, &probe.imageMemory),
          "allocate native probe image memory");
    VkBindImageMemoryInfo imageBind{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    imageBind.image = probe.image; imageBind.memory = probe.imageMemory;
    check(vkBindImageMemory2(context.device, 1, &imageBind), "bind native probe image memory2");
    require(probe.imageMemory != original, "tiny native image reused original cold allocation token");

    auto rejectPriorResource = [&](VkDeviceMemory memory, const char* kind) {
        for (bool api2 : {false, true}) {
            Buffer storage; storage.device = context.device;
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = MiB; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            check(vkCreateBuffer(context.device, &info, nullptr, &storage.handle), "create mixed-pool storage probe");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(context.device, storage.handle, &requirements);
            require(requirements.size <= MiB && requirements.alignment && MiB % requirements.alignment == 0,
                    "mixed-pool probe does not fit its native allocation");
            VkBindBufferMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};
            bind.buffer = storage.handle; bind.memory = memory; bind.memoryOffset = MiB;
            const auto result = api2 ? vkBindBufferMemory2(context.device, 1, &bind)
                                    : vkBindBufferMemory(context.device, storage.handle, memory, MiB);
            require(result == VK_ERROR_FEATURE_NOT_PRESENT, "adopted native pool after an untracked resource bind");
        }
        std::cout << "native adoption refused after prior " << kind << " binding" << std::endl;
    };
    rejectPriorResource(probe.bufferMemory, "ordinary buffer");
    rejectPriorResource(probe.imageMemory, "image");
    Probe rejected; rejected.device = context.device;
    check(vkCreateBuffer(context.device, &bufferInfo, nullptr, &rejected.buffer), "create post-adoption ordinary buffer");
    require(vkBindBufferMemory(context.device, rejected.buffer, original, 0) == VK_ERROR_FEATURE_NOT_PRESENT,
            "ordinary buffer entered an adopted cold pool");
    check(vkCreateImage(context.device, &imageInfo, nullptr, &rejected.image), "create post-adoption image");
    VkBindImageMemoryInfo rejectedImage{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    rejectedImage.image = rejected.image; rejectedImage.memory = original;
    require(vkBindImageMemory2(context.device, 1, &rejectedImage) == VK_ERROR_FEATURE_NOT_PRESENT,
            "image entered an adopted cold pool");
    rejected.cleanup();
    std::cout << "cold adopted pool refused ordinary buffer/image bindings without waking" << std::endl;

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
                      VkDeviceSize byteSize = TotalBytes, std::uint32_t firstChunk = 0) {
    require(byteSize % ChunkBytes == 0, "compute size must contain whole chunks");
    const std::uint32_t chunkCount = static_cast<std::uint32_t>(byteSize / ChunkBytes);
    for (std::uint32_t localChunk = 0; localChunk < chunkCount; ++localChunk) {
        const std::uint32_t chunk=firstChunk+localChunk;
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

VkResult computePipelineCycle(Context& context, VkBuffer buffer, std::uint32_t cycle,
                              bool expectFailure = false) {
    const VkDeviceSize offsets[2]{96 * MiB, 192 * MiB};
    const VkDeviceSize ranges[2]{64 * MiB, 96 * MiB};
    const std::uint32_t chunks[2]{3, 6};
    require(context.properties.limits.maxStorageBufferRange >= ranges[1],
            "pipeline regression requires a 96 MiB storage-buffer range");
    VkDescriptorSetLayout layouts[2]{context.descriptorLayout, context.descriptorLayout};
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = context.descriptorPool; allocate.descriptorSetCount = 2; allocate.pSetLayouts = layouts;
    VkDescriptorSet sets[2]{};
    check(vkAllocateDescriptorSets(context.device, &allocate, sets), "allocate pipeline descriptor sets");
    VkDescriptorBufferInfo buffers[2]{};
    VkWriteDescriptorSet writes[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        buffers[i] = {buffer, offsets[i], ranges[i]};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = sets[i]; writes[i].dstBinding = 0; writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(context.device, 2, writes, 0, nullptr);
    return context.submit([&](VkCommandBuffer command) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        for (std::size_t i = 0; i < 2; ++i) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipelineLayout,
                                    0, 1, &sets[i], 0, nullptr);
            const std::uint32_t push[3]{ChunkWords, cycleSalt(cycle, chunks[i]), 1};
            vkCmdPushConstants(command, context.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(push), push);
            vkCmdDispatch(command, ChunkWords / 256, 1, 1);
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        }
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
    }, 0, expectFailure ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS);
}

VkResult readbackAndVerify(Context& context, VkBuffer buffer, Staging& staging,
                       int cycle, bool releaseForCompute = false,
                       VkDeviceSize byteSize = TotalBytes,
                       std::uint32_t queueIndex = 0, std::uint32_t firstChunk = 0,
                       VkResult expectedFailure = VK_SUCCESS) {
    auto* actual = static_cast<const std::uint32_t*>(staging.mapped);
    require(byteSize % ChunkBytes == 0, "readback size must contain whole chunks");
    const std::uint32_t chunkCount = static_cast<std::uint32_t>(byteSize / ChunkBytes);
    for (std::uint32_t localChunk = 0; localChunk < chunkCount; ++localChunk) {
        const std::uint32_t chunk=firstChunk+localChunk;
        const VkDeviceSize offset = static_cast<VkDeviceSize>(chunk) * ChunkBytes;
        const VkResult result = context.submit([&](VkCommandBuffer command) {
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
        }, queueIndex, expectedFailure);
        if (expectedFailure != VK_SUCCESS) return result;
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
    return VK_SUCCESS;
}

void pipelineRestoreCheck(Context& context, Buffer& resident, Staging& staging,
                          const ZvramSnapshotStatsNX& initial, bool injectFailure) {
    require((!injectFailure || !context.nativeAllocation) && !context.bdaMode,
            "pipeline fault injection requires synthetic descriptor-tracked memory");
    require(context.properties.limits.maxStorageBufferRange >= 96 * MiB,
            "pipeline check requires a 96 MiB storage-buffer range");
    waitCold(context, initial);
    const auto cold = context.stats();
    require(cold.coldLogicalBytes == TotalBytes && cold.residentBytes == 0 &&
            cold.freezes >= initial.freezes + 3,
            "pipeline check did not cold all three 128/128/64 MiB groups");
    metadataWhileCold(context, resident.handle, cold);

    ZvramSnapshotStatsNX beforeRetry = cold;
    if (injectFailure) {
        require(context.armRestoreFailure != nullptr, "pipeline restore fault-arm interface unavailable");
        check(context.armRestoreFailure(context.device, 1), "arm pipeline partial restore failure");
        const auto first = computePipelineCycle(context, resident.handle, 0, true);
        require(first == VK_ERROR_OUT_OF_DEVICE_MEMORY,
                "pipeline fault did not reject the first multi-range submission with OOM");
        beforeRetry = context.stats();
        require(beforeRetry.failures > cold.failures &&
                beforeRetry.lastError == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                beforeRetry.restores > cold.restores &&
                beforeRetry.residentBytes == PipelineFirstChildBytes &&
                beforeRetry.coldLogicalBytes == PipelineColdRemainingBytes,
                "faulted pipeline restore did not leave one 128 MiB group resident and 192 MiB cold");
        std::cout << "PIPELINE_PARTIAL_FAULT resident=" << beforeRetry.residentBytes
                  << " cold=" << beforeRetry.coldLogicalBytes << std::endl;
    }

    check(computePipelineCycle(context, resident.handle, 0), "submit/retry multi-range pipeline workload");
    for (std::uint32_t chunk = 0; chunk < TotalBytes / ChunkBytes; ++chunk) {
        const int cycle = (chunk == 3 || chunk == 6) ? 0 : -1;
        check(readbackAndVerify(context, resident.handle, staging, cycle, false, ChunkBytes, 0, chunk),
              "verify pipeline-restored child bytes");
    }
    const auto restored = context.stats();
    require(restored.restores > beforeRetry.restores && restored.coldLogicalBytes == 0 &&
            restored.residentBytes == TotalBytes,
            "pipeline submission did not restore and retain all three groups");

    vkDestroyBuffer(context.device, resident.handle, nullptr); resident.handle = VK_NULL_HANDLE;
    vkFreeMemory(context.device, resident.memory, nullptr); resident.memory = VK_NULL_HANDLE;
    const auto empty = context.stats();
    require(empty.coldLogicalBytes == 0 && empty.residentBytes == 0,
            "pipeline regression cleanup retained cold or resident bytes");
    std::cout << (injectFailure ?
        "PASS: lookahead joined partial restore failure; retry restored three groups and verified all bytes\n" :
        "PASS: one tracked multi-range submission restored three groups and verified all bytes\n");
}

void suballocationCheck(Context& context, bool automatic, bool api2) {
    constexpr VkDeviceSize PoolBytes = 512 * MiB;
    constexpr VkDeviceSize ABytes = 192 * MiB, AOffset = 128 * MiB;
    constexpr VkDeviceSize BBytes = 128 * MiB, BOffset = 352 * MiB;
    require(context.memory.memoryHeaps[context.memory.memoryTypes[context.virtualType].heapIndex].size == PoolBytes,
            "suballocation check requires a 512 MiB virtual heap");
    Buffer pool; pool.device = context.device;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = PoolBytes; allocation.memoryTypeIndex = context.virtualType;
    if (context.bdaMode) allocation.pNext = &flags;
    auto quotaCharged = [&] {
        VkMemoryAllocateInfo probe = allocation; probe.allocationSize = MiB;
        probe.memoryTypeIndex = context.virtualType;
        VkDeviceMemory extra{};
        const auto result = vkAllocateMemory(context.device, &probe, nullptr, &extra);
        if (result == VK_SUCCESS) vkFreeMemory(context.device, extra, nullptr);
        if (context.nativeAllocation) {
            require(result == VK_SUCCESS, "native pool consumed synthetic heap quota");
            const auto stats = context.stats();
            require(stats.residentBytes + stats.coldLogicalBytes == PoolBytes,
                    "native pool accounting lost the retained allocation");
        } else require(result == VK_ERROR_OUT_OF_DEVICE_MEMORY, "shared pool stopped charging its logical heap quota");
    };
    Buffer a, b, c;
    std::uint32_t commonTypes = UINT32_MAX;
    auto create = [&](Buffer& buffer, VkDeviceSize size) {
        buffer.device = context.device;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const std::uint32_t families[]{context.family, context.secondFamily};
        if (context.twoFamilies) {
            info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = 2; info.pQueueFamilyIndices = families;
        }
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (context.bdaMode) info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        check(vkCreateBuffer(context.device, &info, nullptr, &buffer.handle), "create pool buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(context.device, buffer.handle, &requirements);
        require(requirements.size == size && (requirements.memoryTypeBits & (1u << context.virtualType)),
                "pool buffer has incompatible requirements");
        commonTypes &= requirements.memoryTypeBits;
    };
    auto bind = [&](VkBuffer buffer, VkDeviceSize offset) {
        if (!api2) return vkBindBufferMemory(context.device, buffer, pool.memory, offset);
        VkBindBufferMemoryInfo info{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};
        info.buffer = buffer; info.memory = pool.memory; info.memoryOffset = offset;
        return vkBindBufferMemory2(context.device, 1, &info);
    };
    create(a, ABytes); create(b, BBytes); create(c, ChunkBytes);
    if (context.nativeAllocation) {
        allocation.memoryTypeIndex = gpuOnlyNativeType(context, commonTypes);
        require(allocation.memoryTypeIndex != UINT32_MAX, "no shared native GPU-only pool memory type");
    }
    check(vkAllocateMemory(context.device, &allocation, nullptr, &pool.memory), "allocate shared pool");
    std::cout << "pool memory type=" << allocation.memoryTypeIndex
              << (context.nativeAllocation ? " native" : " synthetic") << std::endl;
    if (api2) {
        VkBindBufferMemoryInfo infos[2]{};
        for (auto& info : infos) { info.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO; info.memory = pool.memory; }
        infos[0].buffer = a.handle; infos[0].memoryOffset = AOffset;
        infos[1].buffer = b.handle; infos[1].memoryOffset = BOffset;
        check(vkBindBufferMemory2(context.device, 2, infos), "bind two pool slices with API2");
    } else {
        check(bind(a.handle, AOffset), "bind pool slice across child boundary");
        check(bind(b.handle, BOffset), "bind second pool slice");
    }
    require(bind(c.handle, AOffset + ChunkBytes) == VK_ERROR_FEATURE_NOT_PRESENT,
            "live overlapping sparse pool ranges were not rejected");
    // A failed API2 bind can leave its buffer indeterminate; recreate before retrying.
    vkDestroyBuffer(context.device, c.handle, nullptr); c.handle = VK_NULL_HANDLE;
    create(c, ChunkBytes);
    check(bind(c.handle, 0), "bind replacement buffer to unused pool range");
    quotaCharged();

    Staging staging; staging.device = context.device;
    VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stagingInfo.size = ChunkBytes; stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(vkCreateBuffer(context.device, &stagingInfo, nullptr, &staging.buffer), "create pool staging buffer");
    VkMemoryRequirements stagingRequirements{};
    vkGetBufferMemoryRequirements(context.device, staging.buffer, &stagingRequirements);
    VkMemoryAllocateInfo stagingAllocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    stagingAllocation.allocationSize = stagingRequirements.size;
    stagingAllocation.memoryTypeIndex = hostCoherentType(context, stagingRequirements.memoryTypeBits);
    require(stagingAllocation.memoryTypeIndex != UINT32_MAX, "no pool staging memory type");
    check(vkAllocateMemory(context.device, &stagingAllocation, nullptr, &staging.memory), "allocate pool staging");
    check(vkBindBufferMemory(context.device, staging.buffer, staging.memory, 0), "bind pool staging");
    check(vkMapMemory(context.device, staging.memory, 0, ChunkBytes, 0, &staging.mapped), "map pool staging");
    auto address = [&](VkBuffer buffer) {
        if (!context.bdaMode) return VkDeviceAddress{0};
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; info.buffer = buffer;
        const auto value = vkGetBufferDeviceAddress(context.device, &info);
        require(value != 0, "pool buffer has no device address"); return value;
    };
    auto compute = [&](VkBuffer buffer, std::uint32_t cycle, VkDeviceSize size) {
        context.bufferAddress = address(buffer);
        check(computeCycle(context, buffer, cycle, (context.twoQueues || context.twoFamilies) ? 1u : 0u,
                           false, size), "compute pool slice");
    };
    const auto aAddress = address(a.handle), bAddress = address(b.handle), cAddress = address(c.handle);
    upload(context, a.handle, staging, ABytes); upload(context, b.handle, staging, BBytes);
    upload(context, c.handle, staging, ChunkBytes);
    compute(a.handle, 0, ABytes); compute(b.handle, 0, BBytes); compute(b.handle, 1, BBytes);
    readbackAndVerify(context, a.handle, staging, 0, false, ABytes);
    readbackAndVerify(context, b.handle, staging, 1, false, BBytes);
    readbackAndVerify(context, c.handle, staging, -1, false, ChunkBytes);
    auto waitCold = [&] {
        const auto deadline = std::chrono::steady_clock::now() + ColdTimeout;
        ZvramSnapshotStatsNX cold{};
        do {
            cold = context.stats();
            if (cold.coldLogicalBytes == PoolBytes && cold.residentBytes == 0 && cold.coldStoredBytes > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < deadline);
        require(cold.coldLogicalBytes == PoolBytes && cold.residentBytes == 0 && cold.coldStoredBytes > 0,
                "shared pool did not become fully cold");
        std::cout << "shared pool cold: logical=" << cold.coldLogicalBytes << " stored=" << cold.coldStoredBytes << std::endl;
    };
    if (automatic) {
        waitCold();
        require(address(a.handle) == aAddress && address(b.handle) == bAddress && address(c.handle) == cAddress,
                "cold pool changed a buffer device address");
        if (context.nativeAllocation) probeNativeTokenLifetimes(context, pool.memory, context.stats());
    }
    compute(a.handle, 1, ABytes); compute(b.handle, 2, BBytes); compute(c.handle, 0, ChunkBytes);
    readbackAndVerify(context, a.handle, staging, 1, false, ABytes);
    readbackAndVerify(context, b.handle, staging, 2, false, BBytes);
    readbackAndVerify(context, c.handle, staging, 0, false, ChunkBytes);
    vkDestroyBuffer(context.device, a.handle, nullptr); a.handle = VK_NULL_HANDLE;
    quotaCharged();
    create(a, ABytes); check(bind(a.handle, AOffset), "rebind pool range after buffer destruction");
    readbackAndVerify(context, a.handle, staging, 1, false, ABytes);
    readbackAndVerify(context, b.handle, staging, 2, false, BBytes);
    std::cout << "shared pool range contents survived buffer destruction/rebind" << std::endl;
    if (automatic) {
        for (Buffer* buffer : {&a, &b, &c}) {
            vkDestroyBuffer(context.device, buffer->handle, nullptr); buffer->handle = VK_NULL_HANDLE;
        }
        quotaCharged();
        waitCold();
        create(a, ABytes); create(b, BBytes); create(c, ChunkBytes);
        check(bind(a.handle, AOffset), "rebind cold pool without live buffers");
        check(bind(b.handle, BOffset), "rebind second retained cold range");
        check(bind(c.handle, 0), "rebind third retained cold range");
        readbackAndVerify(context, a.handle, staging, 1, false, ABytes);
        readbackAndVerify(context, b.handle, staging, 2, false, BBytes);
        readbackAndVerify(context, c.handle, staging, 0, false, ChunkBytes);
        std::cout << "shared pool cold contents survived destruction of every buffer" << std::endl;
    }
    vkFreeMemory(context.device, pool.memory, nullptr); pool.memory = VK_NULL_HANDLE;
    quotaCharged();
    unsigned destroyed = 0;
    for (Buffer* buffer : {&a, &b, &c}) {
        vkDestroyBuffer(context.device, buffer->handle, nullptr); buffer->handle = VK_NULL_HANDLE;
        if (++destroyed < 3) quotaCharged();
    }
    if (automatic) {
        const auto empty = context.stats();
        require(empty.residentBytes == 0 && empty.coldLogicalBytes == 0 && empty.coldStoredBytes == 0,
                "shared pool cleanup retained resident or cold bytes");
    }
    check(vkAllocateMemory(context.device, &allocation, nullptr, &pool.memory), "reuse pool allocation after cleanup");
    std::cout << "PASS: shared 512 MiB " << (context.nativeAllocation ? "native" : "synthetic")
              << " allocation, nonzero offsets, compute/readback, overlap refusal, rebind persistence, accounting recovery" << std::endl;
}

void selectiveBindCheck(Context& context, bool api2) {
    Buffer a, b;
    a.device = b.device = context.device;
    auto create = [&](Buffer& buffer) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = ChunkBytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(context.device, &info, nullptr, &buffer.handle), "create selective-bind buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(context.device, buffer.handle, &req);
        require(req.size == ChunkBytes && req.alignment && ChunkBytes % req.alignment == 0,
                "selective-bind buffer requirements are not one aligned chunk");
        require(req.memoryTypeBits & (1u << context.virtualType),
                "selective-bind buffer lacks virtual memory type");
        return req;
    };
    const auto reqA = create(a), reqB = create(b);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &flags;
    allocation.allocationSize = reqA.size;
    allocation.memoryTypeIndex = context.nativeAllocation
        ? gpuOnlyNativeType(context, reqA.memoryTypeBits & reqB.memoryTypeBits)
        : context.virtualType;
    require(allocation.memoryTypeIndex != UINT32_MAX, "no compatible selective-bind allocation type");
    check(vkAllocateMemory(context.device, &allocation, nullptr, &a.memory), "allocate first selective pool");
    allocation.allocationSize = reqB.size;
    check(vkAllocateMemory(context.device, &allocation, nullptr, &b.memory), "allocate second selective pool");
    auto bind = [&](VkBuffer buffer, VkDeviceMemory memory) {
        if (!api2) return vkBindBufferMemory(context.device, buffer, memory, 0);
        VkBindBufferMemoryInfo info{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};
        info.buffer = buffer; info.memory = memory;
        return vkBindBufferMemory2(context.device, 1, &info);
    };
    check(bind(a.handle, a.memory), "bind first selective pool");
    check(bind(b.handle, b.memory), "bind second selective pool");

    Staging staging; staging.device = context.device;
    VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stagingInfo.size = ChunkBytes;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(context.device, &stagingInfo, nullptr, &staging.buffer), "create selective staging buffer");
    VkMemoryRequirements stagingReq{};
    vkGetBufferMemoryRequirements(context.device, staging.buffer, &stagingReq);
    VkMemoryAllocateInfo stagingAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    stagingAlloc.allocationSize = stagingReq.size;
    stagingAlloc.memoryTypeIndex = hostCoherentType(context, stagingReq.memoryTypeBits);
    require(stagingAlloc.memoryTypeIndex != UINT32_MAX, "no selective-bind staging type");
    check(vkAllocateMemory(context.device, &stagingAlloc, nullptr, &staging.memory), "allocate selective staging");
    check(vkBindBufferMemory(context.device, staging.buffer, staging.memory, 0), "bind selective staging");
    check(vkMapMemory(context.device, staging.memory, 0, ChunkBytes, 0, &staging.mapped), "map selective staging");

    upload(context, a.handle, staging, ChunkBytes);
    upload(context, b.handle, staging, ChunkBytes);
    check(computeCycle(context, a.handle, 0, 0, false, ChunkBytes), "write first distinct pattern");
    check(computeCycle(context, b.handle, 0, 0, false, ChunkBytes), "start second distinct pattern");
    check(computeCycle(context, b.handle, 1, 0, false, ChunkBytes), "finish second distinct pattern");
    readbackAndVerify(context, a.handle, staging, 0, false, ChunkBytes);
    readbackAndVerify(context, b.handle, staging, 1, false, ChunkBytes);

    const auto beforeCold = context.stats();
    vkDestroyBuffer(context.device, a.handle, nullptr); a.handle = VK_NULL_HANDLE;
    vkDestroyBuffer(context.device, b.handle, nullptr); b.handle = VK_NULL_HANDLE;
    const auto deadline = std::chrono::steady_clock::now() + ColdTimeout;
    ZvramSnapshotStatsNX cold{};
    do {
        cold = context.stats();
        if (cold.coldLogicalBytes == reqA.size + reqB.size && cold.residentBytes == 0 &&
            cold.freezes >= beforeCold.freezes + 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    require(cold.coldLogicalBytes == reqA.size + reqB.size && cold.residentBytes == 0 &&
            cold.freezes >= beforeCold.freezes + 2,
            "destroyed selective pools did not both reach cold snapshots");

    Buffer reboundA; reboundA.device = context.device;
    const auto reboundReqA = create(reboundA);
    require(reboundReqA.size == reqA.size, "first pool rebind requirements changed");
    require(context.armRestoreFailure != nullptr, "selective restore fault-arm interface unavailable");
    check(context.armRestoreFailure(context.device, 1), "arm selective restore failure check");
    check(bind(reboundA.handle, a.memory), "bind first cold pool only");
    const auto firstAwake = context.stats();
    require(firstAwake.residentBytes == reqA.size && firstAwake.coldLogicalBytes == reqB.size &&
            firstAwake.restores > cold.restores,
            "binding the first pool did not wake only its own allocation");
    require(firstAwake.failures == cold.failures,
            "restore fault was incorrectly attributed to the other cold allocation");

    Buffer metadata; metadata.device = context.device;
    const auto metadataReq = create(metadata);
    metadataWhileCold(context, metadata.handle, firstAwake, reqB.size, metadataReq.size);
    vkDestroyBuffer(context.device, metadata.handle, nullptr); metadata.handle = VK_NULL_HANDLE;
    const auto stillSelective = context.stats();
    require(stillSelective.residentBytes == reqA.size && stillSelective.coldLogicalBytes == reqB.size &&
            stillSelective.restores == firstAwake.restores,
            "metadata or idle waits woke the independent cold pool");

    Buffer reboundB; reboundB.device = context.device;
    const auto reboundReqB = create(reboundB);
    require(reboundReqB.size == reqB.size, "second pool rebind requirements changed");
    check(bind(reboundB.handle, b.memory), "bind second cold pool");
    const auto bothAwake = context.stats();
    require(bothAwake.residentBytes == reqA.size + reqB.size && bothAwake.coldLogicalBytes == 0 &&
            bothAwake.restores > stillSelective.restores,
            "binding the second pool did not restore its allocation");
    check(computeCycle(context, reboundA.handle, 1, 0, false, ChunkBytes), "verify first pool after both restores");
    check(computeCycle(context, reboundB.handle, 2, 0, false, ChunkBytes), "verify second pool after restore");
    readbackAndVerify(context, reboundA.handle, staging, 1, false, ChunkBytes);
    readbackAndVerify(context, reboundB.handle, staging, 2, false, ChunkBytes);

    vkDestroyBuffer(context.device, reboundA.handle, nullptr); reboundA.handle = VK_NULL_HANDLE;
    vkDestroyBuffer(context.device, reboundB.handle, nullptr); reboundB.handle = VK_NULL_HANDLE;
    vkFreeMemory(context.device, a.memory, nullptr); a.memory = VK_NULL_HANDLE;
    vkFreeMemory(context.device, b.memory, nullptr); b.memory = VK_NULL_HANDLE;
    const auto empty = context.stats();
    require(empty.residentBytes == 0 && empty.coldLogicalBytes == 0 && empty.coldStoredBytes == 0,
            "selective-bind cleanup retained pool bytes");
    std::cout << "PASS: " << (context.nativeAllocation ? "native" : "synthetic")
              << (api2 ? " API2" : " legacy")
              << " independent pools froze cold, woke selectively, and preserved distinct full-byte patterns" << std::endl;
}

void rangeSubmitCheck(Context& context, bool pressure, bool cleanCache = false,
                      bool cacheQuota = false, bool cacheBootstrap = false,
                      bool compressedInitial = false) {
    const std::uint32_t chunkCount = cacheQuota ? 3u : 2u;
    const VkDeviceSize Bytes=static_cast<VkDeviceSize>(chunkCount)*ChunkBytes;
    Buffer pool; pool.device=context.device;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size=Bytes; bufferInfo.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(vkCreateBuffer(context.device,&bufferInfo,nullptr,&pool.handle),"create range pool");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(context.device,pool.handle,&req);
    require(req.size==Bytes,"range check requires two exact 32 MiB children");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=req.size;
    allocation.memoryTypeIndex=context.nativeAllocation?gpuOnlyNativeType(context,req.memoryTypeBits):context.virtualType;
    require(allocation.memoryTypeIndex!=UINT32_MAX,"no compatible range memory type");
    check(vkAllocateMemory(context.device,&allocation,nullptr,&pool.memory),"allocate range pool");
    check(vkBindBufferMemory(context.device,pool.handle,pool.memory,0),"bind range pool");
    Staging staging; staging.device=context.device;
    bufferInfo.size=ChunkBytes; bufferInfo.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(vkCreateBuffer(context.device,&bufferInfo,nullptr,&staging.buffer),"create range staging");
    vkGetBufferMemoryRequirements(context.device,staging.buffer,&req);
    allocation.allocationSize=req.size; allocation.memoryTypeIndex=hostCoherentType(context,req.memoryTypeBits);
    check(vkAllocateMemory(context.device,&allocation,nullptr,&staging.memory),"allocate range staging");
    check(vkBindBufferMemory(context.device,staging.buffer,staging.memory,0),"bind range staging");
    check(vkMapMemory(context.device,staging.memory,0,ChunkBytes,0,&staging.mapped),"map range staging");
    upload(context,pool.handle,staging,Bytes);
    if (!compressedInitial)
        check(computeCycle(context,pool.handle,0,0,false,Bytes),"initialize both range chunks");
    if (cacheBootstrap) {
        require(chunkCount == 2 && cleanCache && pressure,
                "cache bootstrap requires two pressured clean-cache ranges");
        const auto deadline=std::chrono::steady_clock::now()+ColdTimeout;
        bool partialCold=false;
        while(std::chrono::steady_clock::now()<deadline) {
            const auto before=context.stats();
            require(before.failures==0,"snapshot failed during cache bootstrap");
            require(!(before.coldLogicalBytes==Bytes && before.residentBytes==0),
                    "both ranges became cold before preserving one warm");
            if(before.coldLogicalBytes==ChunkBytes && before.residentBytes==ChunkBytes) {
                // Touch child 1. If it was the cold child, this also restores it,
                // leaving child 0 cold while child 1 is now certainly resident.
                readbackAndVerify(context,pool.handle,staging,0,false,ChunkBytes,0,1);
                const auto after=context.stats();
                if(after.coldLogicalBytes==ChunkBytes && after.residentBytes==ChunkBytes) {
                    partialCold=true;
                    break;
                }
            } else {
                readbackAndVerify(context,pool.handle,staging,0,false,ChunkBytes,0,1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(partialCold,"bootstrap did not keep child 1 warm while child 0 became cold");
        const auto beforeBootstrapRestore=context.stats();
        std::cout<<"CACHE_BOOTSTRAP_PREARM_BEGIN"<<std::endl;
        readbackAndVerify(context,pool.handle,staging,0,false,ChunkBytes,0,0);
        const auto afterBootstrapRestore=context.stats();
        require(afterBootstrapRestore.restores>beforeBootstrapRestore.restores &&
                afterBootstrapRestore.coldLogicalBytes==0 && afterBootstrapRestore.failures==0,
                "pre-arm readback did not restore and verify the cold child");
        std::cout<<"CACHE_BOOTSTRAP_PREARM_END"<<std::endl;

        const auto fullColdDeadline=std::chrono::steady_clock::now()+ColdTimeout;
        ZvramSnapshotStatsNX fullCold{};
        while(std::chrono::steady_clock::now()<fullColdDeadline) {
            fullCold=context.stats();
            if(fullCold.coldLogicalBytes==Bytes && fullCold.residentBytes==0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(fullCold.coldLogicalBytes==Bytes && fullCold.residentBytes==0 && fullCold.failures==0,
                "bootstrap ranges did not reach full cold state");
        std::cout<<"CACHE_BOOTSTRAP_ARMED_BEGIN"<<std::endl;
        readbackAndVerify(context,pool.handle,staging,0,false,ChunkBytes,0,0);
        const auto afterArmedRestore=context.stats();
        require(afterArmedRestore.restores>fullCold.restores && afterArmedRestore.failures==0,
                "first post-bootstrap readback did not restore a cold child");

        const auto reuseDeadline=std::chrono::steady_clock::now()+ColdTimeout;
        ZvramSnapshotStatsNX reused{};
        while(std::chrono::steady_clock::now()<reuseDeadline) {
            reused=context.stats();
            if(reused.coldLogicalBytes==Bytes && reused.residentBytes==0 && reused.freezes>fullCold.freezes) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(reused.coldLogicalBytes==Bytes && reused.residentBytes==0 &&
                reused.freezes>fullCold.freezes && reused.failures==0,
                "clean cache did not refreeze after resident admission armed");
        std::cout<<"CACHE_BOOTSTRAP_ARMED_END"<<std::endl;
        std::cout<<"PASS: bootstrap discarded redundant clean copies, then retained and reused cache after arming"<<std::endl;
        return;
    }
    if (pressure) {
        auto requirePressure = [&] {
            const auto stats = context.stats();
            const auto expectedResident = cacheQuota ? 2*ChunkBytes : ChunkBytes;
            require(stats.residentBytes == expectedResident &&
                    stats.coldLogicalBytes == Bytes-expectedResident &&
                    stats.failures == 0, "range pressure did not retain the expected resident chunks");
        };
        requirePressure();
        if (cleanCache) {
            std::cout << "CLEAN_CACHE_READBACK_PHASE_BEGIN" << std::endl;
            for (std::uint32_t pass = 0; pass < 3; ++pass) {
                for (std::uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
                    readbackAndVerify(context, pool.handle, staging, 0, false, ChunkBytes, 0, chunk);
                    requirePressure();
                    std::cout << "CLEAN_CACHE_READBACK pass=" << pass
                              << " chunk=" << chunk << std::endl;
                }
            }
            std::cout << "CLEAN_CACHE_READBACK_PHASE_END" << std::endl;
        }
        for (std::uint32_t cycle = 1; cycle <= 3; ++cycle) {
            for (std::uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
                check(computeCycle(context, pool.handle, cycle, 0, false, ChunkBytes, chunk),
                      "compute pressured range chunk");
                if (cleanCache)
                    std::cout << "CLEAN_CACHE_SHADER_WRITE cycle=" << cycle
                              << " chunk=" << chunk << std::endl;
                requirePressure();
                readbackAndVerify(context, pool.handle, staging, cycle, false, ChunkBytes, 0, chunk);
                requirePressure();
            }
        }
        const auto refuseWholeBuffer = [&](const char* operation, auto&& record) {
            const auto result = context.submit(std::forward<decltype(record)>(record), 0,
                                               VK_ERROR_OUT_OF_DEVICE_MEMORY);
            require(result == VK_ERROR_OUT_OF_DEVICE_MEMORY, operation);
            requirePressure();
        };
        refuseWholeBuffer("whole-buffer fill was not refused under the resident cap",
            [&](VkCommandBuffer command) {
                vkCmdFillBuffer(command, pool.handle, 0, Bytes, 0xfeedfaceu);
            });
        for (std::uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
            readbackAndVerify(context, pool.handle, staging, 3, false, ChunkBytes, 0, chunk);
            requirePressure();
        }

        VkEvent event{};
        VkEventCreateInfo eventInfo{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        check(vkCreateEvent(context.device, &eventInfo, nullptr, &event), "create pressure marker event");
        // Event-only markers access no allocation. They remain legal under a
        // resident cap while the whole-buffer fill above is conservatively refused.
        const auto beforeMarker=context.stats();
        check(context.submit([&](VkCommandBuffer command) {
            vkCmdSetEvent(command, event, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }), "submit resource-free marker under resident cap");
        const auto afterMarker=context.stats();
        require(afterMarker.restores==beforeMarker.restores &&
                afterMarker.residentBytes==beforeMarker.residentBytes &&
                afterMarker.coldLogicalBytes==beforeMarker.coldLogicalBytes,
                "resource-free marker restored cold chunks");
        requirePressure();
        vkDestroyEvent(context.device, event, nullptr);
        for (std::uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
            readbackAndVerify(context, pool.handle, staging, 3, false, ChunkBytes, 0, chunk);
            requirePressure();
        }

        if (context.twoQueues) {
            VkDescriptorBufferInfo info{pool.handle, 0, ChunkBytes};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = context.descriptor; write.dstBinding = 0; write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &info;
            vkUpdateDescriptorSets(context.device, 1, &write, 0, nullptr);
            const std::uint32_t cycle = 4;
            const std::uint32_t push[3]{ChunkWords, cycleSalt(cycle, 0), 1};
            VkCommandBufferAllocateInfo commandAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commandAlloc.commandPool = context.secondCommands;
            commandAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commandAlloc.commandBufferCount = 1;
            VkCommandBuffer pendingCommand{};
            check(vkAllocateCommandBuffers(context.device, &commandAlloc, &pendingCommand),
                  "allocate pending range command");
            try {
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                check(vkBeginCommandBuffer(pendingCommand, &begin), "begin pending range command");
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(pendingCommand, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
                vkCmdBindPipeline(pendingCommand, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipeline);
                vkCmdBindDescriptorSets(pendingCommand, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        context.pipelineLayout, 0, 1, &context.descriptor, 0, nullptr);
                vkCmdPushConstants(pendingCommand, context.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                                   0, sizeof(push), push);
                vkCmdDispatch(pendingCommand, ChunkWords / 256, 1, 1);
                check(vkEndCommandBuffer(pendingCommand), "end pending range command");

                const std::uint64_t waitValue = 1;
                VkTimelineSemaphoreSubmitInfo waitValues{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
                waitValues.waitSemaphoreValueCount = 1; waitValues.pWaitSemaphoreValues = &waitValue;
                const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo pendingInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                pendingInfo.pNext = &waitValues;
                pendingInfo.waitSemaphoreCount = 1; pendingInfo.pWaitSemaphores = &context.pendingTimeline;
                pendingInfo.pWaitDstStageMask = &waitStage;
                pendingInfo.commandBufferCount = 1; pendingInfo.pCommandBuffers = &pendingCommand;
                TimelineWatchdog watchdog(context.device, context.pendingTimeline, waitValue);
                check(vkQueueSubmit(context.secondQueue, 1, &pendingInfo, VK_NULL_HANDLE),
                      "submit pending first-range compute");
                const auto refusedRead = readbackAndVerify(context, pool.handle, staging, 3,
                    false, ChunkBytes, 0, 1, VK_ERROR_OUT_OF_DEVICE_MEMORY);
                require(refusedRead == VK_ERROR_OUT_OF_DEVICE_MEMORY,
                        "in-flight first range did not block second-range readback");
                VkTimelineSemaphoreSubmitInfo signalValues{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
                signalValues.signalSemaphoreValueCount = 1; signalValues.pSignalSemaphoreValues = &waitValue;
                VkSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                signalInfo.pNext = &signalValues; signalInfo.signalSemaphoreCount = 1;
                signalInfo.pSignalSemaphores = &context.pendingTimeline;
                const auto signalResult = vkQueueSubmit(context.queue, 1, &signalInfo, VK_NULL_HANDLE);
                const auto idleResult = vkQueueWaitIdle(context.secondQueue);
                watchdog.cancelAndJoin();
                if (idleResult != VK_SUCCESS) {
                    vkDeviceWaitIdle(context.device);
                    check(idleResult, "finish pending range queue");
                }
                check(signalResult, "signal pending range timeline from first queue");
                check(idleResult, "finish pending range queue");
                vkFreeCommandBuffers(context.device, context.secondCommands, 1, &pendingCommand);
                pendingCommand = VK_NULL_HANDLE;
                readbackAndVerify(context, pool.handle, staging, 4, false, ChunkBytes, 0, 0);
                requirePressure();
                readbackAndVerify(context, pool.handle, staging, 3, false, ChunkBytes, 0, 1);
                requirePressure();
            } catch (...) {
                if (pendingCommand) {
                    vkDeviceWaitIdle(context.device);
                    vkFreeCommandBuffers(context.device, context.secondCommands, 1, &pendingCommand);
                }
                throw;
            }
        }
        std::cout << "PASS: resident-cap range pressure evicted completed chunks, refused broad/unknown submissions, and preserved every byte" << std::endl;
        vkDestroyBuffer(context.device,pool.handle,nullptr);pool.handle=VK_NULL_HANDLE;
        vkFreeMemory(context.device,pool.memory,nullptr);pool.memory=VK_NULL_HANDLE;
        const auto empty=context.stats();
        require(!empty.residentBytes && !empty.coldLogicalBytes && !empty.coldStoredBytes && !empty.failures,
                "range pressure cleanup retained backing or errors");
        return;
    }
    auto waitCold=[&] {
        const auto deadline=std::chrono::steady_clock::now()+ColdTimeout;
        while(std::chrono::steady_clock::now()<deadline) {
            const auto stats=context.stats();
            if(stats.coldLogicalBytes==Bytes && stats.residentBytes==0 && stats.failures==0) return stats;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("range pool failed to reach two cold chunks");
    };
    const auto cold=waitCold();
    const int firstCycle = compressedInitial ? 0 : 1;
    check(computeCycle(context,pool.handle,firstCycle,0,false,ChunkBytes),"restore first descriptor range");
    auto first=context.stats();
    require(first.coldLogicalBytes==ChunkBytes && first.residentBytes==ChunkBytes && first.restores==cold.restores+1 && !first.failures,
            "first descriptor range woke its cold neighbour");
    readbackAndVerify(context,pool.handle,staging,firstCycle,false,ChunkBytes);
    require(context.stats().coldLogicalBytes==ChunkBytes,"first transfer range woke its cold neighbour");
    check(computeCycle(context,pool.handle,firstCycle,0,false,ChunkBytes,1),"restore second descriptor range");
    auto both=context.stats();
    require(both.coldLogicalBytes==0 && both.residentBytes==Bytes && both.restores==cold.restores+2 && !both.failures,
            "second descriptor range did not restore independently");
    readbackAndVerify(context,pool.handle,staging,firstCycle,false,Bytes);
    const auto again=waitCold();
    check(computeCycle(context,pool.handle,firstCycle+1,0,false,ChunkBytes),"restore first range a second time");
    require(context.stats().coldLogicalBytes==ChunkBytes && context.stats().restores==again.restores+1,
            "repeated first-range wake restored neighbour");
    readbackAndVerify(context,pool.handle,staging,firstCycle+1,false,ChunkBytes);
    readbackAndVerify(context,pool.handle,staging,firstCycle,false,ChunkBytes,0,1);
    std::cout<<"PASS: two 32 MiB chunks in one live buffer restored independently; descriptor and transfer ranges preserved every byte across two cold cycles\n";
    vkDestroyBuffer(context.device,pool.handle,nullptr);pool.handle=VK_NULL_HANDLE;
    vkFreeMemory(context.device,pool.memory,nullptr);pool.memory=VK_NULL_HANDLE;
    const auto empty=context.stats();
    require(!empty.residentBytes && !empty.coldLogicalBytes && !empty.coldStoredBytes && !empty.failures,"range cleanup retained backing or errors");
}

void rangeCacheUnknownCheck(Context& context) {
    constexpr VkDeviceSize Bytes=2*ChunkBytes;
    Buffer pool; pool.device=context.device;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size=Bytes;
    bufferInfo.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(vkCreateBuffer(context.device,&bufferInfo,nullptr,&pool.handle),"create clean-cache unknown pool");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(context.device,pool.handle,&req);
    require(req.size==Bytes,"clean-cache unknown check requires two exact 32 MiB children");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=req.size;
    allocation.memoryTypeIndex=context.nativeAllocation?gpuOnlyNativeType(context,req.memoryTypeBits):context.virtualType;
    require(allocation.memoryTypeIndex!=UINT32_MAX,"no compatible clean-cache unknown memory type");
    check(vkAllocateMemory(context.device,&allocation,nullptr,&pool.memory),"allocate clean-cache unknown pool");
    check(vkBindBufferMemory(context.device,pool.handle,pool.memory,0),"bind clean-cache unknown pool");

    Staging staging; staging.device=context.device;
    bufferInfo.size=ChunkBytes;
    bufferInfo.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(vkCreateBuffer(context.device,&bufferInfo,nullptr,&staging.buffer),"create clean-cache unknown staging");
    vkGetBufferMemoryRequirements(context.device,staging.buffer,&req);
    allocation.allocationSize=req.size; allocation.memoryTypeIndex=hostCoherentType(context,req.memoryTypeBits);
    check(vkAllocateMemory(context.device,&allocation,nullptr,&staging.memory),"allocate clean-cache unknown staging");
    check(vkBindBufferMemory(context.device,staging.buffer,staging.memory,0),"bind clean-cache unknown staging");
    check(vkMapMemory(context.device,staging.memory,0,ChunkBytes,0,&staging.mapped),"map clean-cache unknown staging");

    upload(context,pool.handle,staging,Bytes);
    check(computeCycle(context,pool.handle,0,0,false,Bytes),"initialize clean-cache unknown chunks");
    auto waitCold=[&] {
        const auto deadline=std::chrono::steady_clock::now()+ColdTimeout;
        while(std::chrono::steady_clock::now()<deadline) {
            const auto stats=context.stats();
            if(stats.coldLogicalBytes==Bytes && stats.residentBytes==0 && stats.failures==0) return stats;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("clean-cache unknown pool failed to reach two cold chunks");
    };
    const auto cold=waitCold();
    readbackAndVerify(context,pool.handle,staging,0,false,ChunkBytes,0,0);
    readbackAndVerify(context,pool.handle,staging,0,false,ChunkBytes,0,1);
    const auto warm=context.stats();
    require(warm.residentBytes==Bytes && warm.coldLogicalBytes==0 && warm.failures==0,
            "read-only range copies did not restore both cached chunks");

    auto* words=static_cast<std::uint32_t*>(staging.mapped);
    for(std::uint32_t i=0;i<ChunkWords;++i)
        words[i]=initialWord(i)^mix32(i^cycleSalt(0,0))^mix32(i^cycleSalt(1,0));
    VkEvent event{};
    VkEventCreateInfo eventInfo{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    check(vkCreateEvent(context.device,&eventInfo,nullptr,&event),"create accepted-unknown event");
    try {
        check(context.submit([&](VkCommandBuffer command) {
            vkCmdSetEvent(command,event,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            before.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
            before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0,1,&before,0,nullptr,0,nullptr);
            VkBufferCopy copy{0,0,ChunkBytes};
            vkCmdCopyBuffer(command,staging.buffer,pool.handle,1,&copy);
            VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            after.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0,1,&after,0,nullptr,0,nullptr);
        }),"submit accepted unknown write");
    } catch(...) {
        vkDestroyEvent(context.device,event,nullptr);
        throw;
    }
    vkDestroyEvent(context.device,event,nullptr);
    const auto updatedCold=waitCold();
    require(updatedCold.restores>=cold.restores+2,
            "accepted unknown write did not restore both cold cached chunks");
    readbackAndVerify(context,pool.handle,staging,1,false,ChunkBytes,0,0);
    readbackAndVerify(context,pool.handle,staging,0,false,ChunkBytes,0,1);
    std::cout << "PASS: accepted unknown write invalidated clean snapshots and preserved both full-byte patterns" << std::endl;

    vkDestroyBuffer(context.device,pool.handle,nullptr); pool.handle=VK_NULL_HANDLE;
    vkFreeMemory(context.device,pool.memory,nullptr); pool.memory=VK_NULL_HANDLE;
    const auto empty=context.stats();
    require(!empty.residentBytes && !empty.coldLogicalBytes && !empty.coldStoredBytes && !empty.failures,
            "clean-cache unknown cleanup retained backing or errors");
}

void selectiveSubmitCheck(Context& context, bool api2, bool unknownCommand, bool activeSubmit) {
    Buffer a, b;
    a.device = b.device = context.device;
    auto create = [&](Buffer& buffer) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = ChunkBytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (context.bdaMode) info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(context.device, &info, nullptr, &buffer.handle), "create selective-submit buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(context.device, buffer.handle, &req);
        require(req.size == ChunkBytes && req.alignment && ChunkBytes % req.alignment == 0,
                "selective-submit buffer requirements are not one aligned chunk");
        require(req.memoryTypeBits & (1u << context.virtualType),
                "selective-submit buffer lacks virtual memory type");
        return req;
    };
    const auto reqA = create(a), reqB = create(b);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = context.bdaMode ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = context.bdaMode ? &flags : nullptr;
    allocation.allocationSize = reqA.size;
    allocation.memoryTypeIndex = context.virtualType;
    if (context.nativeAllocation) {
        allocation.memoryTypeIndex = gpuOnlyNativeType(context, reqA.memoryTypeBits & reqB.memoryTypeBits);
        require(allocation.memoryTypeIndex != UINT32_MAX, "no compatible native selective-submit memory type");
    }
    check(vkAllocateMemory(context.device, &allocation, nullptr, &a.memory), "allocate first selective-submit pool");
    allocation.allocationSize = reqB.size;
    check(vkAllocateMemory(context.device, &allocation, nullptr, &b.memory), "allocate second selective-submit pool");
    auto bind = [&](VkBuffer buffer, VkDeviceMemory memory) {
        if (!api2) return vkBindBufferMemory(context.device, buffer, memory, 0);
        VkBindBufferMemoryInfo info{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};
        info.buffer = buffer; info.memory = memory;
        return vkBindBufferMemory2(context.device, 1, &info);
    };
    check(bind(a.handle, a.memory), "bind first selective-submit pool");
    check(bind(b.handle, b.memory), "bind second selective-submit pool");
    auto setAddress = [&](VkBuffer buffer) {
        if (!context.bdaMode) return;
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = buffer;
        context.bufferAddress = vkGetBufferDeviceAddress(context.device, &info);
        require(context.bufferAddress != 0, "selective-submit buffer has no device address");
    };
    setAddress(a.handle);
    const VkDeviceAddress addressA = context.bufferAddress;
    setAddress(b.handle);
    const VkDeviceAddress addressB = context.bufferAddress;

    Staging staging; staging.device = context.device;
    VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stagingInfo.size = ChunkBytes;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(context.device, &stagingInfo, nullptr, &staging.buffer), "create selective-submit staging");
    VkMemoryRequirements stagingReq{};
    vkGetBufferMemoryRequirements(context.device, staging.buffer, &stagingReq);
    VkMemoryAllocateInfo stagingAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    stagingAlloc.allocationSize = stagingReq.size;
    stagingAlloc.memoryTypeIndex = hostCoherentType(context, stagingReq.memoryTypeBits);
    require(stagingAlloc.memoryTypeIndex != UINT32_MAX, "no selective-submit staging type");
    check(vkAllocateMemory(context.device, &stagingAlloc, nullptr, &staging.memory), "allocate selective-submit staging");
    check(vkBindBufferMemory(context.device, staging.buffer, staging.memory, 0), "bind selective-submit staging");
    check(vkMapMemory(context.device, staging.memory, 0, ChunkBytes, 0, &staging.mapped), "map selective-submit staging");

    const auto beforeCold = context.stats();
    upload(context, a.handle, staging, ChunkBytes);
    upload(context, b.handle, staging, ChunkBytes);
    setAddress(a.handle);
    check(computeCycle(context, a.handle, 0, 0, false, ChunkBytes), "initialize first selective-submit pattern");
    setAddress(b.handle);
    check(computeCycle(context, b.handle, 0, 0, false, ChunkBytes), "initialize second selective-submit pattern");
    check(computeCycle(context, b.handle, 1, 0, false, ChunkBytes), "distinguish second selective-submit pattern");
    setAddress(a.handle);
    readbackAndVerify(context, a.handle, staging, 0, false, ChunkBytes);
    setAddress(b.handle);
    readbackAndVerify(context, b.handle, staging, 1, false, ChunkBytes);

    if (activeSubmit) {
        require(context.twoQueues && context.secondQueue && context.secondQueue != context.queue,
                "UNSUPPORTED: --active-submit requires --two-queues on one queue family");
        const auto beforeActive = context.stats();
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = context.secondCommands;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        // Declare image first so its destructor runs only after the pending
        // queue submission has been signaled and drained on exceptional exits.
        NativeImage unknownImage;
        if (unknownCommand) createUnknownResourceImage(context, unknownImage);
        PendingSubmission pending{context.device, context.secondQueue, context.secondCommands,
                                  VK_NULL_HANDLE, context.pendingTimeline, false,
                                  unknownCommand ? &unknownImage : nullptr};
        check(vkAllocateCommandBuffers(context.device, &commandInfo, &pending.command),
              "allocate active-submit command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(pending.command, &begin), "begin active-submit command buffer");
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(pending.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        VkBufferCopy copy{0, 0, ChunkBytes};
        vkCmdCopyBuffer(pending.command, a.handle, staging.buffer, 1, &copy);
        if (unknownCommand) recordUnknownResourceClear(pending.command, unknownImage.handle);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(pending.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(pending.command), "end active-submit command buffer");

        VkTimelineSemaphoreSubmitInfo waitValues{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        const std::uint64_t signalValue = 1;
        waitValues.waitSemaphoreValueCount = 1;
        waitValues.pWaitSemaphoreValues = &signalValue;
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo blockedSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        blockedSubmit.pNext = &waitValues;
        blockedSubmit.waitSemaphoreCount = 1;
        blockedSubmit.pWaitSemaphores = &context.pendingTimeline;
        blockedSubmit.pWaitDstStageMask = &waitStage;
        blockedSubmit.commandBufferCount = 1;
        blockedSubmit.pCommandBuffers = &pending.command;
        check(vkQueueSubmit(context.secondQueue, 1, &blockedSubmit, VK_NULL_HANDLE),
              "submit A behind host timeline wait");
        pending.submitted = true;

        auto unblockA = [&] { check(pending.finish(), "finish host-unblocked A submission"); };

        if (unknownCommand) {
            const auto pendingStart = context.stats();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const auto pendingEnd = context.stats();
            const bool protectedAll = pendingEnd.coldLogicalBytes == 0 &&
                pendingEnd.residentBytes == reqA.size + reqB.size &&
                pendingEnd.failures == pendingStart.failures;
            const auto freezeDelta = pendingEnd.freezes - pendingStart.freezes;
            unblockA();
            require(protectedAll && freezeDelta == 0,
                    "unknown pending submission did not protect every pool from active eviction");
            setAddress(a.handle);
            readbackAndVerify(context, a.handle, staging, 0, false, ChunkBytes);
            setAddress(b.handle);
            readbackAndVerify(context, b.handle, staging, 1, false, ChunkBytes);
            const auto restored = context.stats();
            require(restored.coldLogicalBytes == 0 && restored.failures == pendingStart.failures,
                    "unknown-submit A/B readback changed snapshot failure accounting");
            std::cout << "PASS: unknown A submission kept both pools resident while pending; both byte patterns verified\n";
            vkDestroyBuffer(context.device, a.handle, nullptr); a.handle = VK_NULL_HANDLE;
            vkDestroyBuffer(context.device, b.handle, nullptr); b.handle = VK_NULL_HANDLE;
            vkFreeMemory(context.device, a.memory, nullptr); a.memory = VK_NULL_HANDLE;
            vkFreeMemory(context.device, b.memory, nullptr); b.memory = VK_NULL_HANDLE;
            const auto empty = context.stats();
            require(empty.residentBytes == 0 && empty.coldLogicalBytes == 0 && empty.coldStoredBytes == 0,
                    "active-submit unknown cleanup retained pool bytes");
            return;
        }

        const auto deadline = std::chrono::steady_clock::now() + ColdTimeout;
        ZvramSnapshotStatsNX active{};
        do {
            active = context.stats();
            if (active.coldLogicalBytes == reqB.size && active.residentBytes == reqA.size &&
                active.coldStoredBytes > 0 && active.freezes > beforeActive.freezes &&
                active.failures == beforeActive.failures) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < deadline);
        if (active.coldLogicalBytes != reqB.size || active.residentBytes != reqA.size ||
            active.coldStoredBytes == 0 || active.freezes <= beforeActive.freezes ||
            active.failures != beforeActive.failures) {
            const auto error = "active eviction did not cold B alone while A's queue submission was pending: cold=" +
                std::to_string(active.coldLogicalBytes) + " resident=" + std::to_string(active.residentBytes) +
                " failures=" + std::to_string(active.failures);
            unblockA();
            throw std::runtime_error(error);
        }

        bool watchdogFired = false;
        ZvramSnapshotStatsNX afterB{};
        {
            TimelineWatchdog watchdog(context.device, context.pendingTimeline, signalValue);
            setAddress(b.handle);
            readbackAndVerify(context, b.handle, staging, 1, false, ChunkBytes);
            afterB = context.stats();
            watchdogFired = watchdog.fired.load();
            watchdog.cancelAndJoin();
        }
        unblockA();
        require(!watchdogFired,
                "B restore blocked behind A and required the 2 s timeline watchdog signal");
        require(afterB.coldLogicalBytes == 0 && afterB.residentBytes == reqA.size + reqB.size,
                "B readback did not restore B while A remained host-blocked");
        setAddress(b.handle);
        readbackAndVerify(context, b.handle, staging, 1, false, ChunkBytes, 1);
        setAddress(a.handle);
        readbackAndVerify(context, a.handle, staging, 0, false, ChunkBytes);
        const auto restored = context.stats();
        require(restored.coldLogicalBytes == 0 && restored.failures == beforeActive.failures,
                "A/B readback did not restore cold B cleanly");
        std::cout << "PASS: B restored while A stayed host-blocked without watchdog; both byte patterns verified\n";

        vkDestroyBuffer(context.device, a.handle, nullptr); a.handle = VK_NULL_HANDLE;
        vkDestroyBuffer(context.device, b.handle, nullptr); b.handle = VK_NULL_HANDLE;
        vkFreeMemory(context.device, a.memory, nullptr); a.memory = VK_NULL_HANDLE;
        vkFreeMemory(context.device, b.memory, nullptr); b.memory = VK_NULL_HANDLE;
        const auto empty = context.stats();
        require(empty.residentBytes == 0 && empty.coldLogicalBytes == 0 && empty.coldStoredBytes == 0,
                "active-submit cleanup retained pool bytes");
        return;
    }

    const auto coldBytes = reqA.size + reqB.size;
    const auto deadline = std::chrono::steady_clock::now() + ColdTimeout;
    ZvramSnapshotStatsNX cold{};
    do {
        cold = context.stats();
        if (cold.coldLogicalBytes == coldBytes && cold.residentBytes == 0 &&
            cold.coldStoredBytes > 0 && cold.freezes >= beforeCold.freezes + 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    require(cold.coldLogicalBytes == coldBytes && cold.residentBytes == 0 &&
            cold.coldStoredBytes > 0 && cold.freezes >= beforeCold.freezes + 2,
            "both live selective-submit pools did not reach cold snapshots");
    context.bufferAddress = addressA;
    metadataWhileCold(context, a.handle, cold, coldBytes, reqA.size);
    context.bufferAddress = addressB;
    metadataWhileCold(context, b.handle, cold, coldBytes, reqB.size);
    const auto afterIdleWaits = context.stats();
    require(afterIdleWaits.coldLogicalBytes == coldBytes && afterIdleWaits.residentBytes == 0 &&
            afterIdleWaits.restores == cold.restores,
            "queue/device idle or metadata query woke selective-submit pools");

    if (unknownCommand) {
        {
            NativeImage unknownImage;
            createUnknownResourceImage(context, unknownImage);
            try {
                check(context.submit([&](VkCommandBuffer command) {
                    recordUnknownResourceClear(command, unknownImage.handle);
                }), "submit unknown selective-submit image clear");
            } catch (...) {
                // Context::submit may fail while waiting after submission; in
                // that case the image must outlive the uncertain queue work.
                unknownImage.unsafe = true;
                throw;
            }
        }
        const auto restored = context.stats();
        require(restored.residentBytes == coldBytes && restored.coldLogicalBytes == 0 &&
                restored.restores >= afterIdleWaits.restores + 2,
                "unknown command did not conservatively restore every cold pool");
        setAddress(a.handle);
        readbackAndVerify(context, a.handle, staging, 0, false, ChunkBytes);
        setAddress(b.handle);
        readbackAndVerify(context, b.handle, staging, 1, false, ChunkBytes);
        std::cout << "PASS: unknown command restored both cold selective-submit pools" << std::endl;
    } else {
        setAddress(a.handle);
        check(computeCycle(context, a.handle, 1, 0, false, ChunkBytes), "compute first selective-submit cold pool");
        auto firstAwake = context.stats();
        if (context.bdaMode) {
            require(firstAwake.residentBytes == coldBytes && firstAwake.coldLogicalBytes == 0 &&
                    firstAwake.restores >= afterIdleWaits.restores + 2,
                    "BDA compute did not conservatively restore every cold pool");
        } else {
            require(firstAwake.residentBytes == reqA.size && firstAwake.coldLogicalBytes == reqB.size &&
                    firstAwake.restores == afterIdleWaits.restores + 1,
                    "compute of first pool restored unrelated cold pool");
            readbackAndVerify(context, a.handle, staging, 1, false, ChunkBytes);
            const auto afterReadback = context.stats();
            require(afterReadback.residentBytes == reqA.size && afterReadback.coldLogicalBytes == reqB.size &&
                    afterReadback.restores == firstAwake.restores,
                    "readback of first pool restored unrelated cold pool");
            firstAwake = afterReadback;
        }
        setAddress(b.handle);
        check(computeCycle(context, b.handle, 2, 0, false, ChunkBytes), "compute second selective-submit cold pool");
        const auto bothAwake = context.stats();
        require(bothAwake.residentBytes == coldBytes && bothAwake.coldLogicalBytes == 0 &&
                bothAwake.restores >= firstAwake.restores + (context.bdaMode ? 0 : 1),
                "compute of second pool did not leave both pools resident");
        setAddress(a.handle);
        readbackAndVerify(context, a.handle, staging, 1, false, ChunkBytes);
        setAddress(b.handle);
        readbackAndVerify(context, b.handle, staging, 2, false, ChunkBytes);
        std::cout << "PASS: " << (context.nativeAllocation ? "native" : "synthetic")
                  << (api2 ? " API2" : " legacy")
                  << (context.bdaMode ? " BDA" : "")
                  << " selective-submit pools preserved full-byte patterns" << std::endl;
    }

    vkDestroyBuffer(context.device, a.handle, nullptr); a.handle = VK_NULL_HANDLE;
    vkDestroyBuffer(context.device, b.handle, nullptr); b.handle = VK_NULL_HANDLE;
    vkFreeMemory(context.device, a.memory, nullptr); a.memory = VK_NULL_HANDLE;
    vkFreeMemory(context.device, b.memory, nullptr); b.memory = VK_NULL_HANDLE;
    const auto empty = context.stats();
    require(empty.residentBytes == 0 && empty.coldLogicalBytes == 0 && empty.coldStoredBytes == 0,
            "selective-submit cleanup retained pool bytes");
}
} // namespace

int main(int argc, char** argv) try {
    bool expectBudgetRefusal = false, expectBudgetRelease = false;
    bool expectPartialFreeze = false, expectPartialRestore = false;
    bool expectPipelineRestore = false, expectPipelinePartialRestore = false;
    bool bdaMode = false, nativeAllocation = false;
    bool twoQueues = false, twoFamilies = false, exclusiveFamilies = false;
    bool pendingWait = false, pendingBind = false;
    bool activeSubmit = false;
    bool rangeSubmit = false;
    bool rangeCompressed = false;
    bool useZeroPattern = false;
    bool rangePressure = false;
    bool rangeCache = false;
    bool rangeCacheBootstrap = false;
    bool rangeCacheQuota = false, rangeCacheUnknown = false;
    bool robustCore = false;
    bool concurrentWait = false;
    std::uint32_t controlHoldMilliseconds = 0;
    bool suballocation = false, suballocationAuto = false, suballocationApi2 = false;
    bool selectiveBind = false, selectiveBindApi2 = false;
    bool selectiveSubmit = false, selectiveSubmitApi2 = false, selectiveSubmitUnknown = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--expect-budget-refusal") == 0) expectBudgetRefusal = true;
        else if (std::strcmp(argv[i], "--expect-budget-release") == 0) expectBudgetRelease = true;
        else if (std::strcmp(argv[i], "--expect-partial-freeze") == 0) expectPartialFreeze = true;
        else if (std::strcmp(argv[i], "--expect-partial-restore") == 0) expectPartialRestore = true;
        else if (std::strcmp(argv[i], "--expect-pipeline-restore") == 0) expectPipelineRestore = true;
        else if (std::strcmp(argv[i], "--expect-pipeline-partial-restore") == 0) expectPipelinePartialRestore = true;
        else if (std::strcmp(argv[i], "--bda") == 0) bdaMode = true;
        else if (std::strcmp(argv[i], "--native-allocation") == 0) nativeAllocation = true;
        else if (std::strcmp(argv[i], "--two-queues") == 0) twoQueues = true;
        else if (std::strcmp(argv[i], "--two-families") == 0) twoFamilies = true;
        else if (std::strcmp(argv[i], "--exclusive-families") == 0) exclusiveFamilies = true;
        else if (std::strcmp(argv[i], "--pending-wait") == 0) pendingWait = true;
        else if (std::strcmp(argv[i], "--pending-bind") == 0) pendingBind = true;
        else if (std::strcmp(argv[i], "--concurrent-wait") == 0) concurrentWait = true;
        else if (std::strcmp(argv[i], "--suballocation") == 0) suballocation = true;
        else if (std::strcmp(argv[i], "--suballocation-auto") == 0) { suballocation = true; suballocationAuto = true; }
        else if (std::strcmp(argv[i], "--suballocation-api2") == 0) { suballocation = true; suballocationApi2 = true; }
        else if (std::strcmp(argv[i], "--selective-bind") == 0) selectiveBind = true;
        else if (std::strcmp(argv[i], "--selective-bind-api2") == 0) { selectiveBind = true; selectiveBindApi2 = true; }
        else if (std::strcmp(argv[i], "--selective-submit") == 0) selectiveSubmit = true;
        else if (std::strcmp(argv[i], "--selective-submit-api2") == 0) { selectiveSubmit = true; selectiveSubmitApi2 = true; }
        else if (std::strcmp(argv[i], "--selective-submit-unknown") == 0) { selectiveSubmit = true; selectiveSubmitUnknown = true; }
        else if (std::strcmp(argv[i], "--range-submit") == 0) { rangeSubmit=true; selectiveSubmit=true; }
        else if (std::strcmp(argv[i], "--range-compressed") == 0) { rangeSubmit=true; selectiveSubmit=true; rangeCompressed=true; }
        else if (std::strcmp(argv[i], "--zero-pattern") == 0) useZeroPattern=true;
        else if (std::strcmp(argv[i], "--robust-core") == 0) robustCore=true;
        else if (std::strcmp(argv[i], "--range-pressure") == 0) { rangeSubmit=true; rangePressure=true; }
        else if (std::strcmp(argv[i], "--range-cache") == 0) {
            rangeSubmit=true; rangePressure=true; rangeCache=true;
        }
        else if (std::strcmp(argv[i], "--range-cache-bootstrap") == 0) {
            rangeSubmit=true; rangePressure=true; rangeCache=true; rangeCacheBootstrap=true;
        }
        else if (std::strcmp(argv[i], "--range-cache-quota") == 0) {
            rangeSubmit=true; rangePressure=true; rangeCache=true; rangeCacheQuota=true;
        }
        else if (std::strcmp(argv[i], "--range-cache-unknown") == 0) {
            rangeSubmit=true; rangeCache=true; rangeCacheUnknown=true;
        }
        else if (std::strcmp(argv[i], "--control-hold-ms") == 0) {
            require(i+1<argc,"--control-hold-ms requires a value from 0 to 15000");
            const std::string value=argv[++i];
            require(!value.empty() && value.size()<=5 &&
                    value.find_first_not_of("0123456789")==std::string::npos,
                    "--control-hold-ms requires a value from 0 to 15000");
            controlHoldMilliseconds=static_cast<std::uint32_t>(std::stoul(value));
            require(controlHoldMilliseconds<=15000,"--control-hold-ms maximum is 15000");
        }
        else if (std::strcmp(argv[i], "--active-submit") == 0) { selectiveSubmit = true; activeSubmit = true; }
        else throw std::runtime_error("usage: zvram-vulkan-auto-check [--expect-budget-refusal|--expect-budget-release|--expect-partial-freeze|--expect-partial-restore|--expect-pipeline-restore|--expect-pipeline-partial-restore] [--bda] [--native-allocation] [--two-queues|--two-families|--exclusive-families] [--pending-wait|--pending-bind] [--concurrent-wait] [--suballocation|--suballocation-auto] [--suballocation-api2] [--selective-bind|--selective-bind-api2] [--selective-submit|--selective-submit-api2|--selective-submit-unknown] [--range-submit|--range-compressed [--zero-pattern]|--range-pressure|--range-cache|--range-cache-bootstrap|--range-cache-quota|--range-cache-unknown] [--active-submit --two-queues] [--control-hold-ms 0..15000]");
    }
    require(!useZeroPattern || rangeCompressed,
            "--zero-pattern requires --range-compressed");
    zeroPattern = useZeroPattern;
    require(!(expectBudgetRelease && (expectBudgetRefusal || expectPartialFreeze || expectPartialRestore ||
                                      expectPipelineRestore || expectPipelinePartialRestore ||
                                      bdaMode || nativeAllocation || twoQueues || twoFamilies ||
                                      exclusiveFamilies || pendingWait || pendingBind)),
            "budget-release mode uses one synthetic single-queue allocation path");
    require(!(expectBudgetRefusal && bdaMode), "--bda and --expect-budget-refusal cannot be combined");
    require(!(expectBudgetRefusal && nativeAllocation), "budget refusal requires virtual allocation");
    require(!(expectPartialFreeze && (expectBudgetRefusal || nativeAllocation || bdaMode)),
            "partial-freeze mode requires the standard virtual allocation path");
    require(!(expectPartialRestore && (expectBudgetRefusal || expectPartialFreeze || nativeAllocation || bdaMode)),
            "partial-restore mode requires the standard virtual allocation path");
    require(!(expectPipelineRestore && expectPipelinePartialRestore),
            "choose only one pipeline restore mode");
    require(!(expectPipelineRestore && (expectBudgetRefusal || expectBudgetRelease || expectPartialFreeze ||
                                        expectPartialRestore || bdaMode || twoQueues || twoFamilies ||
                                        exclusiveFamilies || pendingWait || pendingBind)),
            "pipeline restore mode requires the standard single-queue descriptor path");
    require(!(expectPipelinePartialRestore && (expectBudgetRefusal || expectBudgetRelease || expectPartialFreeze ||
                                               expectPartialRestore || nativeAllocation || bdaMode || twoQueues ||
                                               twoFamilies || exclusiveFamilies || pendingWait || pendingBind)),
            "pipeline partial-restore mode requires synthetic single-queue memory");
    require(!(twoQueues && (twoFamilies || exclusiveFamilies)), "choose only one multi-queue mode");
    require(!rangePressure || (rangeSubmit && !activeSubmit && !bdaMode && !twoFamilies && !exclusiveFamilies),
            "range pressure requires descriptor-tracked range-submit mode");
    require(!rangePressure || !pendingWait,
            "range pressure manages its own pending timeline test");
    require(!rangeCompressed || (!rangePressure && !rangeCache),
            "compressible range initialization requires the independent range-submit check");
    require(!(rangeCacheQuota && rangeCacheUnknown),
            "choose only one clean-cache extension check");
    require(!(expectBudgetRefusal && exclusiveFamilies), "budget refusal mode does not use exclusive family transfers");
    require(!(pendingWait && pendingBind), "choose only one pending queue test");
    require(!concurrentWait || (pendingWait && twoQueues),
            "--concurrent-wait requires --pending-wait and --two-queues");
    require(!selectiveBind || !(suballocation || expectBudgetRefusal || expectBudgetRelease ||
                                expectPartialFreeze || expectPartialRestore || expectPipelineRestore ||
                                expectPipelinePartialRestore || bdaMode || twoQueues ||
                                twoFamilies || exclusiveFamilies || pendingWait || pendingBind || concurrentWait),
            "selective-bind mode is independent of other Vulkan checks");
    require(!selectiveSubmit || !(selectiveBind || suballocation || expectBudgetRefusal || expectBudgetRelease ||
                                  expectPartialFreeze || expectPartialRestore || expectPipelineRestore ||
                                  expectPipelinePartialRestore || (twoQueues && !activeSubmit && !rangePressure) || twoFamilies ||
                                  exclusiveFamilies || pendingWait || pendingBind || concurrentWait),
            "selective-submit mode is independent of other Vulkan checks");
    if (activeSubmit && !twoQueues) {
        std::cerr << "UNSUPPORTED: --active-submit requires --two-queues (two queues in one family)\n";
        return 77;
    }
    require(!activeSubmit || !bdaMode,
            "--active-submit requires descriptor-tracked mode without BDA");
    require(!pendingBind || (nativeAllocation && (twoQueues || twoFamilies || exclusiveFamilies)),
            "--pending-bind requires --native-allocation and a multi-queue mode");
    Context context;
    require(!suballocation || !(expectBudgetRefusal || expectBudgetRelease || expectPartialFreeze || expectPartialRestore ||
                               expectPipelineRestore || expectPipelinePartialRestore ||
                               exclusiveFamilies || pendingWait || pendingBind || concurrentWait),
            "suballocation mode is independent of refusal, retry, exclusive-family and pending-work checks");
    require(!suballocation || !nativeAllocation || suballocationAuto,
            "native suballocation checks require --suballocation-auto");
    context.initialize(bdaMode, nativeAllocation, twoQueues, twoFamilies, exclusiveFamilies,
                       pendingWait, pendingBind, activeSubmit, rangePressure && twoQueues, robustCore);
    if(controlHoldMilliseconds) {
        std::cout<<"CONTROL_READY hold-ms="<<controlHoldMilliseconds<<std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(controlHoldMilliseconds));
    }
    if (rangeCacheUnknown) { rangeCacheUnknownCheck(context); return 0; }
    if (rangeSubmit) { rangeSubmitCheck(context, rangePressure, rangeCache, rangeCacheQuota, rangeCacheBootstrap, rangeCompressed); return 0; }
    if (selectiveBind) { selectiveBindCheck(context, selectiveBindApi2); return 0; }
    if (selectiveSubmit) { selectiveSubmitCheck(context, selectiveSubmitApi2, selectiveSubmitUnknown, activeSubmit); return 0; }
    if (suballocation) { suballocationCheck(context, suballocationAuto, suballocationApi2); return 0; }
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
    if (expectPipelineRestore || expectPipelinePartialRestore) {
        pipelineRestoreCheck(context, resident, staging, initial, expectPipelinePartialRestore);
        return 0;
    }
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
