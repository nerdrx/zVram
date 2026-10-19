#include <hip/hip_runtime_api.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

static void check(hipError_t status) {
    if (status != hipSuccess) throw std::runtime_error(hipGetErrorString(status));
}
static void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

int main() try {
    int count = 0, device = -1;
    check(hipGetDeviceCount(&count));
    hipDeviceProp_t properties{};
    for (int i = 0; i < count; ++i) {
        check(hipGetDeviceProperties(&properties, i));
        if (std::strncmp(properties.gcnArchName, "gfx1100", 7) == 0) { device = i; break; }
    }
    require(device >= 0, "gfx1100 required for this hardware check");
    check(hipSetDevice(device));
    size_t beforeFree = 0, beforeTotal = 0, deviceTotal = 0;
    check(hipMemGetInfo(&beforeFree, &beforeTotal));
    check(hipDeviceTotalMem(&deviceTotal, device));
    check(hipGetDeviceProperties(&properties, device));
    require(beforeTotal == deviceTotal && beforeTotal == properties.totalGlobalMem,
            "HIP capacity queries disagree");
    const char* setting = std::getenv("ZVRAM_HIP_REPORT_CAPACITY");
    const bool logical = setting && std::strcmp(setting, "1") == 0;
    constexpr size_t MiB = 1024 * 1024;
    if (logical) require(beforeTotal == 80 * MiB && beforeFree == beforeTotal,
                         "expected a free 16 MiB + 64 MiB virtual tier");
    void* pointer = nullptr;
    check(hipMalloc(&pointer, 32 * MiB));
    size_t duringFree = 0, duringTotal = 0;
    const hipError_t query = hipMemGetInfo(&duringFree, &duringTotal);
    const hipError_t freed = hipFree(pointer);
    check(query); check(freed);
    require(duringTotal == beforeTotal && duringFree <= duringTotal,
            "capacity total changed or free exceeded total");
    if (logical) require(duringFree == 48 * MiB, "live 16 MiB + 16 MiB backing was not accounted");
    size_t afterFree = 0, afterTotal = 0;
    check(hipMemGetInfo(&afterFree, &afterTotal));
    require(afterTotal == beforeTotal, "capacity total changed after free");
    if (logical) require(afterFree == beforeFree, "free capacity did not recover");
    std::printf("PASS: %s capacity total=%zu free(before,during,after)=%zu,%zu,%zu; three HIP queries agree\n",
                logical ? "configured virtual" : "native physical", beforeTotal,
                beforeFree, duringFree, afterFree);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "capacity check: %s\n", error.what());
    return 1;
}
