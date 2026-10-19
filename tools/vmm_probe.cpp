#include <hip/hip_runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

__global__ void fill_words(unsigned int* p, size_t n) {
  size_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) p[i] = static_cast<unsigned int>(i * 2654435761u + 0x9e3779b9u);
}

static void check(hipError_t e, const char* where) {
  if (e != hipSuccess) {
    std::fprintf(stderr, "%s: %s (%d)\n", where, hipGetErrorString(e), int(e));
    std::exit(1);
  }
}

static void snapshot(const char* label) {
  auto readMetric = [](const char* path, unsigned long long* value) {
    std::ifstream input(path);
    return bool(input >> *value);
  };
  unsigned long long vram = 0, gtt = 0, memAvailableKiB = 0;
  const bool haveVram = readMetric("/sys/class/drm/card1/device/mem_info_vram_used", &vram);
  const bool haveGtt = readMetric("/sys/class/drm/card1/device/mem_info_gtt_used", &gtt);
  std::ifstream meminfo("/proc/meminfo");
  std::string key, unit;
  while (meminfo >> key) {
    if (key == "MemAvailable:") { meminfo >> memAvailableKiB >> unit; break; }
    std::string rest;
    std::getline(meminfo, rest);
  }
  std::printf("snapshot=%s VRAM_used=%s%llu GTT_used=%s%llu MemAvailable_KiB=%llu\n",
              label, haveVram ? "" : "unavailable:", vram,
              haveGtt ? "" : "unavailable:", gtt, memAvailableKiB);
}

static int runHostOnlyProbe(int device, size_t requestedBytes) {
  constexpr size_t maxHandleBytes = 256ull << 20;
  constexpr size_t verifyChunkBytes = 8ull << 20;
  hipMemAllocationProp prop{};
  prop.type = hipMemAllocationTypePinned;
  prop.location = {hipMemLocationTypeHost, 0};
  size_t granularity = 0;
  check(hipMemGetAllocationGranularity(&granularity, &prop,
                                       hipMemAllocationGranularityMinimum),
        "host granularity");
  size_t totalBytes = requestedBytes;
  if (!granularity || totalBytes % granularity) {
    std::fprintf(stderr, "host allocation size is incompatible with granularity=%zu\n",
                 granularity);
    return 4;
  }

  std::vector<hipMemGenericAllocationHandle_t> handles;
  std::vector<size_t> offsets;
  std::vector<size_t> sizes;
  const size_t segmentCount = totalBytes / maxHandleBytes +
                              (totalBytes % maxHandleBytes != 0);
  try {
    handles.reserve(segmentCount);
    offsets.reserve(segmentCount);
    sizes.reserve(segmentCount);
  } catch (...) {
    std::fprintf(stderr, "could not reserve probe bookkeeping\n");
    return 5;
  }

  snapshot("host_only_before_create");
  size_t offset = 0;
  while (offset < totalBytes) {
    const size_t amount = std::min(maxHandleBytes, totalBytes - offset);
    hipMemGenericAllocationHandle_t handle{};
    const hipError_t status = hipMemCreate(&handle, amount, &prop, 0);
    if (status != hipSuccess) {
      std::fprintf(stderr, "host hipMemCreate at offset=%zu size=%zu: %s (%d)\n",
                   offset, amount, hipGetErrorString(status), int(status));
      for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        const hipError_t cleanup = hipMemRelease(*it);
        if (cleanup != hipSuccess)
          std::fprintf(stderr, "handle cleanup failed: %s\n", hipGetErrorString(cleanup));
      }
      return 6;
    }
    handles.push_back(handle);
    offsets.push_back(offset);
    sizes.push_back(amount);
    offset += amount;
    char label[64];
    std::snprintf(label, sizeof(label), "host_only_after_create_%zu", handles.size());
    snapshot(label);
  }

  void* va = nullptr;
  check(hipMemAddressReserve(&va, totalBytes, granularity, nullptr, 0),
        "host-only VA reserve");
  size_t mapped = 0;
  for (; mapped < handles.size(); ++mapped) {
    check(hipMemMap(static_cast<char*>(va) + offsets[mapped], sizes[mapped], 0,
                    handles[mapped], 0), "host-only map");
  }
  hipMemAccessDesc access{};
  access.location = {hipMemLocationTypeDevice, device};
  access.flags = hipMemAccessFlagsProtReadWrite;
  check(hipMemSetAccess(va, totalBytes, &access, 1), "host-only set access");
  snapshot("host_only_after_map_access");

  const size_t words = totalBytes / sizeof(unsigned int);
  auto* devicePointer = static_cast<unsigned int*>(va);
  hipLaunchKernelGGL(fill_words, dim3((words + 255) / 256), dim3(256), 0, 0,
                     devicePointer, words);
  check(hipGetLastError(), "host-only kernel launch");
  check(hipDeviceSynchronize(), "host-only kernel sync");
  snapshot("host_only_after_full_gpu_write");

  std::vector<unsigned int> staging(verifyChunkBytes / sizeof(unsigned int));
  for (size_t start = 0; start < words; start += staging.size()) {
    const size_t count = std::min(staging.size(), words - start);
    check(hipMemcpy(staging.data(), devicePointer + start,
                    count * sizeof(unsigned int), hipMemcpyDeviceToHost),
          "host-only copyback");
    for (size_t i = 0; i < count; ++i) {
      const size_t index = start + i;
      const unsigned int expected = static_cast<unsigned int>(
          index * 2654435761u + 0x9e3779b9u);
      if (staging[i] != expected) {
        std::fprintf(stderr, "host-only mismatch at word %zu: %08x != %08x\n",
                     index, staging[i], expected);
        return 7;
      }
    }
  }
  std::printf("PASS: host-only VMM GPU wrote and CPU verified %zu MiB via %zu handles, max handle %zu MiB\n",
              totalBytes >> 20, handles.size(), maxHandleBytes >> 20);

  for (size_t i = handles.size(); i > 0; --i)
    check(hipMemUnmap(static_cast<char*>(va) + offsets[i - 1], sizes[i - 1]),
          "host-only unmap");
  for (size_t i = handles.size(); i > 0; --i)
    check(hipMemRelease(handles[i - 1]), "host-only release");
  check(hipMemAddressFree(va, totalBytes), "host-only free VA");
  snapshot("host_only_after_cleanup");
  return 0;
}

int main(int argc, char** argv) {
  int count = 0, dev = -1, vmm = 0;
  check(hipGetDeviceCount(&count), "hipGetDeviceCount");
  for (int i = 0; i < count; ++i) {
    hipDeviceProp_t p{};
    check(hipGetDeviceProperties(&p, i), "hipGetDeviceProperties");
    if (std::string(p.gcnArchName).find("gfx1100") == 0) { dev = i; break; }
  }
  if (dev < 0) { std::fprintf(stderr, "gfx1100 not found\n"); return 2; }
  check(hipSetDevice(dev), "hipSetDevice");
  check(hipDeviceGetAttribute(&vmm, hipDeviceAttributeVirtualMemoryManagementSupported, dev), "VMM attribute");
  std::printf("device=%d VMM=%d\n", dev, vmm);
  if (!vmm) return 3;
  if (argc == 3 && std::string(argv[1]) == "--host-only-mib") {
    char* end = nullptr;
    const unsigned long long mib = std::strtoull(argv[2], &end, 10);
    if (end == argv[2] || *end || mib == 0 || mib > 1024) {
      std::fprintf(stderr, "Usage: vmm-probe [--host-only-mib 1..1024]\n");
      return 2;
    }
    check(hipSetDevice(dev), "host-only set device");
    snapshot("host_only_context_ready");
    return runHostOnlyProbe(dev, static_cast<size_t>(mib) << 20);
  }
  if (argc != 1) {
    std::fprintf(stderr, "Usage: vmm-probe [--host-only-mib 1..1024]\n");
    return 2;
  }

  constexpr size_t total = 64ull << 20, half = total / 2;
  hipMemAllocationProp gp{};
  gp.type = hipMemAllocationTypePinned;
  gp.location = {hipMemLocationTypeDevice, dev};
  hipMemAllocationProp hp{};
  hp.type = hipMemAllocationTypePinned;
  hp.location = {hipMemLocationTypeHost, 0};
  size_t gg = 0, hg = 0;
  check(hipMemGetAllocationGranularity(&gg, &gp, hipMemAllocationGranularityMinimum), "device granularity");
  check(hipMemGetAllocationGranularity(&hg, &hp, hipMemAllocationGranularityMinimum), "host granularity");
  std::printf("granularity device=%zu host=%zu\n", gg, hg);
  if (!gg || !hg || half % gg || half % hg) { std::fprintf(stderr, "granularity incompatible\n"); return 4; }

  hipMemGenericAllocationHandle_t gh{}, hh{};
  void* va = nullptr;
  check(hipMemCreate(&gh, half, &gp, 0), "device hipMemCreate");
  auto e = hipMemCreate(&hh, half, &hp, 0);
  if (e != hipSuccess) {
    std::fprintf(stderr, "host hipMemCreate: %s (%d)\n", hipGetErrorString(e), int(e));
    const hipError_t cleanup = hipMemRelease(gh);
    if (cleanup != hipSuccess)
      std::fprintf(stderr, "device handle cleanup: %s\n", hipGetErrorString(cleanup));
    return 5;
  }
  check(hipMemAddressReserve(&va, total, 0, nullptr, 0), "hipMemAddressReserve");
  check(hipMemMap(va, half, 0, gh, 0), "map device half");
  check(hipMemMap(static_cast<char*>(va) + half, half, 0, hh, 0), "map host half");
  hipMemAccessDesc access{};
  access.location = {hipMemLocationTypeDevice, dev};
  access.flags = hipMemAccessFlagsProtReadWrite;
  check(hipMemSetAccess(va, total, &access, 1), "hipMemSetAccess");

  hipPointerAttribute_t attr{};
  e = hipPointerGetAttributes(&attr, va);
  std::printf("pointer attributes query=%s (%d), memoryType=%d, device=%d, isManaged=%d\n",
              hipGetErrorString(e), int(e), int(attr.type), attr.device, int(attr.isManaged));

  auto* p = static_cast<unsigned int*>(va);
  const size_t words = total / sizeof(unsigned int);
  hipLaunchKernelGGL(fill_words, dim3((words + 255) / 256), dim3(256), 0, 0, p, words);
  check(hipGetLastError(), "kernel launch");
  check(hipDeviceSynchronize(), "kernel sync");
  std::vector<unsigned int> got(words);
  check(hipMemcpy(got.data(), va, total, hipMemcpyDeviceToHost), "copy back mixed mapping");
  for (size_t i = 0; i < words; ++i) {
    unsigned int expected = static_cast<unsigned int>(i * 2654435761u + 0x9e3779b9u);
    if (got[i] != expected) {
      std::fprintf(stderr, "mismatch at word %zu: %08x != %08x\n", i, got[i], expected);
      return 6;
    }
  }
  std::printf("PASS: GPU wrote and HIP copied/verified %zu MiB across one VA (device %zu MiB + host %zu MiB)\n", total >> 20, half >> 20, half >> 20);
  check(hipMemUnmap(va, total), "hipMemUnmap");
  check(hipMemRelease(gh), "release device handle");
  check(hipMemRelease(hh), "release host handle");
  check(hipMemAddressFree(va, total), "hipMemAddressFree");
  return 0;
}
