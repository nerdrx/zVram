# Qwen 27B Q4 BP16 raw-owner OFF/ON pair

Four alternating trials used the same compiled library and prompt, with raw
host input OFF/ON/OFF/ON. All completed 94 decode runs, offloaded 66/66 layers,
and produced identical stdout SHA-256
`ab00fd0d30f1e0c2eb8dd044f34a85228adc47951e04517bd24d53092b0a5b9d`.

| Run | Raw input | Decode rate | Minimum available RAM | Swap growth |
| --- | --- | ---: | ---: | ---: |
| off1 | Off | 1.78 tokens/s | 25,638 MiB | 2,305 MiB |
| on1 | On | 2.29 tokens/s | 27,205 MiB | 1,482 MiB |
| off2 | Off | 1.75 tokens/s | 27,838 MiB | 2,072 MiB |
| on2 | On | 2.29 tokens/s | 28,113 MiB | 88 MiB |

The OFF rates average 1.765 tokens/s and the ON rates average 2.29 tokens/s,
about 30% higher measured decode rate in this small same-binary Q4 pair. The
runtime library SHA-256 was
`04c6d673e812e7ebee1922e6e4b8fe43580bb663a44ed39b67d14788b2bf87d2`.
The run used a forced 12 GiB tracked-residency limit and 8 GiB cold/owner
ceilings. Profiling showed about 20.8 seconds of CPU decode work in OFF runs
and zero in ON runs, while cumulative copy wait remained about 40 seconds in
ON runs. The RAW owner skips repeated CPU staging; it does not remove the
GTT-to-VRAM transfer.

The rates are run observations, not an isolated causal result. Desktop activity
was uncontrolled, and swap grew by 88–2,305 MiB across the four runs while
minimum available RAM ranged from 25,638 to 28,113 MiB. Native Vulkan's separate
34.56-token/s run is a capacity baseline, not a matched throughput comparison.
This Q4 result does not establish a general application speedup, other-model
performance, or full-model fit.

`result.json.gz`, `memory.jsonl.gz`, compressed stdout/stderr logs, per-run
filtered environments and resource samples, command, source patch, and
provenance are preserved here. `SHA256SUMS` covers every archive file except
itself; verify with `sha256sum -c SHA256SUMS` from this directory.
