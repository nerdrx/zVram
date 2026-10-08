# Local BP16 owner-tier 92-token run: exact output, slower rate

A 92-token InternLM2.5-20B F16 run completed with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, 49/49
layers, zero GPU restore fallback, and exit code 0. It took **85,464.02 ms**
(**1.07647639 tokens/s**). Minimum available RAM was **28,699 MiB** and swap
growth was **0 MiB**.

The experiment used a 7 GiB local-owner quota plus a 12 GiB shared/raw quota,
with a 32 GiB cold/owner ceiling, 12 GiB resident limit, and 2.5 GiB reserve.
It reached **7,516,192,768 bytes** of local-owner use. Cumulative encoded input
was **707,272,076,976 local-owner bytes** and **1,311,855,513,712 other bytes**;
these are cumulative traffic, not resident memory. The last recorded profiles
showed 75,982 restores, 79.919 s submit/wait time, and 52.009 s device decode
time. This run was slower than the prior 1.1725 tokens/s clean-first timing
profile; sequential clocks and background activity were uncontrolled, so this
is not a causal comparison or a winning configuration.

The layer binary SHA-256 was
`f495e3b3c553475b64e676f377140dd878a2508848ccbfb67080e095ea73bdec`; the
captured runtime commit was `e394ea8`. External memory-monitor samples are
preserved in the archive. Large result, stderr, and memory-log files are
gzip-compressed; `original-bytes-sha256.json` records pre-compression sizes and
hashes.
