# Recovery wait and empty-scan work

Follow-up to the direct-copy path in v0.4.17. Existing `ZVRAM_VULKAN_GPU_PROFILE=1` now adds a success-only eight-phase breakdown: view creation, allocation, alias detach, private-view binding, copy/wait, private-view unbind, application rebind and release. Measurements are CPU wall time, not GPU timestamps. Additional clock calls are disabled when profiling is off.

## Before changing waits

The user stopped playing before these checks. No game or model worker remained; memory PSI avg10 was zero and available memory exceeded 24 GiB. Background GPU busy was 37 percent at entry, so this is a desktop-load component measurement. Five successful transactions per size verified every byte and balanced teardown.

Median phase times for a 32 MiB child: view 38 us, allocation 51 us, alias detach 63 us, private bind 45 us, copy/wait 8823 us, private unbind 46 us, app rebind 37 us, release 30 us. For a 1 MiB child: 34, 57, 60, 28, 416, 41, 27, 31 us respectively. The copy dominates; removing a host wait is a small improvement opportunity, not evidence of a game FPS gain. Raw logs: `before-details.txt`; parsed values: `before-phases.json`.

## Deferred wait experiment rejected

A temporary same-queue initial private-bind wait deferral retained the semaphore chain and downstream copy completion. Five paired samples showed no improvement: 32 MiB median 9589 -> 9831 us (max 10007 -> 10008), 1 MiB median 717 -> 752 us (max 822 -> 1089). These sequential desktop-load samples do not establish a stable performance difference. The wait change and its temporary CPU ordering checks were removed; production sparse waits remain unchanged. Logs and parsed comparison are retained in `before-*`, `after-*` and `phase-comparison.json`.

## Busy-gate fixture correction

The initial six-gate run failed the two pending-queue checks (`native-ctest.txt`, `native-details.txt`). The fixture queried backing after `pending.finish()` had signaled the timeline and drained the second queue. A faster worker could legitimately recover before that query. The query/assert now executes before release, while the timeline is confirmed unsignaled. No production reference guard was weakened. All six corrected native gates passed, including BDA/full-byte verification and pending second-queue deferral (`corrected-native-*`).

## Retained production change

Recovery selection now exits before scanning pools when maintained tracked nonlocal bytes (`liveOther`) are zero. Eligible explicit nonlocal children necessarily contribute to that counter; unrelated host allocations can only keep the scan conservative. Output handles are cleared before this guard. CPU checks cover the zero counter and accurate two-pool accounting. Existing profiling adds phase diagnostics only when requested. No FPS or lock-latency improvement is claimed from this structural scan removal.

An installed v0.4.17 cold full-buffer graphics fixture with 1 MiB ranges passed full bytes and exact pixels, but submission median was 12267 us/max 13634 us: unknown graphics restores all 32 children. This does not support changing the default range size. General graphics tracking and kernel-transparent GTT placement remain limitations; recovery/async/presentation defaults and user GPU settings remain unchanged.
