#include <hip/hip_runtime.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t MiB = 1024 * 1024;
constexpr size_t Bytes = 32 * MiB;
constexpr size_t Words = Bytes / sizeof(uint64_t);
constexpr size_t HalfWords = Words / 2;
constexpr int ProcVersion = HIP_VERSION_MAJOR * 100 + HIP_VERSION_MINOR;

using ColdBytes = hipError_t (*)(size_t*, size_t*);
using DeviceSynchronize = hipError_t (*)();
using GetProcAddress = hipError_t (*)(const char*, void**, int, uint64_t,
                                      hipDriverProcAddressQueryResult*);

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void check(hipError_t status, const char* operation) {
  if (status != hipSuccess)
    throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(status));
}

template <typename Function>
Function symbol(void* handle, const char* name) {
  dlerror();
  auto* result = dlsym(handle, name);
  const char* error = dlerror();
  require(!error && result, std::string("cannot resolve ") + name +
                               (error ? std::string(": ") + error : ""));
  return reinterpret_cast<Function>(result);
}

__host__ __device__ uint64_t pattern(size_t index, unsigned int cycle) {
  if (index < HalfWords) return UINT64_C(0x5a5a1234cafe0000) + cycle;
  uint64_t value = index + cycle * UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

__global__ void fill(uint64_t* data, unsigned int cycle) {
  const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index < Words) data[index] = pattern(index, cycle);
}

__global__ void verify(const uint64_t* data, unsigned int cycle,
                       unsigned int* mismatch) {
  const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index < Words && data[index] != pattern(index, cycle))
    atomicMin(mismatch, static_cast<unsigned int>(index));
}

struct DeviceAllocation {
  uint64_t* pointer = nullptr;
  ~DeviceAllocation() { if (pointer) (void)hipFree(pointer); }
};

size_t cold(ColdBytes stats) {
  size_t logical = 0, stored = 0;
  check(stats(&logical, &stored), "cold stats");
  if (logical && !stored) throw std::runtime_error("cold allocation has no stored payload");
  return logical;
}

void waitUntilCold(ColdBytes stats, unsigned int cycle) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    const size_t bytes = cold(stats);
    if (bytes == Bytes) {
      std::cout << "cycle " << cycle << " autosnapshot cold=" << bytes << '\n';
      return;
    }
    if (bytes != 0) throw std::runtime_error("unexpected partial cold allocation");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("automatic idle snapshot was not observed within 5 seconds");
}

void fillCycle(uint64_t* pointer, unsigned int cycle) {
  hipLaunchKernelGGL(fill, dim3((Words + 255) / 256), dim3(256), 0, nullptr,
                     pointer, cycle);
  check(hipGetLastError(), "fill kernel launch");
  check(hipDeviceSynchronize(), "fill kernel synchronize");
}

void verifyCycle(uint64_t* pointer, unsigned int cycle, unsigned int* mismatch,
                 unsigned int* mismatchDevice) {
  *mismatch = UINT32_MAX;
  hipLaunchKernelGGL(verify, dim3((Words + 255) / 256), dim3(256), 0, nullptr,
                     pointer, cycle, mismatchDevice);
  check(hipGetLastError(), "verify kernel launch");
  check(hipDeviceSynchronize(), "verify kernel synchronize");
  require(*mismatch == UINT32_MAX,
          "GPU kernel found mismatch at byte " +
              std::to_string(static_cast<size_t>(*mismatch) * sizeof(uint64_t)));

  std::vector<uint64_t> output(Words);
  check(hipMemcpy(output.data(), pointer, Bytes, hipMemcpyDeviceToHost),
        "full native HIP readback");
  for (size_t i = 0; i < Words; ++i)
    if (output[i] != pattern(i, cycle))
      throw std::runtime_error("full HIP readback mismatch at byte " +
                               std::to_string(i * sizeof(uint64_t)));
}

void metadataWhileCold(ColdBytes stats) {
  hipDeviceProp_t properties{};
  check(hipGetDeviceProperties(&properties, 0), "metadata device properties");
  size_t freeBytes = 0, totalBytes = 0;
  check(hipMemGetInfo(&freeBytes, &totalBytes), "metadata memory info");
  require(cold(stats) == Bytes, "metadata query resumed the cold allocation");
  std::cout << "cold metadata device=" << properties.name << " free=" << freeBytes
            << " total=" << totalBytes << "\n";
}

}  // namespace

int main() try {
  const auto stats = symbol<ColdBytes>(RTLD_DEFAULT, "zvramHipColdBytes");
  check(hipSetDevice(0), "select GPU");
  hipDeviceProp_t properties{};
  check(hipGetDeviceProperties(&properties, 0), "device properties");
  require(!properties.integrated && std::strncmp(properties.gcnArchName, "gfx1100", 7) == 0,
          "automatic idle check requires discrete gfx1100");
  require(cold(stats) == 0, "zVram started with a cold allocation");

  void* runtime = dlopen("libamdhip64.so", RTLD_NOW | RTLD_LOCAL);
  if (!runtime) {
    const char* error = dlerror();
    throw std::runtime_error(std::string("cannot open native HIP runtime: ") +
                             (error ? error : "unknown loader error"));
  }
  struct RuntimeHandle {
    void* handle;
    ~RuntimeHandle() { if (handle) dlclose(handle); }
  } runtimeHandle{runtime};
  const auto nativeSync = symbol<DeviceSynchronize>(runtime, "hipDeviceSynchronize");

  void* resolvedPointer = nullptr;
  auto query = HIP_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
  check(hipGetProcAddress("hipDeviceSynchronize", &resolvedPointer, ProcVersion, 0, &query),
        "resolve hipDeviceSynchronize");
  require(resolvedPointer && query == HIP_GET_PROC_ADDRESS_SUCCESS,
          "hipGetProcAddress did not resolve hipDeviceSynchronize");
  const auto resolvedSync = reinterpret_cast<DeviceSynchronize>(resolvedPointer);

  DeviceAllocation allocation;
  check(hipMalloc(reinterpret_cast<void**>(&allocation.pointer), Bytes), "allocate 32 MiB");
  unsigned int* mismatch = nullptr;
  check(hipHostMalloc(reinterpret_cast<void**>(&mismatch), sizeof(*mismatch)),
        "allocate GPU verification status");
  struct HostAllocation {
    unsigned int* pointer;
    ~HostAllocation() { if (pointer) (void)hipHostFree(pointer); }
  } hostAllocation{mismatch};
  void* mismatchAlias = nullptr;
  check(hipHostGetDevicePointer(&mismatchAlias, mismatch, 0), "map GPU verification status");

  for (unsigned int cycle = 1; cycle <= 4; ++cycle) {
    fillCycle(allocation.pointer, cycle);
    verifyCycle(allocation.pointer, cycle, mismatch,
                static_cast<unsigned int*>(mismatchAlias));
    require(cold(stats) == 0, "allocation became cold before the idle wait");
    waitUntilCold(stats, cycle);
    metadataWhileCold(stats);

    if (cycle == 4) {
      // No HIP synchronization precedes this ordinary launch; it must wake the cold pointer.
      verifyCycle(allocation.pointer, cycle, mismatch,
                  static_cast<unsigned int*>(mismatchAlias));
      require(cold(stats) == 0, "ordinary GPU launch did not restore the allocation");
      std::cout << "cycle 4 restore route=ordinary GPU launch verified\n";
      continue;
    }

    const char* route = nullptr;
    if (cycle == 1) {
      route = "direct hipDeviceSynchronize";
      check(hipDeviceSynchronize(), route);
    } else if (cycle == 2) {
      route = "hipGetProcAddress hipDeviceSynchronize";
      check(resolvedSync(), route);
    } else {
      route = "native-library dlsym hipDeviceSynchronize";
      check(nativeSync(), route);
    }
    require(cold(stats) == 0, std::string(route) + " did not restore the allocation");
    verifyCycle(allocation.pointer, cycle, mismatch,
                static_cast<unsigned int*>(mismatchAlias));
    std::cout << "cycle " << cycle << " restore route=" << route << " verified\n";
  }

  waitUntilCold(stats, 5);
  check(hipFree(allocation.pointer), "free restored allocation");
  allocation.pointer = nullptr;
  require(cold(stats) == 0, "free left cold bytes behind");
  check(hipHostFree(mismatch), "free GPU verification status");
  hostAllocation.pointer = nullptr;
  std::cout << "HIP automatic idle integration PASS: 4 restores, final cold free, "
               "metadata while cold, GPU and full 32 MiB verification\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "HIP automatic idle integration FAIL: " << error.what() << '\n';
  return 1;
}
