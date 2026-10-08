# InternLM2.5-20B F16 native Vulkan reference refresh

The unwrapped llama-completion app ran without the zVram layer, snapshot
compression, paging, or resident cap. It completed 92 decode runs in
**54,138.23 ms** (**1.699354 tokens/s**), with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b` and
**49/49** layers offloaded. Minimum available RAM was **29,075 MiB** and swap
grew by **52 MiB**.

This is a fresh native reference for the same prompt and model used in the
wrapped runs. Runs were sequential with unlocked clocks; this result is not a
controlled performance comparison with a wrapped configuration.

The captured application SHA-256 was
`5a927490be2eb10b387e4246442b2f9c4ca6f01fc42d4f97ee98e5bc04d0be9d`. The
runtime context records the unchanged **39,725,643,136-byte** model and
child-only allocator thresholds.

Artifacts: [summary](result.json), [command](native-command.json),
[resources](native.resources.json), [memory samples](memory.jsonl),
[stderr](native.stderr.txt.gz), [stdout](native.stdout.txt.gz), and
[runtime context](runtime-context.json).
