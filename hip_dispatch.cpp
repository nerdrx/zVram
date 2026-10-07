#include <GL/gl.h>
#include <hip/hip_runtime.h>
#include <hip/hip_deprecated.h>
#include <hip/hip_gl_interop.h>
#include <hip/amd_detail/hip_api_trace.hpp>
#include <hip/hip_version.h>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

#if HIP_VERSION_MAJOR != 7 || HIP_VERSION_MINOR != 2 || HIP_VERSION_PATCH != 53211
#error "hip_dispatch_slots.inc targets ROCm HIP 7.2.53211"
#endif
static_assert(HIP_RUNTIME_API_TABLE_MAJOR_VERSION == 0);
static_assert(HIP_RUNTIME_API_TABLE_STEP_VERSION == 18);
static_assert(sizeof(HipDispatchTable) == 4056);

extern "C" __attribute__((visibility("hidden"))) bool
zvramHipInternalActivity() noexcept;
extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramHipActivityEnter(bool needsResident) noexcept;
extern "C" __attribute__((visibility("hidden"))) hipError_t
zvramHipDispatchError(hipError_t native, bool clear) noexcept;
extern "C" __attribute__((visibility("hidden"))) void
zvramHipActivityExit() noexcept;
extern "C" __attribute__((visibility("hidden"))) void
zvramHipDispatchReady(bool supported) noexcept;
extern "C" __attribute__((visibility("hidden"))) void
zvramHipDisableAuto(const char* reason) noexcept;

namespace {
constexpr uint64_t kHipRuntimeVersion =
    10000 * HIP_VERSION_MAJOR + 100 * HIP_VERSION_MINOR;
std::atomic<bool> gPatched{false};

class ActivityGuard {
 public:
  explicit ActivityGuard(bool needsResident) noexcept
      : status_(zvramHipActivityEnter(needsResident)), entered_(status_ == hipSuccess) {}
  ~ActivityGuard() {
    if (entered_) zvramHipActivityExit();
  }
  hipError_t status() const noexcept { return status_; }

 private:
  hipError_t status_;
  bool entered_;
};

template <typename Result>
Result dispatchFailure(hipError_t status) noexcept {
  if constexpr (std::is_void_v<Result>) {
    (void)status;
    return;
  } else if constexpr (std::is_same_v<Result, hipError_t>) {
    return status;
  } else {
    return Result{};
  }
}

template <int ErrorMode, typename Function, typename... Args>
auto invokeOriginal(Function function, Args&&... args) noexcept
    -> std::invoke_result_t<Function, Args...> {
  if constexpr (ErrorMode == 1)
    return zvramHipDispatchError(function(std::forward<Args>(args)...), true);
  else if constexpr (ErrorMode == 2)
    return zvramHipDispatchError(function(std::forward<Args>(args)...), false);
  else
    return function(std::forward<Args>(args)...);
}

#define ZVRAM_UNPAREN(...) __VA_ARGS__

#define ZVRAM_HIP_DISPATCH(member, type, resident, disable, result, params, args, errorMode) \
  static type original_##member = nullptr;
#include "hip_dispatch_slots.inc"
#undef ZVRAM_HIP_DISPATCH

#define ZVRAM_HIP_DISPATCH(member, type, resident, disable, result, params, args, errorMode) \
  static result wrapped_##member params {                                                     \
    const auto original = original_##member;                                                  \
    if (!original) return dispatchFailure<result>(hipErrorNotSupported);                      \
    if (zvramHipInternalActivity()) return original args;                                     \
    if constexpr (disable)                                                                   \
      zvramHipDisableAuto(#member);                                                          \
    ActivityGuard activity(resident);                                                        \
    if (activity.status() != hipSuccess)                                                     \
      return dispatchFailure<result>(activity.status());                                     \
    return std::apply(                                                                        \
        [&](auto&&... callArgs) {                                                             \
          return invokeOriginal<errorMode>(original,                                          \
                                           std::forward<decltype(callArgs)>(callArgs)...);    \
        },                                                                                    \
        std::forward_as_tuple(ZVRAM_UNPAREN args));                                           \
  }
#include "hip_dispatch_slots.inc"
#undef ZVRAM_HIP_DISPATCH

void patchTable(HipDispatchTable* table) noexcept {
#define ZVRAM_HIP_DISPATCH(member, type, resident, disable, result, params, args, errorMode) \
  original_##member = table->member;                                                          \
  if (original_##member) table->member = &wrapped_##member;
#include "hip_dispatch_slots.inc"
#undef ZVRAM_HIP_DISPATCH
}
}  // namespace

extern "C" __attribute__((visibility("default"))) int
rocprofiler_set_api_table(const char* library, uint64_t lib_version,
                          uint64_t lib_instance, void** tables,
                          uint64_t table_count) {
  if (!library || std::strcmp(library, "hip") != 0) return 0;

  const char* autoIdle = std::getenv("ZVRAM_HIP_AUTO_IDLE_MS");
  if (!autoIdle || !*autoIdle) {
    zvramHipDispatchReady(false);
    return 0;
  }
  if (lib_version != kHipRuntimeVersion || lib_instance != 0 || table_count != 1 ||
      !tables || !tables[0]) {
    zvramHipDispatchReady(false);
    std::fprintf(stderr, "[zvram-hip] automatic dispatch rejected: version=%llu instance=%llu tables=%llu\n",
        static_cast<unsigned long long>(lib_version), static_cast<unsigned long long>(lib_instance),
        static_cast<unsigned long long>(table_count));
    return 0;
  }

  auto* table = static_cast<HipDispatchTable*>(tables[0]);
  if (table->size != sizeof(HipDispatchTable)) {
    zvramHipDispatchReady(false);
    std::fprintf(stderr, "[zvram-hip] automatic dispatch rejected: table size=%zu expected=%zu\n",
        table->size, sizeof(HipDispatchTable));
    return 0;
  }
  bool expected = false;
  if (!gPatched.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    zvramHipDispatchReady(false);
    return 0;
  }

  patchTable(table);
  zvramHipDispatchReady(true);
  std::fprintf(stderr, "[zvram-hip] automatic dispatch ready: HIP=70200 slots=506\n");
  return 0;
}
