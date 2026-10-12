#pragma once

#include <vulkan/vulkan.h>

namespace zvram {
// VK_KHR_internally_synchronized_queues assigns this stable flag value even
// when older distro headers do not yet declare its name.
constexpr VkDeviceQueueCreateFlags kAllowedQueueCreateFlags = 0x4;
}
