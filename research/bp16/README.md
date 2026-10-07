# BP16 GPU decoder research

`decode.hlsl` is a bounded lossless decoder for the frame in [`FORMAT.md`](FORMAT.md).
It writes two 16-bit words per invocation and uses 256 threads per workgroup.
The smoke host requires `bp16::validate()` to accept the complete frame and the
expected output size before dispatch. Shader bounds failures set the shared
sticky scratch error bit. Component comparisons are recorded in
[`validation/bp16-component`](../../validation/bp16-component). They measure decoder
work only, not whole-model inference or application frame rates. The production
layer supports opt-in BP16 snapshots and GPU restoration.

The shader reuses `research/gdeflate/vulkan_gdeflate_smoke.cpp` resource,
validation, fence, and readback lifetime through an opt-in codec selector. The
existing GDeflate command syntax and default path are unchanged.

## Build and CPU preflight

The checked-in SPIR-V was generated with the local DXC at
`build/third-party/gdeflate-research/dxc/bin/dxc`, version
`1.9(5191-d355aa83)(1.9.0.5191)`:

```sh
build/third-party/gdeflate-research/dxc/bin/dxc \
  -T cs_6_6 -E CSMain -spirv -fspv-target-env=vulkan1.2 \
  -fvk-bind-register t0 0 0 0 -fvk-bind-register u1 0 2 0 \
  -fvk-bind-register u2 0 3 0 \
  research/bp16/decode.hlsl -Fo research/bp16/decode.spv
spirv-val --target-env vulkan1.2 research/bp16/decode.spv
```

Compile the reusable smoke host outside the repository build tree, then run
only structural CPU preflight with:

```sh
c++ -std=c++17 -I. research/gdeflate/vulkan_gdeflate_smoke.cpp \
  -lvulkan -o /tmp/zvram-gdeflate-bp16-host
/tmp/zvram-gdeflate-bp16-host --codec bp16 --preflight-only \
  research/bp16/decode.spv \
  build/bp16-research/internlm2_5-20b-chat-fp16-64MiB.bp16 \
  build/bp16-research/internlm2_5-20b-chat-fp16-64MiB.raw
```

The GPU path is opt-in; the following mixed-pattern check passed three iterations
with full-byte equality and no validation errors:

```sh
c++ -std=c++17 -O2 -I. research/bp16/gpu_fixture.cpp \
  -o /tmp/bp16-gpu-fixture
/tmp/bp16-gpu-fixture build/bp16-research
ZVRAM_RESEARCH_ITERATIONS=3 /tmp/zvram-gdeflate-bp16-host \
  --codec bp16 --gpu-bounded-smoke research/bp16/decode.spv \
  build/bp16-research/gpu-mixed-pattern.bp16 \
  build/bp16-research/gpu-mixed-pattern.raw
```

`ZVRAM_RESEARCH_ITERATIONS` accepts decimal values 1 through 16 and defaults
to 1. It controls both recorded query slots and submitted decode/readback
iterations. The mixed fixture has one block for each `k=0..16`, varied sparse
masks and nonzero bases, and a final payload that ends at the frame boundary.

Shader source SHA-256: `bce95b495b0b3954e852849290f9ca30ac843241300324197bc5600032da6072`.
SPIR-V SHA-256: `246b5e7f5d5893a1137e31141e7ba41b9bc2b109fae91d671e2cea3b89eff854`.
