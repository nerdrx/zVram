# 20B F16 run with 1 GiB clean-cache cap and raw-owner opt-in

A full 92-token InternLM2.5-20B F16 inference completed with exact stdout
SHA-256 `b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`,
49/49 layers offloaded, decode time 89,156.85 ms, and a reported rate of
**1.031889 tokens/s**. GPU restore fallback count was zero. The minimum
available RAM was 18,205 MiB; system swap grew by 6,161 MiB (27,655 to
33,816 MiB).

The run used a 1 GiB clean-snapshot cache cap, 26 GiB cold/owner ceilings,
19 GiB tracked residency, and a 1.5 GiB headroom reserve. Runtime library
SHA-256: `04c6d673e812e7ebee1922e6e4b8fe43580bb663a44ed39b67d14788b2bf87d2`.
The 1 GiB cache-cap run completed, but the BP16 raw-owner encoder path was not
exercised: the profile recorded zero RAW snapshots and all snapshots were
compressed. This is not evidence of a raw-owner performance gain.

For context, this result is below the separate native 92-token reference at
1.46258 tokens/s and the historical clean-first results at about 1.17 tokens/s.
Those runs were sequential under different memory and clock conditions; they
do not isolate the cache cap, raw-owner path, or any single cause. This run
does not establish broad application performance or full-model fit.

`automatic-result.json.gz` and `memory.jsonl.gz` preserve the large JSON data
in compressed form; the 60 MiB stderr log is also gzip-compressed. `run.py`
reconstructs the filtered environment and command without duplicating a full
environment dump. The source patch, provenance, resource sample, command, and
fdinfo snapshots are included. `SHA256SUMS` covers every archive file except
itself; verify with `sha256sum -c SHA256SUMS` from this directory.
