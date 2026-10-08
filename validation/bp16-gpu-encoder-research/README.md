# Research BP16 GPU encoder

This is a bounded research prototype, not a production codec path. The analyze
shader computes one block's `base` and `varyingMask`; the CPU writes the
canonical frame header and descriptors; the pack shader independently writes
payload dwords. Existing BP16 decode behavior and the smoke host's default
commands are unchanged.

The first real-frame attempt is retained as
[`first-loader-error.log`](first-loader-error.log), but is excluded from clean
validation: the implicit Lossless Scaling layer failed to load
`vkGetInstanceProcAddr`. Clean runs disabled implicit layers for the child
process, selected the Radeon ICD, removed `ROCPROFILER_REGISTER_*` variables,
and enabled Khronos validation with synchronization validation. The selected
device was an AMD Radeon RX 7900 XTX (RADV NAVI31), Mesa RADV 26.2.4; validation
reported zero errors and zero VUIDs in every clean run.

The command form was:

```sh
VK_LOADER_LAYERS_DISABLE='~implicit~' \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json \
VK_LAYER_VALIDATE_SYNC=1 ZVRAM_RESEARCH_ITERATIONS=1 \
timeout --signal=TERM 30s /tmp/zvram-bp16-encoder-smoke \
  --codec bp16 --gpu-encode-bounded-smoke \
  research/bp16/encode_analyze.spv research/bp16/encode_pack.spv \
  RAW.bin EXPECTED.bp16
```

The local DXC 1.9 compiled both shaders for Vulkan 1.2, and `spirv-val
--target-env vulkan1.2` accepted both modules. The smoke host was compiled with
`c++ -std=c++17 -O2 -w -I. research/gdeflate/vulkan_gdeflate_smoke.cpp
-lvulkan -pthread`. CPU preflight accepted the real 32 MiB fixture and the
existing decoder preflight remained valid.

| Fixture | Raw / expected frame | GPU encode total | Matched CPU path total |
| --- | ---: | ---: | ---: |
| 32 MiB F16, 1 iteration | 33,554,432 / 29,202,816 bytes | 5.154 ms | 10.356 ms |
| 32 MiB F16, 3 iterations | 33,554,432 / 29,202,816 bytes | 4.876 / 4.912 / 5.653 ms | 12.696 ms |
| Mixed k=0..16 | 4,352 / 2,328 bytes | 0.232 ms | 0.079 ms |
| All 65,536 masks | 16,777,216 / 8,912,912 bytes | 2.129 ms | 4.457 ms |

Every clean run matched the complete expected frame byte-for-byte, passed
canonical BP16 validation, and decoded back to the original raw bytes. The
matched CPU total starts with a GPU copy from the same resident raw input to a
cached/coherent readback buffer, then includes the fence wait, 32-worker CPU
encode, and cached/coherent output allocation and copy. Readback-buffer
allocation is reported separately as setup. The GPU total includes analyze
submit/wait, CPU prefix generation, output-owner allocation/copy, and pack
submit/wait. The original CPU-to-GPU raw upload is separately reported and
excluded. These bounded component results do not establish model-inference
performance.

Per-run measurements and full validation output are in
[`real32m-1iter.log`](real32m-1iter.log),
[`real32m-3iter.log`](real32m-3iter.log),
[`mixed-1iter.log`](mixed-1iter.log), and
[`all-masks-1iter.log`](all-masks-1iter.log).

## SHA-256 provenance

| Artifact | SHA-256 |
| --- | --- |
| `research/bp16/encode_analyze.hlsl` | `ef6f1fca5d25dd1884d4a124970bc039d277c687094c094df4ff2065d5ed2009` |
| `research/bp16/encode_analyze.spv` | `6142a1be0c708c1101bff551a2ed39e3c39a6160efb514393dce7c1b9325114c` |
| `research/bp16/encode_pack.hlsl` | `d565016d60ba804fb3529ab33f3c084215a4efc8306aa86920ff3eebed7a7370` |
| `research/bp16/encode_pack.spv` | `473455c06d03a07205d564f46e4a1745dbd4b49636003e0902dc3877d3958f63` |
| `research/gdeflate/vulkan_gdeflate_smoke.cpp` | `47a2b09df542a741f42014b57b6b02ea1f266ba6df7f50afd17512927a88c9d8` |
| `/tmp/zvram-bp16-encoder-smoke` | `9680a51405169964b7b703b0f8d7210d937af3ca0cb492393b03baa79e58cf31` |
| 32 MiB raw fixture | `8a67e5cb4721587afbd3c22104e5cbac52f6460f5db9e9117e939f5186a80895` |
| 32 MiB expected BP16 frame | `2b519249c04122e43696dd164d039348af90890c0f47745dd46463ede667cf5a` |
| mixed raw fixture | `715fee79596fd47082fdda475b662772adacae64f8a8da8e76d68d3d4c1e283a` |
| mixed expected BP16 frame | `1fbd14455b7d2d6c0b9ea953347633d00e0a5859715018f869695a29fd2b5c52` |
| all-mask raw fixture | `8a6a3a86d1ac9fa29366e46239252ecb8c98146ef16bf811597b0b47e8aeb77f` |
| all-mask expected BP16 frame | `46136f70500c5f31ac78718247d0ad74cfc4f0199809a256eee1160f73e3ce34` |
