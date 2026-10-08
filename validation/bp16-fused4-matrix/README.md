# Fused-four BP16 component matrix

The research smoke decoded four distinct canonical 32 MiB BP16 payloads in
16 iterations for each storage/dispatch variant. All four variants returned
exact bytes and reported zero validation errors/VUIDs. The shader and host
sources are from commit `39444d7`; the matching built SPIR-V and its hash are
included.

| Input memory | Frames per iteration | Dispatches per iteration | Median GPU decode |
|---|---:|---:|---:|
| Local | 4 | 1 fused | 0.72232 ms |
| Local | 4 | 4 separate | 0.74980 ms |
| GTT | 4 | 1 fused | 4.23748 ms |
| GTT | 4 | 4 separate | 4.28348 ms |

All four variants process the same four distinct 32 MiB frames per iteration.
The fused dispatch measured about 3.7% lower GPU decode time for local input
and 1.1% lower for GTT input than four separate dispatches. This is component
timing only; it does not establish a model-level speed gain. The smoke verifies
correctness and remains research-only. `original-bytes-sha256.json` verifies all
logs, sources, and SPIR-V.
