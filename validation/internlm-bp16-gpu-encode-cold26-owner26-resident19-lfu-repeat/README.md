# BP16 GPU-encoder full-model repeat

A second completed run using the opt-in synchronous BP16 GPU encoder finished
92 decode tokens in **83,444.66 ms** (`92,000 / 83,444.66 = 1.10252711
tokens/s`). It produced the exact output SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, offloaded
**49/49** layers, and reported zero GPU restore fallback. Minimum available RAM
was **18,030 MiB** and swap grew by **2,816 MiB**.

The 26 GiB cold/owner ceilings (separate limits), 19 GiB tracked-resident cap,
2.5 GiB reserve, LFU, 32 BP16 encoding workers, eight upload workers, and
immutable-owner validation cache match the first GPU-encoder run. The final
encoder counters recorded **4,097 calls / 135,199,260,672 raw bytes /
12,286,491,799 ns**, with zero encoder fallback and zero RAW snapshots. The
final snapshot-copy count and bytes were zero. Allocated-host input ended with
27,859,143,808 live bytes under its 27,917,287,424-byte limit. The app exited
without destroying the Vulkan device, but this build's telemetry is emitted
while process teardown is sampled; no unreported timing is inferred.

The first GPU-encoder model run measured **1.09323773 tokens/s** and this repeat
measured **1.10252711 tokens/s**. They share exact output and differ in compiled
runtime only by the telemetry-only change: first run source/runtime `72516bb`,
repeat runtime `23787c6`. This repeats the observed result, but clocks and
background activity were uncontrolled, so it does not isolate a causal speed
effect. The fresh native reference measured **1.69935367 tokens/s**; the older
0.43978 BP16 observation was under a different configuration and is not a
matched comparison. The 12-token **0.61020197 tokens/s** result remains a
separate short run. This is a full model file of 39,725,643,136 bytes (39.725
GB), not 40 GiB.

The loaded library hash, source hashes, commands, output and raw logs are
preserved in this directory. The BP16 decoder shader remains unchanged.
