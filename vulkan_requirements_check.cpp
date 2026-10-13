#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

bool hasExtension(VkPhysicalDevice device, const char* name) {
    std::uint32_t count{};
    check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr),
          "enumerate device extensions");
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()),
          "enumerate device extensions");
    for (const auto& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    return false;
}

struct Candidate {
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::uint32_t queueFamily{};
    bool maintenance4Core{};
    bool maintenance4Extension{};
#ifdef VK_KHR_maintenance6
    bool maintenance6Extension{};
#endif
};

struct BufferGuard {
    VkDevice device{};
    VkBuffer handle{};
    ~BufferGuard() { if (handle) vkDestroyBuffer(device, handle, nullptr); }
};

struct MemoryGuard {
    VkDevice device{};
    VkDeviceMemory handle{};
    ~MemoryGuard() { if (handle) vkFreeMemory(device, handle, nullptr); }
};

void compareRequirements(const char* label, const VkMemoryRequirements2& expected,
                         const VkMemoryDedicatedRequirements& expectedDedicated,
                         const VkMemoryRequirements2& actual,
                         const VkMemoryDedicatedRequirements& actualDedicated) {
    const auto& a = expected.memoryRequirements;
    const auto& b = actual.memoryRequirements;
    if (a.size != b.size || a.alignment != b.alignment || a.memoryTypeBits != b.memoryTypeBits ||
        expectedDedicated.prefersDedicatedAllocation != actualDedicated.prefersDedicatedAllocation ||
        expectedDedicated.requiresDedicatedAllocation != actualDedicated.requiresDedicatedAllocation) {
        throw std::runtime_error(std::string(label) +
            " pre-create and created-buffer memory requirements differ");
    }
}

int run(bool bindStatusMode) {
#ifndef VK_KHR_maintenance6
    if (bindStatusMode) {
        std::cerr << "UNSUPPORTED: bind-status mode requires VK_KHR_maintenance6 headers\n";
        return 77;
    }
#endif
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerateVersion) check(enumerateVersion(&loaderVersion), "query Vulkan loader version");
    if (VK_API_VERSION_MAJOR(loaderVersion) < 1 ||
        (VK_API_VERSION_MAJOR(loaderVersion) == 1 && VK_API_VERSION_MINOR(loaderVersion) < 1)) {
        std::cerr << "UNSUPPORTED: maintenance4 consistency check requires Vulkan 1.1 loader support\n";
        return 77;
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.apiVersion = loaderVersion < VK_API_VERSION_1_3 ? loaderVersion : VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    VkInstance instance{};
    check(vkCreateInstance(&instanceInfo, nullptr, &instance), "create Vulkan instance");

    try {
        std::uint32_t count{};
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate physical devices");
        if (!count) {
            std::cerr << "UNSUPPORTED: no Vulkan physical devices\n";
            vkDestroyInstance(instance, nullptr);
            return 77;
        }
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate physical devices");

        Candidate selected{};
        for (const auto physical : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physical, &properties);
            const bool core = application.apiVersion >= VK_API_VERSION_1_3 &&
                              properties.apiVersion >= VK_API_VERSION_1_3;
            const bool extension = hasExtension(physical, VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
            if (!core && !extension) continue;
#ifdef VK_KHR_maintenance6
            const bool maintenance6Extension = hasExtension(physical, VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
            if (bindStatusMode && !maintenance6Extension) continue;
#endif

            VkPhysicalDeviceMaintenance4Features maintenance4{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &maintenance4;
#ifdef VK_KHR_maintenance6
            VkPhysicalDeviceMaintenance6FeaturesKHR maintenance6{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
            if (bindStatusMode) maintenance4.pNext = &maintenance6;
#endif
            vkGetPhysicalDeviceFeatures2(physical, &features);
            if (!maintenance4.maintenance4 || !features.features.sparseBinding ||
                !features.features.sparseResidencyBuffer) continue;
#ifdef VK_KHR_maintenance6
            if (bindStatusMode && !maintenance6.maintenance6) continue;
#endif

            std::uint32_t familyCount{};
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
            std::uint32_t sparseFamily = UINT32_MAX;
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)) {
                    sparseFamily = i;
                    break;
                }
            }
            if (sparseFamily == UINT32_MAX) continue;
            selected.physical = physical;
            selected.properties = properties;
            selected.queueFamily = sparseFamily;
            selected.maintenance4Core = core;
            selected.maintenance4Extension = extension;
#ifdef VK_KHR_maintenance6
            selected.maintenance6Extension = maintenance6Extension;
#endif
            vkGetPhysicalDeviceMemoryProperties(physical, &selected.memory);
            break;
        }
        if (!selected.physical) {
            std::cerr << (bindStatusMode
                ? "UNSUPPORTED: no device exposes maintenance4+6, sparse binding/residency, and a sparse queue\n"
                : "UNSUPPORTED: no device exposes maintenance4, sparse binding, sparse buffer residency, and a sparse queue\n");
            vkDestroyInstance(instance, nullptr);
            return 77;
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = selected.queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceMaintenance4Features maintenance4{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
        maintenance4.maintenance4 = VK_TRUE;
        VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        enabled.features.sparseBinding = VK_TRUE;
        enabled.features.sparseResidencyBuffer = VK_TRUE;
        enabled.pNext = &maintenance4;
        const char* maintenanceExtensions[2] = {VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
#ifdef VK_KHR_maintenance6
                                                  VK_KHR_MAINTENANCE_6_EXTENSION_NAME
#else
                                                  nullptr
#endif
        };
        const bool enableExtension = selected.maintenance4Extension;
#ifdef VK_KHR_maintenance6
        VkPhysicalDeviceMaintenance6FeaturesKHR enabledMaintenance6{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
        if (bindStatusMode) {
            enabledMaintenance6.maintenance6 = VK_TRUE;
            maintenance4.pNext = &enabledMaintenance6;
        }
#endif
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &enabled;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        if (enableExtension) deviceInfo.ppEnabledExtensionNames = maintenanceExtensions;
#ifdef VK_KHR_maintenance6
        if (bindStatusMode && selected.maintenance6Extension) {
            deviceInfo.enabledExtensionCount = enableExtension ? 2 : 1;
            deviceInfo.ppEnabledExtensionNames = enableExtension
                ? maintenanceExtensions : &maintenanceExtensions[1];
        } else
#endif
        if (enableExtension) deviceInfo.enabledExtensionCount = 1;
        VkDevice device{};
        check(vkCreateDevice(selected.physical, &deviceInfo, nullptr, &device), "create maintenance4 device");
        try {
#ifdef VK_KHR_maintenance6
            if (bindStatusMode) {
                const char* virtualEnv = std::getenv("ZVRAM_VULKAN_VIRTUAL_MIB");
                if (!virtualEnv || !*virtualEnv || std::strcmp(virtualEnv, "0") == 0)
                    throw std::runtime_error("bind-status mode requires an exposed virtual memory type");
                const std::uint32_t virtualType = selected.memory.memoryTypeCount
                    ? selected.memory.memoryTypeCount - 1 : UINT32_MAX;
                if (virtualType >= 32 ||
                    !(selected.memory.memoryTypes[virtualType].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                    throw std::runtime_error("last advertised memory type is not the expected virtual device-local type");

                {
                VkBufferCreateInfo eligibleCreate{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                eligibleCreate.size = 4ull * 1024ull * 1024ull;
                eligibleCreate.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                eligibleCreate.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VkBufferCreateInfo nativeCreate{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                nativeCreate.size = 4ull * 1024ull;
                nativeCreate.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                nativeCreate.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

                MemoryGuard eligibleMemory{device};
                BufferGuard eligibleBuffer{device};
                check(vkCreateBuffer(device, &eligibleCreate, nullptr, &eligibleBuffer.handle),
                      "create eligible bind-status buffer");
                VkBufferMemoryRequirementsInfo2 eligibleInfo{
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
                eligibleInfo.buffer = eligibleBuffer.handle;
                VkMemoryDedicatedRequirements eligibleDedicated{
                    VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
                VkMemoryRequirements2 eligibleRequirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
                eligibleRequirements.pNext = &eligibleDedicated;
                vkGetBufferMemoryRequirements2(device, &eligibleInfo, &eligibleRequirements);
                const VkMemoryRequirements& eligibleReq = eligibleRequirements.memoryRequirements;
                const std::uint32_t virtualMask = 1u << virtualType;
                if (!(eligibleReq.memoryTypeBits & virtualMask))
                    throw std::runtime_error("eligible bind-status buffer omits the advertised virtual type");
                VkMemoryDedicatedAllocateInfo eligibleDedicatedInfo{
                    VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
                eligibleDedicatedInfo.buffer = eligibleBuffer.handle;
                VkMemoryAllocateInfo eligibleAllocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                eligibleAllocate.allocationSize = eligibleReq.size;
                eligibleAllocate.memoryTypeIndex = virtualType;
                if (eligibleDedicated.requiresDedicatedAllocation)
                    eligibleAllocate.pNext = &eligibleDedicatedInfo;
                check(vkAllocateMemory(device, &eligibleAllocate, nullptr, &eligibleMemory.handle),
                      "allocate eligible virtual bind-status memory");

                MemoryGuard nativeMemory{device};
                BufferGuard nativeBuffer{device};
                check(vkCreateBuffer(device, &nativeCreate, nullptr, &nativeBuffer.handle),
                      "create native bind-status buffer");
                VkBufferMemoryRequirementsInfo2 nativeInfo{
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
                nativeInfo.buffer = nativeBuffer.handle;
                VkMemoryDedicatedRequirements nativeDedicated{
                    VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
                VkMemoryRequirements2 nativeRequirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
                nativeRequirements.pNext = &nativeDedicated;
                vkGetBufferMemoryRequirements2(device, &nativeInfo, &nativeRequirements);
                const auto& nativeReq = nativeRequirements.memoryRequirements;
                std::uint32_t nativeType = UINT32_MAX;
                for (std::uint32_t i = 0; i < selected.memory.memoryTypeCount && i < 32; ++i) {
                    if (i != virtualType && (nativeReq.memoryTypeBits & (1u << i)) &&
                        (selected.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                        nativeType = i;
                        break;
                    }
                }
                if (nativeType == UINT32_MAX)
                    throw std::runtime_error("no compatible native host-visible memory type for uniform buffer");
                VkMemoryDedicatedAllocateInfo nativeDedicatedInfo{
                    VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
                nativeDedicatedInfo.buffer = nativeBuffer.handle;
                VkMemoryAllocateInfo nativeAllocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                nativeAllocate.allocationSize = nativeReq.size;
                nativeAllocate.memoryTypeIndex = nativeType;
                if (nativeDedicated.requiresDedicatedAllocation)
                    nativeAllocate.pNext = &nativeDedicatedInfo;
                check(vkAllocateMemory(device, &nativeAllocate, nullptr, &nativeMemory.handle),
                      "allocate native bind-status memory");

                VkResult eligibleResult = VK_ERROR_UNKNOWN;
                VkResult nativeResult = VK_ERROR_UNKNOWN;
                VkBindMemoryStatusKHR eligibleStatus{VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR};
                eligibleStatus.pResult = &eligibleResult;
                VkBindMemoryStatusKHR nativeStatus{VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR};
                nativeStatus.pResult = &nativeResult;
                VkBindBufferMemoryInfo binds[2] = {
                    {VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO, &eligibleStatus,
                     eligibleBuffer.handle, eligibleMemory.handle, 0},
                    {VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO, &nativeStatus,
                     nativeBuffer.handle, nativeMemory.handle, 0}
                };
                const VkResult batchResult = vkBindBufferMemory2(device, 2, binds);
                if (batchResult != VK_SUCCESS || eligibleResult != VK_SUCCESS || nativeResult != VK_SUCCESS)
                    throw std::runtime_error("bind-status batch failed: return=" +
                        std::to_string(batchResult) + " virtual=" + std::to_string(eligibleResult) +
                        " native=" + std::to_string(nativeResult));
                std::cout << "PASS: maintenance6 bind-status batch; virtual and native binds succeeded\n";
                }
                vkDestroyDevice(device, nullptr);
                vkDestroyInstance(instance, nullptr);
                return 0;
            }
#endif
            const auto getDeviceRequirements = reinterpret_cast<PFN_vkGetDeviceBufferMemoryRequirements>(
                vkGetDeviceProcAddr(device, "vkGetDeviceBufferMemoryRequirements"));
            const auto getDeviceRequirementsKHR = enableExtension
                ? reinterpret_cast<PFN_vkGetDeviceBufferMemoryRequirementsKHR>(
                    vkGetDeviceProcAddr(device, "vkGetDeviceBufferMemoryRequirementsKHR"))
                : nullptr;
            const auto getBufferRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(
                vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2"));
            if ((selected.maintenance4Core && !getDeviceRequirements) ||
                (!selected.maintenance4Core && !getDeviceRequirementsKHR) ||
                (enableExtension && !getDeviceRequirementsKHR) || !getBufferRequirements) {
                std::cerr << "UNSUPPORTED: maintenance4 requirement query entry points are unavailable\n";
                vkDestroyDevice(device, nullptr);
                vkDestroyInstance(instance, nullptr);
                return 77;
            }

            const char* virtualEnv = std::getenv("ZVRAM_VULKAN_VIRTUAL_MIB");
            const bool expectVirtual = virtualEnv && *virtualEnv && std::strcmp(virtualEnv, "0") != 0;
            const std::uint32_t virtualType = selected.memory.memoryTypeCount
                ? selected.memory.memoryTypeCount - 1 : UINT32_MAX;
            if (!selected.memory.memoryTypeCount || (expectVirtual && virtualType >= 32))
                throw std::runtime_error("invalid exposed Vulkan memory type table");
            const VkDeviceSize bufferBytes = 4ull * 1024ull * 1024ull;

            auto checkBuffer = [&](const char* label, VkBufferUsageFlags usage, bool eligible) {
                VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                create.size = bufferBytes;
                create.usage = usage;
                create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VkDeviceBufferMemoryRequirements queryInfo{VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS};
                queryInfo.pCreateInfo = &create;
                VkMemoryDedicatedRequirements queriedDedicated{
                    VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
                VkMemoryRequirements2 queried{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
                queried.pNext = &queriedDedicated;
                if (selected.maintenance4Core) getDeviceRequirements(device, &queryInfo, &queried);
                else getDeviceRequirementsKHR(device, &queryInfo, &queried);

                const auto virtualMask = expectVirtual ? (1u << virtualType) : 0u;
                if (eligible && expectVirtual && !(queried.memoryRequirements.memoryTypeBits & virtualMask))
                    throw std::runtime_error("eligible pre-create query omitted expected virtual memory type");
                if (!eligible && expectVirtual && (queried.memoryRequirements.memoryTypeBits & virtualMask))
                    throw std::runtime_error("noneligible pre-create query unexpectedly included virtual memory type");

                if (enableExtension && getDeviceRequirementsKHR) {
                    VkMemoryDedicatedRequirements aliasDedicated{
                        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
                    VkMemoryRequirements2 aliasQueried{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
                    aliasQueried.pNext = &aliasDedicated;
                    getDeviceRequirementsKHR(device, &queryInfo, &aliasQueried);
                    compareRequirements(label, queried, queriedDedicated, aliasQueried, aliasDedicated);
                }

                BufferGuard buffer{device};
                check(vkCreateBuffer(device, &create, nullptr, &buffer.handle),
                      "create requirements-check buffer");
                VkBufferMemoryRequirementsInfo2 createdInfo{
                    VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
                createdInfo.buffer = buffer.handle;
                VkMemoryDedicatedRequirements createdDedicated{
                    VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
                VkMemoryRequirements2 created{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
                created.pNext = &createdDedicated;
                getBufferRequirements(device, &createdInfo, &created);
                compareRequirements(label, queried, queriedDedicated, created, createdDedicated);
                if (eligible && expectVirtual && !(created.memoryRequirements.memoryTypeBits & virtualMask))
                    throw std::runtime_error("eligible created buffer omitted expected virtual memory type");
                if (!eligible && expectVirtual && (created.memoryRequirements.memoryTypeBits & virtualMask))
                    throw std::runtime_error("noneligible created buffer unexpectedly included virtual memory type");
                std::cout << "PASS: " << label << " requirements match; virtual-bit="
                          << ((queried.memoryRequirements.memoryTypeBits & virtualMask) != 0) << '\n';
            };

            checkBuffer("eligible storage+transfer", VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            checkBuffer("noneligible uniform-only", VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false);
            std::cout << "PASS: maintenance4 requirements queries match created buffers"
                      << " api=" << VK_API_VERSION_MAJOR(selected.properties.apiVersion) << '.'
                      << VK_API_VERSION_MINOR(selected.properties.apiVersion)
                      << " khr-alias-enabled=" << enableExtension << '\n';
        } catch (...) {
            vkDestroyDevice(device, nullptr);
            throw;
        }
        vkDestroyDevice(device, nullptr);
    } catch (...) {
        vkDestroyInstance(instance, nullptr);
        throw;
    }
    vkDestroyInstance(instance, nullptr);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        bool bindStatusMode = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--bind-status") == 0) bindStatusMode = true;
            else throw std::runtime_error(std::string("unknown option: ") + argv[i]);
        }
        return run(bindStatusMode);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
