# Local-owner budget-snapshot retry: exact output, no accepted speed gain

The 92-token InternLM2.5-20B F16 run completed in **109,519.77 ms** with exact
stdout SHA-256 `b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`,
49/49 layers, and zero GPU restore fallback. Minimum available RAM was
**21,342 MiB** and swap growth was **2,977 MiB**.

This retried the 3 GiB local-owner + 16 GiB shared/raw configuration with a
2.5 GiB reserve after the earlier load attempt failed. The run used a 32 GiB
cold/owner ceiling (the comparison baseline used 26 GiB), so its **0.84003098
tokens/s** versus the prior **1.1725 tokens/s** is confounded and does not show
a causal change.

The experimental admission-budget snapshot option takes one driver-budget
sample per locked queue call and supplies a conservative estimate for the
existing fresh retry. It is off by default. Worker-side and native-owner paths
still query fresh budgets; the actual resident hard cap remains enforced, and
the reserve estimate is not a reservation. The runtime commit was `7a0d489`;
the layer binary SHA-256 was
`969f3ef9f730d900554a2bc7dfa4a20dad566c85012d861e8566a11af13665ba`.
Large result, stderr, and memory samples are gzip-compressed, with original
hashes and sizes retained.
