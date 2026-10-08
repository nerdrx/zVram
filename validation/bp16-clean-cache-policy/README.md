# BP16 clean-cache policy focused tests

This archive records focused CTest evidence for the opt-in clean-snapshot cache
replacement policies. `ZVRAM_VULKAN_CLEAN_CACHE_POLICY=first` remains the
default; `lru` and `mru` are explicit alternatives. The CPU suite passed 12/12
in 4.54 seconds, and the focused BP16 GPU suite passed 8/8 in 6.85 seconds.
The recorded GPU tests include exact application-byte checks, observed GPU
decoding, zero fallback, and zero validation diagnostics. These are correctness
tests, not a performance comparison.

The layer binary hash and shader hash are in `binary-sha256.txt`. The policy
implementation and layout follow-up commits are listed in
`source-commits.txt`. The checkout later reached 0c0f066; the binary hash is
provided because the later checkout included unrelated work.

- [CPU CTest log](cpu-ctest.log)
- [Focused GPU CTest log](lru-gpu-ctest.log)
- [LastTest log](LastTest.log)
- [Binary and shader hashes](binary-sha256.txt)
