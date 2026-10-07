// Test-only LD_PRELOAD fixture: emulate native hipMalloc exhaustion without
// consuming the desktop GPU's memory. Never load this into a real workload.
#include <hip/hip_runtime_api.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdlib>

extern "C" hipError_t hipMalloc(void** pointer, size_t bytes) {
    if (pointer && bytes && !std::getenv("ZVRAM_TEST_VMM_FAIL_AFTER")) {
        *pointer = nullptr;
        return hipErrorOutOfMemory;
    }
    using Function = hipError_t (*)(void**, size_t);
    static auto next = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "hipMalloc"));
    return next ? next(pointer, bytes) : hipErrorNotSupported;
}

extern "C" hipError_t hipMemCreate(hipMemGenericAllocationHandle_t* handle,
                                    size_t bytes, const hipMemAllocationProp* properties,
                                    unsigned long long flags) {
    static std::atomic<unsigned long> calls{0};
    const char* limit = std::getenv("ZVRAM_TEST_VMM_FAIL_AFTER");
    if (limit && ++calls > std::strtoul(limit, nullptr, 10)) {
        if (handle) *handle = {};
        return hipErrorOutOfMemory;
    }
    using Function = hipError_t (*)(hipMemGenericAllocationHandle_t*, size_t,
                                    const hipMemAllocationProp*, unsigned long long);
    static auto next = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "hipMemCreate"));
    return next ? next(handle, bytes, properties, flags) : hipErrorNotSupported;
}
