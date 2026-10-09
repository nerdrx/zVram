# Experimental cold-cycle recovery

This page records the historical snapshot-based implementation through v0.4.16. The current source uses [direct GPU-copy recovery](../direct-gpu-recovery/README.md); the raw snapshot quota and timing results below describe the earlier path.

Default off; included in Hub release v0.4.10. This prototype recovers completed eligible buffers whose backing explicitly uses a non-device-local Vulkan heap. It cannot identify or move device-local allocations that the kernel has transparently placed in GTT. Allocation type is not proof of physical residency.

## Correctness

RX 7900 XTX / RADV checks force one compatible nonlocal 32 MiB backing allocation, upload a complete pattern, wait for below-cap recovery, verify every byte, and require empty/error-free cleanup. Application buffer plus staging is 64 MiB; the blocked two-queue case adds a 32 MiB native sink; layer helpers are additional. Before GPU tests, the GPU was idle, no games/models ran, and memory PSI avg10 was zero.

The blocked case holds a real second-queue buffer use behind a timeline semaphore for 1200 ms, past the recovery quiet delay, confirms the timeline is still blocked and no freeze occurred, then releases/drains it and verifies recovery. See `raw-busy-deferral-ctest.txt`. Earlier `initial-single-two-queue-ctest.txt` proves only two configured queues, not blocked-use deferral.

Recovery uses existing freeze/restore semantics and temporarily restricts restoration to compatible native local types. Raw snapshots avoid encode/decode work for this immediate restore, remain charged against the cold-storage quota, and preserve the configured compression policy. Local allocation refusal retains cold bytes for ordinary GTT fallback. CPU checks cover allocation refusal, snapshot/restore copy failure, application rebind failure, active references, sparse unbind failure, incompatible masks, quiet age, oversize refusal, raw quota refusal, and accounting. Final application rebind failure retains the existing sticky GPU gate; it is not silently recoverable.

The quiet delay is one second, independent of the launcher idle timer. Recovery moves at most one eligible child per worker pass and remains disabled unless `ZVRAM_VULKAN_COLD_CYCLE_RECOVERY=1` is set. It requires pressure-only active range paging, immediate resident admission, and native-budget headroom. Buffers used continuously may remain ineligible. This prototype is not a general driver-level GTT residency controller.

## Latency limits

`fixed-window-live-cap-probe.txt` samples direct zero-command `vkQueueSubmit` calls for 500 ms, with no intervening stats/status queries, during a 64→32 MiB residency cap reduction. Each mode acknowledged the cap and froze exactly one chunk during sampling. One pair of 500-sample runs reported maximum call time 16.776 ms synchronous and 11.210 ms async; both p50=2 us, p95=9 us. This is one bounded component comparison, not game FPS or a stable speedup estimate. Async remains optional.

Earlier probe archives are retained as diagnostic evidence: `non-overlapping-live-cap-probe.txt` had no eviction during sampling; `live-cap-overlap-probe.txt` and `raw-recovery-submit-probe.txt` included untimed stats queries that could absorb lock stalls. Their maxima do not establish pressure or recovery latency improvements.

The synchronous recovery transaction still holds submission locks. Success logs report its own duration; resource-free submit sampling alone can miss the transaction. Keep recovery opt-in until broader workload and frame-time evidence supports enabling it.

Final fixed-one-second-quiet runs passed both hardware cases (3.79 s total) and all 13 normal CPU checks. Recovery transaction durations were 20.696 ms single-queue and 12.366 ms after busy-queue release; a resource-free submit caught an 18.684 ms stall in the single-queue window. The two-queue window missed the cycle and its 79 us maximum is not a recovery cost estimate. These results justify retaining default-off recovery. See `final-fixed-quiet-duration-ctest.txt` and `cpu-ctest.txt`.

## Smaller recovery ranges

The same 32 MiB full-byte fixture passed with 8 MiB and 4 MiB residency ranges. Only the first child is forced nonlocal; remaining children start local. Recovery transaction durations were 8.633 ms and 5.292 ms respectively, with sampled submit stalls of 6.042 ms and 2.335 ms. These are single bounded observations, not game performance estimates. Smaller ranges increase allocation/binding metadata and can add overhead to large working sets; they remain an explicit `--vulkan-range-mib` choice. See `small-range-ctest.txt`.

Native intercepted allocations retain the app-selected type by default. Opt-in recovery broadens internal backing candidates to compatible ordinary types while preferring the original type, preserving allocation flags and priority, and intersecting all resource and pool-view requirements. Application bindings must still accept the original memory type. Protected, lazy, imported, dedicated and other excluded native allocations retain existing eligibility restrictions; alternate AMD device-coherent/uncached types are excluded.

The default-off negative gate still refuses forced nonlocal backing with pristine accounting. Opt-in native 4 MiB single-queue and blocked second-queue recovery gates pass full 32 MiB byte verification and empty teardown. See `native-opt-in-ctest.txt` and `native-alias-guard-and-bda-preflight.txt`; the latter includes the initial BDA fixture preflight failure. CPU tests also verify original-type preference, fallback, alias narrowing and rejection before mutation. This expands explicit nonlocal backing recovery, not kernel-managed physical residency control.

Final native gates after the alias guard pass all four cases: default refusal, single-queue recovery, blocked second-queue recovery and BDA shader access. The BDA gate confirms a stable device address, verifies the original full byte pattern, executes one address-based shader update, then verifies every updated byte. An initial diagnostic failed because the fixture dispatched cycle 7 once but expected cumulative cycles 0 through 7; using one cycle 0 dispatch and expectation corrected the fixture without changing production code. The failure remains archived in `native-bda-ctest.txt`; the corrected run is `native-final-bda-and-alias-ctest.txt`. All 13 normal CPU checks and 19 Python checks passed after the native alias guard. Recovery remains default off, with no game frame-time claim.

## Published package

v0.4.10 release workflow 37879035839 passed and NX Hub installed the published tarball after SHA-256 verification. Installed launcher and manager match the tag; codec metadata reports test_hooks=false and the Vulkan library contains neither test setter. The installed layer passed the bounded async live-cap reduction/increase fixture with full byte restoration and zero resident/cold/error accounting at teardown (`installed-0.4.10-live-cap.txt`). Recovery and async compression remain opt-in; the published package does not contain the forced-nonlocal test selector.

## Recent completed use

`--vulkan-recover-local-quiet-ms 0` (requires recovery opt-in) removes only the unused-age gate. The default remains1000ms. Current caps, native headroom, pending-reference protection, raw cold quota and one-child-per-pass limits remain. The direct environment setting is strictly decimal uint32, including0; malformed and overflowing values do not become0. CPU gates cover recent completed use, recent pending references, default age refusal and parser boundaries; all13 normalCPU and19Python checks pass.

Six native hardware gates passed after this change, including existing default refusal/single/twoqueue/BDA cases and two new zero-quiet cases. The single-queue case recovered a4MiB nonlocal child during a20ms cadence of completed tracked touches (two touches,20.319ms observed eligibility-to-detection). Its lock-held recovery transaction was4.190ms. The two-queue case kept a pending resource protected for1200ms with an unsignaled timeline, then recovered after release (3.229ms detection, zero additional touches; transaction3.542ms). The latter proves pending-use protection, not repeated hot-use cadence. Both verify the complete32MiB pattern and balanced teardown. See `quiet-zero-native-ctest.txt` and detailed raw log. No game frame-time claim follows; zero quiet can introduce a synchronous frame spike.

Snapshot copy cleanup replaces empty-vector resize-plus-memcpy with range assign for synchronous raw and copied compressed payloads, matching the existing async path. This avoids value-initializing bytes immediately overwritten while preserving owned storage, sizes, quota accounting and the existing allocation-failure rollback. The BP16 exact-capacity move path remains. Both normal/hook builds, all13 normalCPU checks, and all6 native hardware recovery/refusal/BDA/recent-use gates pass after the change. Logs: `snapshot-assign-cpu.txt`, `snapshot-assign-native.txt`, and detailed native log. No isolated timing or game improvement is claimed for this copy cleanup.

Published v0.4.11 (f40a7f5) passed release workflow37881188375 and NX Hub SHA-256 install verification. Installed launcher/manager match the tag, codec metadata disables test hooks, and both test setters are absent from the Vulkan library (`installed-0.4.11-payload.json`). The installed package also passed native two-queue pressure with local recovery enabled and quiet0, verifying bytes, pending-reference refusal and zero-accounting teardown (`installed-0.4.11-native-pressure.txt`). This packaged gate exercises policy configuration and pressure correctness; forced nonlocal migration remains covered separately by the hook-build gates.
