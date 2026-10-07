#include <hip/hip_runtime.h>

#include <algorithm>
#include <cstddef>

namespace {

constexpr unsigned int kThreads = 256;
constexpr unsigned int kMaxBlocks = 65535;

__global__ void copyBytes(unsigned char* destination, const unsigned char* source,
                          size_t bytes) {
  size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  const size_t stride = static_cast<size_t>(gridDim.x) * blockDim.x;
  while (index < bytes) {
    destination[index] = source[index];
    if (bytes - index <= stride) break;
    index += stride;
  }
}

}  // namespace

extern "C" __attribute__((visibility("hidden"))) hipError_t zvramHipCopyMapped(
    void* destination, const void* source, size_t bytes, hipStream_t stream) {
  if (bytes == 0) return hipSuccess;
  if (destination == nullptr || source == nullptr) return hipErrorInvalidValue;

  const size_t needed = bytes / kThreads + (bytes % kThreads != 0);
  const unsigned int blocks = static_cast<unsigned int>(std::min<size_t>(needed, kMaxBlocks));
  hipLaunchKernelGGL(copyBytes, dim3(blocks), dim3(kThreads), 0, stream,
                     static_cast<unsigned char*>(destination),
                     static_cast<const unsigned char*>(source), bytes);
  return hipGetLastError();
}
