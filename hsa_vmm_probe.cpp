#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

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

static void hipCheck(hipError_t e, const char* where) {
  if (e != hipSuccess) {
    std::fprintf(stderr, "%s: %s (%d)\n", where, hipGetErrorString(e), int(e));
    std::exit(1);
  }
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

struct Agents { hsa_agent_t cpu{}, gpu{}; bool haveCpu = false, haveGpu = false; };
static hsa_status_t findAgents(hsa_agent_t agent, void* opaque) {
  auto* out = static_cast<Agents*>(opaque);
  hsa_device_type_t type{};
  if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) != HSA_STATUS_SUCCESS)
    return HSA_STATUS_SUCCESS;
  if (type == HSA_DEVICE_TYPE_CPU && !out->haveCpu) {
    out->cpu = agent;
    out->haveCpu = true;
  } else if (type == HSA_DEVICE_TYPE_GPU && !out->haveGpu) {
    char name[64]{};
    if (hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, name) == HSA_STATUS_SUCCESS &&
        std::string(name).find("gfx1100") != std::string::npos) {
      out->gpu = agent;
      out->haveGpu = true;
    }
  }
  return HSA_STATUS_SUCCESS;
}

struct Pool { hsa_amd_memory_pool_t id{}; size_t granularity = 0; };
struct Pools { std::vector<Pool> cpu, gpu; };
static hsa_status_t collectPools(hsa_amd_memory_pool_t pool, void* opaque) {
  auto* out = static_cast<Pools*>(opaque);
  hsa_amd_memory_pool_location_t location{};
  hsa_amd_segment_t segment{};
  bool allowed = false;
  if (hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_LOCATION,
                                   &location) != HSA_STATUS_SUCCESS ||
      hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT,
                                   &segment) != HSA_STATUS_SUCCESS ||
      hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALLOWED,
                                   &allowed) != HSA_STATUS_SUCCESS ||
      segment != HSA_AMD_SEGMENT_GLOBAL || !allowed)
    return HSA_STATUS_SUCCESS;
  Pool p{pool, 0};
  if (hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_GRANULE,
                                   &p.granularity) != HSA_STATUS_SUCCESS)
    return HSA_STATUS_SUCCESS;
  (location == HSA_AMD_MEMORY_POOL_LOCATION_CPU ? out->cpu : out->gpu).push_back(p);
  return HSA_STATUS_SUCCESS;
}

static void reportHsa(const char* where, hsa_status_t status) {
  std::fprintf(stderr, "%s: HSA status=%d\n", where, int(status));
}

int main() {
  int count = 0, device = -1;
  hipCheck(hipGetDeviceCount(&count), "hipGetDeviceCount");
  for (int i = 0; i < count; ++i) {
    hipDeviceProp_t props{};
    hipCheck(hipGetDeviceProperties(&props, i), "hipGetDeviceProperties");
    if (std::string(props.gcnArchName).find("gfx1100") == 0) { device = i; break; }
  }
  if (device < 0) { std::fprintf(stderr, "gfx1100 not found\n"); return 2; }
  hipCheck(hipSetDevice(device), "hipSetDevice");
  constexpr size_t total = 64ull << 20;
  constexpr size_t half = total / 2;
  snapshot("before_hsa_init");

  hsa_status_t hs = hsa_init();
  if (hs != HSA_STATUS_SUCCESS) { reportHsa("hsa_init", hs); return 3; }
  Agents agents;
  hs = hsa_iterate_agents(findAgents, &agents);
  if (hs != HSA_STATUS_SUCCESS || !agents.haveCpu || !agents.haveGpu) {
    reportHsa("find HSA agents", hs); return 4;
  }
  Pools pools;
  hs = hsa_amd_agent_iterate_memory_pools(agents.cpu, collectPools, &pools);
  if (hs != HSA_STATUS_SUCCESS) { reportHsa("CPU pool enumeration", hs); return 4; }
  hs = hsa_amd_agent_iterate_memory_pools(agents.gpu, collectPools, &pools);
  if (hs != HSA_STATUS_SUCCESS) { reportHsa("GPU pool enumeration", hs); return 4; }
  std::printf("allocatable_global_pools cpu=%zu gpu=%zu\n", pools.cpu.size(), pools.gpu.size());
  for (size_t i = 0; i < pools.cpu.size(); ++i)
    std::printf("cpu_pool[%zu] granularity=%zu\n", i, pools.cpu[i].granularity);
  for (size_t i = 0; i < pools.gpu.size(); ++i)
    std::printf("gpu_pool[%zu] granularity=%zu\n", i, pools.gpu[i].granularity);

  hsa_amd_vmem_alloc_handle_t gh{}, ch{};
  bool gotPair = false;
  Pool gp{}, cp{};
  for (const Pool& candidateGpu : pools.gpu) {
    for (const Pool& candidateCpu : pools.cpu) {
      const size_t gran = std::max(candidateGpu.granularity, candidateCpu.granularity);
      if (!gran || half % candidateGpu.granularity || half % candidateCpu.granularity ||
          total % gran) continue;
      gp = candidateGpu;
      cp = candidateCpu;
      snapshot("before_gpu_handle_create");
      hs = hsa_amd_vmem_handle_create(gp.id, half, MEMORY_TYPE_NONE, 0, &gh);
      std::printf("gpu_handle_create_status=%d\n", int(hs));
      if (hs != HSA_STATUS_SUCCESS) continue;
      snapshot("after_gpu_handle_create");
      hs = hsa_amd_vmem_handle_create(cp.id, half, MEMORY_TYPE_NONE, 0, &ch);
      std::printf("cpu_handle_create_status=%d\n", int(hs));
      if (hs == HSA_STATUS_SUCCESS) { gotPair = true; break; }
      hsa_amd_vmem_handle_release(gh);
      snapshot("after_cpu_handle_create_failed_cleanup");
    }
    if (gotPair) break;
  }
  if (!gotPair) { reportHsa("create GPU+CPU VMM handles", hs); return 5; }
  snapshot("after_both_handles_created");

  void* va = nullptr;
  const size_t alignment = std::max(gp.granularity, cp.granularity);
  hs = hsa_amd_vmem_address_reserve_align(&va, total, 0, alignment, 0);
  if (hs != HSA_STATUS_SUCCESS) {
    reportHsa("reserve VA", hs); hsa_amd_vmem_handle_release(ch); hsa_amd_vmem_handle_release(gh); return 6;
  }
  hs = hsa_amd_vmem_map(va, half, 0, gh, 0);
  if (hs == HSA_STATUS_SUCCESS)
    hs = hsa_amd_vmem_map(static_cast<char*>(va) + half, half, 0, ch, 0);
  if (hs != HSA_STATUS_SUCCESS) {
    reportHsa("map GPU+CPU handles", hs);
    hsa_amd_vmem_unmap(va, half);
    hsa_amd_vmem_address_free(va, total);
    hsa_amd_vmem_handle_release(ch); hsa_amd_vmem_handle_release(gh); return 7;
  }
  hsa_amd_memory_access_desc_t access[2] = {
    {HSA_ACCESS_PERMISSION_RW, agents.cpu},
    {HSA_ACCESS_PERMISSION_RW, agents.gpu},
  };
  hs = hsa_amd_vmem_set_access(va, total, access, 2);
  if (hs != HSA_STATUS_SUCCESS) {
    reportHsa("set GPU access", hs);
    hsa_amd_vmem_unmap(static_cast<char*>(va) + half, half);
    hsa_amd_vmem_unmap(va, half);
    hsa_amd_vmem_address_free(va, total);
    hsa_amd_vmem_handle_release(ch); hsa_amd_vmem_handle_release(gh); return 8;
  }
  snapshot("after_map_access");

  const size_t words = total / sizeof(unsigned int);
  hipLaunchKernelGGL(fill_words, dim3((words + 255) / 256), dim3(256), 0, 0,
                     static_cast<unsigned int*>(va), words);
  hipCheck(hipGetLastError(), "GPU fill launch");
  hipCheck(hipDeviceSynchronize(), "GPU fill sync");
  snapshot("after_full_gpu_write");
  bool valid = true;
  const auto* direct = static_cast<const unsigned int*>(va);
  for (size_t i = 0; i < words; ++i) {
    const unsigned int expected = static_cast<unsigned int>(i * 2654435761u + 0x9e3779b9u);
    if (direct[i] != expected) {
      std::fprintf(stderr, "direct host verify mismatch at word %zu: %08x != %08x\n",
                   i, direct[i], expected); valid = false; break;
    }
  }
  std::vector<unsigned int> staging(words);
  const hipError_t copyStatus = hipMemcpy(staging.data(), va, total, hipMemcpyDeviceToHost);
  std::printf("hipMemcpyDeviceToHost_status=%d (%s)\n", int(copyStatus), hipGetErrorString(copyStatus));
  if (copyStatus == hipSuccess) {
    for (size_t i = 0; i < words; ++i) {
      if (staging[i] != static_cast<unsigned int>(i * 2654435761u + 0x9e3779b9u)) {
        std::fprintf(stderr, "HIP copy verify mismatch at word %zu\n", i); valid = false; break;
      }
    }
  } else valid = false;
  std::printf("%s: one %zu MiB VA (GPU pool %zu MiB + CPU pool %zu MiB)\n",
              valid ? "PASS" : "FAIL", total >> 20, half >> 20, half >> 20);

  hs = hsa_amd_vmem_unmap(static_cast<char*>(va) + half, half);
  std::printf("unmap_cpu_status=%d\n", int(hs));
  hs = hsa_amd_vmem_unmap(va, half);
  std::printf("unmap_gpu_status=%d\n", int(hs));
  hs = hsa_amd_vmem_address_free(va, total);
  std::printf("address_free_status=%d\n", int(hs));
  hs = hsa_amd_vmem_handle_release(ch);
  std::printf("cpu_handle_release_status=%d\n", int(hs));
  hs = hsa_amd_vmem_handle_release(gh);
  std::printf("gpu_handle_release_status=%d\n", int(hs));
  snapshot("after_cleanup");
  return valid ? 0 : 9;
}
