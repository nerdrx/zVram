# BP16 clean-cache replacement policy long runs

These sequential 92-token InternLM2.5-20B F16 runs compared opt-in clean-snapshot
cache replacement policies. All completed with identical stdout (SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`), 49/49
layers offloaded, and zero GPU fallback:

| Cache policy | Decode time | Rate | Minimum available RAM | Swap growth |
|---|---:|---:|---:|---:|
| LRU | 180,787.78 ms | 0.508884 tokens/s | 19,976 MiB | 4,122 MiB |
| MRU | 299,827.77 ms | 0.306843 tokens/s | 19,860 MiB | 5,174 MiB |
| First (default) | 199,795.01 ms | 0.460472 tokens/s | 20,270 MiB | 2,052 MiB |

All three policies were explicitly accepted by the layer. The default run used
`ZVRAM_VULKAN_CLEAN_CACHE_POLICY=first`; the other runs explicitly selected
`lru` and `mru`. This variable selects clean-snapshot replacement; all runs
independently kept range eviction at `--vulkan-eviction-policy mru`. They used
32 BP16 encoder workers, eight upload workers, a 24 GiB cold quota, a 19 GiB
tracked-residency cap, and 2.5 GiB headroom.

All three runs produced identical stdout (SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`), offloaded
49/49 layers, and had zero GPU fallback. They were sequential with unlocked
clocks and uncontrolled background activity. The observed rates do not isolate
a policy effect or justify changing the default. All three also measured below
the separate 0.5727251 tokens/s 32-worker historical observation; that is not a
controlled comparison either.

Each policy directory includes the command, result, stderr, resource and process
memory samples, fdinfo, source/runtime provenance, and child runner. The source
and binary hashes are preserved separately; see each `runtime-source-context.json`.
