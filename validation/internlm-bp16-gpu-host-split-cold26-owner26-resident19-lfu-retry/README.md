# Clean-first restore submission/wait split

A 92-token InternLM2.5-20B F16 run completed with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, 49/49
layers, zero GPU restore fallback, and exit code 0. It took **78,464.32 ms**. Minimum available RAM was
**28,802 MiB** and swap growth was **94 MiB**.

The restore timing profile recorded **54,558 restores**: queue-submit time was
**0.266484886 s**, fence-wait time **74.542501048 s**, and combined host time
**74.811437618 s**. Device profiling recorded **0.26544932 s** transfer,
**52.6819636 s** decode, and **0.51993216 s** finish. Fence waiting dominates
the measured host restore time; these counters do not prove why the wait is
long or establish a performance gain.

The run used the existing clean-first BP16 profile: 26 GiB shared cold/owner
ceiling, 19 GiB tracked residency, 2.5 GiB reserve, LFU, 32 encoder workers,
eight upload workers, and immutable-owner validation caching. The captured
runtime is commit `45bd211`. An external memory-monitor sidecar produced no
samples because its earlier deadline had expired; the run's internal RAM,
swap, and Ollama guards remained active and resource records were captured.

The first launch was stopped at preflight because Ollama had a 9B model using
about 6.1 GB VRAM. That attempt did not prompt or yield a model result; the
model was unloaded before the successful retry. Raw JSON and stderr are
compressed, with pre-compression hashes and sizes in
`original-bytes-sha256.json`.
