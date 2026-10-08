# Discarded three-millisecond BP16 restore fence-spin trial

A 92-token InternLM2.5-20B F16 run enabled the opt-in 3 ms bounded polling
prototype. It completed in **78,357.64 ms** with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, 49/49
layers, zero GPU restore fallback, and exit code 0. Minimum available RAM was
**28,944 MiB** and swap growth was **245 MiB**.

Across **54,530** restores, polling completed **53,065** times and fell back to
blocking **1,465** times (97.3% completed polls). Host timing was **0.27356 s**
queue-submit and **73.9426 s** fence-wait; GPU device time was **0.26545 s**
transfer, **52.6685 s** decode, and **0.51993 s** finish. Against the recent
non-spin timing baseline (78,464.32 ms), this is no meaningful observed gain.
The large blocking-wait share weakens the wakeup hypothesis, but does not
identify the wait's cause.

The profile otherwise kept the clean-first BP16 configuration: 26 GiB shared
cold/owner ceiling, 19 GiB tracked residency, 2.5 GiB reserve, LFU, 32 encoder
workers, eight upload workers, and immutable-owner validation caching. The spin
prototype has been removed from the current runtime. Its implementation patch
is retained as `implementation.patch.gz`; the original run files' sizes and
hashes are in `original-bytes-sha256.json`.

The corresponding focused CTest group passed **20/20** (12 CPU, 8 GPU) in
10.86 s; details are archived at
[focused spin checks](../../bp16-restore-spin3/README.md).
