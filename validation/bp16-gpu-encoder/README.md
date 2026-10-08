# Experimental BP16 GPU snapshot encoder

The opt-in encoder runs two compute passes when a synchronous snapshot freeze
encodes eligible BP16-sized chunks. It requires BP16 GPU restore, allocated
host input, and both checked-in encoder SPIR-V modules. For example, with the
BP16 build directory selected:

```sh
ZVRAM_VULKAN_BP16_GPU_ENCODE=1 \
ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT=1 \
./zvram --build-dir build/bp16-codec --vulkan-codec bp16 --vulkan-bp16-gpu \
  --vulkan-range-mib 32 --vulkan-active-eviction --vulkan-lazy-backing \
  --vulkan-resident-mib 192 -- your-vulkan-app
```

The launcher supplies `ZVRAM_BP16_ENCODE_ANALYZE_SHADER_PATH` and
`ZVRAM_BP16_ENCODE_PACK_SHADER_PATH` when the encoder opt-in is set. Default
behavior remains off. While enabled, snapshot freezing is synchronous: async
compression is disabled for that mode. If a GPU encode cannot be used, the
existing CPU BP16 path handles the snapshot. No model throughput result is
claimed yet.

Validation on the RX 7900 XTX passed **12/12 CPU tests**, the full **121/121
CTest suite** in **85.43 s**, and the final focused GPU suite **8/8** in **6.45 s**.
The focused checks include exact application-byte verification and GPU restore.
A first focused attempt failed because the helper classified intentional RAW
snapshot output as an encoder error; its log is retained as a diagnostic. The
accounting was corrected to distinguish successful GPU encodes from RAW
snapshots, and later retry/final focused runs passed. See
`gpu-encode-gpu8.log`, `gpu-encode-gpu8-retry.log`, and
`gpu-encode-gpu8-final.log`.

Bounded raw-budget checks retained exact bytes in all cases: the 64 MiB quota
case exercised budget-driven CPU fallback, the zero-owner-budget case expected
four encoder fallbacks, and the 100% minimum-savings case intentionally skipped
GPU encoding and kept RAW snapshots. These cases are not zero-fallback encoder
claims. Commands and complete logs are included for each.

The production BP16 decoder SPIR-V is unchanged. Runtime source commit is
`72516bb0d58e658e10796bb5d589496421c65d90`; binary and source hashes are in
`runtime-sha256.txt` and `source-sha256.txt`. Encoder SPIR-V hashes are recorded
in `runtime-sha256.txt`. `LastTest.log` preserves the final focused CTest
record.
