#include <hip/hip_runtime_api.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t MiB = 1024u * 1024u;
constexpr int ProcVersion = HIP_VERSION_MAJOR * 100 + HIP_VERSION_MINOR;
using ErrorQuery = hipError_t (*)();
using FreeAsync = hipError_t (*)(void*, hipStream_t);

void require(bool okay, const std::string& what) {
  if (!okay) throw std::runtime_error(what);
}
void expect(hipError_t got, hipError_t want, const char* what) {
  if (got != want)
    throw std::runtime_error(std::string(what) + ": got " + hipGetErrorString(got) +
                             ", expected " + hipGetErrorString(want));
}
void check(hipError_t status, const char* what) { expect(status, hipSuccess, what); }

template <class Function>
Function resolve(const char* symbol) {
  void* address = nullptr;
  hipDriverProcAddressQueryResult query = HIP_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
  check(hipGetProcAddress(symbol, &address, ProcVersion, 0, &query), symbol);
  require(address && query == HIP_GET_PROC_ADDRESS_SUCCESS,
          std::string("resolver missed ") + symbol);
  return reinterpret_cast<Function>(address);
}

struct Allocation {
  void* ptr = nullptr;
  ~Allocation() { if (ptr) (void)hipFree(ptr); }
};

void checkDevice() {
  int count = 0;
  check(hipGetDeviceCount(&count), "hipGetDeviceCount");
  require(count == 1, "expected HIP_VISIBLE_DEVICES=0 to expose one GPU");
  check(hipSetDevice(0), "hipSetDevice(0)");
  hipDeviceProp_t prop{};
  check(hipGetDeviceProperties(&prop, 0), "hipGetDeviceProperties");
  require(prop.integrated == 0 && std::strncmp(prop.gcnArchName, "gfx1100", 7) == 0,
          "expected discrete gfx1100");
}

void checkCapacity() {
  size_t freeBytes = 0, totalBytes = 0, deviceBytes = 0;
  check(hipMemGetInfo(&freeBytes, &totalBytes), "hipMemGetInfo");
  check(hipDeviceTotalMem(&deviceBytes, 0), "hipDeviceTotalMem");
  hipDeviceProp_t prop{};
  check(hipGetDeviceProperties(&prop, 0), "hipGetDeviceProperties(capacity)");
  require(freeBytes <= totalBytes && totalBytes == deviceBytes &&
              totalBytes == prop.totalGlobalMem,
          "capacity queries disagree or return invalid values");
}

void checkThreadIsolation() {
  std::atomic<bool> oomReady{false}, otherChecked{false};
  std::exception_ptr oomFailure, otherFailure;
  std::thread oomThread([&] {
    try {
      void* ptr = nullptr;
      expect(hipMalloc(&ptr, 128 * MiB), hipErrorOutOfMemory, "thread quota allocation");
      require(ptr == nullptr, "failed allocation returned a pointer");
      expect(hipPeekAtLastError(), hipErrorOutOfMemory, "thread OOM peek");
      oomReady.store(true, std::memory_order_release);
      while (!otherChecked.load(std::memory_order_acquire)) std::this_thread::yield();
      expect(hipGetLastError(), hipErrorOutOfMemory, "thread OOM get");
      expect(hipPeekAtLastError(), hipSuccess, "thread OOM clear");
    } catch (...) { oomFailure = std::current_exception(); oomReady.store(true); }
  });
  std::thread cleanThread([&] {
    try {
      while (!oomReady.load(std::memory_order_acquire)) std::this_thread::yield();
      expect(hipPeekAtLastError(), hipSuccess, "other thread peek");
      expect(hipGetLastError(), hipSuccess, "other thread get");
      otherChecked.store(true, std::memory_order_release);
    } catch (...) { otherFailure = std::current_exception(); otherChecked.store(true); }
  });
  oomThread.join();
  cleanThread.join();
  if (oomFailure) std::rethrow_exception(oomFailure);
  if (otherFailure) std::rethrow_exception(otherFailure);
}
}  // namespace

int main() try {
  auto peek = resolve<ErrorQuery>("hipPeekAtLastError");
  auto get = resolve<ErrorQuery>("hipGetLastError");
  auto extGet = resolve<ErrorQuery>("hipExtGetLastError");
  auto freeAsync = resolve<FreeAsync>("hipFreeAsync");

  expect(get(), hipSuccess, "initial get");
  expect(extGet(), hipSuccess, "initial ext get");
  expect(peek(), hipSuccess, "initial peek");
  checkDevice();

  // The fixture injects a real native 64 GiB OOM into this first call.
  Allocation fallback;
  check(hipMalloc(&fallback.ptr, 32 * MiB), "fallback hipMalloc after native OOM");
  require(fallback.ptr != nullptr, "fallback malloc returned null");
  expect(peek(), hipSuccess, "fallback peek");
  expect(get(), hipSuccess, "fallback get");
  expect(extGet(), hipSuccess, "fallback ext get");
  std::vector<uint8_t> input(64 * 1024), output(input.size());
  for (size_t i = 0; i < input.size(); ++i)
    input[i] = static_cast<uint8_t>((i * 131u + 17u) & 0xffu);
  check(hipMemcpy(fallback.ptr, input.data(), input.size(), hipMemcpyHostToDevice),
        "fallback write");
  check(hipMemcpy(output.data(), fallback.ptr, output.size(), hipMemcpyDeviceToHost),
        "fallback read");
  require(input == output, "fallback allocation data mismatch");
  check(hipFree(fallback.ptr), "fallback hipFree");
  fallback.ptr = nullptr;

  // An unrelated native error must survive successful wrapped calls.
  hipError_t nativeError = hipSetDevice(-1);
  require(nativeError != hipSuccess, "hipSetDevice(-1) unexpectedly succeeded");
  Allocation preserved;
  check(hipMalloc(&preserved.ptr, 64 * 1024), "malloc with pending native error");
  checkCapacity();
  expect(peek(), nativeError, "pending native error peek");
  expect(peek(), nativeError, "pending native error repeated peek");
  expect(get(), nativeError, "pending native error get");
  expect(peek(), hipSuccess, "native error cleared");
  check(hipFree(preserved.ptr), "free after native error");
  preserved.ptr = nullptr;

  void* tooLarge = nullptr;
  expect(hipMalloc(&tooLarge, 128 * MiB), hipErrorOutOfMemory, "quota allocation");
  require(tooLarge == nullptr, "quota failure returned a pointer");
  expect(peek(), hipErrorOutOfMemory, "quota peek");
  expect(get(), hipErrorOutOfMemory, "quota get");
  expect(peek(), hipSuccess, "quota error cleared");

  Allocation async;
  check(hipMalloc(&async.ptr, 32 * MiB), "async-free test allocation");
  expect(freeAsync(async.ptr, nullptr), hipErrorNotSupported, "hipFreeAsync on spill pointer");
  expect(peek(), hipErrorNotSupported, "async-free peek");
  expect(extGet(), hipErrorNotSupported, "async-free ext get");
  expect(get(), hipSuccess, "async-free error cleared");
  check(hipFree(async.ptr), "explicit free after rejected async free");
  async.ptr = nullptr;

  // Native errors take precedence over an older shim error, which is discarded on get.
  expect(hipMalloc(&tooLarge, 128 * MiB), hipErrorOutOfMemory, "precedence quota allocation");
  nativeError = hipSetDevice(-1);
  require(nativeError != hipSuccess, "second hipSetDevice(-1) unexpectedly succeeded");
  expect(peek(), nativeError, "native-over-shadow peek");
  expect(get(), nativeError, "native-over-shadow get");
  expect(peek(), hipSuccess, "no shadow resurrection");

  checkThreadIsolation();
  std::cout << "HIP host error-state check PASS\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "HIP host error-state check FAIL: " << error.what() << '\n';
  return 1;
}
