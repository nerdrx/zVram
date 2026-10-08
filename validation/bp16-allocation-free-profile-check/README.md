# BP16 allocation-free profiling check

The CTest suite passed **8/8 GPU checks in 7.17 s** with
`ZVRAM_VULKAN_GPU_PROFILE=1` and
`ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT=1`. The log records
`allocation-free=1` on the relevant profiling outputs and final profile
markers. This validates opt-in profiling/reporting behavior only; it makes no
performance claim.

Source commit: `a8d113bac4dc2bce8867286f10199d56c09ca2f5`. The tested layer
binary hash is recorded in `runtime-binary-sha256.txt`. See the [CTest
summary](ctest.log) and [full per-test log](LastTest.log).
