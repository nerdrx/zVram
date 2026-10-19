#include <hip/hip_runtime_api.h>

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr size_t MiB = 1024u * 1024u;
constexpr size_t LocalLimit = 16u * MiB;
constexpr size_t SpillLimit = 64u * MiB;
constexpr size_t AllocationSize = 32u * MiB;
constexpr int ProcVersion = HIP_VERSION_MAJOR * 100 + HIP_VERSION_MINOR;

using HipMallocFn = hipError_t (*)(void**, size_t);
using HipFreeFn = hipError_t (*)(void*);
using HipMemGetInfoFn = hipError_t (*)(size_t*, size_t*);
using HipDeviceTotalMemFn = hipError_t (*)(size_t*, hipDevice_t);
using HipGetDevicePropertiesFn = hipError_t (*)(hipDeviceProp_t*, int);
using HipMemsetFn = hipError_t (*)(void*, int, size_t);
using HipMemcpyFn = hipError_t (*)(void*, const void*, size_t, hipMemcpyKind);
using HipGetProcAddressFn = hipError_t (*)(
    const char*, void**, int, uint64_t, hipDriverProcAddressQueryResult*);

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void check(hipError_t status, const char* operation) {
  if (status != hipSuccess)
    throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(status));
}

template <typename Function>
Function resolve(const char* name, int version = ProcVersion) {
  void* pointer = nullptr;
  hipDriverProcAddressQueryResult query = HIP_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
  check(hipGetProcAddress(name, &pointer, version, 0, &query), name);
  require(pointer != nullptr && query == HIP_GET_PROC_ADDRESS_SUCCESS,
          std::string("HIP resolver did not return ") + name);
  return reinterpret_cast<Function>(pointer);
}

void requireIntercepted(HipGetProcAddressFn intercepted, HipGetProcAddressFn native,
                       const char* symbol, int version = ProcVersion) {
  void* wrappedPointer = nullptr;
  void* nativePointer = nullptr;
  auto wrappedQuery = HIP_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
  auto nativeQuery = HIP_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
  check(intercepted(symbol, &wrappedPointer, version, 0, &wrappedQuery), symbol);
  check(native(symbol, &nativePointer, version, 0, &nativeQuery), "native resolver");
  require(wrappedQuery == HIP_GET_PROC_ADDRESS_SUCCESS && wrappedPointer &&
              wrappedPointer != nativePointer,
          std::string("resolver did not return zVram wrapper for ") + symbol);
}

void compareResolverResult(HipGetProcAddressFn intercepted, HipGetProcAddressFn native,
                           const char* symbol, int version, uint64_t flags) {
  void* wrappedPointer = nullptr;
  void* nativePointer = nullptr;
  auto wrappedQuery = HIP_GET_PROC_ADDRESS_SUCCESS;
  auto nativeQuery = HIP_GET_PROC_ADDRESS_SUCCESS;
  const hipError_t wrappedStatus =
      intercepted(symbol, &wrappedPointer, version, flags, &wrappedQuery);
  const hipError_t nativeStatus =
      native(symbol, &nativePointer, version, flags, &nativeQuery);
  require(wrappedStatus == nativeStatus && wrappedPointer == nativePointer &&
              wrappedQuery == nativeQuery,
          std::string("resolver changed unsuccessful/unsupported query: ") + symbol);
}

bool expectPhysicalMode(int argc, char** argv) {
  if (argc == 1) return false;
  require(argc == 2 && std::strcmp(argv[1], "--expect-physical") == 0,
          "usage: hip-proc-check [--expect-physical]");
  return true;
}
}  // namespace

int main(int argc, char** argv) try {
  const bool physicalMode = expectPhysicalMode(argc, argv);
  void* runtime = dlopen("libamdhip64.so", RTLD_NOW | RTLD_LOCAL);
  if (!runtime) {
    const char* error = dlerror();
    throw std::runtime_error(std::string("cannot open HIP runtime: ") +
                             (error ? error : "unknown loader error"));
  }
  auto nativeResolver = reinterpret_cast<HipGetProcAddressFn>(
      dlsym(runtime, "hipGetProcAddress"));
  require(nativeResolver != nullptr, "HIP runtime has no hipGetProcAddress");

  auto wrappedMalloc = resolve<HipMallocFn>("hipMalloc");
  auto wrappedFree = resolve<HipFreeFn>("hipFree");
  auto wrappedMemGetInfo = resolve<HipMemGetInfoFn>("hipMemGetInfo");
  auto wrappedDeviceTotalMem = resolve<HipDeviceTotalMemFn>("hipDeviceTotalMem");
  auto wrappedProperties = resolve<HipGetDevicePropertiesFn>("hipGetDeviceProperties");
  auto memset = resolve<HipMemsetFn>("hipMemset");
  auto memcpy = resolve<HipMemcpyFn>("hipMemcpy");

  for (const char* name : {"hipMalloc", "hipFree", "hipFreeAsync", "hipMemGetInfo",
                           "hipDeviceTotalMem", "hipGetDeviceProperties", "hipGetLastError",
                           "hipExtGetLastError", "hipPeekAtLastError"})
    requireIntercepted(hipGetProcAddress, nativeResolver, name);

  // Compare normal non-wrapper and failure cases with the direct runtime resolver.
  for (const char* name : {"hipMemset", "hipMemcpy"}) {
    void* wrapped = nullptr;
    void* native = nullptr;
    auto wrappedQuery = HIP_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
    auto nativeQuery = HIP_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
    check(hipGetProcAddress(name, &wrapped, ProcVersion, 0, &wrappedQuery), name);
    check(nativeResolver(name, &native, ProcVersion, 0, &nativeQuery), "native resolver");
    require(wrapped == native && wrappedQuery == nativeQuery,
            std::string("resolver modified unwrapped function: ") + name);
  }
  compareResolverResult(hipGetProcAddress, nativeResolver,
                       "zvram_hip_proc_check_missing_symbol", ProcVersion, 0);
  compareResolverResult(hipGetProcAddress, nativeResolver,
                       "hipGetDevicePropertiesR0500", 500, 0);
  compareResolverResult(hipGetProcAddress, nativeResolver, "hipMalloc", -1, 0);
  compareResolverResult(hipGetProcAddress, nativeResolver, "hipMalloc", ProcVersion, ~uint64_t{0});

  int device = -1;
  check(hipGetDevice(&device), "hipGetDevice");
  hipDeviceProp_t properties{};
  check(wrappedProperties(&properties, device), "resolved hipGetDeviceProperties");
  require(std::strncmp(properties.gcnArchName, "gfx1100", 7) == 0,
          "this integrity check requires gfx1100");

  size_t freeBefore = 0, totalBefore = 0;
  size_t deviceTotal = 0;
  check(wrappedMemGetInfo(&freeBefore, &totalBefore), "resolved hipMemGetInfo before");
  check(wrappedDeviceTotalMem(&deviceTotal, device), "resolved hipDeviceTotalMem");

  if (physicalMode) {
    require(totalBefore == deviceTotal && totalBefore == properties.totalGlobalMem,
            "physical-capacity HIP queries disagree");
    require(totalBefore > LocalLimit + SpillLimit,
            "physical mode unexpectedly reports the configured logical 80 MiB total");
  } else {
    require(totalBefore == LocalLimit + SpillLimit && deviceTotal == totalBefore &&
                properties.totalGlobalMem == totalBefore,
            "resolved HIP queries did not agree on the configured 80 MiB total");
    require(freeBefore == totalBefore, "expected 80 MiB free before the allocation");
  }

  void* devicePointer = nullptr;
  check(wrappedMalloc(&devicePointer, AllocationSize), "resolved hipMalloc");
  require(devicePointer != nullptr, "resolved hipMalloc returned null");
  struct Cleanup {
    HipFreeFn free{};
    void** pointer{};
    ~Cleanup() { if (pointer && *pointer) (void)free(*pointer); }
  } cleanup{wrappedFree, &devicePointer};

  if (!physicalMode) {
    hipPointerAttribute_t attributes{};
    check(hipPointerGetAttributes(&attributes, devicePointer), "hipPointerGetAttributes");
    require(attributes.type == hipMemoryTypeDevice && attributes.device == device &&
                !attributes.isManaged,
            "VMM allocation did not report device memory on the current GPU");
  }

  std::vector<std::uint8_t> expected(AllocationSize);
  std::vector<std::uint8_t> actual(AllocationSize);
  check(memset(devicePointer, 0xa5, AllocationSize), "resolved hipMemset");
  check(memcpy(actual.data(), devicePointer, AllocationSize, hipMemcpyDeviceToHost),
        "resolved hipMemcpy after memset");
  require(std::all_of(actual.begin(), actual.end(), [](std::uint8_t value) {
            return value == 0xa5;
          }),
          "resolved hipMemset did not fill every byte");

  for (size_t i = 0; i < expected.size(); ++i)
    expected[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xffu);
  check(memcpy(devicePointer, expected.data(), AllocationSize, hipMemcpyHostToDevice),
        "resolved hipMemcpy HtoD");

  size_t freeLive = 0, totalLive = 0;
  check(wrappedMemGetInfo(&freeLive, &totalLive), "resolved hipMemGetInfo live");
  if (!physicalMode)
    require(totalLive == totalBefore && freeLive == totalBefore - AllocationSize,
            "resolved HIP capacity did not move from 80 MiB to 48 MiB");

  check(memcpy(actual.data(), devicePointer, AllocationSize, hipMemcpyDeviceToHost),
        "resolved hipMemcpy DtoH");
  require(actual == expected, "resolved HIP memory functions failed byte-for-byte verification");
  check(wrappedFree(devicePointer), "resolved hipFree");
  devicePointer = nullptr;

  size_t freeAfter = 0, totalAfter = 0;
  check(wrappedMemGetInfo(&freeAfter, &totalAfter), "resolved hipMemGetInfo after free");
  if (!physicalMode)
    require(totalAfter == totalBefore && freeAfter == totalBefore,
            "resolved HIP capacity did not return from 48 MiB to 80 MiB");

  std::cout << "PASS: hipGetProcAddress wrappers, capacity queries, and 32 MiB byte check; mode="
            << (physicalMode ? "physical" : "logical-vmm")
            << " total=" << totalBefore << " free=" << freeBefore << "->" << freeLive
            << "->" << freeAfter << "\n";
  dlclose(runtime);
  return 0;
} catch (const std::exception& error) {
  std::cerr << "error: " << error.what() << '\n';
  return 1;
}
