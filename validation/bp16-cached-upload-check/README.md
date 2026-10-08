# BP16 cached upload correctness checks

Both recorded CTest passes completed **8/8 GPU tests**: one normal run and one
sampled-profile run. The hidden Gamescope graphics check also passed three
presented frames using allocated-host input, with full-buffer/pixel validation
and zero validation errors. These checks establish correctness for their
bounded fixtures, not full-model performance or game performance.

The first CTest pass used `ZVRAM_VULKAN_BP16_HOST_INPUT=1` and
`ZVRAM_VULKAN_BP16_CACHED_UPLOAD=1`, at source commit
`1161e7dfb7cf516cf04382f99e993cabae35ea16`. The sampled-profile CTest pass used
both variables plus `ZVRAM_VULKAN_GPU_PROFILE=1`, at source
`870a7dd`. The hidden Gamescope pass used
`ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT=1` instead; it did **not** test the
cached-upload preference. Its result records source/binary provenance and
reports zero validation errors.

The hidden check ran with the RADV Vulkan ICD and validation enabled. Its
recorded layer binary hash is `805b4903d452039bd373dc1861c886fb4f71e1d5033f49d913c87ac9e682908f`;
see the individual result for full provenance.

See [CTest log](ctest.log), [sampled-profile log](sampled-profile-ctest.log),
and [hidden graphics result/log](hidden-graphics/).
