# Bounded synthetic recovery churn

This fixture alternates eight A/B/C full-buffer reads across three 4 MiB virtual buffers. It uses a separate 4 MiB staging buffer and a private per-process live-control endpoint to lower the tracked resident cap to 8 MiB, then raise it to 12 MiB. The raised interval lasts 100 ms after the cap acknowledgment. Acknowledgment and eviction waits add to the total cadence. Two otherwise identical cases use recovery quiet delays of 0 and 1000 ms.

The initial three backings are deliberately forced to nonlocal memory with existing compile-only test hooks. This is a controlled synthetic spill, not evidence of natural system pressure or transparent driver migration. No production code or default is changed.

Each read checks every word against a distinct per-buffer pattern. Rows measure the host duration of `vkQueueSubmit` separately from the subsequent fence wait; command recording, cap requests, statistics queries, and byte verification are outside those timing windows. Final layer and driver telemetry must show zero retained resources and failures.

Cold snapshot freeze/restore counters and actual `warm-recovery local-copy` events are reported separately. A backing-type transition is only a candidate observation because cold restore may also select local memory. Eight samples per setting cannot establish stable tail latency, FPS, or the best recovery policy.

## Run

Use a hook-enabled build (`ZVRAM_TEST_ASYNC_HOOK=ON`). Build the current layer and auto-check fixture before running. Check host memory pressure, free RAM, GPU availability, and active applications first. These are compute-only tests with no visible window.

```sh
ctest --test-dir build/async-race -V -R '^vulkan-recovery-churn-quiet-0$' > quiet0.txt 2>&1
ctest --test-dir build/async-race -V -R '^vulkan-recovery-churn-quiet-1000$' > quiet1000.txt 2>&1
python3 validation/recovery-churn/analyze.py quiet0.txt quiet1000.txt
```

The CTest timeout is 30 seconds and each measured fence wait is limited to two seconds. The validation run additionally uses a 15-second outer process-group timeout. No user applications or GPU settings are changed.

## Recorded result (2026-10-14)

Both hook-enabled and normal fixture builds passed. The log parser accepted both real captures and rejected five intentionally invalid variants.

Both cases passed on the AMD Radeon RX 7900 XTX under RADV with synchronization validation enabled (4.45 s and 4.44 s). Each checked all eight 4 MiB reads, with no validation errors and zero final resident, cold, cached, failed, or live driver allocations. Existing model servers and GPU settings were left unchanged.

| Recovery quiet delay | Actual warm copies | Accesses requiring cold restore | Submit median / maximum | Fence median / maximum |
|---|---:|---:|---:|---:|
| 0 ms | 3 (12 MiB) | 8/8 | 3.828 / 5.685 ms | 0.2995 / 0.369 ms |
| 1000 ms | 1 (4 MiB) | 8/8 | 3.724 / 4.382 ms | 0.3235 / 0.349 ms |

Both runs recorded nine freezes and eleven total restores. Three restores materialized the lazy buffers during initialization; the other eight occurred during measured reads. The three-buffer cycle repeatedly revisits data outside the two-buffer resident budget. The longer quiet delay reduced observed warm-copy work in this pair, but did not prevent cold restore churn. This is a useful reproduction for testing future policies, not evidence that changing the default delay improves games.

Quiet delay is measured since each child's last application submission. A 100 ms raised-cap interval does not guarantee that all children remain younger than 1000 ms, and warm promotion can also happen at the 8 MiB cap. Actual event counts, not that assumption, determine the result.

`quiet0.txt`, `quiet1000.txt`, and `summary.json` preserve raw evidence and parsed results. `initial-lazy-seed-failure.txt` preserves the first rejected setup: forcing the next backing before lazy allocation only affected the first later materialization. The fixture now applies the existing hook immediately before each initialization submission. That failure completed cleanly and is not included in the comparison.

