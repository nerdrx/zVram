# Direct GPU recovery validation

The v0.4.16 recovery baseline creates a raw cold snapshot and restores it into local backing. The replacement uses one GPU-to-GPU transfer with separate private views, preserving the old allocation until copying and rebinding succeed. Recovery remains opt-in; a lower transaction duration alone does not prove spike-free gaming.

## Direct-copy correctness and failure ownership

Application aliases are detached before the old source view and replacement destination view are bound. Copy completion is followed by unbinding both private views, rebinding application aliases to the replacement, advancing visibility/binding generations, and finally freeing the old allocation. Destination allocation flags, priority, and all compatible resource type masks are retained. Copy barriers cover prior device writes and subsequent device reads/writes. Completed-reference, quiet-age, size, headroom and configured-cap guards remain.

Recovery creates no cold snapshot and consumes no cold-storage quota. A previous snapshot quota refusal therefore does not block direct promotion; pressure and async snapshot quota guards remain. Allocation refusal and failures before a driver bind/copy attempt retain old backing and apply a 250 ms cooldown. Ambiguous sparse operations or submitted-copy completion failures gate the device and retain both allocations/views until device teardown. This is deferred reclamation, not successful rollback cleanup.

CPU checks cover these transitions, exact barriers, pause-hook no-op behavior, allocation cooldown, alias preparation failure, pre-submit rollback and four sparse failure stages. All 13 normal CPU checks passed. Hardware checks passed native single/blocked-two-queue recovery, BDA shader updates with stable addresses, hot completed-use recovery, pending-use deferral, and unchanged default native type refusal. Every positive case verifies the full 32 MiB pattern and balanced successful teardown. Three production-build pressure/idle/live-cap regressions also passed.

The compile-only pause hook makes initial backing proof deterministic: pause recovery before lazy upload, query actual nonlocal backing after materialization, then unpause while pending references remain blocked. The earlier failed fixture queried before lazy allocation existed; its failure is preserved in `initial-native-single.txt`. No production delay or force-type hook exists. All four test API names are absent from the normal library and present only in the hook build; packaging refuses test-hook metadata.

## Direct-copy observations under a running game

The user explicitly authorized small bounded checks while playing Warframe. GPU busy was approximately 94–100 percent; available RAM was above 22 GiB and memory PSI avg10 below 1 percent at test entry. Each range fixture sequentially creates five 32 MiB app buffers, forces only one child per buffer nonlocal, and verifies every byte. A separate thread samples resource-free queue submissions through the recovery window without stats queries. Private copy profiles confirm exactly five copies with bytes equal to five child sizes; integrity uploads/readbacks use ordinary app commands outside that profile.

| Child size | Recovery median / max | Sampled submit p50 / p95 / max |
| --- | --- | --- |
| 32 MiB | 4.671 / 5.221 ms | 6 / 13 / 4994 us |
| 4 MiB | 1.033 / 1.356 ms | 5 / 14 / 1517 us |
| 1 MiB | 0.673 / 0.871 ms | 5 / 14 / 618 us |

See `direct-summary.json`, `batch-32-details.txt` and `batch-small-details.txt`. Transaction timing includes view creation, allocation, sparse operations, copy/wait, alias rebinding and old-resource release. Sampler maxima can miss the exact transaction peak; these are observed components, not an upper latency bound. The baseline below had different GPU load, so this is not a controlled game speedup comparison. Smaller ranges add allocation/binding metadata and have not been benchmarked in a full game working set. General graphics still conservatively restores all tracked backing on unknown draws. No game FPS or zero-spike claim follows.

## Baseline

Five sequential bounded checks per range size verified every byte using an isolated copy of the pre-change hook layer and binary in `/tmp/zvram-recovery-baseline-0.4.16`. The 32 MiB app buffer stays bounded; only one child is forced to explicit nonlocal backing. Background GPU busy was about 23–24 percent without a game/model process, and memory pressure averaged zero over 10 seconds. These are component observations under that desktop load, not isolated game FPS benchmarks.

{
  "32": {
    "count": 5,
    "median_us": 23372,
    "max_us": 27593
  },
  "4": {
    "count": 5,
    "median_us": 4782,
    "max_us": 5686
  }
}

Full logs and per-run values are retained in `baseline-summary.json` and `baseline-*-repeat-*.txt`. The hot-detection interval is not the transaction duration and can miss a promotion that completes before the polling loop.
