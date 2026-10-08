# Clean-first 26 GiB run with 16 llama.cpp nodes per submit

A single 92-token InternLM2.5-20B F16 run used `GGML_VK_MAX_NODES_PER_SUBMIT=16`
with the same clean-first zVram profile as the 4-node runs. It completed in
**81,065.47 ms** (`92,000 / 81,065.47 = 1.13488517 tokens/s`), with exact stdout
SHA-256 `b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`,
49/49 layers, and zero GPU restore fallback. Minimum available RAM was
**28,940 MiB (28.26 GiB)** and swap growth was **974 MiB**.

The run retained the 26 GiB shared cold/owner ceiling, 19 GiB tracked residency,
2.5 GiB reserve, LFU, 32 BP16 encoder workers, eight upload workers, and the
clean-first cache policy. Runtime/source metadata corresponds to lean commit
`ceabc25`; only the llama.cpp nodes-per-submit setting changed from 4 to 16.

The previous 4-node clean-first repeat measured **1.17068766 tokens/s**, so this
single 16-node run was slower. Runs were sequential with uncontrolled clocks and
background activity; this does not establish a causal effect. Keep 4 as the
recommendation. The raw result and stderr are gzip-compressed, with original
sizes and SHA-256 hashes recorded in `original-bytes-sha256.json`.
