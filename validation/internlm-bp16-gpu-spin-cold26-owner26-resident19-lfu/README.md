# BP16 bounded fence-spin experiment (discarded)

A 92-token InternLM2.5-20B F16 run using the bounded 1 ms fence-polling
prototype completed in **78,778.13 ms** (`92,000 / 78,778.13 = 1.16783681
tokens/s`). It produced exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, offloaded
**49/49** layers, and had zero GPU restore fallback. Minimum available RAM was
**28,344 MiB (27.68 GiB)**; swap growth was **349 MiB**.

Fence telemetry counted **1,752 completed polls** and **54,602 blocking waits**.
The prior clean-first repeat measured **1.17068766 tokens/s**; this sequential
spin run was slightly slower, so it showed no observed speed gain. It is not a
controlled comparison. The fence-spin prototype was removed from the current
runtime; its environment flag is not a supported current CLI option.

The run used the same 26 GiB cold/owner ceilings, 19 GiB tracked residency,
2.5 GiB reserve, LFU, 32 encoder workers, eight upload workers, and
immutable-owner validation caching. Captured source/runtime metadata corresponds
to implementation commit `0237548`. The exact implementation diff from its
parent is preserved as a compressed patch with size and SHA-256 in
[the prototype test archive](../bp16-fence-spin/README.md). Large JSON and
stderr files are gzip-compressed; `original-bytes-sha256.json` records original
file sizes and hashes.
