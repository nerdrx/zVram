#include <hip/hip_runtime.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t MiB = 1024 * 1024;
constexpr size_t Bytes = 32 * MiB;
using ColdBytes = hipError_t (*)(size_t*, size_t*);

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void check(hipError_t status, const char* operation) {
  if (status != hipSuccess)
    throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(status));
}

ColdBytes resolveStats() {
  dlerror();
  auto* address = dlsym(RTLD_DEFAULT, "zvramHipColdBytes");
  const char* error = dlerror();
  require(address && !error, "zVram cold telemetry API is unavailable");
  return reinterpret_cast<ColdBytes>(address);
}

size_t cold(ColdBytes stats) {
  size_t logical = 0, stored = 0;
  check(stats(&logical, &stored), "cold telemetry");
  require(logical == 0 || stored != 0, "cold telemetry has no stored payload");
  return logical;
}

void waitUntilCold(ColdBytes stats) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    const size_t logical = cold(stats);
    if (logical == Bytes) {
      std::cout << "automatic snapshot observed: " << logical << " bytes cold\n";
      return;
    }
    require(logical == 0, "unexpected partial cold allocation");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("automatic idle snapshot was not observed within 5 seconds");
}

struct Resources {
  void* data = nullptr;
  hipStream_t stream = nullptr;
  hipGraph_t graph = nullptr;
  hipGraphExec_t executable = nullptr;

  ~Resources() {
    if (executable) (void)hipGraphExecDestroy(executable);
    if (graph) (void)hipGraphDestroy(graph);
    if (stream) (void)hipStreamDestroy(stream);
    if (data) (void)hipFree(data);
  }
};

void verifyBytes(void* data, unsigned char expected) {
  std::vector<unsigned char> bytes(Bytes);
  check(hipMemcpy(bytes.data(), data, Bytes, hipMemcpyDeviceToHost),
        "full native HIP readback");
  for (size_t i = 0; i < Bytes; ++i)
    if (bytes[i] != expected)
      throw std::runtime_error("readback mismatch at byte " + std::to_string(i));
}

void callback(void* data) {
  static_cast<std::atomic<unsigned int>*>(data)->fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

int main(int argc, char** argv) try {
  const bool callbackMode = argc == 2 && std::strcmp(argv[1], "--callback") == 0;
  require(argc == 1 || callbackMode, "usage: hip-auto-guard-check [--callback]");
  const auto stats = resolveStats();
  check(hipSetDevice(0), "select GPU");
  hipDeviceProp_t properties{};
  check(hipGetDeviceProperties(&properties, 0), "device properties");
  require(!properties.integrated && std::strncmp(properties.gcnArchName, "gfx1100", 7) == 0,
          "automatic guard check requires discrete gfx1100");
  require(cold(stats) == 0, "zVram started with cold memory");

  Resources resources;
  check(hipMalloc(&resources.data, Bytes), "allocate 32 MiB");
  check(hipMemset(resources.data, 0x31, Bytes), "initialize stable bytes");
  check(hipStreamCreateWithFlags(&resources.stream, hipStreamNonBlocking),
        "create nonblocking stream");
  waitUntilCold(stats);

  if (callbackMode) {
    std::atomic<unsigned int> callbackRuns{0};
    check(hipLaunchHostFunc(resources.stream, callback, &callbackRuns),
          "enqueue host callback while cold");
    check(hipStreamSynchronize(resources.stream), "callback stream synchronize");
    require(callbackRuns.load(std::memory_order_relaxed) == 1,
            "host callback did not run exactly once");
    require(cold(stats) == 0, "host callback did not restore the cold allocation");
    verifyBytes(resources.data, 0x31);
    std::cout << "cold hipLaunchHostFunc safely restored and preserved the allocation\n";
  } else {
    check(hipStreamBeginCapture(resources.stream, hipStreamCaptureModeGlobal),
          "begin stream capture while cold");
    check(hipMemsetAsync(resources.data, 0xa7, Bytes, resources.stream),
          "captured memset on cold allocation");
    require(cold(stats) == 0, "captured memset did not restore the cold allocation");
    check(hipStreamEndCapture(resources.stream, &resources.graph), "end stream capture");
    require(resources.graph != nullptr, "capture produced no graph");
    check(hipGraphInstantiate(&resources.executable, resources.graph, nullptr, nullptr, 0),
          "instantiate captured graph");
    check(hipGraphLaunch(resources.executable, resources.stream), "launch captured graph");
    check(hipStreamSynchronize(resources.stream), "captured graph synchronize");
    verifyBytes(resources.data, 0xa7);
    std::cout << "cold captured hipMemsetAsync safely restored and updated the allocation\n";
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  require(cold(stats) == 0, "automatic release remained enabled after unsupported cold operation");
  std::cout << "HIP automatic guard integration PASS: "
            << (callbackMode ? "host callback" : "stream capture")
            << " restored safely and suppressed future auto-release\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "HIP automatic guard integration FAIL: " << error.what() << '\n';
  return 1;
}
