# BP16 four-frame GPU restore batching: full-model run

This opt-in run combined `ZVRAM_VULKAN_BP16_RESTORE_BATCH=1` with synchronous
GPU BP16 snapshot encoding. It completed 92 decode runs in **84,185.48 ms**
(`92,000 / 84,185.48 = 1.09282503 tokens/s`), with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, **49/49**
layers offloaded, and zero GPU restore fallback. Minimum available RAM was
**16,669 MiB** and swap growth was **2,443 MiB**. Telemetry recorded
**15,074 batch submissions / 51,174 frame items**.

The comparable encoder repeat measured **1.10252711 tokens/s**, with device
restore time **52.10 s** versus **52.10 s** for this batch run and host restore
time **70.20 s** versus **70.20 s**. The batch run shows no observed throughput
gain. These are sequential runs with uncontrolled clocks/background activity,
not a controlled performance comparison. Batching remains opt-in and defaults
off.

The run used the 26 GiB cold/owner ceilings, 19 GiB tracked-resident cap,
2.5 GiB reserve, LFU, 32 encoder workers, eight upload workers, and immutable
owner validation cache. Full command, runtime/source hashes, resources, memory
samples, and output are preserved here. Large JSON and stderr files are gzip
compressed; `original-bytes-sha256.json` records hashes and sizes before
compression.
