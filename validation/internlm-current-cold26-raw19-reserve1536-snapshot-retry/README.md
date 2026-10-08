# Current-budget-snapshot run with 1.5 GiB headroom reserve

A 92-token InternLM2.5-20B F16 run completed in **127,687.03 ms**
(**0.72051171 tokens/s**) with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, 49/49
layers, and zero GPU restore fallback. Minimum available RAM was **21,448 MiB**
and swap growth was **2,242 MiB**.

This run used the opt-in admission-budget snapshot, 26 GiB cold/owner ceiling,
19 GiB resident limit, and 1.5 GiB headroom reserve. The prior 2.5 GiB-reserve
run with this option measured **0.743201 tokens/s**, while a same-headroom run
of the untouched installed binary failed during loading under changed machine
budget conditions. The current run was slower, but these sequential results do
not isolate a cause or establish a reserve-size effect. No speed claim follows.

Captured runtime commit: `1bd96d7eacc0da01515bc56a929ba50bf206ea99`. Layer binary
SHA-256: `969f3ef9f730d900554a2bc7dfa4a20dad566c85012d861e8566a11af13665ba`.
Large result, stderr, and memory samples are gzip-compressed; original sizes
and hashes are retained in `original-bytes-sha256.json`.
