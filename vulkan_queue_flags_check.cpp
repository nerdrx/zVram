#include <vulkan/vulkan.h>
#undef VK_KHR_internally_synchronized_queues
#include "vulkan_queue_flags.hpp"

static_assert((0x4u & ~zvram::kAllowedQueueCreateFlags) == 0,
              "internally synchronized queues must remain supported with old headers");
static_assert((0x1u & ~zvram::kAllowedQueueCreateFlags) != 0,
              "protected queues must remain unsupported");
static_assert((0x8u & ~zvram::kAllowedQueueCreateFlags) != 0,
              "unknown queue flags must remain unsupported");

int main() {}
