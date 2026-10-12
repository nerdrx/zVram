# Natural recovery with an unrelated cold peer

One matched baseline-then-unlocked pair uses the same source and fixture,
five fresh 32 MiB pools with only child zero forced nonlocal, and a separate
pristine 4 MiB cold peer per transaction. Both use 4 MiB children, 50 ms quiet,
the same caps/headroom, and the existing 200 ms/5 ms sampler with its 1 ms
quiet-window burst. Only the unlocked-wait environment differs. The new mode
reserves sample storage before timing; the timed loop is unchanged and has no
stats, backing queries or cold-peer submissions.

Both CTests passed with synchronization validation: baseline 1.45 seconds,
unlocked 1.19 seconds. Every transaction proved its peer remained cold through
the sample window, then filled/read back all 4 MiB outside timing, verified
all original 32 MiB and local promotion, and returned resident/cold/error
counters to zero. Final device logs showed zero driver live allocations.
No validation error appeared.

Strict analysis requires all five sample/profile transactions and all five
exact cold-peer proof markers. Its new `--cold-peer` option rejects the old
no-peer logs; old analysis behavior remains available without that option.

```sh
python3 validation/unlocked-recovery-wait/analyze_unhooked.py --cold-peer \
  validation/unlocked-recovery-wait/natural-cold-peer/baseline.txt \
  validation/unlocked-recovery-wait/natural-cold-peer/unlocked.txt
```

All five paired recovery-copy intervals intersected recorded submit calls:

| Transaction | Baseline intersecting call, µs | Unlocked intersecting call, µs |
| --- | ---: | ---: |
| 0 | 853.053 | 39.730 |
| 1 | 577.872 | 9.740 |
| 2 | 692.923 | 199.030 |
| 3 | 922.153 | 470.262 |
| 4 | 673.873 | 408.041 |

Whole-window baseline/unlocked counts were 324/318, medians 13.160/13.391 µs,
p95 98.611/101.780 µs, and maxima 922.153/470.262 µs. This pair shows shorter
submit calls overlapping recovery while unrelated cold data stays asleep.
Whole-window p95 did not improve. One order-sensitive pair under background
GPU activity does not establish stable overall latency or game FPS gains;
profile intervals include copy setup/wait/reacquisition, not pure GPU time.
Sparse prepare/rebind still hold locks. No default, release or install changed.

Before each test, memory PSI averages were zero, RAM available was about 25 GiB,
and background GPU utilization was 59 percent. No game/model runner was found
(the Ollama daemon remained running). Afterward PSI stayed zero; user apps,
LACT auto, clocks/power/fans and unrelated memtest logs were preserved.
