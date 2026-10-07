#include <hip/hip_runtime.h>
#include <dlfcn.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr size_t MiB = 1024 * 1024;
size_t Bytes = 32 * MiB;
size_t Words = Bytes / sizeof(uint64_t);
using Hibernate = hipError_t (*)(size_t);
using Resume = hipError_t (*)();
using ColdBytes = hipError_t (*)(size_t*, size_t*);

void check(hipError_t status, const char* operation) {
  if (status != hipSuccess)
    throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(status));
}
void require(bool okay, const char* message) {
  if (!okay) throw std::runtime_error(message);
}
template <class Function> Function resolve(const char* symbol) {
  auto address = dlsym(RTLD_DEFAULT, symbol);
  require(address != nullptr, "zVram hibernation API missing");
  return reinterpret_cast<Function>(address);
}

__host__ __device__ uint64_t pattern(uint64_t index, unsigned int cycle, size_t words) {
  if (index < words / 2) return uint64_t{0x123456789abcdef0} + cycle;
  uint64_t value = index + cycle * uint64_t{0x9e3779b97f4a7c15};
  value = (value ^ (value >> 30)) * uint64_t{0xbf58476d1ce4e5b9};
  value = (value ^ (value >> 27)) * uint64_t{0x94d049bb133111eb};
  return value ^ (value >> 31);
}
__global__ void fill(uint64_t* pointer, unsigned int cycle, size_t words) {
  const size_t index = size_t{blockIdx.x} * blockDim.x + threadIdx.x;
  if (index < words) pointer[index] = pattern(index, cycle, words);
}
__global__ void verifyGpu(const uint64_t* pointer, unsigned int cycle, size_t words, unsigned int* mismatch) {
  const size_t index = size_t{blockIdx.x} * blockDim.x + threadIdx.x;
  if (index < words && pointer[index] != pattern(index, cycle, words))
    atomicMin(mismatch, static_cast<unsigned int>(index));
}

struct Allocation {
  uint64_t* pointer = nullptr;
  ~Allocation() { if (pointer) (void)hipFree(pointer); }
};
void verify(Allocation& allocation, unsigned int cycle) {
  unsigned int* mismatch = nullptr;
  check(hipHostMalloc(reinterpret_cast<void**>(&mismatch), sizeof(*mismatch)), "verify status allocation");
  void* mismatchDevice = nullptr;
  check(hipHostGetDevicePointer(&mismatchDevice, mismatch, 0), "verify status alias");
  *mismatch = UINT32_MAX;
  hipLaunchKernelGGL(verifyGpu, dim3((Words + 255) / 256), dim3(256), 0, nullptr,
                     allocation.pointer, cycle, Words, static_cast<unsigned int*>(mismatchDevice));
  check(hipDeviceSynchronize(), "GPU verification synchronize");
  const unsigned int first = *mismatch;
  check(hipHostFree(mismatch), "verify status free");
  if (first != UINT32_MAX) {
    std::cerr << "GPU mismatch cycle=" << cycle << " byte=" << first * sizeof(uint64_t) << '\n';
    throw std::runtime_error("GPU kernel detected changed data");
  }
  {
    std::vector<uint64_t> output(Words);
    check(hipMemcpy(output.data(), allocation.pointer, Bytes, hipMemcpyDeviceToHost), "native readback");
    check(hipDeviceSynchronize(), "native readback synchronize");
    for (size_t index = 0; index < Words; ++index) {
      if (output[index] != pattern(index, cycle, Words)) {
        std::cerr << "native readback mismatch cycle=" << cycle << " byte="
                  << index * sizeof(uint64_t) << '\n';
        throw std::runtime_error("native HIP readback differs from GPU verification");
      }
    }
  }
}
void expectCold(ColdBytes stats, size_t expectedLogical, size_t maximumStored) {
  size_t logical = 0, stored = 0;
  check(stats(&logical, &stored), "cold stats");
  require(logical == expectedLogical, "unexpected cold logical bytes");
  require(stored <= maximumStored && (!logical || stored), "invalid cold stored bytes");
  std::cout << "cold logical=" << logical << " stored=" << stored << '\n';
}
void driverUsage(const char* phase) {
  char bus[64]{};
  if (hipDeviceGetPCIBusId(bus, sizeof(bus), 0) != hipSuccess) return;
  std::cout << "driver " << phase;
  for (const char* field : {"mem_info_vram_used", "mem_info_gtt_used"}) {
    uint64_t bytes = 0;
    std::ifstream file(std::string("/sys/bus/pci/devices/") + bus + "/" + field);
    if (file >> bytes) std::cout << ' ' << field << '=' << bytes;
  }
  std::cout << '\n';
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fdinfo")) {
    std::ifstream file(entry.path());
    std::string line;
    while (std::getline(file, line))
      if (line.rfind("drm-client-id:", 0) == 0 || line.rfind("drm-memory-", 0) == 0 ||
          line.rfind("drm-resident-", 0) == 0)
        std::cout << "fdinfo " << phase << " fd=" << entry.path().filename().string()
                  << ' ' << line << '\n';
  }
}
}  // namespace

int main(int argc, char** argv) try {
  const bool retryFailure = argc == 2 && std::strcmp(argv[1], "--retry-map-failure") == 0;
  const bool large = argc == 2 && std::strcmp(argv[1], "--large") == 0;
  require(argc == 1 || retryFailure || large, "unknown test argument");
  if (large) { Bytes = 288 * MiB; Words = Bytes / sizeof(uint64_t); }
  const size_t coldBudget = Bytes / 2 + 2 * MiB;
  const auto hibernate = resolve<Hibernate>("zvramHipHibernate");
  const auto resume = resolve<Resume>("zvramHipResume");
  const auto stats = resolve<ColdBytes>("zvramHipColdBytes");
  check(hipSetDevice(0), "select GPU");
  hipDeviceProp_t properties{};
  check(hipGetDeviceProperties(&properties, 0), "device properties");
  require(!properties.integrated && std::string(properties.gcnArchName).find("gfx1100") == 0,
          "test requires discrete gfx1100");
  expectCold(stats, 0, 0);
  check(resume(), "empty resume");

  Allocation allocation;
  check(hipMalloc(reinterpret_cast<void**>(&allocation.pointer), Bytes), "allocation");
  void* original = allocation.pointer;
  for (unsigned int cycle = 1; cycle <= 2; ++cycle) {
    hipLaunchKernelGGL(fill, dim3((Words + 255) / 256), dim3(256), 0, nullptr,
                       allocation.pointer, cycle, Words);
    check(hipGetLastError(), "fill launch");
    check(hipDeviceSynchronize(), "fill synchronize");
    verify(allocation, cycle);
    if (cycle == 1) {
      require(hibernate(1) == hipErrorOutOfMemory, "undersized cold store must refuse");
      require(hipGetLastError() == hipErrorOutOfMemory, "refusal must expose HIP error");
      expectCold(stats, 0, 0);
      verify(allocation, cycle);
    }
    driverUsage("hot");
    if (cycle == 2)
      require(hipSetDevice(-1) == hipErrorInvalidDevice, "create pending caller error");
    check(hibernate(coldBudget), "hibernate");
    if (cycle == 2)
      require(hipGetLastError() == hipErrorInvalidDevice, "hibernate must preserve caller error");
    driverUsage("cold");
    expectCold(stats, Bytes, coldBudget);
    check(hibernate(coldBudget), "already-cold hibernate");
    if (!retryFailure && cycle == 1) {
      Allocation blocker;
      check(hipMalloc(reinterpret_cast<void**>(&blocker.pointer), Bytes), "reuse released backing cap");
      require(resume() == hipErrorOutOfMemory, "occupied resident cap must refuse resume");
      require(hipGetLastError() == hipErrorOutOfMemory, "resident-cap refusal must expose HIP error");
      expectCold(stats, Bytes, coldBudget);
      check(hipFree(blocker.pointer), "release blocking allocation");
      blocker.pointer = nullptr;
    }
    if (retryFailure && cycle == 1) {
      require(resume() == hipErrorOutOfMemory, "injected remap must fail");
      require(hipGetLastError() == hipErrorOutOfMemory, "failed remap must expose HIP error");
      expectCold(stats, Bytes, coldBudget);
      require(unsetenv("ZVRAM_TEST_VMM_MAP_FAIL_AFTER") == 0, "clear remap fixture");
    }
    check(resume(), "resume");
    driverUsage("restored");
    require(allocation.pointer == original, "GPU pointer identity changed");
    expectCold(stats, 0, 0);
    verify(allocation, cycle);
  }
  check(hibernate(coldBudget), "hibernate before free");
  check(hipFree(allocation.pointer), "free cold allocation");
  allocation.pointer = nullptr;
  expectCold(stats, 0, 0);
  check(resume(), "resume after cold free");
  std::cout << "HIP hibernation PASS: full " << Bytes / MiB << " MiB GPU-mutated data, two cycles, stable pointer, budget refusal, cold free\n";
  if (retryFailure) std::cout << "HIP hibernation partial-remap recovery PASS\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "HIP hibernation FAIL: " << error.what() << '\n';
  return 1;
}
