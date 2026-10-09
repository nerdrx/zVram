# Unlocked recovery copy wait prototype

Experimental, default off: `ZVRAM_VULKAN_UNLOCKED_RECOVERY_WAIT=1` requires
explicit local recovery. The installed v0.4.19 release remains unchanged.

The worker retains the old and replacement allocations, private views, and
copy fence. It releases the device and queue locks only while waiting for the
copy fence. Sparse alias transitions remain synchronous. On completion it
revalidates allocation, child, binding, backing, and device-error state and
rechecks the current cap before committing; a reduced cap can roll back.

Known, disjoint hot submissions may proceed only when no cold data exists.
Matching, unknown, cold-restoration, admission-eviction, and mutation paths
join the pending transaction. Known-empty signals bypass restoration.
Condition-variable waits release the device lock, allowing completion to run.

The compile-only hook holds the worker after both locks are released. A
compile-only waiter counter proves submissions reached the pending wait;
pre-submit thread scheduling alone is not counted as proof. Fixtures cover
disjoint progress and matching/unknown joins, then a separate pristine cold
peer fill/restore. Each checks full data and accounting after release.

Root's 13 normal CPU checks passed (`final-cpu-ctest.txt`), including pending
admission retry, known-empty submits with cold groups, stale identity rejection,
cap rollback, lock release/wakeup, and retained-resource teardown. Both normal
and hook bootstrap checks passed. The selected 13 Python checks also passed.

Both deterministic GPU overlap tests passed in 0.21 seconds
(`final-gpu-ctest.txt`, `final-gpu-details.txt`). They verify the original full
32 MiB pattern, the additional 4 MiB cold peer's fill pattern, actual pending
waiter entry, and zero resident/cold/failure accounting at teardown. During the
artificially held hook, the disjoint hot submission returned in 44 microseconds;
matching and unknown submissions waited for release. That is a single
call-progress observation, not unhooked recovery latency or a game benchmark.

Three existing BDA/busy-two-queue/hot-pending-two-queue gates passed with unlocked
wait enabled. The ordinary async stale-pressure regression passed separately.
Its first mixed batch rejected an invalid unlocked-without-recovery setting;
that configuration failure is preserved in the regression log.

Initial fixture failures are preserved: omitted storage usage, an unsupported
overlapping alias, extra-peer accounting, a legacy sampler with no samples,
and whole-buffer barrier tracking plus a staging write hazard. The final probe
uses a global write-to-transfer-read/write memory dependency and exact-range
copies. No production tracking guard was weakened to obtain these passes.
These are access/lifetime and lock-progress checks, not game FPS or a claim
that spikes are eliminated. Sparse setup/finalization still waits under locks;
general graphics access tracking and kernel-transparent GTT placement remain
unchanged. Proven-idle teardown explicitly cleans retained resources and app
destroy/free requests deferred during a gate. Synthetic error codes alone do
not authorize cleanup: the driver must return success or device loss from its
idle wait. Allocator policy matches ordinary destruction/free paths. The
[Vulkan lost-device rules](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-lost-device)
require explicit child destruction and treat device loss as completion for
determining whether resources remain in use. Other ambiguous wait errors do
not establish safe completion and remain an abnormal teardown limitation.
This prototype is not published or installed.

A later deterministic hardware gate holds the pending copy, lowers the live
cap from 64 to 32 MiB, and verifies rollback to child zero's original NONLOCAL
backing. Seven local children plus an ordinary 4 MiB local allocation exactly
fill the new cap; the replacement would exceed it. The gate passed in 0.31 s,
with all 32 MiB bytes intact, no additional freeze/restore/failure, and zero
tracked and driver-allocation accounting after cleanup (`late-cap-details.txt`).
Both Build workflows for d791d90 passed. This adds rollback proof, not a
release or a game-performance claim.

Finite buffer barriers now retain their named child ranges in all four barrier
wrappers: pipeline barriers and wait events, legacy and synchronization2. Only
promoted buffers with a nonzero finite in-bounds range, no extension chain, and
both queue families ignored are narrowed. Whole-size, ownership, unsupported,
and invalid cases retain whole/unknown handling; write classification remains
conservative. Vulkan specifies buffer-barrier access scopes over the named
[legacy range](https://docs.vulkan.org/refpages/latest/refpages/source/VkBufferMemoryBarrier.html)
and [synchronization2 range](https://docs.vulkan.org/refpages/latest/refpages/source/VkBufferMemoryBarrier2.html).
The driver receives the original synchronization unchanged.

The new finite-barrier overlap gate passed in 0.29 seconds with actual pending
waiter entry, child-one hot progress, child-zero/unknown blocking, full 32 MiB
integrity, and empty cleanup (`finite-barrier-gpu-details.txt`). Thirteen normal
CPU checks passed in 5.71 seconds; wrapper tests cover binding offsets, write
tracking, and boundary/fallback cases. Native clean-cache plus unchanged
conservative graphics gates passed in 1.20 seconds. Both normal/hook libraries
and bootstrap targets built; all six test API names remain absent from the
normal library. These are correctness and held-hook progress gates, not game
latency/FPS measurements. Installed v0.4.19 remains unchanged.

The first unhooked app-call pair passed five fresh 32 MiB transactions per mode
(1.37 s baseline, 1.17 s unlocked), checking the exact child-one 4 MiB sample
pattern, child-zero promotion, full initial/final bytes, and empty cleanup. A
pre-recorded finite-range copy runs at 5 ms cadence for 200 ms after recovery
unpause; setup, fence wait/reset, status and backing queries stay outside the
timed call. No recovery wait hook is installed. Optional profile events expose
same-process monotonic copy-phase intervals while retaining existing log lines.
Those intervals include copy setup/submission/wait and unlocked lock reacquisition;
they are not isolated GPU fence time.

The initial pair is **inconclusive**: 195 baseline calls missed every copy-phase
interval, while only one of 195 unlocked calls intersected. The approximately
0.55–0.97 ms phases are shorter than the sampling cadence. No speedup is inferred
from whole-window distributions. Raw logs and strict post-run correlation are
in `unhooked-baseline.txt`, `unhooked-unlocked.txt`, `unhooked-summary.json`, and
`analyze_unhooked.py`. The parser requires successful transactions and phase
intersection in both modes before labeling a pair window-correlated.

A bounded refinement samples every 1 ms only 40–75 ms after the final child-zero
submit, retaining 5 ms cadence elsewhere. Both modes use identical probes. The
first refined pair passed byte checks but a buffered stdout header was split
by stderr; its logs (`unhooked-burst-*.txt`) are preserved and rejected for
comparison. Timestamp records now flush as complete short lines outside timing.

The final pair passed five transactions per mode (1.18 s each), with 332 baseline
and 330 unlocked calls, full bytes, no validation errors and empty cleanup. Four
of five transaction pairs had actual call/copy-phase intersections on both
sides; only those pairs qualify for overlap comparison. Their baseline calls
were 451/431/1001/486 microseconds versus unlocked maxima 331/26/172/10
microseconds, respectively. This is one window-correlated component pair. The
whole-window p95 was **worse** unlocked: 90.5 versus 38.0 microseconds; p50 was
15.7 versus 10.2 microseconds. Therefore this does not establish a broad or
stable latency improvement, game FPS, or eliminated spikes. Sparse phases and
lock reacquisition remain represented in the profile interval.

`unhooked-final-{baseline,unlocked}.txt` and `unhooked-final-summary.json` retain
the final observations. The parser verifies all five successful transactions,
quiet eligibility, complete contiguous sample records and pair-specific
intersection; unmatched pairs remain excluded. Installed v0.4.19 is unchanged
and this prototype is still not recommended for release or default enablement.
