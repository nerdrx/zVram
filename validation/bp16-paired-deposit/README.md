# BP16 paired-deposit shader experiment

This is a research-only shader comparison. Both shaders ran on the RX 7900 XTX
with allocated cached-host input, 16 iterations per case, full output-byte
comparison, and Vulkan validation enabled. No validation errors or VUIDs were
reported.

| Input | Baseline median decode | Paired-deposit median decode |
|---|---:|---:|
| 32 MiB model slice | 1.06816 ms | 1.05944 ms |
| 16 MiB all-mask fixture | 0.35132 ms | 0.34812 ms |
| 4,352-byte mixed fixture | 0.00316 ms | 0.00312 ms |

The real-slice difference is about 0.8%, too small to support a meaningful
model-speed claim. Keep the production decoder unchanged. The initial
16-iteration sample attempt timed out after 30 seconds; that is a diagnostic,
not a pass. Later three-iteration tiny and real-slice checks passed, followed
by the recorded 16-iteration paired runs. The debug logs are retained
separately from the successful paired results.

The paired shader source is `decode_pair.hlsl`; `decode.hlsl` is the baseline.
`vulkan_gdeflate_smoke.cpp` includes a bulk `memcmp` before locating a mismatch
byte, avoiding a slow bytewise scan of uncached mapped readback on failure.
The same exact-byte comparison remains in force. SPIR-V modules and exact
commands are included here.

Runtime provenance: research host binary SHA-256
`7497b5e3cfeaf63701df35a44e75f617ba95a126ac6973e319d299d0e073d0f7`; the
BP16 layer was `69b48069b2d60a0110b8d8297f3f9bfc6c47c9b8c9b55fa5948549cb983e4779`.
Shader hashes: baseline SPIR-V
`246b5e7f5d5893a1137e31141e7ba41b9bc2b109fae91d671e2cea3b89eff854`; paired
SPIR-V `e7d1a84fe114ebd5f142db9e8d92dfb153875842c168f7c5e42f859ee8ef75bb`;
paired HLSL `68167126054f5b18fcd642ab8cd0fe31af6b331b374ab6e32282b6f815fbc16e`.
Host smoke source SHA-256 is recorded in `SHA256SUMS.txt`.

The latest published CI snapshot checked during this archive showed Build
`37727096837` and Pages build/deployment `37727095919` both successful.
