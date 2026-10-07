# zVram v0.2.0 — hardware validation

Measured on **2026-10-07**. These are prototype integrity and allocation-path checks, not performance benchmarks.

| Environment | Value |
|---|---|
| GPU | AMD Radeon RX 7900 XTX, RADV NAVI31 / HIP gfx1100 |
| Driver | Mesa 26.2.4-arch3.1 |
| OS | Linux, kernel 7.2.8-2-cachyos |
| Physical VRAM | 25,753,026,560 bytes (approximately 24 GiB) |
| AMD GTT limit | 32,411,705,344 bytes (approximately 30 GiB) |
| Host RAM | Approximately 60 GiB usable |
| Build | CMake Release, C++17 |

## Vulkan capacity integrity

The native run and Vulkan-layer run each retained **640 × 64 MiB device-local allocations** simultaneously: **42,949,672,960 bytes (40 GiB)**. The transfer queue uploaded deterministic data to every allocation, then read all allocations back and compared every 64-bit word.

| Run | Data verified | Transfer and verification time | Exit |
|---|---|---:|---:|
| Native RADV baseline | 40 GiB, exact match | 16.0968 s | 0 |
| zVram Vulkan layer | 40 GiB, exact match | 16.1779 s | 0 |

Driver-wide accounting after readback showed approximately 25.42 GB VRAM and 22.67 GB GTT used in each run, including other applications. The layer recorded a 40 GiB peak of requested local allocations, 64 MiB nonlocal allocations, and zero failures. These are allocation counters, not physical residency counters.

**Interpretation:** this RADV setup already preserved and verified more allocation data than physical VRAM by using native system-RAM spillover. The matching baseline and layer results do not show extra capacity caused by zVram, model compatibility, shader random-access behavior, or a performance gain.

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

Both runs exited 0 without core or synchronization validation diagnostics. Raw logs: [native baseline](validation/native-40gib.txt), [zVram layer](validation/zvram-40gib.txt). The 40 GiB test can pressure the desktop; the utility defaults to 64 MiB.

## Managed Vulkan pool

### Mixed-data lifecycle check

The small compute check creates eight 8 MiB buffers (64 MiB total) with a 16 MiB resident budget and 128 MiB host-snapshot budget. It performs GPU mutations and exact readback checks across **two full eviction/restore passes**. It also checks pin refusal, raw fallback, recovery after dirty data exceeds the host budget, and a 3-byte upload/restore case.

Latest run, [`validation/managed-compute.txt`](validation/managed-compute.txt):

| Measurement | Result |
|---|---:|
| Vulkan layer peak local allocation | 16,777,216 B (16 MiB) |
| Vulkan layer peak nonlocal allocation | 1,056,768 B |
| Host snapshots stored | 58,721,120 B |
| Evictions / restores / raw fallbacks | 38 / 30 / 33 |
| Integrity result | Both 64 MiB cycles passed; dirty-data recovery passed |

```sh
./zvram --validate --isolate-layers -- ./build/zvram-managed-check
```

### 40 GiB synthetic-data check

The capacity mode creates **1,280 × 32 MiB buffers (40 GiB total)**. Each buffer starts with a constant value and the GPU mutates it to another constant value; the check verifies every 32-bit word after eviction and restore. It uses a 256 MiB resident budget and an 8 MiB staging chunk.

| Measurement | Result |
|---|---:|
| Vulkan layer peak local allocation | 268,435,456 B (256 MiB) |
| Vulkan layer peak nonlocal allocation | 8,388,608 B (8 MiB) |
| Host-store budget | 67,108,864 B (64 MiB) |
| Host snapshots stored | 3,998,720 B |
| Evictions / restores | 2,553 / 1,273 |
| Elapsed time | 31.264 s, one check only |
| Integrity result | 40 GiB GPU-mutated data verified; exit 0 |

```sh
./zvram --validate --isolate-layers -- \
  ./build/zvram-managed-check --capacity-mib 40960
```

Raw output: [`validation/managed-40gib.txt`](validation/managed-40gib.txt). The per-buffer constant pattern is highly compressible and not representative of model weights. The stored-byte result is a synthetic example, not an expected model compression ratio or a benchmark. This demonstrates the explicit buffer pool lifecycle on this device; it is not transparent application paging.

Both pool checks stream snapshot encoding/restoration by staging chunk. Temporary zstd scratch is bounded by chunk size and the compression bound; caller-owned readback vectors and metadata are outside the host-store budget.

## HIP allocation fallback

One opt-in run kept **40 GiB** of allocations live in 8 MiB chunks. The GPU wrote a deterministic pattern and the CPU verified all **5,368,709,120 64-bit words**. The HIP layer reported a 20 GiB peak for native `hipMalloc` device allocations and a 20 GiB peak for mapped host fallback; all allocations were freed. Elapsed time was 19.163 s for this single check, not a benchmark. [`validation/hip-40gib.txt`](validation/hip-40gib.txt)

```sh
./zvram --hip --hip-local-mib 20480 --hip-host-mib 20480 -- \
  ./build/hip_check --mib 40960
```

Mapped fallback is pinned host RAM, not compressed VRAM. The test establishes this allocation and pointer path; there is no native HIP baseline here, so it does not establish a capacity gain over native HIP behavior. The current shim focuses on `hipMalloc`/`hipFree`; `hipFreeAsync` for mapped-host fallback is rejected. It does not establish compatibility with an unmodified application or model.

A separate allocation-only concurrency check passed with four threads, eight rounds, and nine live 32 KiB allocations per worker. It observed 256 native and 32 mapped-host allocations, with no GPU kernels launched. This checks the wrapper's bookkeeping under concurrent allocation/free calls, not GPU concurrency or workload safety. [`validation/hip-concurrency.txt`](validation/hip-concurrency.txt)

## HIP VMM research probe

The current `--hip-vmm` provider obtains AMDGPU GTT BOs with libdrm, exports them as DMA-BUFs, imports them into HIP VMM, and maps the segments into one virtual address range. A 64 MiB physical-placement probe passed a GPU copy and CPU verification of every byte; measured GTT rose by 64 MiB while VRAM remained unchanged at BO creation.

The integrated report-enabled single-pointer check kept **40 GiB** mapped at once: **20 GiB of local VRAM plus 20 GiB of driver GTT/RAM backing across 160 physical handles**. `hipMemGetInfo` reported total/free capacity as 40/40 GiB before allocation, 40/0 GiB while live, and 40/40 GiB after free. The GPU wrote the allocation and the CPU verified all **5,368,709,120 64-bit words**. Pointer attributes reported memory type `2` (device memory), device ordinal `0`, and unmanaged memory. All allocations were freed; the run exited 0 with no failures or orphaned VMM cleanup. Elapsed time was 8.555 s for this single check, not a benchmark. Raw output: [`validation/hip-vmm-40gib.txt`](validation/hip-vmm-40gib.txt).

Driver-wide snapshots include other applications. VRAM/GTT usage measured 1,263,779,840/3,536,683,008 B before, 23,005,634,560/25,017,876,480 B while live, and 16,471,797,760/3,547,287,552 B immediately after free. Thus the driver's GTT use returned near its starting snapshot, while VRAM use remained elevated at the immediate post-run sample; this run does not show that all VRAM is returned instantly. `MemAvailable` was 40,764,276 KiB before, 20,025,816 KiB while live, and 40,860,676 KiB after.

### Opt-in HIP capacity reporting

By default, `hipMemGetInfo`, `hipDeviceTotalMem`, and the installed `hipGetDeviceProperties` ABI continue to return native physical capacity. `--hip-report-capacity` is an explicit mode that reports the configured local limit plus the bounded GTT-backed tier through only those three HIP entry points. It requires `--hip-vmm`, an explicit local cap, and a positive host/GTT cap. It does not alter physical capacity, older property-query ABIs, or queries made outside these intercepted HIP APIs.

The small query check passed with an 80 MiB configured logical total: free capacity reported 80 MiB before a 32 MiB allocation, 48 MiB during it, and 80 MiB after free. `hipMemGetInfo`, `hipDeviceTotalMem`, and the installed `hipGetDeviceProperties` ABI agreed. Raw output: [`validation/hip-capacity-report.txt`](validation/hip-capacity-report.txt). The 40 GiB run above separately verified `hipMemGetInfo` transitions with the reporting option enabled. These checks validate reporting consistency and allocation integrity for the tested path, not universal availability beyond the configured backing limits.

The all-GTT descriptor test kept **256 × 4 KiB allocations** live, filled each with `hipMemset`, and verified every byte on readback. Open descriptor count moved from **8 to 9 to 8** before, during, and after the allocations, showing the shared GTT provider reused a bounded descriptor set and cleaned up on free. The HIP summary reported zero tracked, pending, local, host, orphaned, and failed allocations. Raw output: [`validation/hip-vmm-descriptors.txt`](validation/hip-vmm-descriptors.txt).

```sh
./zvram --hip --hip-vmm --hip-report-capacity --hip-local-mib 16 --hip-host-mib 64 -- \
  ./build/zvram-hip-capacity-check

./zvram --hip --hip-vmm --hip-report-capacity \
  --hip-local-mib 20480 --hip-host-mib 20480 -- \
  ./build/hip_check --single-allocation --mib 40960
```

An earlier HIP host-location VMM provider is a separate, superseded path: its 40 GiB attempt failed during host-segment creation, and a host-only probe showed each 256 MiB allocation consuming matching VRAM with GTT unchanged at creation. Its historical failure log is [`validation/hip-vmm-40gib-failed.txt`](validation/hip-vmm-40gib-failed.txt); those findings do not describe the current GTT BO provider. For comparison, the non-VMM `hipMalloc` mapped-host fallback also passed a separate 40 GiB test using pinned host memory. These are synthetic pointer/data-integrity checks. No native HIP baseline, model workload, unmodified-application compatibility, compression, or transparent paging has been established. The VMM/GTT build needs ROCm/HIP and libdrm AMDGPU development files.

## Linux TTM paging probe

`dmem_probe.py --inspect` reads cgroup, RAM, and swap state. The privileged `sudo python3 dmem_probe.py --run` path is prepared but **has not been run**. It uses a temporary child cgroup with a 16 MiB default AMDGPU VRAM cap, 512 MiB RAM cap, 256 MiB swap cap, and a 30-second limit around only the 64 MiB capacity check. It makes no global swap or TTM changes and cleans up its cgroup. Even a successful run would not, by itself, prove that TTM shmem pages reached swap or zram, or that GPU data was compressed. See [KERNEL_PAGING.md](KERNEL_PAGING.md).

## Earlier compression check and routine commands

The standalone compression demo verified a 16 MiB repetitive input round trip (58,748 bytes stored) and seeded-random input raw fallback (16,777,216 bytes stored), with exact final GPU readback. It retains correctness copies and is not a process-memory savings measurement. [Raw output](validation/compression.txt).

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Tests require compatible GPU hardware and the relevant runtime. This report distinguishes completed integrity checks from untested application, model, performance, and transparent paging claims.
