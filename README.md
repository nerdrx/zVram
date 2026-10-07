<p align="center"><img src="assets/banner.svg" alt="zVram — Explore memory beyond VRAM" width="100%"></p>

<p align="center"><b>Experimental GPU memory research · Vulkan · AMD RADV · HIP · C++17</b><br><a href="https://nerdrx.github.io/zVram/">Project website</a> · <a href="#quick-start">Quick start</a> · <a href="VALIDATION.md">Measured results</a> · <a href="KERNEL_PAGING.md">Linux paging research</a></p>

zVram tests explicit strategies for GPU memory beyond local VRAM: native driver migration, a managed Vulkan buffer pool with lossless zstd snapshots, and an opt-in HIP `hipMalloc` spillover layer.

**Status: experimental v0.2.0.** The managed pool only controls buffers an application explicitly gives it. HIP offers a narrow `hipMalloc` shim with native, mapped-host, and experimental VMM/GTT backing. The VMM/GTT provider passed one 40 GiB single-pointer integrity check using 20 GiB of VRAM and 20 GiB of GTT on this machine. This is explicit integration research, not transparent arbitrary-application paging or compression; no model workload, unmodified application compatibility, or native HIP baseline has been tested.

## What works today

| Component | Behavior |
|---|---|
| Vulkan launcher and layer | Opt-in allocation telemetry; requests AMD `ALLOWED` overallocation when available and preserves an explicit application policy. |
| Managed Vulkan pool | Logical IDs, pinned acquire/release, LRU eviction, zstd snapshots with raw fallback, and restore/readback under resident and host-store budgets. |
| HIP allocation layer | Opt-in `hipMalloc` routing to native device memory, mapped pinned host memory, or experimental imported GTT BOs through HIP VMM. |
| Integrity checks | Vulkan transfer and compute readback, compression round trips, and HIP GPU-write/CPU-verify tests. |

RADV already migrates allocations between VRAM and GPU-accessible system memory. Successful native checks demonstrate that driver behavior; they do not attribute extra capacity to zVram. The layer's counters report API allocation requests, not physical residency.

## Managed Vulkan buffer pool

Include [`managed_pool.hpp`](managed_pool.hpp) and link `zvram_pool`. The pool owns each Vulkan buffer and allocation. `upload` creates a stable logical ID; `acquire` returns the current `VkBuffer` and pins it; `release` is valid only after the caller has synchronized all GPU work using that buffer. A restored allocation can have a different `VkBuffer`, so refresh descriptors and other references after every acquire. The supplied queue must be externally synchronized with pool calls.

The pool assumes resident data can be modified by GPU work and reads it back before eviction. Host storage uses zstd when smaller and raw bytes otherwise. Snapshot encoding and restoration stream by staging chunk; temporary codec scratch is bounded by the configured chunk and zstd compression bound, while caller-owned readbacks are outside the pool budget. The pool is an explicit application integration API, not a transparent Vulkan layer or virtual-memory implementation.

## HIP allocation layer

Builds when HIP/ROCm development files are available. The `--hip` shim covers `hipMalloc`/`hipFree`; it is not a general HIP memory manager, and asynchronous free of a mapped-host fallback is rejected. `--hip-local-mib` caps native device allocation bytes; without `--hip-vmm`, overflow uses mapped pinned host memory, while `--hip-host-mib` sets its cap. Mapped host is system RAM, not compressed storage.

With `--hip-vmm`, overflow is backed by AMDGPU GTT buffer objects exported through libdrm and imported into one HIP VMM virtual address range. This experimental path requires the `libdrm_amdgpu` development files in addition to ROCm/HIP. One 40 GiB synthetic integrity check passed with 20 GiB each of local VRAM and GTT backing. An earlier HIP host-location VMM provider failed and consumed VRAM in a separate probe; that superseded path is not the current GTT provider. Neither result establishes model compatibility, performance gain, or transparent paging.

By default HIP capacity queries keep reporting native physical capacity. `--hip-report-capacity` opts into reporting the configured local cap plus available GTT-backed tier through `hipMemGetInfo`, `hipDeviceTotalMem`, and the installed `hipGetDeviceProperties` ABI. Use it only with `--hip-vmm`, an explicit `--hip-local-mib`, and a positive `--hip-host-mib`; it does not change physical VRAM, external queries, or older property-query ABIs. This logical report passed both an 80 MiB three-query consistency check and a 40 GiB single-pointer GPU integrity run. It applies only to those HIP entry points and does not reserve memory or promise general application compatibility. See [validation details](VALIDATION.md#hip-vmm-research-probe).

## Quick start

Requires Linux, CMake, a C++17 compiler, Python 3, Vulkan headers and loader, glslc, and zstd development files. HIP tests are optional and require ROCm/HIP tooling; the VMM/GTT path also needs libdrm AMDGPU development files. The GPU checks select a discrete AMD device.

```sh
git clone https://github.com/nerdrx/zVram.git
cd zVram
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Observe allocations for one Vulkan process.
./zvram -- your-application its-arguments

# Managed Vulkan compute, eviction, restoration, and integrity check.
./zvram --validate --isolate-layers -- ./build/zvram-managed-check

# Optional HIP probe: 40 GiB live with 20 GiB local and 20 GiB mapped-host caps.
./zvram --hip --hip-local-mib 20480 --hip-host-mib 20480 -- \
  ./build/hip_check --mib 40960

# Experimental HIP VMM/GTT path: one 40 GiB pointer and opt-in capacity reporting.
./zvram --hip --hip-vmm --hip-report-capacity \
  --hip-local-mib 20480 --hip-host-mib 20480 -- \
  ./build/hip_check --single-allocation --mib 40960

# Small three-query capacity consistency check (80 MiB configured total).
./zvram --hip --hip-vmm --hip-report-capacity --hip-local-mib 16 --hip-host-mib 64 -- \
  ./build/zvram-hip-capacity-check
```

The launcher uses the build directory beside itself. Reconfigure CMake after moving the checkout so its layer manifest points to the current library.

## Scoped kernel paging probe

[`dmem_probe.py`](dmem_probe.py) has a read-only `--inspect` mode and an opt-in privileged `--run` mode:

```sh
python3 dmem_probe.py --inspect
sudo python3 dmem_probe.py --run
```

The run creates one temporary cgroup, limits the selected AMDGPU VRAM region to 16 MiB by default, caps child RAM and swap, and runs only the 64 MiB capacity integrity check. It requires root cgroup-v2 `dmem` and `memory` controllers to already be available and enabled. The helper changes no global swap or TTM settings and removes its cgroup on exit. It measures cgroup accounting and integrity only; it does not prove that TTM shmem pages reached swap, nor that GPU data was compressed. See [the kernel paging notes](KERNEL_PAGING.md).

## Validation

The current hardware evidence and exact commands are in [VALIDATION.md](VALIDATION.md). Results include 40 GiB native Vulkan integrity, a 40 GiB managed-pool check under a 256 MiB resident budget using highly compressible synthetic data, two smaller mixed-data compute/readback cycles, and an independent 40 GiB HIP mapped-host fallback check. None is an inference benchmark, representative model-weight compression ratio, or proof of universal application compatibility.

## Development

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
```

GPU tests require supported AMD hardware and the relevant runtime. Contributions should include reproducible workloads and distinguish native driver behavior from zVram behavior.

MIT licensed.
