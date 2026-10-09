# Log line coalescing CPU check

`check.py` extracts the current production `logf` from `layer.cpp`, compiles it
into a temporary CPU-only harness, and compares its bytes with the former
prefix/`vfprintf`/newline path. The cases cover a common trace, the exact stack
buffer boundary, fallback just above that boundary and at 10 KiB, width and
precision formatting, UTF-8 text, and a safe `vsnprintf` encoding-error case.

It also sends 1,000 identical short trace lines to `/dev/null` and reads
`syscw` from `/proc/self/io`. Five old/new samples on this host produced:

| Path | Median write syscalls | Median elapsed time |
| --- | ---: | ---: |
| Former three-call stdio path | 6,000 | 361,982 ns |
| Coalesced short-line path | 1,000 | 105,871 ns |

All seven output cases matched byte-for-byte. These are helper-level CPU
measurements; they do not measure Vulkan work, lock contention, application
latency, or game frame pacing. Long or formatting-error lines intentionally
retain the former output path. The log protocol and default diagnostics remain
unchanged.

Run with:

```sh
rtk python3 validation/log-line-coalescing/check.py
```

The final normal shared layer and resident-bootstrap harness rebuilt successfully. All13 normal CPU checks passed (5.54s), model-budget/parser checks passed, and normal production graphics conservative-fallback plus async live-cap GPU checks both passed (1.82s) with full32MiB integrity, pixels and clean teardown. Normal test API strings remain absent. The byte/proc-I/O proof is now required by Build and Release CI; it uses the compiler directly and does not depend on RTK being installed. `summary.json` contains the final raw five-sample helper results. This change preserves diagnostic events instead of rate-limiting or hiding them.
