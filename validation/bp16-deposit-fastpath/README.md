# BP16 deposit fastpath validation archive

This archive records the isolated candidate experiment. It does not replace
the production `research/bp16/decode.hlsl` or `research/bp16/decode.spv`.
The candidate adds contiguous-mask and low-contiguous-run-plus-bit-15 deposit
paths, retaining the original per-bit loop as fallback.

## Results

All bounded Vulkan cases below ran on AMD Radeon RX 7900 XTX (RADV NAVI31),
with validation enabled. Each of the six 32 MiB matrix runs decoded the exact
32 MiB output across three iterations with zero validation errors/VUIDs. The
two direct-host pairs showed small decode-time changes: 1,064,560 to 1,049,040
ns (about 1.5%) and 1,060,560 to 1,051,160 ns (about 0.9%). The device-input
pair changed from 117,800 to 72,880 ns (about 38.1% lower).

On the exact 16 MiB all-mask fixture, all three input modes passed exact-byte
checks with zero validation errors/VUIDs. The separate three-iteration
device-input A/B measured 58,800 ns baseline and 78,560 ns candidate decode
(about 33.6% slower). The mixed-pattern fixture also passed in device, direct
host, and allocated-host modes, with zero validation errors/VUIDs. These are
bounded shader component measurements; results vary by workload and do not
establish snapshot or full-model speedup.

The candidate passed the CPU deposit equivalence check for all 65,536 masks
with five fixed and eight deterministic random gathered values per mask.
Baseline and candidate HLSL both compiled with local DXC 1.9 and passed
`spirv-val --target-env vulkan1.2`. The candidate SPIR-V SHA-256 is
`43e49ac77c0e6adef9db24dadc6df889a285cc0e9505f03dbbf9ba80e1445ab4`.

## Included evidence

- `baseline.hlsl`, `baseline.spv`, `candidate.hlsl`, `candidate.spv`: shader
  sources and modules; baseline hashes match the production research shader
  at archive time.
- `deposit_check.cpp`, `deposit_check.log`: exhaustive CPU formula comparison.
- `all_masks_fixture.cpp`: generator source only. The generated raw and BP16
  payload files are intentionally omitted; their hashes are recorded below.
- `00-...log` through `05-...log` and `gpu-matrix.json`: six three-iteration
  32 MiB baseline/candidate timing runs.
- `exact-all-masks-...log`, `exact-mixed-...log`, `all-mask-gpu.json`: six
  one-iteration exact-output runs across device, direct-host, and allocated-
  host input modes.
- `baseline-all-masks-timing.log`, `candidate-all-masks-timing.log`,
  `all-mask-timing.json`: separate three-iteration all-mask A/B.

## Fixture and host provenance

The 32 MiB real fixture payloads are not copied into this archive. SHA-256 at
test time:

```text
8a67e5cb4721587afbd3c22104e5cbac52f6460f5db9e9117e939f5186a80895  real-fixture.raw
2b519249c04122e43696dd164d039348af90890c0f47745dd46463ede667cf5a  real-fixture.bp16
```

Research host provenance:

```text
29477a9315e9f01b6a7482f93edfb2f89f595eea0315a2efabffb0b99a1002a1  research/gdeflate/vulkan_gdeflate_smoke.cpp
49513fd22718c1e11511a531b0279077853df1fd01abb810f960696e15aac68d  zvram-gdeflate-research
```

All-mask fixture hashes (fixture files omitted):

```text
8a6a3a86d1ac9fa29366e46239252ecb8c98146ef16bf811597b0b47e8aeb77f  all-masks.raw
46136f70500c5f31ac78718247d0ad74cfc4f0199809a256eee1160f73e3ce34  all-masks.bp16
```
