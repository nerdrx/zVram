# BP16 clean-first single model run

A single 92-token InternLM2.5-20B F16 run used the clean-first resident victim
preference with synchronous GPU BP16 encoding. It completed in **78,652.76 ms**
(`92,000 / 78,652.76 = 1.16969830 tokens/s`), with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, **49/49**
layers offloaded, and zero GPU restore fallback. Minimum available RAM was
**29,113 MiB (28.43 GiB)**; swap growth was **0 MiB**.

The run recorded **891 GPU encodes / 29,639,376,896 raw bytes / 2.7809621 s
encoder host time**, zero encoder fallback, and zero RAW snapshots. Host restore
time was **74.63998 s**; device decode time was **52.67389 s**. It used the 26 GiB
cold/owner ceilings, 19 GiB tracked residency, 2.5 GiB reserve, LFU, 32 encoder
workers, eight upload workers, and immutable-owner validation caching.

The prior GPU-encoder repeat measured **1.10252711 tokens/s**; this single
clean-first run is numerically about **6.09% higher**. Runs were sequential with
uncontrolled clocks and background activity. This does not establish that
clean-first caused the difference; a same-profile repeat is pending.

Captured source/runtime metadata corresponds to compiled commit `3c76d7d`; do
not use the current checkout's source hashes as provenance for this run. Large
JSON and stderr files are gzip-compressed. `original-bytes-sha256.json` records
the pre-compression sizes and hashes.
