# BP16 restore and sparse-remap batching: full-model run

This opt-in run combined four-frame BP16 GPU restore batches with a single
post-decode sparse remap transaction, plus synchronous GPU BP16 snapshot
encoding. It completed 92 decode runs in **83,382.74 ms**
(`92,000 / 83,382.74 = 1.10334585 tokens/s`), with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, **49/49**
layers offloaded, zero GPU restore fallback, and no errors. Minimum available RAM
was **18,027 MiB** and swap grew by **2,888 MiB**. Final telemetry recorded
**14,957 restore batch submissions / 50,894 frames**, and the same count for
combined remap calls / child frames.

The prior encoder repeat without these batching options measured **1.10252711
tokens/s**; this run is effectively the same rate. Host submit/wait time was
**70.54 s** versus **70.20 s**. These sequential runs had uncontrolled clocks and
background activity, so they do not establish a speed gain or isolate cause.
Both batching options remain opt-in and default off.

The run used 26 GiB cold/owner ceilings, 19 GiB tracked residency, a 2.5 GiB
reserve, LFU, 32 encoder workers, eight upload workers, and immutable-owner
validation caching. Captured runtime/source metadata is included; do not infer
source from current checkout files. Large JSON and stderr files are gzip
compressed. `original-bytes-sha256.json` records original sizes and SHA-256 values
before compression.
