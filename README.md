<p align="center"><img src="assets/banner.svg" alt="zVram — Explore memory beyond VRAM" width="100%"></p>

<p align="center"><b>Experimental GPU memory research · Vulkan · AMD RADV · HIP · C++17</b><br><a href="https://nerdrx.github.io/zVram/">Project website</a> · <a href="#quick-start">Quick start</a> · <a href="VALIDATION.md">Measured results</a> · <a href="KERNEL_PAGING.md">Linux paging research</a></p>

zVram tests explicit strategies for GPU memory beyond local VRAM: native driver migration, a managed Vulkan buffer pool with lossless zstd snapshots, and an opt-in HIP `hipMalloc` spillover layer.

**Status: experimental v0.2.0.** The managed pool controls buffers an application explicitly gives it. HIP offers a narrow `hipMalloc` shim with native, mapped-host, and experimental VMM/GTT backing, plus opt-in automatic compression of idle, tracked VMM allocations on one exact ROCm HIP dispatch ABI. The VMM/GTT provider passed one 40 GiB single-pointer integrity check using 20 GiB of VRAM and 20 GiB of GTT on this machine. An unmodified ROCm copy example and a small 135M-parameter llama.cpp HIP run both passed; the latter produced identical output on native and VMM/GTT paths. Native HIP rejected one 40 GiB allocation with out-of-memory; the VMM/GTT provider accepted and verified that size. Unchanged 30B and 27B models also passed concurrent inference, with 32,612.75 MiB of combined GPU model buffers on this 24 GiB card. A single 40 GiB model and broad application compatibility remain untested; idle compression is process hibernation, not transparent active-working-set paging.

## What works today

| Component | Behavior |
|---|---|
| Vulkan launcher and layer | Opt-in allocation telemetry; requests AMD `ALLOWED` overallocation when available and preserves an explicit application policy. |
| Managed Vulkan pool | Logical IDs, pinned acquire/release, LRU eviction, zstd snapshots with raw fallback, and restore/readback under resident and host-store budgets. |
| HIP allocation layer | Opt-in `hipMalloc` routing to native device memory, mapped pinned host memory, or experimental imported GTT BOs through HIP VMM. |
| HIP cold snapshots | Explicit userspace hibernate/resume for owned VMM allocations: lossless Zstd/raw backing, reserved GPU pointers, and retryable restore. |
| HIP automatic idle snapshots | Opt-in dispatch hooks hibernate tracked VMM allocations after an idle timeout and restore them before resident work. Supported on the tested HIP 7.2.53211 ABI only. |
| Integrity checks | Vulkan transfer and compute readback, compression round trips, and HIP GPU-write/CPU-verify tests. |

RADV already migrates allocations between VRAM and GPU-accessible system memory. Successful native checks demonstrate that driver behavior; they do not attribute extra capacity to zVram. The layer's counters report API allocation requests, not physical residency.

## Managed Vulkan buffer pool

Include [`managed_pool.hpp`](managed_pool.hpp) and link `zvram_pool`. The pool owns each Vulkan buffer and allocation. `upload` creates a stable logical ID; `acquire` returns the current `VkBuffer` and pins it; `release` is valid only after the caller has synchronized all GPU work using that buffer. By default a restored allocation can have a different `VkBuffer`, so refresh descriptors and other references after every acquire. With `Config::stableSparseBuffers = true`, each buffer keeps its handle while physical backing is removed and restored through sparse binding. That mode requires `sparseBinding` and `sparseResidencyBuffer` enabled at device creation, plus a sparse-binding transfer queue; physical support checks cannot verify what the caller enabled on an existing device. It still requires the same explicit acquire/release boundaries. Serialize all calls on a pool instance. The supplied queue must be externally synchronized with pool calls.

The pool assumes resident data can be modified by GPU work and reads it back before eviction. Host storage uses zstd when smaller and raw bytes otherwise. Snapshot encoding and restoration stream by staging chunk; temporary codec scratch is bounded by the configured chunk and zstd compression bound, while caller-owned readbacks are outside the pool budget. The pool is an explicit application integration API. Sparse mode rebinds memory at synchronized application boundaries; it does not supply fault-driven paging or discover arbitrary application buffer use. After an uncertain queue operation the pool retains backing and stops further acquire/upload operations.

## HIP allocation layer

Builds when HIP/ROCm development files are available. The `--hip` shim covers `hipMalloc`/`hipFree`; it is not a general HIP memory manager, and asynchronous free of a mapped-host fallback is rejected. `--hip-local-mib` caps native device allocation bytes; without `--hip-vmm`, overflow uses mapped pinned host memory, while `--hip-host-mib` sets its cap. Mapped host is system RAM, not compressed storage.

With `--hip-vmm`, overflow is backed by AMDGPU GTT buffer objects exported through libdrm and imported into one HIP VMM virtual address range. This experimental path requires the `libdrm_amdgpu` development files in addition to ROCm/HIP. One 40 GiB synthetic integrity check passed with 20 GiB each of local VRAM and GTT backing. An earlier HIP host-location VMM provider failed and consumed VRAM in a separate probe; that superseded path is not the current GTT provider. A native HIP 40 GiB single-allocation baseline returned out-of-memory, so the current GTT provider demonstrated extra single-allocation capacity on this stack. A small unmodified llama.cpp HIP run also passed on both native and VMM/GTT paths; this does not establish 40 GiB model loading, performance gain, or broad application compatibility.

By default HIP capacity queries keep reporting native physical capacity. `--hip-report-capacity` opts into reporting the configured local cap plus available GTT-backed tier through `hipMemGetInfo`, `hipDeviceTotalMem`, and the installed `hipGetDeviceProperties` ABI. Supported `hipGetProcAddress` lookups also return these wrappers when their ABI matches the installed runtime. Use it only with `--hip-vmm`, an explicit `--hip-local-mib`, and a positive `--hip-host-mib`; it does not change physical VRAM, external queries, or older property-query ABIs. This logical report passed both an 80 MiB three-query consistency check and a 40 GiB single-pointer GPU integrity run. It applies only to those HIP entry points and does not reserve memory or promise general application compatibility. See [validation details](VALIDATION.md#hip-vmm-research-probe).

## Userspace HIP hibernation

The experimental C API in [`hip_hibernate.hpp`](hip_hibernate.hpp) adds `zvramHipHibernate(coldBudgetBytes)`, `zvramHipResume()`, and `zvramHipColdBytes()` to the preloaded HIP library. It needs no root service or driver patch. Only zVram-owned VMM allocations participate; native and mapped-host allocations remain outside this path. The caller must serialize all HIP/HSA activity, hibernate only at a safe idle boundary, and successfully resume before using any cold pointer.

An optional HIP dispatch bridge can automate that lifecycle for supported HIP calls. Its automatic mode was tested with the installed ROCm HIP 7.2.53211 dispatch ABI (major 0, step 18, 4,056-byte table; bridge runtime version 70200, 506 slots). Other ABI versions/stacks disable automatic mode. The bridge covers direct calls, `hipGetProcAddress`, and native `dlsym` resolution. It closes HIP entry while snapshotting, drains earlier GPU work, and restores all cold tracked VMM allocations before resident work proceeds. Capture and host callbacks restore cold data and disable later automatic snapshots. IPC, external allocations, raw VMM, and memory pools are outside the supported path. Raw HSA and direct submissions outside HIP dispatch are also untracked; profiler bridge conflicts are rejected.

Enable it explicitly with `--hip --hip-vmm --hip-auto-idle-ms 1000 --hip-cold-mib 1024`. The idle timeout and cold-store limit are required. This is process hibernation: while work runs, the full active working set must fit in VRAM plus GTT. It does not provide universal paging or compressed active inference. A small unchanged 135M F16 llama.cpp run, with HIP graph capture disabled, ran with automatic mode and matched native stdout, with all 31/31 layers offloaded; 307,704,064 logical cold bytes compressed to 206,994,149 retained bytes. The existing Odysseus 27B Q4_K_M model also matched stdout after idle restore: 16.15 GB logical buffers retained 15.70 GB cold payload (2.78% saved), with 41.8 s background compression and an 11.5 s wake. At identical 8 GiB VRAM/RAM placement, decode measured 2.91 tokens/s without automatic mode and 2.94 tokens/s with it. These single runs do not measure the total loss versus fully GPU-resident Odyssey inference.

Snapshots use bounded mapped staging and GPU copies, with lossless Zstd or raw fallback per chunk. GPU virtual addresses stay reserved while allocation handles and GTT BOs are released. Cold backing stops consuming the configured resident caps; another allocation can use that capacity, so resume can return out-of-memory until it is released. An undersized cold budget refuses before eviction; later failures retain recoverable snapshots/backing. Resume maps the full allocation before restoring it, so it needs room for active backing plus the remaining cold snapshots.

On the RX 7900 XTX, 32 MiB and 288 MiB mixed-data allocations passed two GPU-mutated cycles, full native readback, stable pointers, budget refusal, resident-cap reuse, failed-remap recovery, and cold free. The 288 MiB allocation stored 151,014,528 bytes; its process DRM client released 16 MiB of VRAM and 272 MiB of GTT while cold. These are synthetic explicit lifecycle checks, not compressed inference or general application paging. See [evidence and reproduction](VALIDATION.md#userspace-hip-hibernation). The GPU copy helper targets `ZVRAM_HIP_ARCH` (default `gfx1100`); other architectures and multiple GPUs have not been validated.

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

# Stable-buffer sparse compute and negative integrity checks.
./zvram --validate --isolate-layers -- ./build/zvram-sparse-check ./build/managed_check.spv

# Real-file lossless roundtrip, bounded to files up to 512 MiB.
./zvram --validate --isolate-layers -- ./build/zvram-file-pool-check --file model.gguf --sparse

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

# Experimental automatic compression of idle tracked HIP VMM allocations.
./zvram --hip --hip-vmm --hip-local-mib 64 --hip-host-mib 2048 \
  --hip-auto-idle-ms 1000 --hip-cold-mib 1024 -- \
  ./your-hip-application
```

The launcher uses the build directory beside itself. Reconfigure CMake after moving the checkout so its layer manifest points to the current library.

## Unmodified HIP application check

[`check_primbench.py`](check_primbench.py) fetches two hash-verified files from a pinned official ROCm commit and builds the unchanged HIP copy example. Run `python3 check_primbench.py --run` to check native execution and VMM/GTT spillover with a 32 MiB local cap. Its two 32 MiB data buffers and internal 256 MiB cache buffer exceed that cap; the upstream assertion checks only the first three copied values. This is an application integration gate, not full-buffer integrity, model validation, or a performance comparison. Full logs and limits are in [VALIDATION.md](VALIDATION.md#unmodified-rocm-application).

## Scoped kernel paging probe

[`dmem_probe.py`](dmem_probe.py) has a read-only `--inspect` mode and an opt-in privileged `--run` mode:

```sh
python3 dmem_probe.py --inspect
sudo python3 dmem_probe.py --run
```

The run creates one temporary cgroup, limits the selected AMDGPU VRAM region to 16 MiB by default, caps child RAM and swap, and runs only the 64 MiB capacity integrity check. It requires root cgroup-v2 `dmem` and `memory` controllers to already be available and enabled. The helper changes no global swap or TTM settings and removes its cgroup on exit. It measures cgroup accounting and integrity only; it does not prove that TTM shmem pages reached swap, nor that GPU data was compressed. See [the kernel paging notes](KERNEL_PAGING.md).

## Validation

The current hardware evidence and exact commands are in [VALIDATION.md](VALIDATION.md). Results include 40 GiB native Vulkan integrity, a 40 GiB managed-pool check under a 256 MiB resident budget using highly compressible synthetic data, two smaller mixed-data compute/readback cycles, a 40 GiB HIP mapped-host fallback check, and a small llama.cpp model run through native and VMM/GTT paths. A separate full-file roundtrip of the 270,885,952-byte SmolLM2 F16 GGUF stored 207,311,475 bytes (23.47% saved) under a 16 MiB resident budget, in both default and sparse modes. That is one measured file, not a prediction for other models. These checks are not an inference benchmark, 40 GiB model test, or proof of universal application compatibility.

## Development

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
```

GPU tests require supported AMD hardware and the relevant runtime. Contributions should include reproducible workloads and distinguish native driver behavior from zVram behavior.

MIT licensed.

### Check an existing model

`check_model.py` runs an existing unmodified llama.cpp HIP `llama-completion` with an existing GGUF, compares native/zVram output, and verifies full layer offload, actual model-buffer size, and cleanup. It disables llama.cpp's fit pass and unified-memory shortcut so the tested buffers use `hipMalloc`. It does not download or quantize a model.

```sh
python3 check_model.py --binary /path/to/llama-completion --model /path/to/model.gguf \
  --local-mib 64 --host-mib 2048 --min-model-mib 250
```

For a model that exceeds physical VRAM, `--vmm-only` skips native comparison and explicitly makes no native-baseline claim. Set backing caps to match available resources; the configured total must also accommodate context and compute allocations. Logs and the JSON report default to `build/model-check`. A small F16 model passed the reproducible comparison. Two existing Odysseus models (30B and 27B) passed separate and concurrent VMM-only checks; their combined actual GPU model buffers were about 34.2 GB. See [the measured limits and logs](VALIDATION.md#existing-odysseus-models).
