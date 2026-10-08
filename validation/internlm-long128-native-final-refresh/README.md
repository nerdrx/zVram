# Fresh native InternLM2.5-20B F16 reference

A native Vulkan run without zVram paging, snapshot compression, or a tracked
resident cap completed the same 92-token prompt in **54,145.45 ms**
(`92,000 / 54,145.45 = 1.6991 tokens/s`, reported as **1.70**). It produced
stdout SHA-256 `b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`,
offloaded **49/49** layers, and exited normally. Minimum available RAM was
**27,722 MiB** and swap growth was **34 MiB**.

The clean-first GPU-encoder runs measured **1.16969830** and **1.17068766
tokens/s** on the same prompt with identical output. Relative to this native
rate, the compressed runs were about **31% slower by rate** (about **45% more
time per token**). These sequential runs had uncontrolled clocks and background
activity and are not a controlled comparison.

The command, run script, resource samples, exact output, and per-file SHA-256
manifest are preserved here. The llama-completion executable hash is recorded
separately; its mtime predates the run. The model file is not copied or hashed.
