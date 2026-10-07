#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>

#include "hip_activity.hpp"
#include "hip_hibernate.hpp"

namespace {
using Clock = std::chrono::steady_clock;
thread_local bool internal = false;
thread_local unsigned int activityDepth = 0;
thread_local bool residentActivity = false;

struct InternalScope {
  bool previous = internal;
  InternalScope() { internal = true; }
  ~InternalScope() { internal = previous; }
};

size_t setting(const char* name, size_t maximum) {
  const char* value = std::getenv(name);
  if (!value || !*value || *value == '-') return 0;
  char* end = nullptr;
  errno = 0;
  const unsigned long long number = std::strtoull(value, &end, 10);
  return errno || *end || number > maximum ? 0 : static_cast<size_t>(number);
}

enum class Phase { Hot, Freezing, Cold, Restoring };

// ponytail: one per-process gate; HIP callers run concurrently while hot, and
// the coordinator closes entry only during an idle snapshot or restore.
class Coordinator {
 public:
  const std::chrono::milliseconds idle{setting("ZVRAM_HIP_AUTO_IDLE_MS", 60000)};
  const size_t coldBudget = setting("ZVRAM_HIP_COLD_LIMIT_MIB", 40960) * size_t{1024 * 1024};
  bool requested() const { return idle.count() > 0 && coldBudget > 0; }

  hipError_t enter(bool needsResident) {
    if (!requested()) { ++activityDepth; return hipSuccess; }
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      changed.wait(lock, [&] { return phase != Phase::Freezing && phase != Phase::Restoring; });
      if (phase != Phase::Cold || !needsResident) break;
      if (inFlight) {
        changed.wait(lock, [&] { return !inFlight || phase != Phase::Cold; });
        continue;
      }
      phase = Phase::Restoring;
      lock.unlock();
      hipError_t status = hipErrorUnknown;
      size_t logical = 0, stored = 0;
      const auto started = Clock::now();
      {
        InternalScope scope;
        status = zvramHipResume();
        (void)zvramHipColdBytes(&logical, &stored);
      }
      lock.lock();
      phase = logical ? Phase::Cold : Phase::Hot;
      lastWork = Clock::now();
      const double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
      std::fprintf(stderr, "[zvram-hip] auto resume: status=%d logical=%zu stored=%zu elapsed_ms=%.3f\n",
                   int(status), logical, stored, elapsedMs);
      changed.notify_all();
      if (status != hipSuccess) return status;
    }
    ++inFlight;
    ++activityDepth;
    residentActivity = needsResident;
    return hipSuccess;
  }

  void exit() {
    if (!activityDepth) return;
    --activityDepth;
    if (!requested()) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (inFlight) --inFlight;
    if (residentActivity) lastWork = Clock::now();
    residentActivity = false;
    // Start after the initial HIP call has returned, rather than from its
    // runtime initialization callback. Stop before HIP's earlier exit hooks.
    if (!worker.joinable() && ready && !disabled && !stopping) start();
    changed.notify_all();
  }

  void registration(bool supported) {
    std::lock_guard<std::mutex> lock(mutex);
    ready = supported;
    if (!supported) disabled = true;
    changed.notify_all();
  }

  void disable(const char* reason) {
    std::lock_guard<std::mutex> lock(mutex);
    if (requested() && !disabled)
      std::fprintf(stderr, "[zvram-hip] automatic hibernation disabled: %s\n", reason);
    disabled = true;
    changed.notify_all();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      changed.notify_all();
    }
    if (worker.joinable()) worker.join();
  }
  ~Coordinator() { stop(); }

 private:
  void start();
  void run() {
    InternalScope scope;
    std::unique_lock<std::mutex> lock(mutex);
    while (!stopping) {
      if (disabled || !ready || phase == Phase::Cold) {
        changed.wait(lock, [&] { return stopping || (!disabled && ready && phase != Phase::Cold); });
        continue;
      }
      if (phase != Phase::Hot || inFlight) {
        changed.wait(lock, [&] { return stopping || disabled || (phase == Phase::Hot && !inFlight); });
        continue;
      }
      const auto deadline = lastWork + idle;
      if (Clock::now() < deadline) {
        changed.wait_until(lock, deadline);
        continue;
      }
      phase = Phase::Freezing;
      lock.unlock();
      const auto started = Clock::now();
      const hipError_t status = zvramHipHibernate(coldBudget);
      size_t logical = 0, stored = 0;
      const hipError_t stats = zvramHipColdBytes(&logical, &stored);
      lock.lock();
      // A late snapshot failure can retain partial cold data. Keep the gate's
      // cold state until a caller successfully resumes or frees that data.
      phase = logical ? Phase::Cold : Phase::Hot;
      lastWork = Clock::now();
      if (logical || status != hipSuccess || stats != hipSuccess)
        std::fprintf(stderr, "[zvram-hip] auto hibernate: status=%d logical=%zu stored=%zu elapsed_ms=%.3f\n",
                     int(status), logical, stored,
                     std::chrono::duration<double, std::milli>(Clock::now() - started).count());
      changed.notify_all();
    }
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::thread worker;
  Phase phase = Phase::Hot;
  size_t inFlight = 0;
  Clock::time_point lastWork = Clock::now();
  bool ready = false;
  bool disabled = false;
  bool stopping = false;
};

Coordinator& coordinator() { static Coordinator instance; return instance; }
void stopCoordinator() { coordinator().stop(); }
void Coordinator::start() {
  try {
    // Register once, after HIP initialization and before starting the worker.
    if (std::atexit(stopCoordinator) != 0) { disabled = true; return; }
    worker = std::thread([this] { run(); });
  } catch (...) {
    disabled = true;
    std::fprintf(stderr, "[zvram-hip] automatic hibernation disabled: worker unavailable\n");
  }
}
}  // namespace

extern "C" bool zvramHipInternalActivity() noexcept {
  return internal || activityDepth || zvramHipLayerInternal();
}
extern "C" bool zvramHipAutoRequested() noexcept {
  try { return coordinator().requested(); } catch (...) { return false; }
}
extern "C" hipError_t zvramHipActivityEnter(bool needsResident) noexcept {
  try { return coordinator().enter(needsResident); }
  catch (...) { return hipErrorUnknown; }
}
extern "C" void zvramHipActivityExit() noexcept {
  try { coordinator().exit(); } catch (...) {}
}
extern "C" void zvramHipDispatchReady(bool supported) noexcept {
  try { coordinator().registration(supported); } catch (...) {}
}
extern "C" void zvramHipDisableAuto(const char* reason) noexcept {
  try { coordinator().disable(reason); } catch (...) {}
}

#ifndef ZVRAM_HAS_HIP_DISPATCH
extern "C" int rocprofiler_set_api_table(const char*, uint64_t, uint64_t, void**, uint64_t) {
  zvramHipDispatchReady(false);
  std::fprintf(stderr, "[zvram-hip] automatic hibernation unavailable: unsupported HIP dispatch ABI\n");
  return 0;
}
#endif
