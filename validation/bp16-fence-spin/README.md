# BP16 bounded fence-polling prototype (removed)

The prototype tried `vkWaitForFences` with zero timeout for up to 1 ms, then
used the original blocking wait. CPU12 and focused GPU8 checks passed with full
byte validation. A full-model run measured **1.16783681 tokens/s**, slightly
below the clean-first repeat at **1.17068766**, so it showed no observed gain;
the sequential comparison is not causal. The prototype was removed from the
current runtime and `ZVRAM_VULKAN_BP16_SPIN_WAIT` is not a supported current
option.

Implementation commit: `0237548`; parent: `6001356`.
`implementation.patch.gz` preserves the exact changes to `gdeflate_gpu.hpp`,
`layer.cpp`, and `test_gdeflate_gpu_layer.py`; `implementation.json` records
patch size and SHA-256. [Model result](../internlm-bp16-gpu-spin-cold26-owner26-resident19-lfu/README.md).
