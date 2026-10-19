// Test-only LD_PRELOAD fixture: emulate native hipMalloc exhaustion without
// consuming the desktop GPU's memory. Never load this into a real workload.
#include <hip/hip_runtime_api.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdlib>

namespace {
bool probeNativeOom() {
    using Malloc = hipError_t (*)(void**, size_t);
    using Free = hipError_t (*)(void*);
    static void* runtime = dlopen("libamdhip64.so", RTLD_NOW | RTLD_LOCAL);
    if (!runtime) return false;
    static auto nativeMalloc = reinterpret_cast<Malloc>(dlsym(runtime, "hipMalloc"));
    static auto nativeFree = reinterpret_cast<Free>(dlsym(runtime, "hipFree"));
    if (!nativeMalloc) return false;
    constexpr size_t probeBytes = size_t{64} * 1024 * 1024 * 1024;
    void* pointer = nullptr;
    const hipError_t status = nativeMalloc(&pointer, probeBytes);
    if (status == hipSuccess) {
        if (pointer && nativeFree) (void)nativeFree(pointer);
        return false;
    }
    return status == hipErrorOutOfMemory;
}
bool injectNativeOom() {
    if (!std::getenv("ZVRAM_TEST_NATIVE_ERROR")) return true;
    static std::atomic<bool> probed{false};
    return probed.exchange(true) || probeNativeOom();
}
}  // namespace

extern "C" hipError_t hipMalloc(void** pointer, size_t bytes) {
    if (pointer && bytes && !std::getenv("ZVRAM_TEST_VMM_FAIL_AFTER")) {
        if (!injectNativeOom()) {
            *pointer = nullptr;
            return hipErrorUnknown;
        }
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
    if (!injectNativeOom()) {
        if (handle) *handle = {};
        return hipErrorUnknown;
    }
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

extern "C" hipError_t hipMemMap(void* pointer, size_t bytes, size_t offset,
                                 hipMemGenericAllocationHandle_t handle,
                                 unsigned long long flags) {
    static std::atomic<unsigned long> calls{0};
    const char* limit = std::getenv("ZVRAM_TEST_VMM_MAP_FAIL_AFTER");
    if (limit && ++calls > std::strtoul(limit, nullptr, 10)) return hipErrorOutOfMemory;
    using Function = hipError_t (*)(void*, size_t, size_t,
                                    hipMemGenericAllocationHandle_t, unsigned long long);
    static auto next = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "hipMemMap"));
    return next ? next(pointer, bytes, offset, handle, flags) : hipErrorNotSupported;
}
