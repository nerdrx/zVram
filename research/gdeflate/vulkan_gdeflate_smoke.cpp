// Minimal host for the pinned GDeflate DXC shader; this is a research smoke,
// not production code. Run only with trusted shader/input/expected files.
#include <vulkan/vulkan.h>
#include "gdeflate_envelope.hpp"
#include "../bp16/bp16.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t WaitNanoseconds = 5'000'000'000ull;
constexpr std::uint32_t TileBytes = 64u * 1024u;
constexpr std::uint32_t ShaderThreads = 32;

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " failed: " + std::to_string(result));
}

std::vector<std::uint8_t> readFile(const char* path, std::size_t maximum) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error(std::string("cannot open ") + path);
    const auto end = file.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) > maximum)
        throw std::runtime_error(std::string(path) + " exceeds bounded nonempty file limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error(std::string("cannot read ") + path);
    return bytes;
}

bool hasInstanceLayer(const char* name) {
    std::uint32_t count = 0;
    check(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate instance layer count");
    std::vector<VkLayerProperties> layers(count);
    check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "enumerate instance layers");
    return std::any_of(layers.begin(), layers.end(), [&](const auto& layer) {
        return std::strcmp(layer.layerName, name) == 0;
    });
}

bool hasInstanceExtension(const char* name) {
    std::uint32_t count = 0;
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "enumerate instance extension count");
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()),
          "enumerate instance extensions");
    return std::any_of(extensions.begin(), extensions.end(), [&](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
    std::uint32_t count = 0;
    check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr),
          "enumerate device extension count");
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()),
          "enumerate device extensions");
    return std::any_of(extensions.begin(), extensions.end(), [&](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

struct ValidationCounts {
    std::atomic<unsigned> errors{};
    std::atomic<unsigned> vuids{};
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                           VkDebugUtilsMessageTypeFlagsEXT,
                                           const VkDebugUtilsMessengerCallbackDataEXT* data,
                                           void* user) {
    auto* counts = static_cast<ValidationCounts*>(user);
    const char* message = data && data->pMessage ? data->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++counts->errors;
        if (std::strstr(message, "VUID-")) ++counts->vuids;
        std::cerr << "Vulkan validation error: " << message << '\n';
    }
    return VK_FALSE;
}

struct Runtime {
    ValidationCounts& validation;
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceSubgroupProperties subgroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties subgroupSizeProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    std::uint32_t queueFamily{};
    std::uint32_t timestampValidBits{};
    VkDevice device{};
    VkQueue queue{};
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipelineLayout{};
    VkShaderModule shader{};
    VkPipeline pipeline{};
    VkDescriptorPool descriptorPool{};
    VkCommandPool commandPool{};
    VkFence fence{};
    VkQueryPool timestampPool{};
    bool abandonOnExit{};
    bool software{};

    explicit Runtime(ValidationCounts& counts, bool cpu = false) : validation(counts), software(cpu) {}

    ~Runtime() {
        // A timed-out or lost device may never complete pending work. Let process
        // teardown reclaim these handles instead of blocking in another wait.
        if (abandonOnExit) return;
        if (device) vkDeviceWaitIdle(device);
        if (device && fence) vkDestroyFence(device, fence, nullptr);
        if (device && timestampPool) vkDestroyQueryPool(device, timestampPool, nullptr);
        if (device && commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        if (device && descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (device && pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (device && shader) vkDestroyShaderModule(device, shader, nullptr);
        if (device && pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (device && setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance && messenger && destroyMessenger) destroyMessenger(instance, messenger, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    void initInstance() {
        const bool validationAvailable = hasInstanceLayer("VK_LAYER_KHRONOS_validation");
        const bool debugAvailable = hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        std::vector<const char*> layers, extensions;
        if (validationAvailable) layers.push_back("VK_LAYER_KHRONOS_validation");
        if (debugAvailable) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "zVram GDeflate research smoke";
        app.apiVersion = VK_API_VERSION_1_2;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = debugMessage;
        debug.pUserData = &validation;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pNext = debugAvailable ? &debug : nullptr;
        create.pApplicationInfo = &app;
        create.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        create.ppEnabledLayerNames = layers.data();
        create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        check(vkCreateInstance(&create, nullptr, &instance), "create Vulkan 1.2 instance");
        std::cout << "validation=" << (validationAvailable ? "on" : "unavailable")
                  << " debug-utils=" << (debugAvailable ? "on" : "unavailable") << '\n';
        if (debugAvailable) {
            auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (createMessenger && destroyMessenger)
                check(createMessenger(instance, &debug, nullptr, &messenger), "create validation messenger");
        }
    }

    void pickDevice(bool simpleShader = false) {
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate physical device count");
        if (!count) throw std::runtime_error("no Vulkan physical device");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate physical devices");
        int bestScore = std::numeric_limits<int>::min();
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (software != (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)) continue;
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                if (!(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) || !families[i].queueCount) continue;
                const std::string name = props.deviceName;
                int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 0;
                if (props.vendorID == 0x1002) score += 50;
                if (name.find("7900") != std::string::npos || name.find("RADV") != std::string::npos)
                    score += 200;
                if (score > bestScore) {
                    bestScore = score; physical = candidate; properties = props; queueFamily = i;
                }
                break;
            }
        }
        if (!physical) throw std::runtime_error("no Vulkan compute queue");
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        const bool subgroupSizeControl =
            hasDeviceExtension(physical, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
        if (!simpleShader && !subgroupSizeControl)
            throw std::runtime_error("selected device lacks VK_EXT_subgroup_size_control");
        subgroupProperties.pNext = &subgroupSizeProperties;
        if (!subgroupSizeControl) subgroupProperties.pNext = nullptr;
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &subgroupProperties;
        vkGetPhysicalDeviceProperties2(physical, &props2);
        VkPhysicalDeviceSubgroupSizeControlFeatures subgroupFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
        VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if (subgroupSizeControl) supported.pNext = &subgroupFeatures;
        vkGetPhysicalDeviceFeatures2(physical, &supported);
        if (!simpleShader && !supported.features.shaderInt64)
            throw std::runtime_error("selected device lacks shaderInt64 required by the pinned SPIR-V OpCapability Int64");
        constexpr VkSubgroupFeatureFlags requiredSubgroupOps =
            VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
            VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
        if (!simpleShader && !software && (!subgroupFeatures.subgroupSizeControl || !subgroupFeatures.computeFullSubgroups ||
            !(subgroupProperties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
            (subgroupProperties.supportedOperations & requiredSubgroupOps) != requiredSubgroupOps ||
            !(subgroupSizeProperties.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
            ShaderThreads < subgroupSizeProperties.minSubgroupSize ||
            ShaderThreads > subgroupSizeProperties.maxSubgroupSize ||
            properties.limits.maxComputeWorkGroupInvocations < ShaderThreads ||
            properties.limits.maxComputeWorkGroupSize[0] < ShaderThreads))
            throw std::runtime_error("selected device lacks the compute subgroup size 32 / ballot, arithmetic, "
                                     "or shuffle contract required by this shader");
        if (simpleShader && (properties.limits.maxComputeWorkGroupInvocations < 256 ||
                             properties.limits.maxComputeWorkGroupSize[0] < 256))
            throw std::runtime_error("selected device cannot run the 256-thread BP16 workgroup");
        std::cout << "device=" << properties.deviceName << " vendor=0x" << std::hex
                  << properties.vendorID << std::dec << " type=" << properties.deviceType
                  << " queue-family=" << queueFamily
                  << " shaderInt64=" << (!simpleShader ? "enabled" : "not-required")
                  << " execution=" << (software ? "CPU-software" :
                                         (simpleShader ? "GPU-compute" : "GPU-wave32"))
                  << " default-subgroup=" << subgroupProperties.subgroupSize
                  << " supported-stages=0x" << std::hex << subgroupProperties.supportedStages
                  << " supported-operations=0x" << subgroupProperties.supportedOperations
                  << std::dec << " subgroup-range=" << subgroupSizeProperties.minSubgroupSize
                  << '-' << subgroupSizeProperties.maxSubgroupSize << '\n';
        timestampValidBits = 0;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        const float priority = 1.0f;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabled{};
        enabled.shaderInt64 = simpleShader ? VK_FALSE : VK_TRUE;
        VkPhysicalDeviceSubgroupSizeControlFeatures enabledSubgroup{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
        enabledSubgroup.subgroupSizeControl = simpleShader ? VK_FALSE : VK_TRUE;
        enabledSubgroup.computeFullSubgroups = (software || simpleShader) ? VK_FALSE : VK_TRUE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        const char* deviceExtensions[]{VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME};
        deviceInfo.enabledExtensionCount = simpleShader ? 0u : 1u;
        deviceInfo.ppEnabledExtensionNames = simpleShader ? nullptr : deviceExtensions;
        VkPhysicalDeviceFeatures2 enabledFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        enabledFeatures.features = enabled;
        enabledFeatures.pNext = simpleShader ? nullptr : &enabledSubgroup;
        deviceInfo.pNext = &enabledFeatures;
        deviceInfo.pEnabledFeatures = nullptr;
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "create Vulkan device");
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        timestampValidBits = families[queueFamily].timestampValidBits;
    }

    void initPipeline(const std::vector<std::uint8_t>& code, std::uint32_t iterations,
                      bool simpleShader = false) {
        if (code.size() % sizeof(std::uint32_t)) throw std::runtime_error("SPIR-V size is not word aligned");
        VkDescriptorSetLayoutBinding bindings[4]{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 4;
        layoutInfo.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout), "create descriptor layout");
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout;
        check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout),
              "create pipeline layout");
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = code.size();
        shaderInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
        check(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader), "create shader module");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroupSize{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        subgroupSize.requiredSubgroupSize = ShaderThreads;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.flags = (software || simpleShader) ? 0 : VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
        pipelineInfo.stage.pNext = (software || simpleShader) ? nullptr : &subgroupSize;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader;
        pipelineInfo.stage.pName = "CSMain";
        pipelineInfo.layout = pipelineLayout;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
              "create compute pipeline");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool), "create descriptor pool");
        VkCommandPoolCreateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commandInfo.queueFamilyIndex = queueFamily;
        check(vkCreateCommandPool(device, &commandInfo, nullptr, &commandPool), "create command pool");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "create fence");
        if (timestampValidBits) {
            VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = iterations * 5;
            check(vkCreateQueryPool(device, &queryInfo, nullptr, &timestampPool), "create timestamp query pool");
        }
    }
};

struct Buffer {
    Runtime* owner{};
    VkDevice device{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize allocationSize{};
    VkDeviceSize descriptorSize{};
    VkMemoryPropertyFlags memoryFlags{};
    void* mapped{};

    ~Buffer() {
        if (owner && owner->abandonOnExit) return;
        if (device && mapped) vkUnmapMemory(device, memory);
        if (device && buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (device && memory) vkFreeMemory(device, memory, nullptr);
    }

    void init(Runtime& runtime, VkDeviceSize size, VkBufferUsageFlags usage,
              VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0,
              VkMemoryPropertyFlags avoid = 0) {
        owner = &runtime;
        device = runtime.device;
        descriptorSize = size;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "create storage buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        std::uint32_t memoryType = UINT32_MAX;
        int bestScore = std::numeric_limits<int>::min();
        for (std::uint32_t i = 0; i < runtime.memoryProperties.memoryTypeCount; ++i) {
            if (!(requirements.memoryTypeBits & (1u << i))) continue;
            const auto flags = runtime.memoryProperties.memoryTypes[i].propertyFlags;
            if ((flags & required) != required) continue;
            int score = 0;
            for (VkMemoryPropertyFlags bit = 1; bit; bit <<= 1)
                if ((preferred & bit) && (flags & bit)) ++score;
            for (VkMemoryPropertyFlags bit = 1; bit; bit <<= 1)
                if ((avoid & bit) && (flags & bit)) --score;
            if (score > bestScore) { bestScore = score; memoryType = i; }
        }
        if (memoryType == UINT32_MAX) throw std::runtime_error("no compatible Vulkan memory type for buffer");
        memoryFlags = runtime.memoryProperties.memoryTypes[memoryType].propertyFlags;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        allocationSize = requirements.size;
        check(vkAllocateMemory(device, &allocation, nullptr, &memory), "allocate host-coherent storage memory");
        check(vkBindBufferMemory(device, buffer, memory, 0), "bind storage buffer");
        if (memoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            check(vkMapMemory(device, memory, 0, allocationSize, 0, &mapped), "map host-visible buffer");
            std::memset(mapped, 0, static_cast<std::size_t>(allocationSize));
        }
        std::cout << "buffer bytes=" << size << " allocation-bytes=" << allocationSize
                  << " memory-type=" << memoryType << " flags=0x" << std::hex << memoryFlags
                  << std::dec << " host-visible="
                  << ((memoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "yes" : "no")
                  << " device-local=" << ((memoryFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "yes" : "no")
                  << '\n';
    }
};

std::uint32_t researchIterations() {
    const char* text = std::getenv("ZVRAM_RESEARCH_ITERATIONS");
    if (!text) return 1;
    if (!*text) throw std::runtime_error("ZVRAM_RESEARCH_ITERATIONS must be a decimal integer from 1 to 16");
    std::uint32_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9')
            throw std::runtime_error("ZVRAM_RESEARCH_ITERATIONS must be a decimal integer from 1 to 16");
        const auto digit = static_cast<std::uint32_t>(*text - '0');
        if (value > (16u - digit) / 10u)
            throw std::runtime_error("ZVRAM_RESEARCH_ITERATIONS must be from 1 to 16");
        value = value * 10u + digit;
    }
    if (!value) throw std::runtime_error("ZVRAM_RESEARCH_ITERATIONS must be from 1 to 16");
    return value;
}

void run(Runtime& runtime, const std::vector<std::uint8_t>& encoded,
         const std::vector<std::uint8_t>& expected, std::uint32_t iterations,
         bool bp16 = false) {
    const auto rawSize = expected.size();
    if (!rawSize || rawSize > UINT32_MAX || encoded.size() > UINT32_MAX - 3u)
        throw std::runtime_error("shader input and output must fit nonzero uint32 byte offsets");
    const auto groups = bp16 ? (rawSize / 4 + 255) / 256 : (rawSize + TileBytes - 1) / TileBytes;
    if (!groups || groups > runtime.properties.limits.maxComputeWorkGroupCount[0])
        throw std::runtime_error(bp16 ? "BP16 dispatch exceeds maxComputeWorkGroupCount[0]"
                                      : "tile dispatch exceeds maxComputeWorkGroupCount[0]");
    const auto inputSize = (encoded.size() + 3u) & ~std::size_t(3u);
    const auto outputSize = (rawSize + 3u) & ~std::size_t(3u);
    if (inputSize > runtime.properties.limits.maxStorageBufferRange ||
        outputSize > runtime.properties.limits.maxStorageBufferRange)
        throw std::runtime_error("input/output exceeds maxStorageBufferRange");

    const auto hostCoherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const auto deviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const auto excluded = runtime.software ? VkMemoryPropertyFlags(0) : VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    Buffer upload, input, control, output, scratch, readback;
    upload.init(runtime, inputSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostCoherent);
    input.init(runtime, inputSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               deviceLocal, deviceLocal, excluded);
    control.init(runtime, 12, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 hostCoherent);
    output.init(runtime, outputSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, deviceLocal, deviceLocal, excluded);
    scratch.init(runtime, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 deviceLocal, deviceLocal, excluded);
    readback.init(runtime, outputSize + 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hostCoherent);
    std::memcpy(upload.mapped, encoded.data(), encoded.size());
    const std::uint32_t streamControl[3]{1, 0, 0};
    std::memcpy(control.mapped, streamControl, sizeof(streamControl));

    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = runtime.descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &runtime.setLayout;
    VkDescriptorSet set{};
    check(vkAllocateDescriptorSets(runtime.device, &setInfo, &set), "allocate descriptor set");
    Buffer* buffers[4]{&input, &control, &output, &scratch};
    VkDescriptorBufferInfo bufferInfos[4]{};
    VkWriteDescriptorSet writes[4]{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        bufferInfos[i] = {buffers[i]->buffer, 0, buffers[i]->descriptorSize};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(runtime.device, 4, writes, 0, nullptr);

    std::vector<VkCommandBuffer> commands(iterations);
    VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = runtime.commandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = iterations;
    check(vkAllocateCommandBuffers(runtime.device, &commandAllocation, commands.data()),
          "allocate command buffers");
    for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
        const auto command = commands[iteration];
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(command, &begin), "begin command buffer");
        const auto queryBase = iteration * 5;
        VkMemoryBarrier reuseReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        reuseReady.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                   VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        reuseReady.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuseReady,
                             0, nullptr, 0, nullptr);
        if (runtime.timestampPool)
            vkCmdResetQueryPool(command, runtime.timestampPool, queryBase, 5);
        if (runtime.timestampPool)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, runtime.timestampPool, queryBase);
        const VkBufferCopy uploadRegion{0, 0, inputSize};
        vkCmdCopyBuffer(command, upload.buffer, input.buffer, 1, &uploadRegion);
        if (runtime.timestampPool)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TRANSFER_BIT, runtime.timestampPool, queryBase + 1);
        vkCmdFillBuffer(command, control.buffer, 0, 4, 1);
        vkCmdFillBuffer(command, output.buffer, 0, outputSize, 0);
        vkCmdFillBuffer(command, scratch.buffer, 0, 4, 0);
        VkMemoryBarrier computeReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        computeReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
        computeReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &computeReady,
                             0, nullptr, 0, nullptr);
        if (runtime.timestampPool)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                runtime.timestampPool, queryBase + 2);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, runtime.pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, runtime.pipelineLayout,
                                0, 1, &set, 0, nullptr);
        vkCmdDispatch(command, static_cast<std::uint32_t>(groups), 1, 1);
        if (runtime.timestampPool)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                runtime.timestampPool, queryBase + 3);
        VkMemoryBarrier shaderToTransfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        shaderToTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        shaderToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkBufferMemoryBarrier hostRead[2]{};
        for (auto& barrier : hostRead) {
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.offset = 0;
        }
        hostRead[0].buffer = control.buffer; hostRead[0].size = 12;
        hostRead[1].buffer = scratch.buffer; hostRead[1].size = 4;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &shaderToTransfer, 2, hostRead, 0, nullptr);
        const VkBufferCopy readbackRegion{0, 0, outputSize};
        vkCmdCopyBuffer(command, output.buffer, readback.buffer, 1, &readbackRegion);
        const VkBufferCopy errorRegion{0, outputSize, 4};
        vkCmdCopyBuffer(command, scratch.buffer, readback.buffer, 1, &errorRegion);
        if (runtime.timestampPool)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TRANSFER_BIT, runtime.timestampPool, queryBase + 4);
        VkBufferMemoryBarrier readbackReady{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        readbackReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        readbackReady.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        readbackReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readbackReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readbackReady.buffer = readback.buffer;
        readbackReady.offset = 0;
        readbackReady.size = outputSize + 4;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &readbackReady, 0, nullptr);
        check(vkEndCommandBuffer(command), "end command buffer");
        if (iteration) check(vkResetFences(runtime.device, 1, &runtime.fence), "reset compute fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        const auto submitResult = vkQueueSubmit(runtime.queue, 1, &submit, runtime.fence);
        if (submitResult == VK_ERROR_DEVICE_LOST) runtime.abandonOnExit = true;
        check(submitResult, "submit GDeflate compute");
        const auto waitResult = vkWaitForFences(runtime.device, 1, &runtime.fence, VK_TRUE,
            runtime.software ? 30'000'000'000ull : WaitNanoseconds);
        if (waitResult == VK_TIMEOUT || waitResult == VK_ERROR_DEVICE_LOST)
            runtime.abandonOnExit = true;
        check(waitResult, "wait for GDeflate compute fence");

        const auto* actual = static_cast<const std::uint8_t*>(readback.mapped);
        std::uint32_t shaderError{};
        std::memcpy(&shaderError, actual + outputSize, sizeof(shaderError));
        if (shaderError)
            throw std::runtime_error(std::string(bp16 ? "BP16 shader rejected input, error mask="
                                                      : "bounded shader rejected tile, error mask=") +
                                     std::to_string(shaderError));
        const auto mismatch = std::mismatch(expected.begin(), expected.end(), actual);
        if (mismatch.first != expected.end()) {
            const auto* controlWords = static_cast<const std::uint32_t*>(control.mapped);
            const auto shown = std::min<std::size_t>(16, expected.size());
            std::cerr << "mismatch diagnostic offset="
                      << std::distance(expected.begin(), mismatch.first)
                      << " control=";
            if (controlWords)
                std::cerr << '{' << controlWords[0] << ',' << controlWords[1] << ','
                          << controlWords[2] << '}';
            else
                std::cerr << "unmapped";
            if (scratch.mapped)
                std::cerr << " scratch0=" << *static_cast<const std::uint32_t*>(scratch.mapped);
            else
                std::cerr << " scratch0=unmapped";
            std::cerr << (bp16 ? " groups=" : " tiles=") << groups
                      << "\nexpected=" << std::hex << std::setfill('0');
            for (std::size_t i = 0; i < shown; ++i)
                std::cerr << (i ? " " : "") << std::setw(2) << static_cast<unsigned>(expected[i]);
            std::cerr << "\nactual=";
            for (std::size_t i = 0; i < shown; ++i)
                std::cerr << (i ? " " : "") << std::setw(2) << static_cast<unsigned>(actual[i]);
            std::cerr << std::dec << '\n';
            throw std::runtime_error("decoded bytes differ at offset " +
                                     std::to_string(std::distance(expected.begin(), mismatch.first)));
        }
    }
    if (runtime.timestampPool) {
        std::vector<std::uint64_t> values(iterations * 5);
        check(vkGetQueryPoolResults(runtime.device, runtime.timestampPool, 0,
                                    static_cast<std::uint32_t>(values.size()),
                                    values.size() * sizeof(values[0]), values.data(),
                                    sizeof(values[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "read GPU timestamps");
        const auto mask = runtime.timestampValidBits == 64 ? UINT64_MAX :
                          ((std::uint64_t{1} << runtime.timestampValidBits) - 1);
        std::vector<std::uint64_t> uploadNs, decodeNs, copyNs;
        for (std::uint32_t i = 0; i < iterations; ++i) {
            const auto base = i * 5;
            uploadNs.push_back(static_cast<std::uint64_t>((((values[base + 1] - values[base]) & mask) *
                                                          runtime.properties.limits.timestampPeriod)));
            decodeNs.push_back(static_cast<std::uint64_t>((((values[base + 3] - values[base + 2]) & mask) *
                                                          runtime.properties.limits.timestampPeriod)));
            copyNs.push_back(static_cast<std::uint64_t>((((values[base + 4] - values[base + 3]) & mask) *
                                                        runtime.properties.limits.timestampPeriod)));
        }
        auto median = [](std::vector<std::uint64_t> samples) {
            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2];
        };
        std::cout << (runtime.software ? "cpu-software-timing iterations=" : "gpu-timing iterations=") << iterations
                  << " upload-ns-median=" << median(uploadNs)
                  << " decode-ns-median=" << median(decodeNs)
                  << " readback-copy-ns-median=" << median(copyNs) << '\n';
    } else {
        std::cout << "gpu-timing unavailable: compute queue has no timestamp bits\n";
    }
    std::cout << "PASS: decoded " << rawSize << " exact bytes in " << groups
              << (bp16 ? " BP16 workgroups" : " tile workgroups")
              << " across " << iterations << " iterations\n";
}

} // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    const bool bp16 = argc == 7 && std::strcmp(argv[1], "--codec") == 0 &&
                      std::strcmp(argv[2], "bp16") == 0 &&
                      (std::strcmp(argv[3], "--preflight-only") == 0 ||
                       std::strcmp(argv[3], "--gpu-bounded-smoke") == 0);
    const bool regular = argc == 5 &&
                         (std::strcmp(argv[1], "--preflight-only") == 0 ||
                          std::strcmp(argv[1], "--gpu-smoke") == 0 ||
                          std::strcmp(argv[1], "--gpu-bounded-smoke") == 0 ||
                          std::strcmp(argv[1], "--software-smoke") == 0);
    if (!bp16 && !regular) {
        std::cerr << "usage: vulkan_gdeflate_smoke --preflight-only|--gpu-smoke|--gpu-bounded-smoke|--software-smoke SHADER.spv ENCODED.bin EXPECTED.raw\n"
                     "       vulkan_gdeflate_smoke --codec bp16 --preflight-only|--gpu-bounded-smoke SHADER.spv FRAME.bp16 EXPECTED.raw\n";
        return 2;
    }
    const int arg = bp16 ? 3 : 1;
    const bool singleTile = !bp16 && std::strcmp(argv[arg], "--gpu-smoke") == 0;
    const bool gpu = singleTile || std::strcmp(argv[arg], "--gpu-bounded-smoke") == 0;
    const bool software = !bp16 && std::strcmp(argv[arg], "--software-smoke") == 0;
    ValidationCounts validation;
    int status = 1;
    try {
        const auto shader = readFile(argv[arg + 1], 4u * 1024u * 1024u);
        const auto encoded = readFile(argv[arg + 2], 64u * 1024u * 1024u);
        const auto expected = readFile(argv[arg + 3], 32u * 1024u * 1024u);
        if (shader.size() < 20 || shader.size() % 4 ||
            zvram::gdeflate::loadLe32(shader.data()) != 0x07230203u)
            throw std::runtime_error("invalid SPIR-V envelope");
        if (bp16) {
            zvram::bp16::FrameInfo frame{};
            if (!zvram::bp16::validate(encoded.data(), encoded.size(),
                                       static_cast<std::uint32_t>(expected.size()), &frame))
                throw std::runtime_error("invalid BP16 frame or expected raw size");
            std::cout << "preflight codec=BP16 blocks=" << frame.blockCount
                      << " decoded-bytes=" << frame.rawBytes
                      << " payload-bytes=" << (encoded.size() - frame.payloadBegin) << '\n';
        } else {
            const zvram::gdeflate::Limits limits{64u * 1024u * 1024u,
                32u * 1024u * 1024u, expected.size(), 0, 0, 512};
            zvram::gdeflate::Info envelope;
            if (!zvram::gdeflate::validateEnvelope(encoded.data(), encoded.size(), limits, &envelope))
                throw std::runtime_error("invalid GDeflate envelope");
            std::cout << "preflight tiles=" << envelope.tileCount << " decoded-bytes="
                      << envelope.decodedBytes << " payload-bytes=" << envelope.payloadBytes << '\n';
            if (singleTile && (envelope.tileCount != 1 || expected.size() > TileBytes))
                throw std::runtime_error("--gpu-smoke is restricted to one tile; use --gpu-bounded-smoke for up to 32 MiB");
        }
        if (!gpu && !software) {
            std::cout << (bp16 ? "CPU-only BP16 frame preflight; shader decode is unverified\n"
                               : "CPU-only envelope preflight; compressed payload and GPU decoder are unverified\n");
            return 0;
        }
        if (software) {
            const auto* driver = std::getenv("VK_DRIVER_FILES");
            if (!driver || !*driver || std::strchr(driver, ':'))
                throw std::runtime_error("software mode requires one explicitly isolated VK_DRIVER_FILES manifest");
        }
        const auto iterations = researchIterations();
        Runtime runtime(validation, software);
        runtime.initInstance();
        runtime.pickDevice(bp16);
        runtime.initPipeline(shader, iterations, bp16);
        run(runtime, encoded, expected, iterations, bp16);
        status = 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
    }
    std::cout << "validation-errors=" << validation.errors.load()
              << " validation-vuids=" << validation.vuids.load() << '\n';
    if (validation.errors.load()) return 1;
    return status;
}
