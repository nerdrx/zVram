# zVram v0.1.0 — hardware validation

Measured on **2026-10-07**. These are single-run prototype checks, not performance benchmarks.

| Environment | Value |
|---|---|
| GPU | AMD Radeon RX 7900 XTX, RADV NAVI31 |
| Driver | Mesa 26.2.4-arch3.1 |
| OS | Linux, kernel 7.2.8-2-cachyos |
| Physical VRAM | 25,753,026,560 bytes (approximately 24 GiB) |
| AMD GTT limit | 32,411,705,344 bytes (approximately 30 GiB) |
| Host RAM | Approximately 60 GiB usable |
| Build | CMake Release, C++17 |

## Full capacity integrity check

Both runs retained **640 × 64 MiB device-local Vulkan allocations** simultaneously: **42,949,672,960 bytes (40 GiB)**. The GPU transfer queue copied distinct deterministic SplitMix64 data into every chunk. After all uploads completed, every chunk was copied back and all 64-bit words were compared against the expected data.

The staging allocation is 64 MiB of non-device-local, host-visible/coherent memory, preferring a cached memory type. Verification also uses one 64 MiB cached host scratch buffer. Tested device allocations use low memory priority where supported.

| Run | Data verified | Generation, transfer and verification time | Exit |
|---|---|---|---|
| Native RADV baseline | 40 GiB, exact match | 16.0968 s | 0 |
| zVram Vulkan layer | 40 GiB, exact match | 16.1779 s | 0 |

Driver-wide accounting after readback showed approximately **25.42 GB VRAM** and **22.67 GB GTT** used in each run, including other applications. The layer reported a peak of **40 GiB requested local allocations**, **64 MiB nonlocal allocations**, and **zero allocation failures**. Allocation counters are not physical residency counters.

**Interpretation:** this driver already backs more than physical VRAM with native system-RAM spillover. The layer preserved that behavior. These results demonstrate transfer integrity beyond physical VRAM, not a capacity increase caused by zVram, transparent compression, shader random-access performance, or the ability to run a specific model.

```sh
# Native baseline
VK_LOADER_LAYERS_DISABLE='~implicit~' \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
VK_VALIDATION_VALIDATE_SYNC=1 \
./build/zvram-capacity-check --mib 40960

# Same check through the layer
./zvram --validate --isolate-layers -- \
  ./build/zvram-capacity-check --mib 40960
```

Raw output: [native baseline](validation/native-40gib.txt), [zVram layer](validation/zvram-40gib.txt). Both completed without core/synchronization validation diagnostics. The large check is optional and can pressure the desktop; begin with the default 64 MiB.

## Managed compression round trip

Each case uploaded 16 MiB, verified its initial GPU readback, compressed on the CPU, freed its GPU allocation, decompressed/reallocated/uploaded, and verified the final GPU readback exactly.

| Synthetic input | Original | Stored | Mode |
|---|---|---|---|
| Repetitive pattern | 16,777,216 B | 58,748 B | zstd level 3 |
| Seeded random bytes | 16,777,216 B | 16,777,216 B | Raw fallback |

```sh
./zvram --validate --isolate-layers -- ./build/zvram-compression-check
```

Exit 0, no core/synchronization validation diagnostics. [Raw output](validation/compression.txt).

This is an explicitly controlled buffer lifecycle. The demo retains reference/scratch copies for correctness checking and does not measure total process memory savings. Synthetic compressibility is not evidence for model-weight compression.

## Headless graphics smoke

`vkcube` completed 60 frames inside Gamescope's headless backend with zVram and core/synchronization validation enabled. Gamescope exited 0; no Vulkan validation diagnostics were emitted. Expected Xwayland keymap and shutdown messages were present.

```sh
VK_LOADER_LAYERS_DISABLE='~implicit~' timeout 30s \
gamescope --backend headless -W 640 -H 480 -w 640 -h 480 -r 60 -- \
./zvram --validate --isolate-layers -- vkcube --wsi xcb --c 60
```

This verifies a small graphics application's initialization, allocation, rendering and teardown path. It is not broad game compatibility or an FPS measurement.

## Routine checks

- Release build: passed.
- Launcher Python syntax: passed.
- CTest managed compression and 64 MiB layer capacity integrity: 2/2 passed with core/synchronization validation enabled.
- Website: desktop and 390 px mobile render inspected; navigation targets checked.

Remaining gates: general compute/inference workloads, other GPU drivers, HIP/CUDA/OpenGL/DirectX integration, and transparent compressed eviction.
