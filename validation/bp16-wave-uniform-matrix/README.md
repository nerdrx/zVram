# BP16 wave-uniform descriptor component matrix

The research-only shader from `76c2e8c` was compared against the base BP16
shader on 32 MiB canonical frames, 16 iterations per case. Both corrected
candidate cases passed exact-byte validation with zero VUIDs. The matching
SPIR-V and shader source are archived.

| Input | Base decode median | Candidate median | Result |
|---|---:|---:|---|
| Device input | 119,000 ns | 120,520 ns | no gain |
| Direct host-visible/GTT input | 1,059,280 ns | 1,057,000 ns | effectively unchanged |

The candidate is not a production shader and these component checks do not
establish model performance. The initial `wave-*-uniform-invalid-binding.log`
files came from an incorrect compiler binding mapping and failed byte checks;
they are retained only to distinguish that invalid attempt. The corrected
compile must use explicit mappings:

```sh
dxc -T cs_6_6 -E CSMain -spirv -fspv-target-env=vulkan1.2 \
  -fvk-bind-register t0 0 0 0 \
  -fvk-bind-register u1 0 2 0 \
  -fvk-bind-register u2 0 3 0
```

`original-bytes-sha256.json` records hashes for logs, candidate source, and
SPIR-V.
