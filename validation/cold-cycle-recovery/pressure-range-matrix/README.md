# Bounded pressure range and async comparison

RX 7900 XTX / RADV, installed v0.4.10 layer and launcher with the source fixture binary. A 64 MiB pool was lowered from a 96 MiB cap to 32 MiB, then raised and fully byte-verified. Each 500 ms observation window sampled direct zero-command vkQueueSubmit calls without intervening stats queries. GPU utilization was 0% and memory PSI avg10 was zero before the matrix; each run rechecked pressure and GPU busy. No games or model runners were active.

Two alternating runs per configuration passed full restoration and zero-error cleanup:

| Range | Async | Maximum submit call in run 1 / 2 | Evictions per run |
| --- | --- | --- | --- |
| 32 MiB | off | 15.933 / 16.196 ms | 1 |
| 32 MiB | on | 10.513 / 12.509 ms | 1 |
| 4 MiB | off | 3.042 / 3.762 ms | 8 |
| 4 MiB | on | 3.801 / 4.010 ms | 8 |

All windows acknowledged the 32 MiB cap. Every run p50 was 2 us; p95 ranged 9–12 us. Smaller ranges spread the same 32 MiB eviction across eight worker transactions; these samples do not measure exact cap-settlement time or full working-set throughput. Async did not improve the sampled maximum at 4 MiB. This is a bounded submit-lock observation, not game frame time or FPS, and does not justify changing defaults. `--vulkan-range-mib 4` is an existing explicit tuning option. Snapshot helpers and CPU copies still contribute stalls. Raw logs and summary.json preserve all results.
