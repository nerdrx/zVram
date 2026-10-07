#include <hip/hip_runtime_api.h>
#include "hip_activity.hpp"

#ifdef ZVRAM_HAS_DRM_VMM
#include <amdgpu.h>
#include <amdgpu_drm.h>
#endif

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

#define ZVRAM_STRINGIFY_IMPL(name) #name
#define ZVRAM_STRINGIFY(name) ZVRAM_STRINGIFY_IMPL(name)

extern "C" __attribute__((visibility("hidden"))) hipError_t zvramWrappedHipMalloc(void**, size_t);
extern "C" __attribute__((visibility("hidden"))) hipError_t zvramWrappedHipFree(void*);
extern "C" __attribute__((visibility("hidden"))) hipError_t zvramWrappedHipFreeAsync(void*, hipStream_t);
extern "C" __attribute__((visibility("hidden"))) hipError_t zvramWrappedHipMemGetInfo(size_t*, size_t*);
extern "C" __attribute__((visibility("hidden"))) hipError_t zvramWrappedHipDeviceTotalMem(size_t*, hipDevice_t);
extern "C" __attribute__((visibility("hidden"))) hipError_t zvramWrappedHipGetDeviceProperties(hipDeviceProp_t*, int);
extern "C" __attribute__((visibility("hidden"))) hipError_t zvramWrappedHipGetLastError();
extern "C" __attribute__((visibility("hidden"))) hipError_t zvramWrappedHipExtGetLastError();
extern "C" __attribute__((visibility("hidden"))) hipError_t zvramWrappedHipPeekAtLastError();

using HipMallocFn = hipError_t (*)(void**, size_t);
using HipHostMallocFn = hipError_t (*)(void**, size_t, unsigned int);
using HipHostGetDevicePointerFn = hipError_t (*)(void**, void*, unsigned int);
using HipFreeFn = hipError_t (*)(void*);
using HipHostFreeFn = hipError_t (*)(void*);
using HipGetDeviceFn = hipError_t (*)(int*);
using HipSetDeviceFn = hipError_t (*)(int);
using HipDeviceSynchronizeFn = hipError_t (*)();
using HipGetDeviceAttributeFn = hipError_t (*)(int*, hipDeviceAttribute_t, int);
using HipFreeAsyncFn = hipError_t (*)(void*, hipStream_t);
using HipMemGetInfoFn = hipError_t (*)(size_t*, size_t*);
using HipDeviceTotalMemFn = hipError_t (*)(size_t*, hipDevice_t);
using HipGetDevicePropertiesFn = hipError_t (*)(hipDeviceProp_t*, int);
using HipGetProcAddressFn = hipError_t (*)(const char*, void**, int, uint64_t,
                                            hipDriverProcAddressQueryResult*);
using HipLastErrorFn = hipError_t (*)();
using HipMemGetGranularityFn = hipError_t (*)(size_t*, const hipMemAllocationProp*,
                                               hipMemAllocationGranularity_flags);
using HipMemCreateFn = hipError_t (*)(hipMemGenericAllocationHandle_t*, size_t,
                                      const hipMemAllocationProp*, unsigned long long);
using HipMemAddressReserveFn = hipError_t (*)(void**, size_t, size_t, void*,
                                              unsigned long long);
using HipMemMapFn = hipError_t (*)(void*, size_t, size_t,
                                   hipMemGenericAllocationHandle_t,
                                   unsigned long long);
using HipMemSetAccessFn = hipError_t (*)(void*, size_t, const hipMemAccessDesc*, size_t);
using HipMemUnmapFn = hipError_t (*)(void*, size_t);
using HipMemReleaseFn = hipError_t (*)(hipMemGenericAllocationHandle_t);
using HipMemAddressFreeFn = hipError_t (*)(void*, size_t);
using HipMemImportFn = hipError_t (*)(hipMemGenericAllocationHandle_t*, void*,
                                     hipMemAllocationHandleType);
using HipMemGetAllocationPropsFn = hipError_t (*)(hipMemAllocationProp*,
                                                  hipMemGenericAllocationHandle_t);
#ifdef ZVRAM_HAS_DRM_VMM
using HipDeviceGetPCIBusIdFn = hipError_t (*)(char*, int, int);
#endif

template <typename Function>
Function nextSymbol(const char* name) {
  return reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
}

HipMallocFn realHipMalloc() {
  static const auto function = nextSymbol<HipMallocFn>("hipMalloc");
  return function;
}
HipHostMallocFn realHipHostMalloc() {
  static const auto function = nextSymbol<HipHostMallocFn>("hipHostMalloc");
  return function;
}
HipHostGetDevicePointerFn realHipHostGetDevicePointer() {
  static const auto function =
      nextSymbol<HipHostGetDevicePointerFn>("hipHostGetDevicePointer");
  return function;
}
HipFreeFn realHipFree() {
  static const auto function = nextSymbol<HipFreeFn>("hipFree");
  return function;
}
HipHostFreeFn realHipHostFree() {
  static const auto function = nextSymbol<HipHostFreeFn>("hipHostFree");
  return function;
}
HipGetDeviceFn realHipGetDevice() {
  static const auto function = nextSymbol<HipGetDeviceFn>("hipGetDevice");
  return function;
}
HipSetDeviceFn realHipSetDevice() {
  static const auto function = nextSymbol<HipSetDeviceFn>("hipSetDevice");
  return function;
}
HipDeviceSynchronizeFn realHipDeviceSynchronize() {
  static const auto function = nextSymbol<HipDeviceSynchronizeFn>("hipDeviceSynchronize");
  return function;
}
HipGetDeviceAttributeFn realHipGetDeviceAttribute() {
  static const auto function =
      nextSymbol<HipGetDeviceAttributeFn>("hipDeviceGetAttribute");
  return function;
}
HipFreeAsyncFn realHipFreeAsync() {
  static const auto function = nextSymbol<HipFreeAsyncFn>("hipFreeAsync");
  return function;
}
HipMemGetInfoFn realHipMemGetInfo() {
  static const auto function = nextSymbol<HipMemGetInfoFn>("hipMemGetInfo");
  return function;
}
HipDeviceTotalMemFn realHipDeviceTotalMem() {
  static const auto function = nextSymbol<HipDeviceTotalMemFn>("hipDeviceTotalMem");
  return function;
}
HipGetDevicePropertiesFn realHipGetDeviceProperties() {
  static const auto function = nextSymbol<HipGetDevicePropertiesFn>(
      ZVRAM_STRINGIFY(hipGetDeviceProperties));
  return function;
}
HipGetProcAddressFn realHipGetProcAddress() {
  static const auto function = nextSymbol<HipGetProcAddressFn>("hipGetProcAddress");
  return function;
}
HipLastErrorFn realHipGetLastError() {
  static const auto function = nextSymbol<HipLastErrorFn>("hipGetLastError");
  return function;
}
HipLastErrorFn realHipExtGetLastError() {
  static const auto function = nextSymbol<HipLastErrorFn>("hipExtGetLastError");
  return function;
}
HipLastErrorFn realHipPeekAtLastError() {
  static const auto function = nextSymbol<HipLastErrorFn>("hipPeekAtLastError");
  return function;
}

thread_local hipError_t gShadowError = hipSuccess;
thread_local unsigned int gPrimaryDepth = 0;

// Native HIP retains an internal OOM even after a successful host fallback.
// Preserve the caller's pending error, then expose only the wrapped call's result.
class PrimaryCallBoundary {
 public:
  PrimaryCallBoundary() noexcept : outer_(gPrimaryDepth == 0) {
    ++gPrimaryDepth;
    if (!outer_) return;
    const auto peek = realHipPeekAtLastError();
    prior_ = peek ? peek() : hipSuccess;
    if (prior_ == hipSuccess) prior_ = gShadowError;
    const auto get = realHipGetLastError();
    if (get) (void)get();
  }

  PrimaryCallBoundary(const PrimaryCallBoundary&) = delete;
  PrimaryCallBoundary& operator=(const PrimaryCallBoundary&) = delete;

  ~PrimaryCallBoundary() noexcept {
    if (!finished_) (void)finish(hipErrorOutOfMemory);
  }

  hipError_t finish(hipError_t status) noexcept {
    if (finished_) return status;
    if (outer_) {
      const auto get = realHipGetLastError();
      if (get) (void)get();
      if (gPrimaryDepth) --gPrimaryDepth;
      gShadowError = status == hipSuccess ? prior_ : status;
    } else if (gPrimaryDepth) {
      --gPrimaryDepth;
    }
    finished_ = true;
    return status;
  }

 private:
  bool outer_{};
  bool finished_{};
  hipError_t prior_{hipSuccess};
};
HipMemGetGranularityFn realHipMemGetGranularity() {
  static const auto function = nextSymbol<HipMemGetGranularityFn>("hipMemGetAllocationGranularity");
  return function;
}
HipMemCreateFn realHipMemCreate() {
  static const auto function = nextSymbol<HipMemCreateFn>("hipMemCreate");
  return function;
}
HipMemAddressReserveFn realHipMemAddressReserve() {
  static const auto function = nextSymbol<HipMemAddressReserveFn>("hipMemAddressReserve");
  return function;
}
HipMemMapFn realHipMemMap() {
  static const auto function = nextSymbol<HipMemMapFn>("hipMemMap");
  return function;
}
HipMemSetAccessFn realHipMemSetAccess() {
  static const auto function = nextSymbol<HipMemSetAccessFn>("hipMemSetAccess");
  return function;
}
HipMemUnmapFn realHipMemUnmap() {
  static const auto function = nextSymbol<HipMemUnmapFn>("hipMemUnmap");
  return function;
}
HipMemReleaseFn realHipMemRelease() {
  static const auto function = nextSymbol<HipMemReleaseFn>("hipMemRelease");
  return function;
}
HipMemAddressFreeFn realHipMemAddressFree() {
  static const auto function = nextSymbol<HipMemAddressFreeFn>("hipMemAddressFree");
  return function;
}
HipMemImportFn realHipMemImport() {
  static const auto function = nextSymbol<HipMemImportFn>("hipMemImportFromShareableHandle");
  return function;
}
HipMemGetAllocationPropsFn realHipMemGetAllocationProps() {
  static const auto function = nextSymbol<HipMemGetAllocationPropsFn>(
      "hipMemGetAllocationPropertiesFromHandle");
  return function;
}
#ifdef ZVRAM_HAS_DRM_VMM
HipDeviceGetPCIBusIdFn realHipDeviceGetPCIBusId() {
  static const auto function = nextSymbol<HipDeviceGetPCIBusIdFn>("hipDeviceGetPCIBusId");
  return function;
}
#endif

struct Limit {
  bool set = false;
  size_t bytes = 0;
};

Limit readLimit(const char* name) {
  const char* text = std::getenv(name);
  if (!text || !*text) return {};
  for (const char* digit = text; *digit; ++digit) {
    if (*digit < '0' || *digit > '9') {
      std::fprintf(stderr, "[zvram-hip] ignoring invalid %s value\n", name);
      return {};
    }
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (errno || end == text || *end != '\0') {
    std::fprintf(stderr, "[zvram-hip] ignoring invalid %s value\n", name);
    return {};
  }
  constexpr size_t kMiB = 1024u * 1024u;
  const size_t bytes = value > std::numeric_limits<size_t>::max() / kMiB
                           ? std::numeric_limits<size_t>::max()
                           : static_cast<size_t>(value) * kMiB;
  return {true, bytes};
}

const Limit& localLimit() {
  static const Limit limit = readLimit("ZVRAM_HIP_LOCAL_LIMIT_MIB");
  return limit;
}
const Limit& hostLimit() {
  static const Limit limit = readLimit("ZVRAM_HIP_HOST_LIMIT_MIB");
  return limit;
}

bool hybridVmmEnabled() {
  const char* value = std::getenv("ZVRAM_HIP_VMM");
  return value && std::strcmp(value, "1") == 0;
}

constexpr size_t kHostReserveBytes = 8ull * 1024ull * 1024ull * 1024ull;

bool memAvailableBytes(size_t* bytes) {
  std::ifstream input("/proc/meminfo");
  std::string key;
  unsigned long long kib = 0;
  std::string unit;
  while (input >> key >> kib >> unit) {
    if (key == "MemAvailable:") {
      if (kib > std::numeric_limits<size_t>::max() / 1024u) return false;
      *bytes = static_cast<size_t>(kib) * 1024u;
      return true;
    }
  }
  return false;
}

enum class Origin { Native, MappedHost, HybridVmm };
struct VmmSegment {
  size_t offset = 0;
  size_t bytes = 0;
  bool host = false;
  bool mapped = false;
  bool accessGranted = false;
  bool accounted = true;
  hipMemGenericAllocationHandle_t handle{};
  std::unique_ptr<unsigned char[]> snapshot;
  size_t snapshotBytes = 0;
#ifdef ZVRAM_HAS_DRM_VMM
  amdgpu_bo_handle gttBo = nullptr;
  int exportFd = -1;
#endif
};
#ifdef ZVRAM_HAS_DRM_VMM
struct DrmProvider {
  int fd = -1;
  amdgpu_device_handle device = nullptr;

  bool close() {
    if (device) {
      const int result = amdgpu_device_deinitialize(device);
      if (result != 0) {
        std::fprintf(stderr, "[zvram-hip] amdgpu_device_deinitialize failed: %s (%d)\n",
                     std::strerror(-result), result);
        return false;
      }
      device = nullptr;
    }
    if (fd >= 0) {
      if (::close(fd) != 0) {
        std::fprintf(stderr, "[zvram-hip] close AMDGPU render fd failed: %s\n",
                     std::strerror(errno));
        return false;
      }
      fd = -1;
    }
    return true;
  }

  ~DrmProvider() {
    (void)close();
  }
};
#endif
struct VmmStorage {
  void* base = nullptr;
  size_t mappedBytes = 0;
  size_t logicalBytes = 0;
  bool addressReserved = false;
  bool coldLogical = false;
  bool freeStarted = false;
  std::vector<VmmSegment> segments;
#ifdef ZVRAM_HAS_DRM_VMM
  std::shared_ptr<DrmProvider> drm;
#endif
};
struct Allocation {
  Origin origin;
  void* hostPointer;
  size_t bytes;
  int device;
  size_t mappedBytes = 0;
  size_t deviceBytes = 0;
  std::shared_ptr<VmmStorage> vmm;
};
struct DeviceBytes {
  size_t current = 0;
  size_t peak = 0;
};
struct Totals {
  size_t localCurrent = 0;
  size_t localPeak = 0;
  size_t hostCurrent = 0;
  size_t hostPending = 0;
  size_t hostPeak = 0;
  size_t allocations = 0;
  size_t allocationsPeak = 0;
  unsigned long long nativeAllocations = 0;
  unsigned long long hostAllocations = 0;
  unsigned long long vmmAllocations = 0;
  unsigned long long asyncFreeRejections = 0;
  unsigned long long failures = 0;
};

std::mutex gMutex;
std::recursive_mutex gResidencyMutex;
#ifdef ZVRAM_HAS_DRM_VMM
std::mutex gDrmProviderMutex;
std::unordered_map<int, std::weak_ptr<DrmProvider>> gDrmProviders;
#endif
std::map<void*, Allocation, std::less<void*>> gAllocations;
std::unordered_map<void*, size_t> gFreeingCounts;
std::unordered_map<int, DeviceBytes> gDeviceBytes;
std::vector<std::shared_ptr<VmmStorage>> gOrphanedVmm;
size_t gOrphanSlotsReserved = 0;
size_t gColdLogicalBytes = 0;
size_t gColdStoredBytes = 0;
Totals gTotals;
bool gSummaryRegistered = false;

struct OrphanSlot {
  bool held = false;
  ~OrphanSlot() {
    if (!held) return;
    std::lock_guard<std::mutex> lock(gMutex);
    if (gOrphanSlotsReserved) --gOrphanSlotsReserved;
  }
  void retain(const std::shared_ptr<VmmStorage>& storage) {
    std::lock_guard<std::mutex> lock(gMutex);
    gOrphanedVmm.push_back(storage);
    if (gOrphanSlotsReserved) --gOrphanSlotsReserved;
    held = false;
  }
};

void printSummary() {
  Totals snapshot{};
  size_t orphanedVmm = 0;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    snapshot = gTotals;
    orphanedVmm = gOrphanedVmm.size();
  }
  std::fprintf(stderr,
               "[zvram-hip] summary: tracked=%zu peak_allocations=%zu "
               "hipMalloc_api_bytes_current=%zu peak=%zu "
               "host_pinned_backing_bytes_current=%zu pending=%zu peak=%zu "
               "native_allocations=%llu mapped_host_allocations=%llu "
               "hybrid_vmm_allocations=%llu "
               "orphaned_vmm_cleanup=%zu "
               "mapped_host_async_free_rejections=%llu failures=%llu; "
               "mapped host is pinned host RAM, not compressed VRAM\n",
               snapshot.allocations, snapshot.allocationsPeak,
               snapshot.localCurrent, snapshot.localPeak, snapshot.hostCurrent,
               snapshot.hostPending, snapshot.hostPeak, snapshot.nativeAllocations,
               snapshot.hostAllocations, snapshot.vmmAllocations,
               orphanedVmm,
               snapshot.asyncFreeRejections,
               snapshot.failures);
}

void registerSummary() {
  bool registerNow = false;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gSummaryRegistered) {
      gSummaryRegistered = true;
      registerNow = true;
    }
  }
  if (registerNow) std::atexit(printSummary);
}

void noteFailure() {
  std::lock_guard<std::mutex> lock(gMutex);
  ++gTotals.failures;
}

bool getCurrentDevice(int* device) {
  const auto function = realHipGetDevice();
  return function && function(device) == hipSuccess;
}

bool canMapHostMemory(int device) {
  const auto function = realHipGetDeviceAttribute();
  int supported = 0;
  return function &&
         function(&supported, hipDeviceAttributeCanMapHostMemory, device) ==
             hipSuccess &&
         supported != 0;
}

bool ensureDeviceEntry(int device) {
  try {
    std::lock_guard<std::mutex> lock(gMutex);
    gDeviceBytes.try_emplace(device);
    return true;
  } catch (...) {
    return false;
  }
}

enum class NativeReservation { Reserved, LocalLimit, Failed };

NativeReservation reserveNativeBytes(size_t bytes, int device) {
  const Limit& limit = localLimit();
  std::lock_guard<std::mutex> lock(gMutex);
  const auto found = gDeviceBytes.find(device);
  if (found == gDeviceBytes.end()) return NativeReservation::Failed;
  const size_t current = found->second.current;
  if (limit.set &&
      (bytes > limit.bytes || current > limit.bytes - bytes))
    return NativeReservation::LocalLimit;
  if (bytes > std::numeric_limits<size_t>::max() - current ||
      bytes > std::numeric_limits<size_t>::max() - gTotals.localCurrent)
    return NativeReservation::Failed;
  found->second.current += bytes;
  if (found->second.current > found->second.peak)
    found->second.peak = found->second.current;
  gTotals.localCurrent += bytes;
  if (gTotals.localCurrent > gTotals.localPeak)
    gTotals.localPeak = gTotals.localCurrent;
  return NativeReservation::Reserved;
}

void releaseNativeBytes(size_t bytes, int device) {
  std::lock_guard<std::mutex> lock(gMutex);
  const auto found = gDeviceBytes.find(device);
  if (found != gDeviceBytes.end())
    found->second.current = bytes > found->second.current
                                ? 0
                                : found->second.current - bytes;
  gTotals.localCurrent = bytes > gTotals.localCurrent
                             ? 0
                             : gTotals.localCurrent - bytes;
}

bool reserveHostBytes(size_t bytes) {
  const Limit& configured = hostLimit();
  std::lock_guard<std::mutex> lock(gMutex);
  size_t available = 0;
  if (!memAvailableBytes(&available) || available <= kHostReserveBytes) return false;
  const size_t availableBudget = available - kHostReserveBytes;
  if (bytes > availableBudget || gTotals.hostPending > availableBudget - bytes)
    return false;
  if (configured.set &&
      (bytes > configured.bytes ||
       gTotals.hostCurrent > configured.bytes - bytes ||
       gTotals.hostPending > configured.bytes - bytes - gTotals.hostCurrent))
    return false;
  gTotals.hostPending += bytes;
  const size_t reserved = gTotals.hostCurrent + gTotals.hostPending;
  if (reserved > gTotals.hostPeak) gTotals.hostPeak = reserved;
  return true;
}

bool roundUpTo(size_t bytes, size_t granularity, size_t* rounded) {
  if (!granularity || bytes > std::numeric_limits<size_t>::max() - (granularity - 1))
    return false;
  *rounded = ((bytes + granularity - 1) / granularity) * granularity;
  return true;
}

size_t greatestCommonDivisor(size_t a, size_t b) {
  while (b) {
    const size_t remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

bool reserveHybridLocal(size_t maxBytes, size_t granularity, int device,
                        size_t* reservedBytes) {
  *reservedBytes = 0;
  const Limit& limit = localLimit();
  if (!limit.set || !granularity) return true;
  std::lock_guard<std::mutex> lock(gMutex);
  const auto found = gDeviceBytes.find(device);
  if (found == gDeviceBytes.end()) return false;
  const size_t available = found->second.current < limit.bytes
                               ? limit.bytes - found->second.current
                               : 0;
  size_t bytes = std::min(maxBytes, available);
  bytes -= bytes % granularity;
  if (bytes > std::numeric_limits<size_t>::max() - found->second.current ||
      bytes > std::numeric_limits<size_t>::max() - gTotals.localCurrent)
    return false;
  found->second.current += bytes;
  if (found->second.current > found->second.peak) found->second.peak = found->second.current;
  gTotals.localCurrent += bytes;
  if (gTotals.localCurrent > gTotals.localPeak) gTotals.localPeak = gTotals.localCurrent;
  *reservedBytes = bytes;
  return true;
}

void releaseHostBytes(size_t bytes) {
  std::lock_guard<std::mutex> lock(gMutex);
  gTotals.hostPending = bytes > gTotals.hostPending ? 0 : gTotals.hostPending - bytes;
}

void commitHostBytes(size_t bytes) {
  std::lock_guard<std::mutex> lock(gMutex);
  gTotals.hostPending = bytes > gTotals.hostPending ? 0 : gTotals.hostPending - bytes;
  if (bytes <= std::numeric_limits<size_t>::max() - gTotals.hostCurrent)
    gTotals.hostCurrent += bytes;
  if (gTotals.hostCurrent > gTotals.hostPeak) gTotals.hostPeak = gTotals.hostCurrent;
}

void releaseCurrentHostBytes(size_t bytes) {
  std::lock_guard<std::mutex> lock(gMutex);
  gTotals.hostCurrent = bytes > gTotals.hostCurrent ? 0 : gTotals.hostCurrent - bytes;
}

void setColdLogical(VmmStorage& storage, bool cold) {
  std::lock_guard<std::mutex> lock(gMutex);
  if (storage.coldLogical == cold) return;
  if (cold) {
    if (storage.logicalBytes <= std::numeric_limits<size_t>::max() - gColdLogicalBytes)
      gColdLogicalBytes += storage.logicalBytes;
    storage.coldLogical = true;
  } else {
    gColdLogicalBytes = storage.logicalBytes > gColdLogicalBytes
                            ? 0 : gColdLogicalBytes - storage.logicalBytes;
    storage.coldLogical = false;
  }
}

void addColdStored(size_t bytes) {
  std::lock_guard<std::mutex> lock(gMutex);
  if (bytes <= std::numeric_limits<size_t>::max() - gColdStoredBytes)
    gColdStoredBytes += bytes;
}

void removeColdStored(size_t bytes) {
  std::lock_guard<std::mutex> lock(gMutex);
  gColdStoredBytes = bytes > gColdStoredBytes ? 0 : gColdStoredBytes - bytes;
}

bool rememberAllocation(void* returnedPointer, const Allocation& allocation) {
  if (!returnedPointer) return false;
  try {
    std::lock_guard<std::mutex> lock(gMutex);
    auto [it, inserted] = gAllocations.emplace(returnedPointer, allocation);
    if (!inserted) return false;
    ++gTotals.allocations;
    if (gTotals.allocations > gTotals.allocationsPeak)
      gTotals.allocationsPeak = gTotals.allocations;
    if (allocation.origin == Origin::Native) {
      ++gTotals.nativeAllocations;
    } else if (allocation.origin == Origin::MappedHost ||
               allocation.origin == Origin::HybridVmm) {
      if (allocation.bytes > gTotals.hostPending ||
          allocation.bytes > std::numeric_limits<size_t>::max() - gTotals.hostCurrent) {
        gAllocations.erase(it);
        --gTotals.allocations;
        return false;
      }
      gTotals.hostPending -= allocation.bytes;
      gTotals.hostCurrent += allocation.bytes;
      if (allocation.origin == Origin::MappedHost)
        ++gTotals.hostAllocations;
      else
        ++gTotals.vmmAllocations;
    }
    return true;
  } catch (...) {
    return false;
  }
}

using AllocationNode = decltype(gAllocations)::node_type;
enum class LookupResult { Untracked, Busy, Failed, Found };

LookupResult extractForFree(void* pointer, Allocation* allocation,
                            AllocationNode* node) {
  std::lock_guard<std::mutex> lock(gMutex);
  const auto found = gAllocations.find(pointer);
  if (found == gAllocations.end())
    return gFreeingCounts.find(pointer) == gFreeingCounts.end()
               ? LookupResult::Untracked
               : LookupResult::Busy;
  *allocation = found->second;
  try {
    auto [inFlight, inserted] = gFreeingCounts.try_emplace(pointer, 0);
    (void)inserted;
    ++inFlight->second;
    *node = gAllocations.extract(found);
  } catch (...) {
    return LookupResult::Failed;
  }
  return LookupResult::Found;
}

void finishTrackedFree(void* pointer, const Allocation& allocation,
                       AllocationNode node, bool success, bool countFailure) {
  std::lock_guard<std::mutex> lock(gMutex);
  if (!success) {
    node.mapped() = allocation;
    const auto inserted = gAllocations.insert(std::move(node));
    if (!inserted.inserted)
      std::fprintf(stderr, "[zvram-hip] failed to restore allocation tracking after free error\n");
    if (countFailure) ++gTotals.failures;
  } else {
    if (allocation.origin == Origin::Native) {
      const auto device = gDeviceBytes.find(allocation.device);
      if (device != gDeviceBytes.end())
        device->second.current = allocation.bytes > device->second.current
                                     ? 0
                                     : device->second.current - allocation.bytes;
      gTotals.localCurrent = allocation.bytes > gTotals.localCurrent
                                 ? 0
                                 : gTotals.localCurrent - allocation.bytes;
    } else if (allocation.origin == Origin::MappedHost) {
      gTotals.hostCurrent = allocation.bytes > gTotals.hostCurrent
                                ? 0
                                : gTotals.hostCurrent - allocation.bytes;
    } else {
      size_t hostBytes = 0, deviceBytes = 0, storedBytes = 0;
      if (allocation.vmm) {
        for (const auto& segment : allocation.vmm->segments) {
          if (segment.accounted) {
            if (segment.host) hostBytes += segment.bytes;
            else deviceBytes += segment.bytes;
          }
          storedBytes += segment.snapshotBytes;
        }
        if (allocation.vmm->coldLogical) {
          gColdLogicalBytes = allocation.vmm->logicalBytes > gColdLogicalBytes
                                  ? 0 : gColdLogicalBytes - allocation.vmm->logicalBytes;
          allocation.vmm->coldLogical = false;
        }
      }
      gColdStoredBytes = storedBytes > gColdStoredBytes ? 0 : gColdStoredBytes - storedBytes;
      gTotals.hostCurrent = hostBytes > gTotals.hostCurrent
                                ? 0 : gTotals.hostCurrent - hostBytes;
      const auto device = gDeviceBytes.find(allocation.device);
      if (device != gDeviceBytes.end())
        device->second.current = deviceBytes > device->second.current
                                     ? 0
                                     : device->second.current - deviceBytes;
      gTotals.localCurrent = deviceBytes > gTotals.localCurrent
                                 ? 0 : gTotals.localCurrent - deviceBytes;
    }
    if (gTotals.allocations) --gTotals.allocations;
  }
  const auto inFlight = gFreeingCounts.find(pointer);
  if (inFlight != gFreeingCounts.end()) {
    if (inFlight->second > 1)
      --inFlight->second;
    else
      gFreeingCounts.erase(inFlight);
  }
}

void noteAsyncFreeRejection() {
  std::lock_guard<std::mutex> lock(gMutex);
  ++gTotals.asyncFreeRejections;
}

hipError_t allocateMappedHost(void** output, size_t bytes, int device,
                              hipError_t originalError) {
  if (output) *output = nullptr;
  if (!canMapHostMemory(device)) {
    std::fprintf(stderr,
                 "[zvram-hip] mapped-host fallback unavailable: device %d cannot map host memory\n",
                 device);
    noteFailure();
    return originalError;
  }
  if (!reserveHostBytes(bytes)) {
    std::fprintf(stderr,
                 "[zvram-hip] mapped-host fallback refused by available-RAM/reserve or host limit\n");
    noteFailure();
    return originalError;
  }

  const auto hostMalloc = realHipHostMalloc();
  const auto getDevicePointer = realHipHostGetDevicePointer();
  const auto hostFree = realHipHostFree();
  if (!hostMalloc || !getDevicePointer || !hostFree) {
    releaseHostBytes(bytes);
    noteFailure();
    return hipErrorNotSupported;
  }

  void* hostPointer = nullptr;
  hipError_t status = hostMalloc(&hostPointer, bytes, hipHostMallocMapped);
  if (status != hipSuccess) {
    releaseHostBytes(bytes);
    std::fprintf(stderr, "[zvram-hip] hipHostMalloc fallback failed: %s\n",
                 hipGetErrorString(status));
    noteFailure();
    return status;
  }
  void* devicePointer = nullptr;
  status = getDevicePointer(&devicePointer, hostPointer, 0);
  if (status != hipSuccess || !devicePointer) {
    const hipError_t cleanup = hostFree(hostPointer);
    releaseHostBytes(bytes);
    std::fprintf(stderr, "[zvram-hip] hipHostGetDevicePointer fallback failed: %s\n",
                 hipGetErrorString(status));
    if (cleanup != hipSuccess)
      std::fprintf(stderr, "[zvram-hip] host cleanup failed: %s\n",
                   hipGetErrorString(cleanup));
    noteFailure();
    return status == hipSuccess ? hipErrorInvalidValue : status;
  }

  Allocation allocation{Origin::MappedHost, hostPointer, bytes, device, 0, 0, {}};
  if (!rememberAllocation(devicePointer, allocation)) {
    const hipError_t cleanup = hostFree(hostPointer);
    releaseHostBytes(bytes);
    std::fprintf(stderr, "[zvram-hip] could not track mapped-host allocation\n");
    if (cleanup != hipSuccess)
      std::fprintf(stderr, "[zvram-hip] host cleanup failed: %s\n",
                   hipGetErrorString(cleanup));
    noteFailure();
    return hipErrorOutOfMemory;
  }
  *output = devicePointer;
  return hipSuccess;
}

#ifdef ZVRAM_HAS_DRM_VMM
bool validPciBusId(const char* value) {
  if (!value || !*value) return false;
  const char* firstColon = std::strchr(value, ':');
  const char* secondColon = firstColon ? std::strchr(firstColon + 1, ':') : nullptr;
  const char* dot = secondColon ? std::strchr(secondColon + 1, '.') : nullptr;
  if (!firstColon || !secondColon || !dot || firstColon == value ||
      secondColon == firstColon + 1 || dot == secondColon + 1 || !dot[1] ||
      std::strchr(secondColon + 1, ':') || std::strchr(dot + 1, '.'))
    return false;
  unsigned colons = 0, dots = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
    if (*p == ':') ++colons;
    else if (*p == '.') ++dots;
    else if (!std::isxdigit(*p)) return false;
  }
  return colons == 2 && dots == 1;
}

std::shared_ptr<DrmProvider> makeGttProvider(int device, hipError_t* statusOut) {
  *statusOut = hipErrorNotSupported;
  const auto getBusId = realHipDeviceGetPCIBusId();
  if (!getBusId) return {};
  char busId[64]{};
  hipError_t status = getBusId(busId, sizeof(busId), device);
  if (status != hipSuccess || !validPciBusId(busId)) {
    std::fprintf(stderr, "[zvram-hip] invalid or unavailable HIP PCI bus ID for device %d\n",
                 device);
    *statusOut = status == hipSuccess ? hipErrorInvalidValue : status;
    return {};
  }
  char path[128]{};
  const int length = std::snprintf(path, sizeof(path),
                                   "/dev/dri/by-path/pci-%s-render", busId);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) {
    *statusOut = hipErrorInvalidValue;
    return {};
  }
  auto provider = std::make_shared<DrmProvider>();
  provider->fd = open(path, O_RDWR | O_CLOEXEC);
  if (provider->fd < 0) {
    std::fprintf(stderr, "[zvram-hip] cannot open matching AMDGPU render node %s: %s\n",
                 path, std::strerror(errno));
    *statusOut = hipErrorNotSupported;
    return {};
  }
  uint32_t major = 0, minor = 0;
  const int result = amdgpu_device_initialize(provider->fd, &major, &minor,
                                              &provider->device);
  if (result != 0) {
    std::fprintf(stderr, "[zvram-hip] amdgpu_device_initialize failed: %s (%d)\n",
                 std::strerror(-result), result);
    *statusOut = hipErrorNotSupported;
    return {};
  }
  *statusOut = hipSuccess;
  return provider;
}

hipError_t acquireGttProvider(int device, std::shared_ptr<DrmProvider>* output) {
  if (!output) return hipErrorInvalidValue;
  *output = {};
  {
    std::lock_guard<std::mutex> lock(gDrmProviderMutex);
    const auto found = gDrmProviders.find(device);
    if (found != gDrmProviders.end()) {
      if (auto provider = found->second.lock()) {
        *output = std::move(provider);
        return hipSuccess;
      }
      gDrmProviders.erase(found);
    }
  }

  hipError_t status = hipErrorNotSupported;
  std::shared_ptr<DrmProvider> candidate;
  try {
    candidate = makeGttProvider(device, &status);
  } catch (...) {
    return hipErrorOutOfMemory;
  }
  if (!candidate) return status;

  try {
    std::lock_guard<std::mutex> lock(gDrmProviderMutex);
    const auto found = gDrmProviders.find(device);
    if (found != gDrmProviders.end()) {
      if (auto provider = found->second.lock()) {
        *output = std::move(provider);
      } else {
        found->second = candidate;
        *output = candidate;
      }
    } else {
      gDrmProviders.emplace(device, candidate);
      *output = candidate;
    }
  } catch (...) {
    // The uncached provider remains valid and is destroyed outside this lock.
    *output = std::move(candidate);
  }
  return hipSuccess;
}

bool cleanupGttSegment(VmmSegment& segment) {
  bool ok = true;
  if (segment.exportFd >= 0) {
    if (close(segment.exportFd) == 0) segment.exportFd = -1;
    else {
      std::fprintf(stderr, "[zvram-hip] close GTT dma-buf failed: %s\n",
                   std::strerror(errno));
      ok = false;
    }
  }
  if (segment.gttBo && !segment.handle && !segment.mapped) {
    const int result = amdgpu_bo_free(segment.gttBo);
    if (result == 0) segment.gttBo = nullptr;
    else {
      std::fprintf(stderr, "[zvram-hip] amdgpu_bo_free failed: %s (%d)\n",
                   std::strerror(-result), result);
      ok = false;
    }
  }
  return ok && !segment.gttBo && segment.exportFd < 0;
}

bool cleanupGttProvider(VmmStorage& storage) {
  bool ok = true;
  for (auto& segment : storage.segments) {
    if (segment.gttBo || segment.exportFd >= 0)
      ok = cleanupGttSegment(segment) && ok;
  }
  const bool hasObjects = std::any_of(storage.segments.begin(), storage.segments.end(),
      [](const VmmSegment& segment) {
        return segment.gttBo || segment.exportFd >= 0 || segment.handle || segment.mapped;
      });
  if (!hasObjects) storage.drm.reset();
  return ok && !hasObjects && !storage.drm;
}
#endif

hipError_t createSegmentBacking(VmmStorage& storage, VmmSegment& segment, int device);

hipError_t createHybridVmm(void** output, size_t bytes, int device,
                           hipError_t originalError) {
  *output = nullptr;
  int supported = 0;
  const auto getAttribute = realHipGetDeviceAttribute();
  const auto getGranularity = realHipMemGetGranularity();
  const auto create = realHipMemCreate();
  const auto reserve = realHipMemAddressReserve();
  const auto map = realHipMemMap();
  const auto setAccess = realHipMemSetAccess();
  const auto unmap = realHipMemUnmap();
  const auto release = realHipMemRelease();
  const auto addressFree = realHipMemAddressFree();
  const auto import = realHipMemImport();
  const auto getProperties = realHipMemGetAllocationProps();
  if (!getAttribute || !getGranularity || !create || !reserve || !map ||
      !setAccess || !unmap || !release || !addressFree || !import ||
      !getProperties ||
      getAttribute(&supported, hipDeviceAttributeVirtualMemoryManagementSupported,
                   device) != hipSuccess || !supported) {
    noteFailure();
    return originalError;
  }

  hipMemAllocationProp deviceProp{};
  deviceProp.type = hipMemAllocationTypePinned;
  deviceProp.location = {hipMemLocationTypeDevice, device};
  hipMemAllocationProp hostProp{};
  hostProp.type = hipMemAllocationTypePinned;
  hostProp.location = {hipMemLocationTypeHost, 0};
  size_t deviceGranularity = 0, hostGranularity = 0;
  hipError_t status = getGranularity(&deviceGranularity, &deviceProp,
                                     hipMemAllocationGranularityMinimum);
  if (status == hipSuccess)
    status = getGranularity(&hostGranularity, &hostProp,
                            hipMemAllocationGranularityMinimum);
  if (status != hipSuccess || !deviceGranularity || !hostGranularity) {
    noteFailure();
    return status == hipSuccess ? originalError : status;
  }
  const size_t gcd = greatestCommonDivisor(deviceGranularity, hostGranularity);
  if (!gcd || deviceGranularity / gcd >
                  std::numeric_limits<size_t>::max() / hostGranularity) {
    noteFailure();
    return originalError;
  }
  const size_t splitGranularity = (deviceGranularity / gcd) * hostGranularity;
  if ((splitGranularity & (splitGranularity - 1)) != 0) {
    std::fprintf(stderr, "[zvram-hip] hybrid VMM requires power-of-two mapping granularity\n");
    noteFailure();
    return originalError;
  }

  constexpr size_t kMaxHandleBytes = 256ull * 1024ull * 1024ull;
  const size_t maxDeviceHandle = (kMaxHandleBytes / deviceGranularity) * deviceGranularity;
  const size_t maxHostHandle = (kMaxHandleBytes / hostGranularity) * hostGranularity;
  if (!maxDeviceHandle || !maxHostHandle) { noteFailure(); return originalError; }
  const auto countSegments = [](size_t regionBytes, size_t maxBytes, size_t* count) {
    if (!maxBytes) return false;
    *count = regionBytes / maxBytes + (regionBytes % maxBytes != 0);
    return true;
  };
  size_t maxHostBacking = 0;
  if (!roundUpTo(bytes, hostGranularity, &maxHostBacking)) {
    noteFailure();
    return originalError;
  }
  size_t maxDeviceSegments = 0, maxHostSegments = 0;
  if (!countSegments(bytes, maxDeviceHandle, &maxDeviceSegments) ||
      !countSegments(maxHostBacking, maxHostHandle, &maxHostSegments) ||
      maxDeviceSegments > std::numeric_limits<size_t>::max() - maxHostSegments) {
    noteFailure();
    return originalError;
  }
  std::shared_ptr<VmmStorage> storage;
  OrphanSlot orphanSlot;
  try {
    storage = std::make_shared<VmmStorage>();
    storage->segments.reserve(maxDeviceSegments + maxHostSegments);
    std::lock_guard<std::mutex> lock(gMutex);
    if (gOrphanSlotsReserved == std::numeric_limits<size_t>::max() ||
        gOrphanedVmm.size() > std::numeric_limits<size_t>::max() -
                                  gOrphanSlotsReserved - 1)
      throw std::bad_alloc();
    gOrphanedVmm.reserve(gOrphanedVmm.size() + gOrphanSlotsReserved + 1);
    ++gOrphanSlotsReserved;
    orphanSlot.held = true;
  } catch (...) {
    noteFailure();
    return hipErrorOutOfMemory;
  }

  size_t deviceBytes = 0;
  if (!reserveHybridLocal(bytes, splitGranularity, device, &deviceBytes)) {
    noteFailure();
    return hipErrorOutOfMemory;
  }
  size_t hostBytes = 0;
  if (!roundUpTo(bytes - std::min(bytes, deviceBytes), hostGranularity, &hostBytes) ||
      hostBytes > std::numeric_limits<size_t>::max() - deviceBytes ||
      (hostBytes && !reserveHostBytes(hostBytes))) {
    releaseNativeBytes(deviceBytes, device);
    noteFailure();
    return originalError;
  }
  const size_t mappedBytes = deviceBytes + hostBytes;
  if (!mappedBytes) {
    if (hostBytes) releaseHostBytes(hostBytes);
    if (deviceBytes) releaseNativeBytes(deviceBytes, device);
    noteFailure();
    return originalError;
  }
  storage->mappedBytes = mappedBytes;
  storage->logicalBytes = bytes;

#ifdef ZVRAM_HAS_DRM_VMM
  if (hostBytes) {
    status = acquireGttProvider(device, &storage->drm);
    if (status != hipSuccess) {
      releaseHostBytes(hostBytes);
      releaseNativeBytes(deviceBytes, device);
      noteFailure();
      return status;
    }
  }
#else
  if (hostBytes) {
    releaseHostBytes(hostBytes);
    releaseNativeBytes(deviceBytes, device);
    std::fprintf(stderr,
                 "[zvram-hip] hybrid VMM host tier unavailable: libdrm AMDGPU support was not built\n");
    noteFailure();
    return originalError;
  }
#endif

  void* va = nullptr;
  bool countersReserved = true;
  auto rollback = [&]() {
    bool cleaned = true;
    for (auto it = storage->segments.rbegin(); it != storage->segments.rend(); ++it) {
      if (it->mapped) {
        const hipError_t cleanup = unmap(static_cast<char*>(va) + it->offset, it->bytes);
        if (cleanup == hipSuccess) it->mapped = false;
        else {
          cleaned = false;
          std::fprintf(stderr, "[zvram-hip] VMM rollback unmap failed: %s\n",
                       hipGetErrorString(cleanup));
        }
      }
      if (it->handle && !it->mapped) {
        const hipError_t cleanup = release(it->handle);
        if (cleanup == hipSuccess) it->handle = {};
        else {
          cleaned = false;
          std::fprintf(stderr, "[zvram-hip] VMM rollback release failed: %s\n",
                       hipGetErrorString(cleanup));
        }
      }
#ifdef ZVRAM_HAS_DRM_VMM
      if (!cleanupGttSegment(*it)) cleaned = false;
#endif
      if (it->mapped || it->handle) cleaned = false;
    }
#ifdef ZVRAM_HAS_DRM_VMM
    if (!cleanupGttProvider(*storage)) cleaned = false;
#endif
    if (cleaned && storage->addressReserved) {
      const hipError_t cleanup = addressFree(va, mappedBytes);
      if (cleanup == hipSuccess) storage->addressReserved = false;
      else {
        cleaned = false;
        std::fprintf(stderr, "[zvram-hip] VMM rollback address release failed: %s\n",
                     hipGetErrorString(cleanup));
      }
    }
    if (storage->addressReserved) cleaned = false;
    if (countersReserved && cleaned) {
      if (hostBytes) releaseHostBytes(hostBytes);
      if (deviceBytes) releaseNativeBytes(deviceBytes, device);
      countersReserved = false;
    } else if (!cleaned) {
      orphanSlot.retain(storage);
      countersReserved = false;
      std::fprintf(stderr,
                   "[zvram-hip] VMM rollback left resources retained and accounted\n");
    }
  };

  status = reserve(&va, mappedBytes, splitGranularity, nullptr, 0);
  if (status != hipSuccess) { rollback(); noteFailure(); return status; }
  storage->base = va;
  storage->addressReserved = true;
  auto mapSegments = [&](size_t regionBytes, size_t maxSegmentBytes,
                         const hipMemAllocationProp&, bool host) -> hipError_t {
    size_t offset = host ? deviceBytes : 0;
    const size_t end = offset + regionBytes;
    while (offset < end) {
      const size_t amount = std::min(maxSegmentBytes, end - offset);
      VmmSegment added{};
      added.offset = offset;
      added.bytes = amount;
      added.host = host;
      storage->segments.push_back(std::move(added));
      VmmSegment& segment = storage->segments.back();
      const hipError_t result = createSegmentBacking(*storage, segment, device);
      if (result != hipSuccess) return result;
      offset += amount;
    }
    return hipSuccess;
  };
  if (deviceBytes) {
    status = mapSegments(deviceBytes, maxDeviceHandle, deviceProp, false);
    if (status != hipSuccess) {
      std::fprintf(stderr, "[zvram-hip] VMM device segment creation failed: %s\n",
                   hipGetErrorString(status));
      rollback(); noteFailure(); return status;
    }
  }
  if (hostBytes) {
    status = mapSegments(hostBytes, maxHostHandle, hostProp, true);
    if (status != hipSuccess) {
      std::fprintf(stderr, "[zvram-hip] VMM host segment creation failed: %s\n",
                   hipGetErrorString(status));
      rollback(); noteFailure(); return status;
    }
  }
  hipMemAccessDesc access{};
  access.location = {hipMemLocationTypeDevice, device};
  access.flags = hipMemAccessFlagsProtReadWrite;
  status = setAccess(va, mappedBytes, &access, 1);
  if (status != hipSuccess) { rollback(); noteFailure(); return status; }
  for (auto& segment : storage->segments) segment.accessGranted = true;

  Allocation allocation{Origin::HybridVmm, va, hostBytes, device,
                        mappedBytes, deviceBytes, storage};
  if (!rememberAllocation(va, allocation)) {
    rollback();
    noteFailure();
    return hipErrorOutOfMemory;
  }
  countersReserved = false;
  std::fprintf(stderr,
               "[zvram-hip] hybrid VMM allocation: logical=%zu mapped=%zu VRAM=%zu host_pinned=%zu handles=%zu%s\n",
               bytes, mappedBytes, deviceBytes, hostBytes, storage->segments.size(),
               localLimit().set ? "" : " (native OOM; all-host VMM fallback)");
  *output = va;
  return hipSuccess;
}

struct SnapshotChunkHeader {
  uint32_t rawBytes;
  uint32_t storedBytes;
  uint32_t compressed;
};
constexpr size_t kSnapshotChunkBytes = 1024u * 1024u;

extern "C" __attribute__((visibility("hidden"))) hipError_t zvramHipCopyMapped(
    void*, const void*, size_t, hipStream_t);

struct SnapshotScratch {
  unsigned char* pointer = nullptr;
  void* devicePointer = nullptr;
  ~SnapshotScratch() { if (pointer && realHipHostFree()) (void)realHipHostFree()(pointer); }
  hipError_t allocate() {
    const auto allocate = realHipHostMalloc();
    const auto alias = realHipHostGetDevicePointer();
    if (!allocate || !alias) return hipErrorNotSupported;
    const hipError_t status = allocate(reinterpret_cast<void**>(&pointer), kSnapshotChunkBytes,
        hipHostMallocMapped | hipHostMallocPortable);
    return status == hipSuccess ? alias(&devicePointer, pointer, 0) : status;
  }
  unsigned char* data() { return pointer; }
  void* deviceData() { return devicePointer; }
  size_t size() const { return kSnapshotChunkBytes; }
};

hipError_t copyVmmBytes(void* destination, const void* source, size_t bytes,
                        hipMemcpyKind) {
  // ponytail: bounded mapped scratch and a GPU kernel avoid remapped-VMM
  // readback corruption observed with ROCm 7.2's ordinary memcpy path.
  const hipError_t status = zvramHipCopyMapped(destination, source, bytes, nullptr);
  if (status != hipSuccess) return status;
  const auto sync = realHipDeviceSynchronize();
  return sync ? sync() : hipErrorNotSupported;
}

bool chooseSnapshotChunk(const unsigned char* raw, size_t bytes,
                         std::vector<unsigned char>* compressed,
                         bool* useCompressed, size_t* payloadBytes) {
  const size_t result = ZSTD_compress(compressed->data(), compressed->size(), raw, bytes, 3);
  if (ZSTD_isError(result)) return false;
  *useCompressed = result < bytes;
  *payloadBytes = *useCompressed ? result : bytes;
  return true;
}

hipError_t measureSegmentSnapshot(const VmmStorage& storage, const VmmSegment& segment,
                                  SnapshotScratch* raw,
                                  std::vector<unsigned char>* compressed,
                                  size_t* encodedBytes) {
  *encodedBytes = 0;
  size_t offset = 0;
  while (offset < segment.bytes) {
    const size_t amount = std::min(kSnapshotChunkBytes, segment.bytes - offset);
    hipError_t status = copyVmmBytes(raw->deviceData(),
        static_cast<const char*>(storage.base) + segment.offset + offset, amount,
        hipMemcpyDeviceToHost);
    if (status != hipSuccess) return status;
    bool compressedChunk = false;
    size_t payloadBytes = 0;
    if (!chooseSnapshotChunk(raw->data(), amount, compressed, &compressedChunk,
                             &payloadBytes) ||
        payloadBytes > std::numeric_limits<size_t>::max() - sizeof(SnapshotChunkHeader) ||
        *encodedBytes > std::numeric_limits<size_t>::max() -
                            sizeof(SnapshotChunkHeader) - payloadBytes)
      return hipErrorOutOfMemory;
    *encodedBytes += sizeof(SnapshotChunkHeader) + payloadBytes;
    offset += amount;
  }
  return hipSuccess;
}

hipError_t buildSegmentSnapshot(VmmStorage& storage, VmmSegment& segment,
                                size_t encodedBytes,
                                SnapshotScratch* raw,
                                std::vector<unsigned char>* compressed) {
  std::unique_ptr<unsigned char[]> snapshot(
      encodedBytes ? new (std::nothrow) unsigned char[encodedBytes] : nullptr);
  if (encodedBytes && !snapshot) return hipErrorOutOfMemory;
  size_t offset = 0, written = 0;
  while (offset < segment.bytes) {
    const size_t amount = std::min(kSnapshotChunkBytes, segment.bytes - offset);
    hipError_t status = copyVmmBytes(raw->deviceData(),
        static_cast<const char*>(storage.base) + segment.offset + offset, amount,
        hipMemcpyDeviceToHost);
    if (status != hipSuccess) return status;
    bool compressedChunk = false;
    size_t payloadBytes = 0;
    if (!chooseSnapshotChunk(raw->data(), amount, compressed, &compressedChunk,
                             &payloadBytes))
      return hipErrorUnknown;
    SnapshotChunkHeader header{static_cast<uint32_t>(amount),
                               static_cast<uint32_t>(payloadBytes),
                               compressedChunk ? 1u : 0u};
    if (sizeof(header) + payloadBytes > encodedBytes - written)
      return hipErrorUnknown;
    std::memcpy(snapshot.get() + written, &header, sizeof(header));
    written += sizeof(header);
    const unsigned char* payload = compressedChunk ? compressed->data() : raw->data();
    std::memcpy(snapshot.get() + written, payload, payloadBytes);
    written += payloadBytes;
    offset += amount;
  }
  if (written != encodedBytes) return hipErrorUnknown;
  segment.snapshot = std::move(snapshot);
  segment.snapshotBytes = encodedBytes;
  addColdStored(encodedBytes);
  setColdLogical(storage, true);
  return hipSuccess;
}

hipError_t restoreSegmentSnapshot(VmmStorage& storage, VmmSegment& segment,
                                  SnapshotScratch* raw) {
  if (!segment.snapshot) return hipSuccess;
  size_t streamOffset = 0, outputOffset = 0;
  while (streamOffset < segment.snapshotBytes) {
    if (segment.snapshotBytes - streamOffset < sizeof(SnapshotChunkHeader))
      return hipErrorInvalidValue;
    SnapshotChunkHeader header{};
    std::memcpy(&header, segment.snapshot.get() + streamOffset, sizeof(header));
    streamOffset += sizeof(header);
    if (!header.rawBytes || header.rawBytes > kSnapshotChunkBytes ||
        header.storedBytes > segment.snapshotBytes - streamOffset ||
        header.rawBytes > segment.bytes - std::min(outputOffset, segment.bytes))
      return hipErrorInvalidValue;
    const unsigned char* payload = segment.snapshot.get() + streamOffset;
    if (header.compressed) {
      const size_t decoded = ZSTD_decompress(raw->data(), raw->size(), payload,
                                             header.storedBytes);
      if (ZSTD_isError(decoded) || decoded != header.rawBytes) return hipErrorInvalidValue;
    } else {
      if (header.storedBytes != header.rawBytes) return hipErrorInvalidValue;
      std::memcpy(raw->data(), payload, header.rawBytes);
    }
    const hipError_t status = copyVmmBytes(
        static_cast<char*>(storage.base) + segment.offset + outputOffset,
        raw->deviceData(), header.rawBytes, hipMemcpyHostToDevice);
    if (status != hipSuccess) return status;
    outputOffset += header.rawBytes;
    streamOffset += header.storedBytes;
  }
  if (outputOffset != segment.bytes) return hipErrorInvalidValue;
  const auto sync = realHipDeviceSynchronize();
  const hipError_t synchronized = sync ? sync() : hipErrorNotSupported;
  if (synchronized != hipSuccess) return synchronized;
  const size_t releasedBytes = segment.snapshotBytes;
  segment.snapshot.reset();
  segment.snapshotBytes = 0;
  removeColdStored(releasedBytes);
  return hipSuccess;
}

bool snapshotMemoryAvailable(size_t bytes) {
  size_t available = 0;
  if (!memAvailableBytes(&available) || available <= kHostReserveBytes) return false;
  const size_t scratch = kSnapshotChunkBytes + ZSTD_compressBound(kSnapshotChunkBytes);
  if (bytes > std::numeric_limits<size_t>::max() - scratch) return false;
  return bytes + scratch <= available - kHostReserveBytes;
}

hipError_t createSegmentBacking(VmmStorage& storage, VmmSegment& segment, int device) {
  const auto create = realHipMemCreate();
  const auto map = realHipMemMap();
  const auto setAccess = realHipMemSetAccess();
  const auto getGranularity = realHipMemGetGranularity();
  const auto import = realHipMemImport();
  if (!create || !map || !setAccess || !getGranularity || !import)
    return hipErrorNotSupported;
  if (!segment.accounted) {
    if (segment.host) {
      if (!reserveHostBytes(segment.bytes)) return hipErrorOutOfMemory;
#ifdef ZVRAM_HAS_DRM_VMM
      hipError_t status = storage.drm ? hipSuccess : acquireGttProvider(device, &storage.drm);
      if (status != hipSuccess) { releaseHostBytes(segment.bytes); return status; }
      if (!segment.gttBo) {
        size_t granularity = 0;
        hipMemAllocationProp prop{};
        prop.type = hipMemAllocationTypePinned;
        prop.location = {hipMemLocationTypeHost, 0};
        status = getGranularity(&granularity, &prop, hipMemAllocationGranularityMinimum);
        if (status != hipSuccess || !granularity) {
          releaseHostBytes(segment.bytes);
          return status == hipSuccess ? hipErrorInvalidValue : status;
        }
        amdgpu_bo_alloc_request request{};
        request.alloc_size = segment.bytes;
        request.phys_alignment = granularity;
        request.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
        if (amdgpu_bo_alloc(storage.drm->device, &request, &segment.gttBo) != 0) {
          releaseHostBytes(segment.bytes);
          return hipErrorOutOfMemory;
        }
      }
      commitHostBytes(segment.bytes);
      segment.accounted = true;
#else
      releaseHostBytes(segment.bytes);
      return hipErrorNotSupported;
#endif
    } else {
      const NativeReservation reservation = reserveNativeBytes(segment.bytes, device);
      if (reservation != NativeReservation::Reserved) return hipErrorOutOfMemory;
      hipMemAllocationProp prop{};
      prop.type = hipMemAllocationTypePinned;
      prop.location = {hipMemLocationTypeDevice, device};
      const hipError_t status = create(&segment.handle, segment.bytes, &prop, 0);
      if (status != hipSuccess) {
        releaseNativeBytes(segment.bytes, device);
        return status;
      }
      segment.accounted = true;
    }
  }

  if (!segment.host && !segment.handle) {
    hipMemAllocationProp prop{};
    prop.type = hipMemAllocationTypePinned;
    prop.location = {hipMemLocationTypeDevice, device};
    const hipError_t status = create(&segment.handle, segment.bytes, &prop, 0);
    if (status != hipSuccess) return status;
  }

  if (segment.host && !segment.handle) {
#ifdef ZVRAM_HAS_DRM_VMM
    if (!storage.drm) return hipErrorInvalidValue;
    if (!segment.gttBo) {
      size_t granularity = 0;
      hipMemAllocationProp prop{};
      prop.type = hipMemAllocationTypePinned;
      prop.location = {hipMemLocationTypeHost, 0};
      hipError_t status = getGranularity(&granularity, &prop,
                                         hipMemAllocationGranularityMinimum);
      if (status != hipSuccess || !granularity)
        return status == hipSuccess ? hipErrorInvalidValue : status;
      amdgpu_bo_alloc_request request{};
      request.alloc_size = segment.bytes;
      request.phys_alignment = granularity;
      request.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
      if (amdgpu_bo_alloc(storage.drm->device, &request, &segment.gttBo) != 0)
        return hipErrorOutOfMemory;
      amdgpu_bo_info info{};
      if (amdgpu_bo_query_info(segment.gttBo, &info) != 0 ||
          info.alloc_size != segment.bytes)
        return hipErrorInvalidValue;
      if (!segment.accounted) {
        commitHostBytes(segment.bytes);
        segment.accounted = true;
      }
    }
    if (segment.exportFd < 0) {
      uint32_t exportedFd = 0;
      if (amdgpu_bo_export(segment.gttBo, amdgpu_bo_handle_type_dma_buf_fd,
                           &exportedFd) != 0)
        return hipErrorNotSupported;
      segment.exportFd = static_cast<int>(exportedFd);
    }
    if (fcntl(segment.exportFd, F_SETFD, FD_CLOEXEC) != 0)
      return hipErrorUnknown;
    hipError_t status = import(&segment.handle,
        reinterpret_cast<void*>(static_cast<intptr_t>(segment.exportFd)),
        hipMemHandleTypePosixFileDescriptor);
    if (close(segment.exportFd) == 0) segment.exportFd = -1;
    else if (status == hipSuccess) status = hipErrorUnknown;
    if (status != hipSuccess) return status;
    const auto getProperties = realHipMemGetAllocationProps();
    if (!getProperties) return hipErrorNotSupported;
    hipMemAllocationProp importedProp{};
    status = getProperties(&importedProp, segment.handle);
    if (status != hipSuccess) return status;
    size_t importedGranularity = 0;
    status = getGranularity(&importedGranularity, &importedProp,
                            hipMemAllocationGranularityMinimum);
    if (status != hipSuccess) return status;
    if (!importedGranularity || segment.bytes % importedGranularity)
      return hipErrorInvalidValue;
#else
    return hipErrorNotSupported;
#endif
  }
  if (!segment.handle) return hipErrorInvalidValue;
  if (!segment.mapped) {
    const hipError_t status = map(static_cast<char*>(storage.base) + segment.offset,
                                  segment.bytes, 0, segment.handle, 0);
    if (status != hipSuccess) return status;
    segment.mapped = true;
  }
  return hipSuccess;
}

hipError_t releaseSegmentBacking(VmmStorage& storage, VmmSegment& segment,
                                 int device) {
  if (segment.mapped) {
    const auto unmap = realHipMemUnmap();
    const hipError_t status = unmap
        ? unmap(static_cast<char*>(storage.base) + segment.offset, segment.bytes)
        : hipErrorNotSupported;
    if (status != hipSuccess) return status;
    segment.mapped = false;
    segment.accessGranted = false;
  }
  if (segment.handle) {
    const auto release = realHipMemRelease();
    const hipError_t status = release ? release(segment.handle) : hipErrorNotSupported;
    if (status != hipSuccess) return status;
    segment.handle = {};
  }
#ifdef ZVRAM_HAS_DRM_VMM
  if (!cleanupGttSegment(segment)) return hipErrorUnknown;
#endif
  if (segment.accounted) {
    if (segment.host) releaseCurrentHostBytes(segment.bytes);
    else releaseNativeBytes(segment.bytes, device);
    segment.accounted = false;
  }
  return hipSuccess;
}

hipError_t switchDeviceFor(int device, int* previous, bool* switched) {
  const auto get = realHipGetDevice();
  const auto set = realHipSetDevice();
  if (!get || !set) return hipErrorNotSupported;
  hipError_t status = get(previous);
  if (status != hipSuccess) return status;
  *switched = *previous != device;
  return *switched ? set(device) : hipSuccess;
}

hipError_t zvramHibernate(size_t coldBudgetBytes) {
  std::lock_guard<std::recursive_mutex> residency(gResidencyMutex);
  std::vector<std::shared_ptr<VmmStorage>> storages;
  try {
    std::lock_guard<std::mutex> lock(gMutex);
    for (const auto& item : gAllocations) {
      if (item.second.origin == Origin::HybridVmm && item.second.vmm &&
          std::find(storages.begin(), storages.end(), item.second.vmm) == storages.end())
        storages.push_back(item.second.vmm);
    }
  } catch (...) { return hipErrorOutOfMemory; }
  if (storages.empty()) return hipSuccess;
  for (const auto& storage : storages)
    if (storage->freeStarted) return hipErrorInvalidValue;

  std::vector<int> devices;
  for (const auto& storage : storages)
    for (const auto& item : gAllocations)
      if (item.second.vmm == storage &&
          std::find(devices.begin(), devices.end(), item.second.device) == devices.end())
        devices.push_back(item.second.device);
  const auto sync = realHipDeviceSynchronize();
  int originalDevice = -1;
  bool haveOriginal = false;
  const auto getDevice = realHipGetDevice();
  const auto setDevice = realHipSetDevice();
  if (!getDevice || !setDevice || !sync) return hipErrorNotSupported;
  hipError_t status = getDevice(&originalDevice);
  if (status != hipSuccess) return status;
  haveOriginal = true;
  for (int device : devices) {
    if ((status = setDevice(device)) != hipSuccess) break;
    status = sync();
    if (status != hipSuccess) break;
  }
  if (haveOriginal && getDevice) {
    const hipError_t restore = setDevice(originalDevice);
    if (status == hipSuccess && restore != hipSuccess) status = restore;
  }
  if (status != hipSuccess) return status;

  SnapshotScratch raw;
  const hipError_t scratchStatus = raw.allocate();
  if (scratchStatus != hipSuccess) return scratchStatus;
  std::vector<unsigned char> compressed(ZSTD_compressBound(kSnapshotChunkBytes));
  std::vector<std::vector<size_t>> plans(storages.size());
  size_t additionalBytes = 0;
  for (size_t i = 0; i < storages.size(); ++i) {
    const auto& storage = storages[i];
    plans[i].resize(storage->segments.size());
    int previous = -1; bool switched = false;
    status = switchDeviceFor([&] {
      for (const auto& item : gAllocations) if (item.second.vmm == storage) return item.second.device;
      return 0;
    }(), &previous, &switched);
    if (status != hipSuccess) return status;
    for (size_t j = 0; j < storage->segments.size(); ++j) {
      const auto& segment = storage->segments[j];
      if (segment.snapshot || !segment.mapped) continue;
      size_t planned = 0;
      status = measureSegmentSnapshot(*storage, segment, &raw, &compressed, &planned);
      if (status != hipSuccess) break;
      if (planned > std::numeric_limits<size_t>::max() - additionalBytes) {
        status = hipErrorOutOfMemory; break;
      }
      plans[i][j] = planned;
      additionalBytes += planned;
    }
    if (switched) {
      const hipError_t restore = realHipSetDevice()(previous);
      if (status == hipSuccess && restore != hipSuccess) status = restore;
    }
    if (status != hipSuccess) return status;
  }
  size_t existingCold = 0;
  { std::lock_guard<std::mutex> lock(gMutex); existingCold = gColdStoredBytes; }
  if (additionalBytes > coldBudgetBytes || existingCold > coldBudgetBytes - additionalBytes)
    return hipErrorOutOfMemory;

  // Release host-tier segments first so their GTT backing returns RAM before the
  // bounded per-segment snapshot buffer for local VRAM segments is allocated.
  for (size_t pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < storages.size(); ++i) {
      auto& storage = *storages[i];
      int device = 0;
      for (const auto& item : gAllocations) if (item.second.vmm == storages[i]) { device = item.second.device; break; }
      int previous = -1; bool switched = false;
      status = switchDeviceFor(device, &previous, &switched);
      if (status != hipSuccess) return status;
      for (size_t j = 0; j < storage.segments.size(); ++j) {
        auto& segment = storage.segments[j];
        if (static_cast<size_t>(segment.host ? 0 : 1) != pass) continue;
        if (!segment.snapshot && segment.mapped) {
          const size_t planned = plans[i][j];
          if (planned && !snapshotMemoryAvailable(planned)) {
            status = hipErrorOutOfMemory; break;
          }
          status = buildSegmentSnapshot(storage, segment, planned, &raw, &compressed);
          if (status != hipSuccess) break;
        }
        if (segment.snapshot) setColdLogical(storage, true);
        status = releaseSegmentBacking(storage, segment, device);
        if (status != hipSuccess) break;
      }
      if (switched) {
        const hipError_t restore = realHipSetDevice()(previous);
        if (status == hipSuccess && restore != hipSuccess) status = restore;
      }
      if (status != hipSuccess) return status;
    }
  }
  return hipSuccess;
}

hipError_t zvramResume() {
  std::lock_guard<std::recursive_mutex> residency(gResidencyMutex);
  std::vector<std::shared_ptr<VmmStorage>> storages;
  try {
    std::lock_guard<std::mutex> lock(gMutex);
    for (const auto& item : gAllocations)
      if (item.second.origin == Origin::HybridVmm && item.second.vmm &&
          item.second.vmm->coldLogical &&
          std::find(storages.begin(), storages.end(), item.second.vmm) == storages.end())
        storages.push_back(item.second.vmm);
  } catch (...) { return hipErrorOutOfMemory; }
  if (storages.empty()) return hipSuccess;
  SnapshotScratch raw;
  const hipError_t scratchStatus = raw.allocate();
  if (scratchStatus != hipSuccess) return scratchStatus;
  for (const auto& shared : storages) {
    auto& storage = *shared;
    if (storage.freeStarted) return hipErrorInvalidValue;
    int device = 0;
    for (const auto& item : gAllocations) if (item.second.vmm == shared) { device = item.second.device; break; }
    int previous = -1; bool switched = false;
    hipError_t status = switchDeviceFor(device, &previous, &switched);
    if (status != hipSuccess) return status;
    for (auto& segment : storage.segments) {
      if (!segment.accounted || !segment.mapped) {
        status = createSegmentBacking(storage, segment, device);
        if (status != hipSuccess) break;
      }
    }
    if (status == hipSuccess) {
      // ROCm 7.2 validates access ranges from the parent's first child, so a
      // later child's size alone can fail. Grant the complete mapped range.
      hipMemAccessDesc access{};
      access.location = {hipMemLocationTypeDevice, device};
      access.flags = hipMemAccessFlagsProtReadWrite;
      const auto setAccess = realHipMemSetAccess();
      status = setAccess ? setAccess(storage.base, storage.mappedBytes, &access, 1)
                         : hipErrorNotSupported;
      if (status == hipSuccess)
        for (auto& segment : storage.segments) segment.accessGranted = true;
    }
    if (status == hipSuccess) {
      for (auto& segment : storage.segments) {
        status = restoreSegmentSnapshot(storage, segment, &raw);
        if (status != hipSuccess) break;
      }
    }
    if (switched) {
      const hipError_t restore = realHipSetDevice()(previous);
      if (status == hipSuccess && restore != hipSuccess) status = restore;
    }
    if (status != hipSuccess) return status;
    bool allHot = std::all_of(storage.segments.begin(), storage.segments.end(),
        [](const VmmSegment& segment) { return segment.accounted && segment.mapped && !segment.snapshot; });
    if (allHot) setColdLogical(storage, false);
  }
  return hipSuccess;
}

hipError_t freeTracked(void* pointer, Allocation* allocation, bool* freed) {
  *freed = false;
  if (!allocation) return hipErrorInvalidValue;
  const auto getDevice = realHipGetDevice();
  const auto setDevice = realHipSetDevice();
  if (!getDevice || !setDevice) return hipErrorNotSupported;

  int previousDevice = -1;
  hipError_t status = getDevice(&previousDevice);
  if (status != hipSuccess) return status;
  const bool switched = previousDevice != allocation->device;
  if (switched) {
    status = setDevice(allocation->device);
    if (status != hipSuccess) return status;
  }

  if (allocation->origin == Origin::HybridVmm) {
    const auto synchronize = realHipDeviceSynchronize();
    status = synchronize ? synchronize() : hipErrorNotSupported;
    if (status != hipSuccess) {
      if (switched) (void)setDevice(previousDevice);
      return status;
    }
  }

  if (allocation->origin == Origin::MappedHost) {
    const auto hostFree = realHipHostFree();
    status = hostFree ? hostFree(allocation->hostPointer) : hipErrorNotSupported;
  } else if (allocation->origin == Origin::HybridVmm) {
    const auto unmap = realHipMemUnmap();
    const auto release = realHipMemRelease();
    const auto addressFree = realHipMemAddressFree();
    const auto storage = allocation->vmm;
    if (!unmap || !release || !addressFree || !storage) {
      status = hipErrorNotSupported;
    } else {
      storage->freeStarted = true;
      bool changed = false;
      for (auto it = storage->segments.rbegin(); it != storage->segments.rend(); ++it) {
        if (!it->mapped) continue;
        status = unmap(static_cast<char*>(storage->base) + it->offset, it->bytes);
        if (status != hipSuccess) break;
        it->mapped = false;
        changed = true;
      }
      if (status == hipSuccess) {
        for (auto it = storage->segments.rbegin(); it != storage->segments.rend(); ++it) {
          if (!it->handle) continue;
          status = release(it->handle);
          if (status != hipSuccess) break;
          it->handle = {};
          changed = true;
        }
      }
#ifdef ZVRAM_HAS_DRM_VMM
      if (status == hipSuccess && !cleanupGttProvider(*storage)) {
        status = hipErrorUnknown;
        changed = true;
      }
#endif
      if (status == hipSuccess && storage->addressReserved) {
        status = addressFree(storage->base, storage->mappedBytes);
        if (status == hipSuccess) storage->addressReserved = false;
      }
      if (status != hipSuccess && changed)
        std::fprintf(stderr,
                     "[zvram-hip] VMM free partially cleaned; do not use pointer, retry hipFree to finish: %s\n",
                     hipGetErrorString(status));
    }
  } else {
    const auto free = realHipFree();
    status = free ? free(pointer) : hipErrorNotSupported;
  }
  *freed = status == hipSuccess;

  if (switched) {
    const hipError_t restore = setDevice(previousDevice);
    if (status == hipSuccess && restore != hipSuccess) status = restore;
  }
  return status;
}

// Opt-in queries describe the configured combined backing tier. Native hardware
// queries remain unchanged unless the caller explicitly requests this mode.
thread_local bool gCapacityQuery = false;
struct CapacityQueryGuard {
  bool previous = gCapacityQuery;
  CapacityQueryGuard() { gCapacityQuery = true; }
  ~CapacityQueryGuard() { gCapacityQuery = previous; }
};

bool capacityReportingEnabled() {
  const char* enabled = std::getenv("ZVRAM_HIP_REPORT_CAPACITY");
  return !gCapacityQuery && enabled && std::strcmp(enabled, "1") == 0 &&
         hybridVmmEnabled() && localLimit().set && hostLimit().set && hostLimit().bytes;
}

bool combinedCapacity(int device, size_t nativeTotal, size_t nativeFree,
                      size_t* total, size_t* free) {
#ifdef ZVRAM_HAS_DRM_VMM
  int integrated = 1;
  const auto getAttribute = realHipGetDeviceAttribute();
  if (!getAttribute || getAttribute(&integrated, hipDeviceAttributeIntegrated, device) != hipSuccess || integrated)
    return false;
  VmmStorage storage;
  if (acquireGttProvider(device, &storage.drm) != hipSuccess) return false;
  amdgpu_heap_info heap{};
  if (amdgpu_query_heap_info(storage.drm->device, AMDGPU_GEM_DOMAIN_GTT, 0, &heap) != 0)
    return false;
  size_t available = 0;
  if (!memAvailableBytes(&available)) return false;
  size_t localCurrent = 0, hostCurrent = 0, hostPending = 0;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto found = gDeviceBytes.find(device);
    if (found != gDeviceBytes.end()) localCurrent = found->second.current;
    hostCurrent = gTotals.hostCurrent;
    hostPending = gTotals.hostPending;
  }
  const size_t local = std::min(localLimit().bytes, nativeTotal);
  const size_t host = std::min<size_t>(hostLimit().bytes, heap.heap_size);
  if (host > std::numeric_limits<size_t>::max() - local) return false;
  *total = local + host;
  const size_t localFree = localCurrent < local ? std::min(nativeFree, local - localCurrent) : 0;
  const size_t hostRemaining = hostCurrent < host ? host - hostCurrent : 0;
  const size_t reservedRemaining = hostPending < hostRemaining ? hostRemaining - hostPending : 0;
  const size_t ramFree = available > kHostReserveBytes ? available - kHostReserveBytes : 0;
  const size_t safeRamFree = hostPending < ramFree ? ramFree - hostPending : 0;
  const size_t gttFree = heap.heap_usage < heap.heap_size ? heap.heap_size - heap.heap_usage : 0;
  *free = localFree + std::min({reservedRemaining, safeRamFree, gttFree});
  return true;
#else
  (void)device; (void)nativeTotal; (void)nativeFree; (void)total; (void)free;
  return false;
#endif
}

}  // namespace

namespace {

hipError_t wrapHipMemGetInfo(size_t* free, size_t* total) {
  const auto next = realHipMemGetInfo();
  if (!next) return hipErrorNotSupported;
  const bool report = capacityReportingEnabled();
  CapacityQueryGuard guard;
  const hipError_t status = next(free, total);
  if (status != hipSuccess || !report || !free || !total) return status;
  try {
    int device = -1; size_t logicalTotal = 0, logicalFree = 0;
    if (getCurrentDevice(&device) && combinedCapacity(device, *total, *free, &logicalTotal, &logicalFree)) {
      *total = logicalTotal; *free = logicalFree;
    }
  } catch (...) { /* Keep the successful native query when reporting is unavailable. */ }
  return status;
}

hipError_t wrapHipDeviceTotalMem(size_t* bytes, hipDevice_t device) {
  const auto next = realHipDeviceTotalMem();
  if (!next) return hipErrorNotSupported;
  const bool report = capacityReportingEnabled();
  CapacityQueryGuard guard;
  const hipError_t status = next(bytes, device);
  if (status != hipSuccess || !report || !bytes) return status;
  try {
    size_t logicalTotal = 0, ignoredFree = 0;
    if (combinedCapacity(device, *bytes, 0, &logicalTotal, &ignoredFree)) *bytes = logicalTotal;
  } catch (...) { }
  return status;
}

// The installed HIP headers name this ABI hipGetDevicePropertiesR0600. Older
// property ABIs and queries outside HIP continue to report physical capacity.
hipError_t wrapHipGetDeviceProperties(hipDeviceProp_t* properties, int device) {
  const auto next = realHipGetDeviceProperties();
  if (!next) return hipErrorNotSupported;
  const bool report = capacityReportingEnabled();
  CapacityQueryGuard guard;
  const hipError_t status = next(properties, device);
  if (status != hipSuccess || !report || !properties) return status;
  try {
    size_t logicalTotal = 0, ignoredFree = 0;
    if (combinedCapacity(device, properties->totalGlobalMem, 0, &logicalTotal, &ignoredFree))
      properties->totalGlobalMem = logicalTotal;
  } catch (...) { }
  return status;
}

hipError_t wrapHipMalloc(void** pointer, size_t bytes) {
  std::lock_guard<std::recursive_mutex> residency(gResidencyMutex);
  try {
    const auto nativeMalloc = realHipMalloc();
    if (!nativeMalloc) return hipErrorNotSupported;
    registerSummary();

    if (!pointer) return nativeMalloc(pointer, bytes);
    if (bytes == 0) return nativeMalloc(pointer, bytes);

    int device = -1;
    const bool haveDevice = getCurrentDevice(&device);
    if (haveDevice && !ensureDeviceEntry(device)) {
      *pointer = nullptr;
      noteFailure();
      return hipErrorOutOfMemory;
    }

    // Automatic hibernation must own the physical mappings even when the
    // allocation fits entirely in local VRAM. Native hipMalloc is not evictable.
    if (haveDevice && hybridVmmEnabled() && zvramHipAutoRequested())
      return createHybridVmm(pointer, bytes, device, hipErrorOutOfMemory);

    NativeReservation reservation = NativeReservation::Failed;
    if (haveDevice) {
      reservation = reserveNativeBytes(bytes, device);
      if (reservation == NativeReservation::Failed) {
        *pointer = nullptr;
        noteFailure();
        return hipErrorOutOfMemory;
      }
      if (reservation == NativeReservation::LocalLimit)
        return hybridVmmEnabled()
                   ? createHybridVmm(pointer, bytes, device, hipErrorOutOfMemory)
                   : allocateMappedHost(pointer, bytes, device, hipErrorOutOfMemory);
    }

    const hipError_t status = nativeMalloc(pointer, bytes);
    if (status != hipSuccess) {
      if (haveDevice) releaseNativeBytes(bytes, device);
      if (status != hipErrorOutOfMemory || !haveDevice) {
        noteFailure();
        return status;
      }
      return hybridVmmEnabled()
                 ? createHybridVmm(pointer, bytes, device, status)
                 : allocateMappedHost(pointer, bytes, device, status);
    }

    if (haveDevice && pointer && *pointer) {
      const Allocation allocation{Origin::Native, nullptr, bytes, device, 0, 0, {}};
      if (!rememberAllocation(*pointer, allocation)) {
        const auto free = realHipFree();
        const hipError_t cleanup = free ? free(*pointer) : hipErrorNotSupported;
        releaseNativeBytes(bytes, device);
        if (cleanup != hipSuccess)
          std::fprintf(stderr, "[zvram-hip] native cleanup failed: %s\n",
                       hipGetErrorString(cleanup));
        *pointer = nullptr;
        noteFailure();
        return hipErrorOutOfMemory;
      }
    } else if (haveDevice) {
      releaseNativeBytes(bytes, device);
    }
    return hipSuccess;
  } catch (...) {
    if (pointer) *pointer = nullptr;
    noteFailure();
    return hipErrorOutOfMemory;
  }
}

hipError_t wrapHipFree(void* pointer) {
  std::lock_guard<std::recursive_mutex> residency(gResidencyMutex);
  try {
    registerSummary();
    Allocation allocation{};
    AllocationNode node;
    const LookupResult found = extractForFree(pointer, &allocation, &node);
    if (found == LookupResult::Untracked) {
      const auto free = realHipFree();
      return free ? free(pointer) : hipErrorNotSupported;
    }
    if (found == LookupResult::Busy) return hipErrorInvalidDevicePointer;
    if (found == LookupResult::Failed) return hipErrorOutOfMemory;
    bool freed = false;
    const hipError_t status = freeTracked(pointer, &allocation, &freed);
    finishTrackedFree(pointer, allocation, std::move(node), freed, true);
    return status;
  } catch (...) {
    noteFailure();
    return hipErrorOutOfMemory;
  }
}

hipError_t wrapHipFreeAsync(void* pointer, hipStream_t stream) {
  std::lock_guard<std::recursive_mutex> residency(gResidencyMutex);
  try {
    registerSummary();
    Allocation allocation{};
    AllocationNode node;
    const LookupResult found = extractForFree(pointer, &allocation, &node);
    if (found == LookupResult::Untracked) {
      const auto freeAsync = realHipFreeAsync();
      return freeAsync ? freeAsync(pointer, stream) : hipErrorNotSupported;
    }
    if (found == LookupResult::Busy) return hipErrorInvalidDevicePointer;
    if (found == LookupResult::Failed) return hipErrorOutOfMemory;
    if (allocation.origin != Origin::Native) {
      finishTrackedFree(pointer, allocation, std::move(node), false, false);
      noteAsyncFreeRejection();
      return hipErrorNotSupported;
    }
    const auto freeAsync = realHipFreeAsync();
    const hipError_t status = freeAsync ? freeAsync(pointer, stream) : hipErrorNotSupported;
    finishTrackedFree(pointer, allocation, std::move(node), status == hipSuccess, true);
    return status;
  } catch (...) {
    noteFailure();
    return hipErrorOutOfMemory;
  }
}

template <typename Function>
void* functionAddress(Function function) {
  return reinterpret_cast<void*>(function);
}

void* wrappedProcedure(const char* symbol, void* realAddress) {
  if (!symbol) return nullptr;
  if (std::strcmp(symbol, "hipMalloc") == 0 && realAddress == functionAddress(realHipMalloc()))
    return functionAddress(&zvramWrappedHipMalloc);
  if (std::strcmp(symbol, "hipFree") == 0 && realAddress == functionAddress(realHipFree()))
    return functionAddress(&zvramWrappedHipFree);
  if (std::strcmp(symbol, "hipFreeAsync") == 0 && realAddress == functionAddress(realHipFreeAsync()))
    return functionAddress(&zvramWrappedHipFreeAsync);
  if (std::strcmp(symbol, "hipMemGetInfo") == 0 && realAddress == functionAddress(realHipMemGetInfo()))
    return functionAddress(&zvramWrappedHipMemGetInfo);
  if (std::strcmp(symbol, "hipDeviceTotalMem") == 0 && realAddress == functionAddress(realHipDeviceTotalMem()))
    return functionAddress(&zvramWrappedHipDeviceTotalMem);
  const bool devicePropertiesName =
      std::strcmp(symbol, "hipGetDeviceProperties") == 0 ||
      std::strcmp(symbol, ZVRAM_STRINGIFY(hipGetDeviceProperties)) == 0;
  if (devicePropertiesName &&
      realAddress == functionAddress(realHipGetDeviceProperties()))
    return functionAddress(&zvramWrappedHipGetDeviceProperties);
  if (std::strcmp(symbol, "hipGetLastError") == 0 &&
      realAddress == functionAddress(realHipGetLastError()))
    return functionAddress(&zvramWrappedHipGetLastError);
  if (std::strcmp(symbol, "hipExtGetLastError") == 0 &&
      realAddress == functionAddress(realHipExtGetLastError()))
    return functionAddress(&zvramWrappedHipExtGetLastError);
  if (std::strcmp(symbol, "hipPeekAtLastError") == 0 &&
      realAddress == functionAddress(realHipPeekAtLastError()))
    return functionAddress(&zvramWrappedHipPeekAtLastError);
  return nullptr;
}

}  // namespace

// Experimental explicit lifecycle API: callers must serialize all HIP/GPU use
// of these allocations and must not dereference them until resume succeeds.
extern "C" hipError_t zvramHipHibernate(size_t coldBudgetBytes) {
  const bool external = !zvramHipInternalActivity();
  PrimaryCallBoundary boundary;
  // The coordinator alone owns lifecycle changes in automatic mode.
  if (zvramHipAutoRequested() && external)
    return boundary.finish(hipErrorNotSupported);
  try {
    return boundary.finish(zvramHibernate(coldBudgetBytes));
  } catch (const std::bad_alloc&) {
    return boundary.finish(hipErrorOutOfMemory);
  } catch (...) {
    return boundary.finish(hipErrorUnknown);
  }
}

extern "C" hipError_t zvramHipResume() {
  const bool external = !zvramHipInternalActivity();
  PrimaryCallBoundary boundary;
  if (zvramHipAutoRequested() && external)
    return boundary.finish(hipErrorNotSupported);
  try {
    return boundary.finish(zvramResume());
  } catch (const std::bad_alloc&) {
    return boundary.finish(hipErrorOutOfMemory);
  } catch (...) {
    return boundary.finish(hipErrorUnknown);
  }
}

extern "C" hipError_t zvramHipColdBytes(size_t* logicalColdBytes,
                                         size_t* storedColdBytes) {
  PrimaryCallBoundary boundary;
  if (!logicalColdBytes || !storedColdBytes)
    return boundary.finish(hipErrorInvalidValue);
  try {
    std::lock_guard<std::recursive_mutex> residency(gResidencyMutex);
    size_t logical = 0, stored = 0;
    {
      std::lock_guard<std::mutex> lock(gMutex);
      logical = gColdLogicalBytes;
      stored = gColdStoredBytes;
    }
    *logicalColdBytes = logical;
    *storedColdBytes = stored;
    return boundary.finish(hipSuccess);
  } catch (...) {
    return boundary.finish(hipErrorUnknown);
  }
}

extern "C" bool zvramHipLayerInternal() noexcept { return gPrimaryDepth != 0; }
extern "C" hipError_t zvramHipDispatchError(hipError_t native, bool clear) noexcept {
  const hipError_t result = native == hipSuccess ? gShadowError : native;
  if (clear) gShadowError = hipSuccess;
  return result;
}

extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramWrappedHipMalloc(void** pointer, size_t bytes) {
  ZvramHipActivity activity(true);
  PrimaryCallBoundary boundary;
  if (activity.status != hipSuccess) return boundary.finish(activity.status);
  return boundary.finish(wrapHipMalloc(pointer, bytes));
}

extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramWrappedHipFree(void* pointer) {
  ZvramHipActivity activity(false);
  PrimaryCallBoundary boundary;
  if (activity.status != hipSuccess) return boundary.finish(activity.status);
  return boundary.finish(wrapHipFree(pointer));
}

extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramWrappedHipFreeAsync(void* pointer, hipStream_t stream) {
  ZvramHipActivity activity(false);
  PrimaryCallBoundary boundary;
  if (activity.status != hipSuccess) return boundary.finish(activity.status);
  return boundary.finish(wrapHipFreeAsync(pointer, stream));
}

extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramWrappedHipMemGetInfo(size_t* free, size_t* total) {
  ZvramHipActivity activity(false);
  PrimaryCallBoundary boundary;
  if (activity.status != hipSuccess) return boundary.finish(activity.status);
  return boundary.finish(wrapHipMemGetInfo(free, total));
}

extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramWrappedHipDeviceTotalMem(size_t* bytes, hipDevice_t device) {
  ZvramHipActivity activity(false);
  PrimaryCallBoundary boundary;
  if (activity.status != hipSuccess) return boundary.finish(activity.status);
  return boundary.finish(wrapHipDeviceTotalMem(bytes, device));
}

extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramWrappedHipGetDeviceProperties(hipDeviceProp_t* properties, int device) {
  ZvramHipActivity activity(false);
  PrimaryCallBoundary boundary;
  if (activity.status != hipSuccess) return boundary.finish(activity.status);
  return boundary.finish(wrapHipGetDeviceProperties(properties, device));
}

extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramWrappedHipGetLastError() {
  const auto next = realHipGetLastError();
  if (!next) return hipErrorNotSupported;
  if (gPrimaryDepth) return next();
  const hipError_t native = next();
  const hipError_t result = native == hipSuccess ? gShadowError : native;
  gShadowError = hipSuccess;
  return result;
}

extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramWrappedHipExtGetLastError() {
  const auto next = realHipExtGetLastError();
  if (!next) return hipErrorNotSupported;
  if (gPrimaryDepth) return next();
  const hipError_t native = next();
  const hipError_t result = native == hipSuccess ? gShadowError : native;
  gShadowError = hipSuccess;
  return result;
}

extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramWrappedHipPeekAtLastError() {
  const auto next = realHipPeekAtLastError();
  if (!next) return hipErrorNotSupported;
  if (gPrimaryDepth) return next();
  const hipError_t native = next();
  return native == hipSuccess ? gShadowError : native;
}

extern "C" hipError_t hipMalloc(void** pointer, size_t bytes) {
  return zvramWrappedHipMalloc(pointer, bytes);
}

extern "C" hipError_t hipFree(void* pointer) {
  return zvramWrappedHipFree(pointer);
}

extern "C" hipError_t hipFreeAsync(void* pointer, hipStream_t stream) {
  return zvramWrappedHipFreeAsync(pointer, stream);
}

extern "C" hipError_t hipMemGetInfo(size_t* free, size_t* total) {
  return zvramWrappedHipMemGetInfo(free, total);
}

extern "C" hipError_t hipDeviceTotalMem(size_t* bytes, hipDevice_t device) {
  return zvramWrappedHipDeviceTotalMem(bytes, device);
}

// The installed headers expose this ABI as hipGetDevicePropertiesR0600.
extern "C" hipError_t hipGetDeviceProperties(hipDeviceProp_t* properties, int device) {
  return zvramWrappedHipGetDeviceProperties(properties, device);
}

extern "C" hipError_t hipGetLastError() {
  return zvramWrappedHipGetLastError();
}

extern "C" hipError_t hipExtGetLastError() {
  return zvramWrappedHipExtGetLastError();
}

extern "C" hipError_t hipPeekAtLastError() {
  return zvramWrappedHipPeekAtLastError();
}

extern "C" hipError_t hipGetProcAddress(
    const char* symbol, void** pfn, int hipVersion, uint64_t flags,
    hipDriverProcAddressQueryResult* symbolStatus) {
  const auto next = realHipGetProcAddress();
  if (!next) return hipErrorNotSupported;
  const hipError_t status = next(symbol, pfn, hipVersion, flags, symbolStatus);
  // Interpose only the documented default lookup mode. Some runtimes accept
  // malformed versions/flags; preserve their result rather than guessing an ABI.
  if (status != hipSuccess || flags != 0 || hipVersion < 0 || !symbol || !pfn || !*pfn ||
      (symbolStatus && *symbolStatus != HIP_GET_PROC_ADDRESS_SUCCESS))
    return status;
  if (void* replacement = wrappedProcedure(symbol, *pfn)) *pfn = replacement;
  return status;
}
