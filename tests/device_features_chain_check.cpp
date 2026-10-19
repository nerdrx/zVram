#include "device_features_chain.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return EXIT_FAILURE; \
    } \
} while (false)

int main() {
    VkLayerDeviceLink link{};
    VkPhysicalDeviceVulkan12Features suffix{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.features.sparseBinding = VK_FALSE;
    features.pNext = &suffix;
    VkLayerDeviceCreateInfo second{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO};
    second.function = VK_LAYER_LINK_INFO;
    second.u.pLayerInfo = &link;
    second.pNext = &features;
    VkLayerDeviceCreateInfo first{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO};
    first.function = VK_LAYER_LINK_INFO;
    first.u.pLayerInfo = &link;
    first.pNext = &second;

    VkPhysicalDeviceFeatures2 amended{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    std::vector<VkLayerDeviceCreateInfo> prefix;
    const void* output = nullptr;
    CHECK(zvram::amendableFeatures2Chain(&first, amended, prefix, output));
    CHECK(prefix.size() == 2);
    amended.features.sparseBinding = VK_TRUE;
    const auto* outFirst = static_cast<const VkLayerDeviceCreateInfo*>(output);
    const auto* outSecond = static_cast<const VkLayerDeviceCreateInfo*>(outFirst->pNext);
    CHECK(outFirst != &first && outSecond != &second);
    CHECK(outFirst->sType == first.sType && outFirst->function == first.function && outFirst->u.pLayerInfo == &link);
    CHECK(outSecond->sType == second.sType && outSecond->function == second.function && outSecond->u.pLayerInfo == &link);
    CHECK(outSecond->pNext == &amended && amended.pNext == &suffix);
    CHECK(features.features.sparseBinding == VK_FALSE && first.pNext == &second && second.pNext == &features);

    VkPhysicalDeviceFeatures2 head{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    head.pNext = &suffix;
    CHECK(zvram::amendableFeatures2Chain(&head, amended, prefix, output));
    CHECK(prefix.empty() && output == &amended && amended.pNext == &suffix && head.pNext == &suffix);

    VkDeviceGroupDeviceCreateInfo unknown{VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO};
    unknown.pNext = &features;
    output = &head;
    CHECK(!zvram::amendableFeatures2Chain(&unknown, amended, prefix, output));
    CHECK(output == nullptr && prefix.empty() && unknown.pNext == &features);
    return EXIT_SUCCESS;
}
