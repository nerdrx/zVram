#pragma once

#include <hip/hip_runtime_api.h>

// Private boundary shared by the allocation shim and versioned dispatch adapter.
extern "C" {
__attribute__((visibility("hidden"))) bool zvramHipInternalActivity() noexcept;
__attribute__((visibility("hidden"))) bool zvramHipLayerInternal() noexcept;
__attribute__((visibility("hidden"))) bool zvramHipAutoRequested() noexcept;
__attribute__((visibility("hidden"))) hipError_t zvramHipActivityEnter(bool needsResident) noexcept;
__attribute__((visibility("hidden"))) void zvramHipActivityExit() noexcept;
__attribute__((visibility("hidden"))) void zvramHipDispatchReady(bool supported) noexcept;
__attribute__((visibility("hidden"))) void zvramHipDisableAuto(const char* reason) noexcept;
__attribute__((visibility("hidden"))) hipError_t zvramHipDispatchError(hipError_t native, bool clear) noexcept;
}

struct ZvramHipActivity {
  hipError_t status = hipSuccess;
  bool entered = false;
  explicit ZvramHipActivity(bool needsResident) noexcept {
    if (!zvramHipInternalActivity()) {
      status = zvramHipActivityEnter(needsResident);
      entered = status == hipSuccess;
    }
  }
  ~ZvramHipActivity() { if (entered) zvramHipActivityExit(); }
  ZvramHipActivity(const ZvramHipActivity&) = delete;
  ZvramHipActivity& operator=(const ZvramHipActivity&) = delete;
};
