#include <hip/hip_runtime.h>
#include <amdgpu.h>
#include <amdgpu_drm.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

__global__ void fill_words(unsigned int* p, size_t n) {
  size_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) p[i] = static_cast<unsigned int>(i * 2654435761u + 0x9e3779b9u);
}

static bool hipOk(hipError_t e, const char* where) {
  if (e == hipSuccess) return true;
  std::fprintf(stderr, "%s: %s (%d)\n", where, hipGetErrorString(e), int(e));
  return false;
}

static bool drmOk(int e, const char* where) {
  if (e == 0) return true;
  std::fprintf(stderr, "%s: %s (%d)\n", where, std::strerror(-e), e);
  return false;
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

int main() {
  constexpr size_t total = 64ull << 20;
  int count = 0, device = -1;
  if (!hipOk(hipGetDeviceCount(&count), "hipGetDeviceCount")) return 1;
  for (int i = 0; i < count; ++i) {
    hipDeviceProp_t props{};
    if (!hipOk(hipGetDeviceProperties(&props, i), "hipGetDeviceProperties")) return 1;
    if (std::string(props.gcnArchName).find("gfx1100") == 0) { device = i; break; }
  }
  if (device < 0 || !hipOk(hipSetDevice(device), "hipSetDevice")) return 2;

  int drmFd = -1, dmaBufFd = -1;
  uint32_t major = 0, minor = 0;
  amdgpu_device_handle drmDevice = nullptr;
  amdgpu_bo_handle bo = nullptr;
  void* cpuMap = nullptr;
  hipMemGenericAllocationHandle_t imported{};
  void* va = nullptr;
  bool importedReady = false, mapped = false, cpuMapped = false;
  int result = 3;
  snapshot("before_drm_open");
  drmFd = open("/dev/dri/by-path/pci-0000:03:00.0-render", O_RDWR | O_CLOEXEC);
  if (drmFd < 0) { std::perror("open gfx1100 render node"); goto cleanup; }
  if (!drmOk(amdgpu_device_initialize(drmFd, &major, &minor, &drmDevice),
             "amdgpu_device_initialize")) goto cleanup;
  std::printf("libdrm_amdgpu=%u.%u\n", major, minor);

  {
    amdgpu_bo_alloc_request request{};
    request.alloc_size = total;
    request.phys_alignment = 4096;
    request.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
    request.flags = 0;
    snapshot("before_gtt_bo_alloc");
    if (!drmOk(amdgpu_bo_alloc(drmDevice, &request, &bo), "amdgpu_bo_alloc GTT")) goto cleanup;
    snapshot("after_gtt_bo_alloc");
    amdgpu_bo_info info{};
    if (!drmOk(amdgpu_bo_query_info(bo, &info), "amdgpu_bo_query_info")) goto cleanup;
    std::printf("gtt_bo_alloc_size=%llu preferred_heap=0x%x\n",
                static_cast<unsigned long long>(info.alloc_size), request.preferred_heap);
    if (info.alloc_size < total) {
      std::fprintf(stderr, "GTT BO smaller than requested\n"); goto cleanup;
    }
    if (!drmOk(amdgpu_bo_cpu_map(bo, &cpuMap), "amdgpu_bo_cpu_map")) goto cleanup;
    cpuMapped = true;
    uint32_t exported = 0;
    if (!drmOk(amdgpu_bo_export(bo, amdgpu_bo_handle_type_dma_buf_fd, &exported),
               "amdgpu_bo_export DMA_BUF_FD")) goto cleanup;
    dmaBufFd = static_cast<int>(exported);
    std::printf("exported_dma_buf_fd=%d\n", dmaBufFd);
  }
  snapshot("after_export_gtt_bo");

  {
    const hipError_t e = hipMemImportFromShareableHandle(
        &imported, reinterpret_cast<void*>(static_cast<intptr_t>(dmaBufFd)),
        hipMemHandleTypePosixFileDescriptor);
    std::printf("hipMemImportFromShareableHandle status=%d (%s)\n", int(e), hipGetErrorString(e));
    if (e != hipSuccess) goto cleanup;
    importedReady = true;
  }
  if (close(dmaBufFd) != 0) { std::perror("close dma-buf fd"); dmaBufFd = -1; goto cleanup; }
  dmaBufFd = -1;
  {
    hipMemAllocationProp prop{};
    const hipError_t e = hipMemGetAllocationPropertiesFromHandle(&prop, imported);
    std::printf("imported_properties_status=%d location_type=%d location_id=%d allocation_type=%d\n",
                int(e), int(prop.location.type), prop.location.id, int(prop.type));
    if (!hipOk(e, "hipMemGetAllocationPropertiesFromHandle")) goto cleanup;
    size_t granularity = 0;
    if (!hipOk(hipMemGetAllocationGranularity(&granularity, &prop,
            hipMemAllocationGranularityMinimum), "imported handle granularity")) goto cleanup;
    std::printf("imported_granularity=%zu\n", granularity);
    if (!granularity || total % granularity) { std::fprintf(stderr, "invalid import granularity\n"); goto cleanup; }
    if (!hipOk(hipMemAddressReserve(&va, total, granularity, nullptr, 0),
               "hipMemAddressReserve")) goto cleanup;
    if (!hipOk(hipMemMap(va, total, 0, imported, 0), "hipMemMap imported GTT BO")) goto cleanup;
    mapped = true;
    hipMemAccessDesc access{};
    access.location = {hipMemLocationTypeDevice, device};
    access.flags = hipMemAccessFlagsProtReadWrite;
    if (!hipOk(hipMemSetAccess(va, total, &access, 1), "hipMemSetAccess")) goto cleanup;
  }
  snapshot("after_import_map_access");

  {
    const size_t words = total / sizeof(uint32_t);
    hipLaunchKernelGGL(fill_words, dim3((words + 255) / 256), dim3(256), 0, 0,
                       static_cast<unsigned int*>(va), words);
    if (!hipOk(hipGetLastError(), "GPU fill launch") ||
        !hipOk(hipDeviceSynchronize(), "GPU fill sync")) goto cleanup;
    snapshot("after_full_gpu_write");
    std::vector<uint32_t> copy(words);
    if (!hipOk(hipMemcpy(copy.data(), va, total, hipMemcpyDeviceToHost),
               "hipMemcpy imported GTT VMM")) goto cleanup;
    const auto* host = static_cast<const uint32_t*>(cpuMap);
    for (size_t i = 0; i < words; ++i) {
      const uint32_t expected = static_cast<uint32_t>(i * 2654435761u + 0x9e3779b9u);
      if (copy[i] != expected || host[i] != expected) {
        std::fprintf(stderr, "verify mismatch at word %zu: copy=%08x cpu=%08x expected=%08x\n",
                     i, copy[i], host[i], expected);
        goto cleanup;
      }
    }
    std::printf("PASS: GPU VMM over exported AMDGPU_GEM_DOMAIN_GTT BO wrote and verified %zu MiB\n",
                total >> 20);
  }
  result = 0;

cleanup:
  if (dmaBufFd >= 0 && close(dmaBufFd) != 0) { std::perror("cleanup close dma-buf"); result = 10; }
  if (mapped && !hipOk(hipMemUnmap(va, total), "cleanup hipMemUnmap")) result = 10;
  if (va && !hipOk(hipMemAddressFree(va, total), "cleanup hipMemAddressFree")) result = 10;
  if (importedReady && !hipOk(hipMemRelease(imported), "cleanup hipMemRelease")) result = 10;
  if (cpuMapped && !drmOk(amdgpu_bo_cpu_unmap(bo), "cleanup amdgpu_bo_cpu_unmap")) result = 10;
  if (bo && !drmOk(amdgpu_bo_free(bo), "cleanup amdgpu_bo_free")) result = 10;
  snapshot("after_bo_free");
  if (drmDevice && !drmOk(amdgpu_device_deinitialize(drmDevice), "amdgpu_device_deinitialize")) result = 10;
  if (drmFd >= 0 && close(drmFd) != 0) { std::perror("close render node"); result = 10; }
  snapshot("after_cleanup");
  return result;
}
