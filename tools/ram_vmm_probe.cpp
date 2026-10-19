#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

__global__ void fill_words(unsigned int* p, size_t n) {
  size_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) p[i] = static_cast<unsigned int>(i * 2654435761u + 0x9e3779b9u);
}

static void snapshot(const char* label) {
  auto readMetric = [](const char* path, unsigned long long* value) {
    std::ifstream in(path);
    return bool(in >> *value);
  };
  unsigned long long vram = 0, gtt = 0, availableKiB = 0;
  bool haveVram = readMetric("/sys/class/drm/card1/device/mem_info_vram_used", &vram);
  bool haveGtt = readMetric("/sys/class/drm/card1/device/mem_info_gtt_used", &gtt);
  std::ifstream meminfo("/proc/meminfo");
  std::string key, unit;
  while (meminfo >> key) {
    if (key == "MemAvailable:") { meminfo >> availableKiB >> unit; break; }
    std::string rest;
    std::getline(meminfo, rest);
  }
  std::printf("snapshot=%s VRAM_used=%s%llu GTT_used=%s%llu MemAvailable_KiB=%llu\n",
              label, haveVram ? "" : "unavailable:", vram,
              haveGtt ? "" : "unavailable:", gtt, availableKiB);
}

static bool hipOk(hipError_t e, const char* where) {
  if (e == hipSuccess) return true;
  std::fprintf(stderr, "%s: %s (%d)\n", where, hipGetErrorString(e), int(e));
  return false;
}

static bool hsaOk(hsa_status_t e, const char* where) {
  if (e == HSA_STATUS_SUCCESS) return true;
  const char* message = nullptr;
  hsa_status_string(e, &message);
  std::fprintf(stderr, "%s: %s (%d)\n", where,
               message ? message : "unknown HSA error", int(e));
  return false;
}

int main() {
  constexpr size_t total = 64ull << 20;
  constexpr size_t maxMiB = 64;
  static_assert(total == maxMiB * (1ull << 20));
  int count = 0, device = -1;
  if (!hipOk(hipGetDeviceCount(&count), "hipGetDeviceCount")) return 1;
  for (int i = 0; i < count; ++i) {
    hipDeviceProp_t props{};
    if (!hipOk(hipGetDeviceProperties(&props, i), "hipGetDeviceProperties")) return 1;
    if (std::string(props.gcnArchName).find("gfx1100") == 0) { device = i; break; }
  }
  if (device < 0) { std::fprintf(stderr, "gfx1100 not found\n"); return 2; }
  if (!hipOk(hipSetDevice(device), "hipSetDevice")) return 1;
  if (!hsaOk(hsa_init(), "hsa_init")) return 3;
  snapshot("before_host_alloc");

  void* host = nullptr;
  hipMemGenericAllocationHandle_t imported{};
  void* va = nullptr;
  int fd = -1;
  bool mapped = false;
  bool importedReady = false;
  int result = 4;
  if (!hipOk(hipHostMalloc(&host, total, hipHostMallocMapped), "hipHostMalloc mapped 64MiB"))
    goto cleanup;
  snapshot("after_hipHostMalloc");
  {
    void* deviceAlias = nullptr;
    if (!hipOk(hipHostGetDevicePointer(&deviceAlias, host, 0), "hipHostGetDevicePointer"))
      goto cleanup;
    std::printf("hipHostMalloc host=%p device_alias=%p\n", host, deviceAlias);
  }

  {
    uint64_t exportOffset = 0;
    const hsa_status_t exportStatus =
        hsa_amd_portable_export_dmabuf(host, total, &fd, &exportOffset);
    if (!hsaOk(exportStatus, "hsa_amd_portable_export_dmabuf(host allocation)"))
      goto cleanup;
    std::printf("portable_export fd=%d offset=%llu\n", fd,
                static_cast<unsigned long long>(exportOffset));
    if (exportOffset != 0) {
      std::fprintf(stderr, "host allocation export has nonzero offset; whole-handle map not tested\n");
      goto cleanup;
    }
  }
  snapshot("after_dmabuf_export");
  {
    const hipError_t e = hipMemImportFromShareableHandle(
        &imported, reinterpret_cast<void*>(static_cast<intptr_t>(fd)),
        hipMemHandleTypePosixFileDescriptor);
    std::printf("hipMemImportFromShareableHandle status=%d (%s)\n", int(e), hipGetErrorString(e));
    if (e != hipSuccess) goto cleanup;
    importedReady = true;
  }
  {
    const hsa_status_t closeStatus = hsa_amd_portable_close_dmabuf(fd);
    fd = -1;
    if (!hsaOk(closeStatus, "hsa_amd_portable_close_dmabuf")) goto cleanup;
  }
  {
    hipMemAllocationProp prop{};
    const hipError_t e = hipMemGetAllocationPropertiesFromHandle(&prop, imported);
    std::printf("imported_properties_status=%d location_type=%d location_id=%d allocation_type=%d\n",
                int(e), int(prop.location.type), prop.location.id, int(prop.type));
    if (e != hipSuccess) goto cleanup;
    size_t granularity = 0;
    if (!hipOk(hipMemGetAllocationGranularity(&granularity, &prop,
            hipMemAllocationGranularityMinimum), "imported handle granularity")) goto cleanup;
    std::printf("imported_granularity=%zu\n", granularity);
    if (!granularity || total % granularity) {
      std::fprintf(stderr, "64MiB size does not satisfy imported handle granularity\n");
      goto cleanup;
    }
    if (!hipOk(hipMemAddressReserve(&va, total, granularity, nullptr, 0),
               "hipMemAddressReserve")) goto cleanup;
    if (!hipOk(hipMemMap(va, total, 0, imported, 0), "hipMemMap imported host dma-buf"))
      goto cleanup;
    mapped = true;
    hipMemAccessDesc access{};
    access.location = {hipMemLocationTypeDevice, device};
    access.flags = hipMemAccessFlagsProtReadWrite;
    if (!hipOk(hipMemSetAccess(va, total, &access, 1), "hipMemSetAccess")) goto cleanup;
  }
  snapshot("after_import_map_access");

  {
    const size_t words = total / sizeof(unsigned int);
    hipLaunchKernelGGL(fill_words, dim3((words + 255) / 256), dim3(256), 0, 0,
                       static_cast<unsigned int*>(va), words);
    if (!hipOk(hipGetLastError(), "GPU fill launch")) goto cleanup;
    if (!hipOk(hipDeviceSynchronize(), "GPU fill sync")) goto cleanup;
    snapshot("after_full_gpu_write");
    std::vector<unsigned int> copy(words);
    if (!hipOk(hipMemcpy(copy.data(), va, total, hipMemcpyDeviceToHost),
               "hipMemcpy mapped host VMM")) goto cleanup;
    const auto* original = static_cast<const unsigned int*>(host);
    for (size_t i = 0; i < words; ++i) {
      const unsigned int expected = static_cast<unsigned int>(
          i * 2654435761u + 0x9e3779b9u);
      if (copy[i] != expected || original[i] != expected) {
        std::fprintf(stderr, "verification mismatch at word %zu: mapped=%08x original=%08x expected=%08x\n",
                     i, copy[i], original[i], expected);
        goto cleanup;
      }
    }
    std::printf("PASS: GPU VMM mapping of exported hipHostMalloc allocation wrote and verified %zu MiB\n",
                total >> 20);
  }
  result = 0;

cleanup:
  if (fd >= 0) {
    const hsa_status_t e = hsa_amd_portable_close_dmabuf(fd);
    if (!hsaOk(e, "cleanup dmabuf close")) result = 10;
  }
  if (mapped) {
    const hipError_t e = hipMemUnmap(va, total);
    if (!hipOk(e, "cleanup unmap")) result = 10;
  }
  if (va) {
    const hipError_t e = hipMemAddressFree(va, total);
    if (!hipOk(e, "cleanup address free")) result = 10;
  }
  if (importedReady) {
    const hipError_t e = hipMemRelease(imported);
    if (!hipOk(e, "cleanup imported handle release")) result = 10;
  }
  if (host) {
    const hipError_t e = hipHostFree(host);
    if (!hipOk(e, "cleanup hipHostFree")) result = 10;
  }
  snapshot("after_cleanup");
  const hsa_status_t shutdownStatus = hsa_shut_down();
  if (!hsaOk(shutdownStatus, "hsa_shut_down")) result = 10;
  return result;
}
