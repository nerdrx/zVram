# BP16 GPU-encoder full-model run (pending repeat)

An opt-in GPU-encoder run completed 92 decode tokens in **84,153.70 ms**;
`92,000 / 84,153.70 = 1.09323773 tokens/s`. It produced the expected output
SHA-256 `b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`,
offloaded **49/49** layers, and reported zero GPU restore fallback. Minimum
available RAM was **17,610 MiB**, swap grew by **4,806 MiB**. The model file is
39.725 GB (39,725,643,136 bytes), not 40 GiB.

The run used the experimental synchronous BP16 GPU encoder with the existing
26 GiB cold/owner ceiling, 19 GiB resident cap, 2.5 GiB reserve, LFU, 32 CPU
encoder workers, eight upload workers, and immutable-owner validation cache.
The encoder metrics normally emitted at device teardown are absent because the
model app did not destroy the Vulkan device before exit; no encode host-time or
per-call total is inferred here. The captured state records 4,382 cold-freeze
events and zero async commits. This is one completed run, pending an exact
repeat; it does not replace the current headline until reproduced. Clocks and
background activity were uncontrolled, so the combined configuration is not a
causal comparison.

Runtime source commit: `72516bb0d58e658e10796bb5d589496421c65d90`. The loaded
layer SHA-256 is in `runtime-binary-sha256.json`; per-run source hashes and the
unchanged BP16 decoder shader hash are in `automatic-result.json` and
`runtime-source-context.json`. Raw commands, logs, memory samples and fdinfo
are preserved alongside the result.
