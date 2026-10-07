#include "active_refs.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <vector>

template <typename Handle>
Handle fakeHandle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<Handle>(value);
    else return static_cast<Handle>(value);
}

static void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    ActiveRefs refs;
    const auto queue = fakeHandle<VkQueue>(1);
    const auto otherQueue = fakeHandle<VkQueue>(2);
    const auto a = fakeHandle<VkDeviceMemory>(11);
    const auto b = fakeHandle<VkDeviceMemory>(12);
    const auto c = fakeHandle<VkDeviceMemory>(13);

    refs.record(queue, {a, b, a}, true);
    require(refs.busy(a) && refs.busy(b), "tail resources busy and duplicates harmless");
    require(refs.status(queue).hasTail && !refs.status(queue).hasCovered, "tail status");
    require(refs.cover(queue), "first tail covered");
    refs.record(queue, {c}, true);
    require(refs.status(queue).hasCovered && refs.status(queue).hasTail,
            "covered epoch and newer tail coexist");
    require(!refs.cover(queue), "cannot cover before prior epoch retires");
    refs.retire(queue);
    require(!refs.busy(a) && !refs.busy(b) && refs.busy(c), "retirement preserves newer tail");
    require(refs.cover(queue), "newer tail covered after retirement");
    refs.retire(queue);

    refs.record(otherQueue, {}, true);
    require(refs.cover(otherQueue), "empty known submit still creates covered epoch");
    require(refs.status(otherQueue).hasCovered && !refs.status(otherQueue).blocksAll,
            "empty covered epoch status");
    refs.record(queue, {a}, false);
    require(refs.status(queue).blocksAll && refs.busy(a) && refs.busy(c),
            "unknown tail blocks all resources");
    require(refs.cover(queue), "unknown tail covered");
    require(refs.busy(a) && refs.busy(c), "unknown covered epoch blocks all resources");
    refs.retire(queue);
    require(!refs.status(queue).blocksAll && !refs.busy(a) && !refs.busy(c),
            "unknown state clears only when epoch retires");

    std::cout << "active refs checks passed\n";
}
