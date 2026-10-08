# Fused-four BP16 component matrix

The research smoke decoded four distinct canonical 32 MiB BP16 payloads in
16 iterations for each storage/dispatch variant. All four variants returned
exact bytes and reported zero validation errors/VUIDs. The shader and host
sources are from commit `39444d7`; the matching built SPIR-V and its hash are
included.

| Input memory | Dispatches | Median GPU decode |
|---|---:|---:|
| Local, one frame | 1 | 0.72232 ms |
| Local, four frames | 1 fused dispatch | 0.74980 ms |
| GTT, one frame | 1 | 4.23748 ms |
| GTT, four frames | 1 fused dispatch | 4.28348 ms |

The four-frame total decode median was about 3.8% above the one-frame median
for local input and 1.1% above for GTT input. The cases process different
amounts of data, so this is a component observation, not normalized throughput
or a matched model test. It makes no full-model speed claim; the smoke verifies
correctness and records component timing only. It remains research-only. `original-bytes-sha256.json` verifies all logs,
sources, and SPIR-V.
