# Clean-first 28 GiB shared cold/owner-cap run

A 92-token InternLM2.5-20B F16 run used clean-first victim ranking and GPU BP16
encoding with a **28 GiB shared cold/owner ceiling**, 19 GiB tracked residency,
and a 2.5 GiB reserve. It completed in **78,341.41 ms**
(`92,000 / 78,341.41 = 1.17434700 tokens/s`), with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, 49/49
layers, and zero GPU restore fallback. Minimum available RAM was **29,057 MiB
(28.38 GiB)** and swap growth was **100 MiB**.

The final cold state recorded **18,356,633,600 logical bytes / 14,738,514,208
stored bytes**, **679,491,504 bytes** clean-cache storage, 54,277 clean reuses,
211 invalidations, 55,066 freezes, and 55,682 restores with no snapshot failures.
GPU restore host time was **74.72404 s** and device decode time **52.71852 s**.
The run recorded **1,452,239,431,904 encoded-input bytes**. This build reported
no encoder-call counter.

The prior clean-first 26 GiB run measured **1.17068766 tokens/s**. The 28 GiB
run is only about **0.31% higher**, with uncontrolled sequential clocks and
background activity; this is no meaningful observed gain. Keep the 26 GiB
profile as the recommendation. Captured runtime/source metadata corresponds to
lean commit `ceabc25`. Large JSON/stderr files are gzip-compressed;
`original-bytes-sha256.json` preserves pre-compression sizes and hashes.
