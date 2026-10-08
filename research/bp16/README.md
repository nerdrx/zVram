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

## Transfer and allocation experiments

The smoke host accepts BP16-only `--host-input`, `--fresh-output`, and
`--robust-access2` switches. The first lets the shader read a coherent host
buffer directly; its GPU decode timing includes PCIe reads. The second replaces
the output buffer after each completed fence. The third enables supported
robust buffer access 2, matching the strict production configuration.
All switches are opt-in; GDeflate and the default BP16 component path retain
the existing behavior. Disable unrelated implicit layers when reproducing:
`VK_LOADER_LAYERS_DISABLE='~implicit~'`.

Sequential 32 MiB component runs passed full-byte comparisons and validation.
Direct host input measured 1.062 ms GPU decode without an upload copy; device
input measured 1.130 ms upload plus 0.118 ms decode. This is a small component
transfer difference, not a measured inference gain. Twelve-iteration median
host submit/wait times were 3.298 ms with reused output and 3.098 ms with fresh
output, so these runs did not establish an allocation reuse advantage.
Robust access 2 measured 0.122 ms decode over three iterations; these results
do not support disabling robustness to explain the full-model delay.
[Logs and scope](../../validation/bp16-transfer-experiments/summary.json).

Production restoration profiling is opt-in through
`ZVRAM_VULKAN_GPU_PROFILE=1`. It records CPU validation, input preparation,
submit/wait and backing allocation/free/sparse-bind costs. Where native queue
timestamps are supported, it also records GPU transfer, compute and finish
intervals. Counters are cumulative, and sparse-bind time includes its existing
wait. GPU query failure disables GPU timing; restoration safety and its normal
fallback behavior remain unchanged.

The production BP16 decoder now omits its redundant full-output clear: canonical
frames are CPU-validated, and every output word is overwritten. GDeflate keeps
its clear, and both paths retain the sticky error check and poisoned lifetimes.
The opt-in `ZVRAM_VULKAN_BP16_HOST_INPUT=1` makes BP16 restoration read a coherent
host storage buffer directly, instead of copying compressed input to VRAM.
It requires a host-visible/coherent memory type without DEVICE_LOCAL; unsupported
hardware retains the existing CPU fallback. Default BP16 still uses device input.
GPU decode timing in this mode includes reads from host memory. Eight GPU checks
passed in each mode, including zero/nonzero output, native/synthetic range,
resident-pressure, queue lifetime and partial-restore cases.
[Regression evidence](../../validation/bp16-host-input/summary.json).

The separate smoke-only `--import-host-input` experiment imports a BP16 frame
from an owned aligned host allocation with `VK_EXT_external_memory_host`; it
does not import `std::vector` storage. It requires an importable storage buffer,
the reported pointer alignment and compatible HOST_VISIBLE|HOST_COHERENT
non-device-local memory type; unsupported devices fail before dispatch. The
allocation stays immutable and alive through the fence, and is intentionally
retained on timeout/device loss. This does not guarantee driver pinning or
zero-copy behavior. Bounded RX 7900 XTX checks passed for a mixed 4,352-byte
frame and a 32 MiB F16 slice (three exact-byte iterations each, no validation
errors). The 32 MiB GPU decode median was 1.064 ms, including host reads; this
is component evidence, not an inference speed result.
[Evidence](../../validation/bp16-import-host-input/summary.json). Try it only with the
bounded smoke command:

```sh
/tmp/zvram-bp16-import-host-smoke --codec bp16 --import-host-input \
  --gpu-bounded-smoke research/bp16/decode.spv \
  build/bp16-research/gpu-mixed-pattern.bp16 \
  build/bp16-research/gpu-mixed-pattern.raw
```
