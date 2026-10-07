#include <hip/hip_runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kChunkBytes = 8u * 1024u * 1024u;
constexpr uint32_t kThreads = 256;

__host__ __device__ uint64_t splitmix64(uint64_t value) {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

__global__ void fill(uint64_t* data, uint64_t words, uint64_t baseWord) {
  const uint64_t index = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index < words) data[index] = splitmix64(baseWord + index);
}

bool report(hipError_t status, const char* operation) {
  if (status == hipSuccess) return true;
  std::cerr << operation << ": " << hipGetErrorString(status) << " ("
            << static_cast<int>(status) << ")\n";
  return false;
}

bool capacity(const char* phase, size_t* freeBytes, size_t* totalBytes) {
  if (!report(hipMemGetInfo(freeBytes, totalBytes), "hipMemGetInfo")) return false;
  std::cout << "HIP capacity " << phase << ": total=" << *totalBytes
            << " free=" << *freeBytes << std::endl;
  return true;
}

bool findGpu(int* selected, hipDeviceProp_t* properties) {
  int count = 0;
  if (!report(hipGetDeviceCount(&count), "hipGetDeviceCount")) return false;
  for (int device = 0; device < count; ++device) {
    hipDeviceProp_t candidate{};
    if (!report(hipGetDeviceProperties(&candidate, device), "hipGetDeviceProperties"))
      continue;
    if (candidate.integrated == 0 &&
        std::string(candidate.gcnArchName).find("gfx1100") == 0) {
      *selected = device;
      *properties = candidate;
      return true;
    }
  }
  std::cerr << "No discrete gfx1100 device found; refusing to run elsewhere.\n";
  return false;
}

bool checkSmallFreePaths(int device) {
  int deviceCount = 0;
  if (!report(hipGetDeviceCount(&deviceCount), "hipGetDeviceCount")) return false;
  bool ok = true;
  for (int cycle = 0; cycle < 2; ++cycle) {
    void* pointer = nullptr;
    if (!report(hipMalloc(&pointer, 4096), "small hipMalloc")) return false;
    hipPointerAttribute_t attributes{};
    if (report(hipPointerGetAttributes(&attributes, pointer),
               "small hipPointerGetAttributes")) {
      std::cout << "small allocation cycle " << cycle + 1
                << ": pointer type=" << static_cast<int>(attributes.type)
                << " managed=" << attributes.isManaged
                << " hostPointer=" << attributes.hostPointer
                << " devicePointer=" << attributes.devicePointer << '\n';
    } else {
      ok = false;
    }
    const char* vmm = std::getenv("ZVRAM_HIP_VMM");
    const char* localLimit = std::getenv("ZVRAM_HIP_LOCAL_LIMIT_MIB");
    const bool expectAllHostVmm = vmm && std::string(vmm) == "1" && localLimit &&
                                  std::string(localLimit) == "0";
    if (attributes.type == hipMemoryTypeHost || expectAllHostVmm) {
      const hipError_t asyncFree = hipFreeAsync(pointer, nullptr);
      if (asyncFree != hipErrorNotSupported) {
        std::cerr << "Mapped-host hipFreeAsync guard returned "
                  << hipGetErrorString(asyncFree) << ", expected hipErrorNotSupported\n";
        ok = false;
      } else {
        std::cout << "mapped-host hipFreeAsync correctly rejected; pointer remains "
                     "available for hipFree\n";
      }
    }

    int freeDevice = device;
    if (cycle == 1 && deviceCount > 1) freeDevice = (device + 1) % deviceCount;
    if (freeDevice != device) {
      const hipError_t switched = hipSetDevice(freeDevice);
      if (switched != hipSuccess) {
        std::cerr << "Cross-device free check skipped: "
                  << hipGetErrorString(switched) << '\n';
        if (!report(hipFree(pointer), "small hipFree")) ok = false;
        continue;
      }
    }

    if (!report(hipFree(pointer), "small hipFree")) ok = false;
    int current = -1;
    if (!report(hipGetDevice(&current), "hipGetDevice after free")) {
      ok = false;
    } else if (current != freeDevice) {
      std::cerr << "Free changed current device from " << freeDevice << " to "
                << current << '\n';
      ok = false;
    }
    if (freeDevice != device && !report(hipSetDevice(device), "restore test device"))
      ok = false;
  }
  if (deviceCount <= 1)
    std::cout << "Cross-device free check skipped: HIP exposes one GPU.\n";
  return ok;
}

bool parseArgs(int argc, char** argv, size_t* mib, bool* singleAllocation,
               bool* concurrentOnly) {
  *mib = 32;
  *singleAllocation = false;
  *concurrentOnly = false;
  bool sawMib = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--single-allocation") {
      *singleAllocation = true;
    } else if (arg == "--concurrent-only") {
      *concurrentOnly = true;
    } else if (arg == "--mib" && i + 1 < argc && !sawMib) {
      char* end = nullptr;
      const unsigned long long value = std::strtoull(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || value == 0 || value > 40960) {
        std::cerr << "--mib must be between 1 and 40960\n";
        return false;
      }
      *mib = static_cast<size_t>(value);
      sawMib = true;
    } else {
      std::cerr << "Usage: hip-check [--mib 1..40960] [--single-allocation] | --concurrent-only\n";
      return false;
    }
  }
  if (*concurrentOnly && (sawMib || *singleAllocation)) {
    std::cerr << "--concurrent-only cannot be combined with workload options\n";
    return false;
  }
  return true;
}

uint64_t expected(uint64_t word) { return splitmix64(word); }

struct Chunk {
  uint64_t* pointer = nullptr;
  size_t bytes = 0;
  uint64_t baseWord = 0;
};

class Barrier {
 public:
  explicit Barrier(size_t participants) : participants_(participants) {}

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    const size_t generation = generation_;
    if (++arrived_ == participants_) {
      arrived_ = 0;
      ++generation_;
      condition_.notify_all();
    } else {
      condition_.wait(lock, [&] { return generation_ != generation; });
    }
  }

 private:
  const size_t participants_;
  size_t arrived_ = 0;
  size_t generation_ = 0;
  std::mutex mutex_;
  std::condition_variable condition_;
};

bool concurrentCheck(int device) {
  constexpr size_t kWorkers = 4;
  constexpr size_t kIterations = 8;
  constexpr size_t kAllocationsPerWorker = 9;
  constexpr size_t kAllocationBytes = 32u * 1024u;
  Barrier barrier(kWorkers);
  std::atomic<bool> ok{true};
  std::atomic<size_t> mappedHostCount{0};
  std::atomic<size_t> nativeCount{0};
  std::array<std::thread, kWorkers> workers;

  for (size_t worker = 0; worker < kWorkers; ++worker) {
    workers[worker] = std::thread([&, worker] {
      (void)worker;
      if (!report(hipSetDevice(device), "thread hipSetDevice")) ok = false;
      for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        std::array<void*, kAllocationsPerWorker> pointers{};
        size_t count = 0;
        for (; count < pointers.size(); ++count) {
          if (hipMalloc(&pointers[count], kAllocationBytes) != hipSuccess) {
            ok = false;
            break;
          }
          hipPointerAttribute_t attributes{};
          if (hipPointerGetAttributes(&attributes, pointers[count]) != hipSuccess) {
            ok = false;
          } else if (attributes.type == hipMemoryTypeHost) {
            ++mappedHostCount;
          } else if (attributes.type == hipMemoryTypeDevice) {
            ++nativeCount;
          }
        }

        // Hold this round's allocations until every worker reaches the cap.
        barrier.wait();
        while (count > 0) {
          --count;
          if (!report(hipFree(pointers[count]), "thread hipFree")) ok = false;
        }
        barrier.wait();
      }
    });
  }
  for (auto& worker : workers) worker.join();

  std::cout << "Concurrent check: 4 threads x " << kIterations
            << " rounds x 9 live 32 KiB allocations; observed native="
            << nativeCount.load() << " mapped-host=" << mappedHostCount.load()
            << " (no GPU kernels launched).\n";
  const char* localLimit = std::getenv("ZVRAM_HIP_LOCAL_LIMIT_MIB");
  if (localLimit && std::string(localLimit) == "1" && mappedHostCount == 0) {
    std::cerr << "1 MiB local cap was set but no mapped-host allocation was observed.\n";
    ok = false;
  }
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  size_t mib = 0;
  bool singleAllocation = false, concurrentOnly = false;
  if (!parseArgs(argc, argv, &mib, &singleAllocation, &concurrentOnly)) return 2;

  int device = -1;
  hipDeviceProp_t properties{};
  if (!findGpu(&device, &properties) ||
      !report(hipSetDevice(device), "hipSetDevice(gfx1100)"))
    return 1;
  std::cout << "Device " << device << ": " << properties.name << " ("
            << properties.gcnArchName << ")\n"
            << (concurrentOnly
                    ? "Running allocation-only concurrency check.\n"
                    : "Workload=" + std::to_string(mib) +
                          " MiB; " +
                          (singleAllocation
                               ? "one live allocation, checked in 8 MiB chunks.\n"
                               : "8 MiB allocations, all live through verification.\n"));
  if (concurrentOnly) return concurrentCheck(device) ? 0 : 1;
  if (!checkSmallFreePaths(device)) return 1;
  size_t beforeFree = 0, beforeTotal = 0;
  if (!capacity("before", &beforeFree, &beforeTotal)) return 1;

  const size_t totalBytes = mib * 1024u * 1024u;
  const uint64_t totalWords = totalBytes / sizeof(uint64_t);
  std::vector<Chunk> chunks;
  std::vector<uint64_t> host(kChunkBytes / sizeof(uint64_t));
  chunks.reserve((totalBytes + kChunkBytes - 1) / kChunkBytes);

  bool ok = true;
  size_t allocatedBytes = 0;
  uint64_t* singlePointer = nullptr;
  if (singleAllocation) {
    if (!report(hipMalloc(reinterpret_cast<void**>(&singlePointer), totalBytes),
                "single-allocation hipMalloc")) {
      ok = false;
    } else {
      hipPointerAttribute_t attributes{};
      if (report(hipPointerGetAttributes(&attributes, singlePointer),
                 "single-allocation hipPointerGetAttributes")) {
        std::cout << "single allocation pointer attributes: type="
                  << static_cast<int>(attributes.type)
                  << " device=" << attributes.device
                  << " managed=" << attributes.isManaged
                  << " hostPointer=" << attributes.hostPointer
                  << " devicePointer=" << attributes.devicePointer << '\n';
      } else {
        ok = false;
      }
      while (allocatedBytes < totalBytes) {
        const size_t bytes = std::min(kChunkBytes, totalBytes - allocatedBytes);
        chunks.push_back({singlePointer + allocatedBytes / sizeof(uint64_t), bytes,
                          allocatedBytes / sizeof(uint64_t)});
        allocatedBytes += bytes;
      }
    }
  } else {
    while (allocatedBytes < totalBytes) {
      const size_t bytes = std::min(kChunkBytes, totalBytes - allocatedBytes);
      void* pointer = nullptr;
      if (!report(hipMalloc(&pointer, bytes), "workload hipMalloc")) {
        ok = false;
        break;
      }
      chunks.push_back({static_cast<uint64_t*>(pointer), bytes,
                        allocatedBytes / sizeof(uint64_t)});
      allocatedBytes += bytes;
    }
  }

  size_t duringFree = 0, duringTotal = 0;
  if (ok) ok = capacity("allocated", &duringFree, &duringTotal);
  const char* reporting = std::getenv("ZVRAM_HIP_REPORT_CAPACITY");
  const bool logicalCapacity = reporting && std::string(reporting) == "1";
  if (ok && logicalCapacity &&
      (duringTotal != beforeTotal || duringFree > beforeFree ||
       beforeFree - duringFree < totalBytes)) {
    std::cerr << "Reported logical capacity did not account for live allocation.\n";
    ok = false;
  }
  hipStream_t stream = nullptr;
  if (ok) ok = report(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking),
                      "hipStreamCreateWithFlags");
  if (ok) {
    for (const Chunk& chunk : chunks) {
      const uint64_t words = chunk.bytes / sizeof(uint64_t);
      const dim3 blocks(static_cast<unsigned int>((words + kThreads - 1) / kThreads));
      hipLaunchKernelGGL(fill, blocks, dim3(kThreads), 0, stream, chunk.pointer,
                         words, chunk.baseWord);
      if (!report(hipGetLastError(), "fill kernel launch")) {
        ok = false;
        break;
      }
    }
    if (ok) ok = report(hipStreamSynchronize(stream), "hipStreamSynchronize");
  }

  if (ok) {
    for (const Chunk& chunk : chunks) {
      const size_t words = chunk.bytes / sizeof(uint64_t);
      if (!report(hipMemcpy(host.data(), chunk.pointer, chunk.bytes,
                            hipMemcpyDeviceToHost),
                  "hipMemcpy(device to host)")) {
        ok = false;
        break;
      }
      for (size_t i = 0; i < words; ++i) {
        if (host[i] != expected(chunk.baseWord + i)) {
          std::cerr << "Data mismatch at global word " << chunk.baseWord + i
                    << ", expected 0x" << std::hex << expected(chunk.baseWord + i)
                    << ", got 0x" << host[i] << std::dec << '\n';
          ok = false;
          break;
        }
      }
      if (!ok) break;
    }
  }
  if (ok)
    std::cout << "GPU wrote and CPU verified all " << totalWords
              << " words across "
              << (singleAllocation
                      ? "one live allocation checked in " + std::to_string(chunks.size()) + " chunks.\n"
                      : std::to_string(chunks.size()) + " live HIP allocations.\n");

  if (singlePointer) {
    if (!report(hipFree(singlePointer), "single-allocation hipFree")) ok = false;
  } else {
    for (Chunk& chunk : chunks)
      if (chunk.pointer && !report(hipFree(chunk.pointer), "workload hipFree")) ok = false;
  }
  if (stream && !report(hipStreamDestroy(stream), "hipStreamDestroy")) ok = false;
  size_t afterFree = 0, afterTotal = 0;
  if (!capacity("freed", &afterFree, &afterTotal)) ok = false;
  if (logicalCapacity && (afterTotal != beforeTotal || afterFree < beforeFree)) {
    std::cerr << "Reported logical capacity did not recover after free.\n";
    ok = false;
  }
  std::cout << "This validates HIP pointer usability and data integrity only; it "
               "does not imply compression or transparent paging.\n";
  return ok ? 0 : 1;
}
