#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <vector>

namespace zvram {

// Clone loader-owned prefix nodes so a copied Features2 node can be placed at
// its original position without dropping loader dispatch state. Unknown prefixes fail
// closed because their structure size and copy semantics are not known here.
inline bool amendableFeatures2Chain(const void* chain, VkPhysicalDeviceFeatures2& amended,
                                    std::vector<VkLayerDeviceCreateInfo>& loaderPrefix,
                                    const void*& output) {
    loaderPrefix.clear();
    output = nullptr;
    const auto* node = static_cast<const VkBaseInStructure*>(chain);
    std::size_t prefixCount = 0;
    while (node && node->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
        if (node->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) return false;
        ++prefixCount;
        node = node->pNext;
    }
    if (!node) return false;

    const auto* features = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node);
    amended = *features;
    loaderPrefix.reserve(prefixCount);
    node = static_cast<const VkBaseInStructure*>(chain);
    for (std::size_t i = 0; i < prefixCount; ++i) {
        loaderPrefix.push_back(*reinterpret_cast<const VkLayerDeviceCreateInfo*>(node));
        node = node->pNext;
    }
    amended.pNext = features->pNext;
    for (std::size_t i = 0; i < loaderPrefix.size(); ++i)
        loaderPrefix[i].pNext = i + 1 < loaderPrefix.size()
            ? static_cast<const void*>(&loaderPrefix[i + 1])
            : static_cast<const void*>(&amended);
    output = loaderPrefix.empty() ? static_cast<const void*>(&amended)
                                  : static_cast<const void*>(loaderPrefix.data());
    return true;
}

} // namespace zvram
