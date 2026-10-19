#pragma once

#include <algorithm>
#include <cstdint>

namespace zvram {

// VK_EXT_memory_budget reports process usage including these tracked backing
// allocations; subtract them before deriving the cap for the backing itself.
// See https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryBudgetPropertiesEXT.html
inline constexpr std::uint64_t residentBudgetLimit(std::uint64_t hardCap,
                                                    std::uint64_t budget,
                                                    std::uint64_t usage,
                                                    std::uint64_t trackedOnHeap,
                                                    std::uint64_t reserve) noexcept {
    const auto otherUsage = usage > trackedOnHeap ? usage - trackedOnHeap : 0;
    const auto available = budget > reserve ? budget - reserve : 0;
    const auto trackedCap = available > otherUsage ? available - otherUsage : 0;
    return std::min(hardCap, trackedCap);
}

} // namespace zvram
