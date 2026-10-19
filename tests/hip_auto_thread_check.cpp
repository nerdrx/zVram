#include <hip/hip_runtime.h>
#include <dlfcn.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t MiB = 1024 * 1024;
constexpr size_t Bytes = 32 * MiB;
constexpr size_t Words = Bytes / sizeof(uint64_t);
constexpr size_t HalfWords = Words / 2;
constexpr unsigned int WorkerCount = 2;
constexpr unsigned int CycleCount = 2;

using ColdBytes = hipError_t (*)(size_t*, size_t*);

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void check(hipError_t status, const char* operation) {
  if (status != hipSuccess)
    throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(status));
}

__host__ __device__ uint64_t pattern(size_t index, unsigned int worker,
                                     unsigned int cycle) {
  if (index < HalfWords)
    return UINT64_C(0x123400005a5a0000) + worker * 17u + cycle;
  uint64_t value = index + (worker + 1u) * UINT64_C(0x9e3779b97f4a7c15) +
                   cycle * UINT64_C(0xd1b54a32d192ed03);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

__global__ void fill(uint64_t* data, unsigned int worker, unsigned int cycle) {
  const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index < Words) data[index] = pattern(index, worker, cycle);
}

__global__ void verify(const uint64_t* data, unsigned int worker,
                       unsigned int cycle, unsigned int* mismatch) {
  const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index < Words && data[index] != pattern(index, worker, cycle))
    atomicMin(mismatch, static_cast<unsigned int>(index));
}

struct Coordinator {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned int ready = 0;
  unsigned int done = 0;
  unsigned int releaseCycle = 0;
  bool stop = false;
  std::exception_ptr error;
};

struct WorkerResources {
  uint64_t* data = nullptr;
  unsigned int* mismatch = nullptr;
  hipStream_t stream = nullptr;

  ~WorkerResources() {
    if (stream) {
      (void)hipStreamSynchronize(stream);
      (void)hipStreamDestroy(stream);
    }
    if (data) (void)hipFree(data);
    if (mismatch) (void)hipHostFree(mismatch);
  }
};

void setFailure(Coordinator& coordinator, std::exception_ptr error) {
  {
    std::lock_guard<std::mutex> lock(coordinator.mutex);
    if (!coordinator.error) coordinator.error = error;
    coordinator.stop = true;
    coordinator.releaseCycle = CycleCount;
  }
  coordinator.changed.notify_all();
}

void runWorker(Coordinator& coordinator, unsigned int worker) {
  try {
    check(hipSetDevice(0), "worker select GPU");
    WorkerResources resources;
    check(hipStreamCreateWithFlags(&resources.stream, hipStreamNonBlocking),
          "create nonblocking stream");
    check(hipMalloc(reinterpret_cast<void**>(&resources.data), Bytes),
          "allocate worker 32 MiB VMM buffer");
    check(hipHostMalloc(reinterpret_cast<void**>(&resources.mismatch),
                        sizeof(*resources.mismatch)),
          "allocate mapped verification status");
    void* mismatchAlias = nullptr;
    check(hipHostGetDevicePointer(&mismatchAlias, resources.mismatch, 0),
          "map verification status for GPU");

    for (unsigned int cycle = 1; cycle <= CycleCount; ++cycle) {
      hipLaunchKernelGGL(fill, dim3((Words + 255) / 256), dim3(256), 0,
                         resources.stream, resources.data, worker, cycle);
      check(hipGetLastError(), "fill kernel launch");
      check(hipStreamSynchronize(resources.stream), "fill stream synchronize");
      {
        std::lock_guard<std::mutex> lock(coordinator.mutex);
        ++coordinator.ready;
      }
      coordinator.changed.notify_all();

      {
        std::unique_lock<std::mutex> lock(coordinator.mutex);
        coordinator.changed.wait(lock, [&] {
          return coordinator.stop || coordinator.releaseCycle >= cycle;
        });
        if (coordinator.stop) return;
      }

      *resources.mismatch = UINT32_MAX;
      hipLaunchKernelGGL(verify, dim3((Words + 255) / 256), dim3(256), 0,
                         resources.stream, resources.data, worker, cycle,
                         static_cast<unsigned int*>(mismatchAlias));
      check(hipGetLastError(), "verify kernel launch");
      check(hipStreamSynchronize(resources.stream), "verify stream synchronize");
      require(*resources.mismatch == UINT32_MAX,
              "GPU verify mismatch worker=" + std::to_string(worker) +
                  " byte=" + std::to_string(
                      static_cast<size_t>(*resources.mismatch) * sizeof(uint64_t)));

      std::vector<uint64_t> output(Words);
      check(hipMemcpy(output.data(), resources.data, Bytes, hipMemcpyDeviceToHost),
            "full native HIP readback");
      for (size_t i = 0; i < Words; ++i)
        if (output[i] != pattern(i, worker, cycle))
          throw std::runtime_error("readback mismatch worker=" + std::to_string(worker) +
                                   " byte=" + std::to_string(i * sizeof(uint64_t)));
      {
        std::lock_guard<std::mutex> lock(coordinator.mutex);
        ++coordinator.done;
      }
      coordinator.changed.notify_all();
    }
  } catch (...) {
    setFailure(coordinator, std::current_exception());
  }
}

size_t coldBytes(ColdBytes stats) {
  size_t logical = 0, stored = 0;
  check(stats(&logical, &stored), "cold telemetry");
  require(logical == 0 || stored != 0, "cold telemetry has no stored payload");
  return logical;
}

void waitForCount(Coordinator& coordinator, unsigned int& count,
                  unsigned int expected, const char* description) {
  std::unique_lock<std::mutex> lock(coordinator.mutex);
  require(coordinator.changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return count >= expected || coordinator.stop;
          }), std::string("timed out waiting for ") + description);
  if (coordinator.error) std::rethrow_exception(coordinator.error);
  require(!coordinator.stop, std::string("workers stopped while waiting for ") + description);
}

void waitUntilBothCold(Coordinator& coordinator, ColdBytes stats, unsigned int cycle) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const size_t expected = WorkerCount * Bytes;
  do {
    {
      std::lock_guard<std::mutex> lock(coordinator.mutex);
      if (coordinator.error) std::rethrow_exception(coordinator.error);
      require(!coordinator.stop, "workers stopped during cold wait");
    }
    const size_t logical = coldBytes(stats);
    if (logical == expected) {
      std::cout << "cycle " << cycle << " both worker allocations cold=" << logical << '\n';
      return;
    }
    require(logical == 0 || logical == Bytes,
            "unexpected logical cold bytes while waiting for both workers");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("both 32 MiB allocations did not become cold within 5 seconds");
}

}  // namespace

int main() try {
  auto* rawStats = dlsym(RTLD_DEFAULT, "zvramHipColdBytes");
  require(rawStats != nullptr, "zVram cold telemetry API is unavailable");
  const auto stats = reinterpret_cast<ColdBytes>(rawStats);
  check(hipSetDevice(0), "select GPU");
  hipDeviceProp_t properties{};
  check(hipGetDeviceProperties(&properties, 0), "device properties");
  require(!properties.integrated && std::strncmp(properties.gcnArchName, "gfx1100", 7) == 0,
          "automatic concurrency check requires discrete gfx1100");
  require(coldBytes(stats) == 0, "zVram started with cold allocations");

  Coordinator coordinator;
  std::array<std::thread, WorkerCount> workers;
  for (unsigned int worker = 0; worker < WorkerCount; ++worker)
    workers[worker] = std::thread(runWorker, std::ref(coordinator), worker);

  std::exception_ptr mainError;
  try {
    for (unsigned int cycle = 1; cycle <= CycleCount; ++cycle) {
      waitForCount(coordinator, coordinator.ready, cycle * WorkerCount,
                   "both workers to finish filling");
      waitUntilBothCold(coordinator, stats, cycle);
      {
        std::lock_guard<std::mutex> lock(coordinator.mutex);
        coordinator.releaseCycle = cycle;
      }
      coordinator.changed.notify_all();
      waitForCount(coordinator, coordinator.done, cycle * WorkerCount,
                   "both workers to verify and read back");
      std::cout << "cycle " << cycle << " concurrent GPU verification passed\n";
    }
  } catch (...) {
    mainError = std::current_exception();
    setFailure(coordinator, mainError);
  }

  for (auto& worker : workers)
    if (worker.joinable()) worker.join();
  if (mainError) std::rethrow_exception(mainError);
  if (coordinator.error) std::rethrow_exception(coordinator.error);
  require(coldBytes(stats) == 0, "worker cleanup left cold bytes behind");
  std::cout << "HIP automatic thread integration PASS: 2 threads x 2 cycles, "
               "64 MiB autosnapshot budget, concurrent GPU and full readback\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "HIP automatic thread integration FAIL: " << error.what() << '\n';
  return 1;
}
