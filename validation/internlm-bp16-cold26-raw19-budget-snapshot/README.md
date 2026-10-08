# 26 GiB cold/owner budget-snapshot model run

A 92-token InternLM2.5-20B F16 run completed in **123,783.66 ms**
(**0.743201 tokens/s**), with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, 49/49
layers, and zero GPU restore fallback. Minimum available RAM was **18,126 MiB**;
swap grew **1,816 MiB**.

This enabled the optional single-locked-queue-call admission budget snapshot
with the same 26 GiB cold/owner, 19 GiB resident, and 2.5 GiB reserve profile.
A subsequent run of the previously validated installed binary with the same
headroom settings failed to load with Vulkan out-of-device-memory. Machine
budget changed between these runs, so the slower 0.7432 rate versus earlier
runs cannot be attributed to this code or compared as a controlled result.
No speed claim follows.

The runtime commit was `e12792c`; the captured layer binary SHA-256 is
`969f3ef9f730d900554a2bc7dfa4a20dad566c85012d861e8566a11af13665ba`. Original
hashes and sizes are in `original-bytes-sha256.json`; large result/stderr/memory
files are gzip-compressed.
