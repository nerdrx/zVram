# Experimental cold-cycle recovery

Default off; not included in installed v0.4.9. This prototype recovers completed eligible buffers whose backing explicitly uses a non-device-local Vulkan heap. It cannot identify or move device-local allocations that the kernel has transparently placed in GTT. Allocation type is not proof of physical residency.

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
