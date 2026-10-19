#include <hip/hip_runtime.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr size_t kBytes = 64u * 1024u * 1024u;
constexpr uint32_t kSeed = 0x9e3779b9u;
constexpr uint32_t kThreads = 256;

__global__ void fill(uint32_t* data, uint32_t count) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < count) data[i] = i ^ kSeed;
}

bool report(hipError_t status, const char* operation) {
  if (status == hipSuccess) return true;
  std::cerr << operation << ": " << hipGetErrorString(status) << " ("
            << static_cast<int>(status) << ")\n";
  return false;
}

bool checkPattern(const uint32_t* data, uint32_t count, const char* label) {
  for (uint32_t i = 0; i < count; ++i) {
    if (data[i] != (i ^ kSeed)) {
      std::cerr << label << ": mismatch at word " << i << ", expected 0x"
                << std::hex << (i ^ kSeed) << ", got 0x" << data[i]
                << std::dec << '\n';
      return false;
    }
  }
  std::cout << label << ": 64 MiB GPU write + full verification passed\n";
  return true;
}

void printAttributes(const char* label, const void* pointer) {
  hipPointerAttribute_t attributes{};
  if (!report(hipPointerGetAttributes(&attributes, pointer), label)) return;
  std::cout << label << ": type=" << static_cast<int>(attributes.type)
            << " device=" << attributes.device
            << " managed=" << attributes.isManaged
            << " allocationFlags=0x" << std::hex << attributes.allocationFlags
            << std::dec << " hostPointer=" << attributes.hostPointer
            << " devicePointer=" << attributes.devicePointer << '\n';
}

bool launchAndWait(uint32_t* devicePointer, hipStream_t stream) {
  const uint32_t count = static_cast<uint32_t>(kBytes / sizeof(uint32_t));
  const dim3 blocks((count + kThreads - 1) / kThreads);
  hipLaunchKernelGGL(fill, blocks, dim3(kThreads), 0, stream, devicePointer, count);
  return report(hipGetLastError(), "fill kernel launch") &&
         report(hipStreamSynchronize(stream), "hipStreamSynchronize");
}

bool probeDeviceAllocation(hipStream_t stream) {
  void* device = nullptr;
  std::vector<uint32_t> host(kBytes / sizeof(uint32_t));
  bool ok = report(hipMalloc(&device, kBytes), "hipMalloc(64 MiB)");
  if (ok) ok = launchAndWait(static_cast<uint32_t*>(device), stream);
  if (ok)
    ok = report(hipMemcpy(host.data(), device, kBytes, hipMemcpyDeviceToHost),
                "hipMemcpy(device to host)");
  if (ok) ok = checkPattern(host.data(), static_cast<uint32_t>(host.size()), "hipMalloc");
  if (device) ok = report(hipFree(device), "hipFree(device)") && ok;
  return ok;
}

bool probeMappedHostAllocation(hipStream_t stream) {
  void* host = nullptr;
  void* device = nullptr;
  bool ok = report(hipHostMalloc(&host, kBytes, hipHostMallocMapped),
                   "hipHostMalloc(64 MiB, Mapped)");
  if (ok) ok = report(hipHostGetDevicePointer(&device, host, 0),
                      "hipHostGetDevicePointer");
  if (ok) {
    printAttributes("mapped host pointer attributes", host);
    printAttributes("mapped device pointer attributes", device);
    ok = launchAndWait(static_cast<uint32_t*>(device), stream);
  }
  if (ok)
    ok = checkPattern(static_cast<const uint32_t*>(host),
                      static_cast<uint32_t>(kBytes / sizeof(uint32_t)),
                      "hipHostMalloc mapped; CPU read after stream sync");
  if (host) ok = report(hipHostFree(host), "hipHostFree(mapped)") && ok;
  return ok;
}

bool probeManagedAllocation(hipStream_t stream) {
  void* managed = nullptr;
  bool ok = report(hipMallocManaged(&managed, kBytes, hipMemAttachGlobal),
                   "hipMallocManaged(64 MiB)");
  if (!ok) return false;
  printAttributes("managed pointer attributes", managed);
  ok = launchAndWait(static_cast<uint32_t*>(managed), stream);
  if (ok)
    ok = checkPattern(static_cast<const uint32_t*>(managed),
                      static_cast<uint32_t>(kBytes / sizeof(uint32_t)),
                      "hipMallocManaged; CPU read after stream sync");
  if (managed) ok = report(hipFree(managed), "hipFree(managed)") && ok;
  return ok;
}

void queryAttribute(int device, hipDeviceAttribute_t attribute, const char* name) {
  int value = 0;
  if (report(hipDeviceGetAttribute(&value, attribute, device), name))
    std::cout << "  " << name << "=" << value << '\n';
}

}  // namespace

int main() {
  int deviceCount = 0;
  if (!report(hipGetDeviceCount(&deviceCount), "hipGetDeviceCount")) return 1;

  int device = -1;
  hipDeviceProp_t properties{};
  for (int candidate = 0; candidate < deviceCount; ++candidate) {
    hipDeviceProp_t candidateProperties{};
    if (!report(hipGetDeviceProperties(&candidateProperties, candidate),
                "hipGetDeviceProperties"))
      continue;
    if (std::string(candidateProperties.gcnArchName).find("gfx1100") == 0 &&
        candidateProperties.integrated == 0) {
      device = candidate;
      properties = candidateProperties;
      break;
    }
  }
  if (device < 0) {
    std::cerr << "No discrete gfx1100 device found; refusing to run elsewhere.\n";
    return 1;
  }
  if (!report(hipSetDevice(device), "hipSetDevice(gfx1100)")) return 1;

  std::cout << "Device " << device << ": " << properties.name << " ("
            << properties.gcnArchName << ")\n"
            << "Each probe uses at most 64 MiB; native hipMalloc verification "
               "adds a 64 MiB host readback buffer (128 MiB maximum for that "
               "step). Probes run sequentially.\n";

  std::cout << "Device capabilities:\n";
  queryAttribute(device, hipDeviceAttributeManagedMemory, "managedMemory");
  queryAttribute(device, hipDeviceAttributeConcurrentManagedAccess,
                 "concurrentManagedAccess");
  queryAttribute(device, hipDeviceAttributePageableMemoryAccess,
                 "pageableMemoryAccess");
  queryAttribute(device, hipDeviceAttributePageableMemoryAccessUsesHostPageTables,
                 "pageableMemoryAccessUsesHostPageTables");
  queryAttribute(device, hipDeviceAttributeCanMapHostMemory, "canMapHostMemory");

  hipStream_t stream = nullptr;
  if (!report(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking),
              "hipStreamCreateWithFlags"))
    return 1;

  bool allPassed = probeDeviceAllocation(stream);
  allPassed = probeMappedHostAllocation(stream) && allPassed;
  allPassed = probeManagedAllocation(stream) && allPassed;

  const bool destroyed = report(hipStreamDestroy(stream), "hipStreamDestroy");
  if (!allPassed || !destroyed) return 1;
  std::cout << "All supported 64 MiB probes passed. This tests mapping and "
               "data integrity only; it does not demonstrate VRAM "
               "oversubscription or compression.\n";
  return 0;
}
