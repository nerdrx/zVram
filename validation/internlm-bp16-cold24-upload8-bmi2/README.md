# BP16 BMI2 short full-model run

This 12-token InternLM2.5-20B F16 run used the rebuilt BMI2-inline BP16 encoder, 24 GiB cold quota, 19 GiB tracked cap, 2.5 GiB reserve, 32 BP16 encoding workers, and eight upload workers. It completed in **20,204.26 ms** (**0.59393415 tokens/s**), with exact output (SHA-256 `8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), **49/49** layers, and zero GPU fallback. Child allocator settings: child allocator thresholds unset (MALLOC_ARENA_MAX=2 only). Minimum available RAM was **18,296 MiB** and swap grew by **923 MiB**.

The runtime was built from `d453be4` (BMI2 encoder plus profiling); layer SHA-256 `7135d7a577af73123c414b29fd5e2519bbc0cc8e1d5da361613a19a5ce264157`; production shader SHA-256 `246b5e7f5d5893a1137e31141e7ba41b9bc2b109fae91d671e2cea3b89eff854`. The launch source commit and per-file hashes are recorded separately. Cumulative profiles recorded **5.284 s** direct-copy and **7.813 s** GPU decode time; these are restoration-phase context, not inference-time attribution.

This is a sequential run with unlocked clocks and background activity. The 12-token rate is below the earlier 0.61020197 tokens/s short-run best, which used a different runtime build. It does not change the headline short-run result or defaults.

[Command](command.json), [result](result.json.gz), [resource samples](automatic.resources.json), [stderr](automatic.stderr.txt.gz), [stdout](automatic.stdout.txt.gz), [runtime hashes](runtime-binary-sha256.json), and [source revision](source-commit.txt).
