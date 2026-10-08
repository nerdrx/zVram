# BP16 LFU combined-cap retry

The successful 92-token InternLM2.5-20B F16 run used immutable-owner frame
validation, LFU, 26 GiB cold and allocated-owner ceilings (separate limits), a
19 GiB tracked-resident cap, a 2.5 GiB headroom reserve, 32 encoder workers, and
eight upload workers. It completed in **103,473.50 ms** (**0.889117 tokens/s**)
with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, **49/49**
layers, and zero GPU fallback. Minimum available RAM was **18,644 MiB** and
swap grew by **4,453 MiB**.

Profiles recorded **6.259 ms** of validation time, zero BP16 upload-copy time,
and **51.993 s** of GPU decode. Final snapshot counters were **4,248** copies /
**138,672,406,528 bytes** / **14.421 s**, **51,959** clean reuses, and **3,199**
invalidations. The allocated-host input owner ended at **27,882,356,224 live
bytes** under its **27,917,287,424-byte** cap.

This is an observed combined configuration result, not an isolated optimization
comparison. Clocks and background activity were uncontrolled. The earlier
28 GiB / 21 GiB-resident / 1 GiB-reserve attempt aborted with GPU OOM and is
archived separately as a diagnostic, not as a performance result. The first
26 GiB attempt was stopped by the Ollama-GPU preflight before prompting; its
small diagnostic record is in [preflight-abort](preflight-abort/).

Runtime code commit: `dba6a62`; layer SHA-256
`69b48069b2d60a0110b8d8297f3f9bfc6c47c9b8c9b55fa5948549cb983e4779`. BP16
shader SHA-256:
`246b5e7f5d5893a1137e31141e7ba41b9bc2b109fae91d671e2cea3b89eff854`.

Artifacts: [command](automatic-command.json), [result](automatic-result.json.gz),
[resources](automatic.resources.json), [memory samples](memory.jsonl),
[stderr](automatic.stderr.txt.gz), [stdout](automatic.stdout.txt.gz), and
[source/runtime provenance](runtime-source-context.json).
