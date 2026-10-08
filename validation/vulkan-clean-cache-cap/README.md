# Vulkan clean-cache cap validation

This archive captures the clean-cache-cap focused fixtures against the
pre-raw-prototype library. The tested library SHA-256 is
`be279aedb05b94ab8832e251869e85ab9878e2624c053a9f6c39860ad9941dce`, matching
the library hash in the Qwen pressure run provenance. Its cap-source patch and
source provenance are included as `cap-source.patch` and
`cap-source-provenance.json`.

The first selected full CTest run finished **124/125**. Its only failure was
`vulkan-range-clean-cache-cap-native`: the native-allocation application flag
was misplaced and parsed as a launcher option (`unrecognized arguments:
--native-allocation`). The harness separator was corrected, then the focused
synthetic and native tests passed **2/2**. This archive preserves both the
initial failure (`full-cache-cap-selected-build.log`) and the corrected rerun
(`cache-cap-both-fixed.log`). The focused cap pair also passed **2/2** under
each cache policy: LRU, MRU, and LFU. A single cap GPU test passed 1/1. The
separate 9/9 clean-cache focused log predates the launcher fix and does not
select the be279 library consistently, so it is excluded from this archive.

The cap fixture checks a 1 MiB expendable-cache limit, cache trimming, retained
lossless cold snapshots, full fixture-byte correctness, and that cache
accounting stays within the cap. These are focused behavior tests, not a model
throughput or full-model-fit result.

`test_vulkan_clean_cache_cap.py` and `ctest_launcher.py.in` are the current
harness sources. `harness-provenance.json` records their hashes, the current
CMakeLists hash, and the tested binary hash. The CMake excerpt documents the
configured launcher copy/path and test registration. The current raw-prototype
source changes were not in this tested library and are not validated by these
results.

`SHA256SUMS` covers every archive file except itself. Verify with
`sha256sum -c SHA256SUMS` from this directory.
