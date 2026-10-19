#include <hip/hip_runtime_api.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace {
void check(hipError_t result) {
    if (result != hipSuccess) throw std::runtime_error(hipGetErrorString(result));
}
size_t descriptorCount() {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}
struct Allocations {
    std::vector<void*> pointers;
    ~Allocations() { for (void* pointer : pointers) (void)hipFree(pointer); }
};
}

int main() try {
    int count = 0, selected = -1;
    check(hipGetDeviceCount(&count));
    for (int device = 0; device < count; ++device) {
        hipDeviceProp_t properties{};
        check(hipGetDeviceProperties(&properties, device));
        if (!properties.integrated) { selected = device; break; }
    }
    if (selected < 0) throw std::runtime_error("No discrete HIP device");
    check(hipSetDevice(selected));
    check(hipFree(nullptr)); // Initialize native runtime before counting.
    const size_t before = descriptorCount();
    Allocations allocations;
    allocations.pointers.reserve(256);
    for (size_t index = 0; index < 256; ++index) {
        void* pointer = nullptr;
        check(hipMalloc(&pointer, 4096));
        allocations.pointers.push_back(pointer);
        check(hipMemset(pointer, static_cast<int>(index), 4096));
    }
    check(hipDeviceSynchronize());
    const size_t during = descriptorCount();
    if (during > before + 16) throw std::runtime_error("DRM descriptors grow with allocation count");
    std::array<unsigned char, 4096> output{};
    for (size_t index = 0; index < allocations.pointers.size(); ++index) {
        check(hipMemcpy(output.data(), allocations.pointers[index], output.size(), hipMemcpyDeviceToHost));
        for (unsigned char byte : output)
            if (byte != static_cast<unsigned char>(index)) throw std::runtime_error("GPU byte mismatch");
    }
    while (!allocations.pointers.empty()) {
        check(hipFree(allocations.pointers.back()));
        allocations.pointers.pop_back();
    }
    const size_t after = descriptorCount();
    if (after > before + 4) throw std::runtime_error("DRM descriptors retained after cleanup");
    std::printf("PASS: 256 live GTT VMM allocations, every byte verified; descriptors before=%zu during=%zu after=%zu\n", before, during, after);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "VMM descriptor check: %s\n", error.what());
    return 1;
}
