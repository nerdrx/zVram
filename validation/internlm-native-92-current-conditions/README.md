# Native 92-token reference under current machine conditions

The native Vulkan run completed 92 decode tokens in **60,018.45 ms**
(**1.532863 tokens/s**) with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, 49/49
layers, and exit code 0. Minimum available RAM was **19,926 MiB** and swap
growth was **2,176 MiB**. The Ollama GPU guard detected no model.

This is a fresh reference with no zVram paging, snapshots, compression, or
resident cap. A prior fresh native observation measured **1.6991 tokens/s**;
these sequential runs occurred under different machine-budget conditions and
do not identify a cause. Do not treat the difference as a controlled native
regression or use it to explain compressed-run rates.

The llama.cpp executable SHA-256 is
`5a927490be2eb10b387e4246442b2f9c4ca6f01fc42d4f97ee98e5bc04d0be9d`. The full
command, resource samples, and original file hashes are preserved here.
