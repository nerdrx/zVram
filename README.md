<p align="center"><img src="assets/banner.svg" alt="zVram — Explore memory beyond VRAM" width="100%"></p>

<p align="center"><b>Experimental GPU memory research · Vulkan · AMD RADV · HIP · C++17</b><br><a href="https://nerdrx.github.io/zVram/">Project website</a> · <a href="#quick-start">Quick start</a> · <a href="VALIDATION.md">Measured results</a> · <a href="KERNEL_PAGING.md">Linux paging research</a></p>

zVram tests GPU memory beyond local VRAM: segmented Vulkan allocations, lossless idle snapshots for eligible Vulkan/HIP allocations, and an explicit managed buffer pool.

**Status: experimental v0.2.0.** The managed pool controls buffers an application explicitly gives it. HIP offers a narrow `hipMalloc` shim with native, mapped-host, and experimental VMM/GTT backing, plus opt-in automatic compression of idle, tracked VMM allocations on one exact ROCm HIP dispatch ABI. The VMM/GTT provider passed a 40 GiB single-pointer integrity check using 20 GiB each of VRAM and GTT. An official 39.73 GB InternLM2.5-20B F16 GGUF also loaded through HIP VMM/GTT with a 36,798.77 MiB GPU model buffer and all 49/49 layers offloaded; the native full-GPU HIP request OOMed. A separate native CPU/GPU HIP run offloaded 24/49 layers and matched VMM output. The same model also completed through the zVram Vulkan virtual heap and matched native Vulkan output. With llama.cpp's own n-gram self-drafting, a repeated-text prompt measured 6.33 versus 1.10 tokens/s and matched output; an ordinary code explanation measured 1.45 versus 1.43 tokens/s but diverged in output. These short sequential runs show workload-specific app behavior, not a zVram or general speedup. A 40 GiB GPU model buffer and broad app compatibility remain unverified. Idle compression is not transparent active-working-set paging.

## What works today

| Component | Behavior |
|---|---|
| Vulkan launcher and layer | Opt-in allocation telemetry; requests AMD `ALLOWED` overallocation when available and preserves an explicit application policy. |
| Vulkan segmented memory | Experimental virtual GPU-only memory type for eligible storage buffers. Sparse binding backs one logical allocation with native chunks; a 40 GiB single-buffer transfer/readback check passed on the tested RADV driver. |
| Vulkan automatic snapshots | Opt-in idle compression now passes both a standalone buffer integrity test and one unchanged Vulkan llama.cpp run; see the exact limits below. |
| Managed Vulkan pool | Logical IDs, pinned acquire/release, LRU eviction, zstd snapshots with raw fallback, and restore/readback under resident and host-store budgets. |
| HIP allocation layer | Opt-in `hipMalloc` routing to native device memory, mapped pinned host memory, or experimental imported GTT BOs through HIP VMM. |
| HIP cold snapshots | Explicit userspace hibernate/resume for owned VMM allocations: lossless Zstd/raw backing, reserved GPU pointers, and retryable restore. |
| HIP automatic idle snapshots | Opt-in dispatch hooks hibernate tracked VMM allocations after an idle timeout and restore them before resident work. Supported on the tested HIP 7.2.53211 ABI only. |
| Integrity checks | Vulkan transfer and compute readback, compression round trips, and HIP GPU-write/CPU-verify tests. |

RADV already migrates allocations between VRAM and GPU-accessible system memory. Successful native checks demonstrate that driver behavior; they do not attribute extra capacity to zVram. The layer's counters report API allocation requests, not physical residency.

## Experimental Vulkan virtual memory

```bash
# Unchanged memtest sees the configured 96 GiB virtual heap; tests 2 GiB.
./zvram --verbose --isolate-layers --vulkan-virtual-gib 96 -- memtest_vulkan 1 2147483648

# Verify every byte of one 40 GiB logical allocation with chunked staging.
./zvram --validate --isolate-layers --vulkan-virtual-mib 49152 -- \
  ./build/zvram-capacity-check --single-allocation --api2 --mib 40960
```

This userspace mode adds a GPU-only virtual heap/type while preserving native heaps and memory types. On the tested discrete RADV device, it enables sparse binding and promotes storage buffers of at least 1 MiB with storage/transfer usage (and optionally BDA), no creation flags, and no buffer `pNext` chain. Synthetic allocations are backed at bind time by aligned native chunks of at most 256 MiB; native local allocation types are preferred, with compatible system types as fallback. RADV remains responsible for migration between VRAM and RAM. The layer supports multiple eligible, nonoverlapping promoted storage-buffer ranges in one synthetic `VkDeviceMemory`, including aligned nonzero offsets and ranges crossing native-chunk boundaries. It retains range contents across buffer destruction and rebinding. The synthetic allocation remains allocated after buffer destruction; `vkFreeMemory` is deferred until its last bound buffer is destroyed. The 40 GiB transfer check verified one logical allocation, one buffer, and 160 native chunks; unchanged memtest completed a bounded 2 GiB compute check with eight chunks and displayed 40 GB.

The configured heap is a logical cap, not reserved memory or a free-capacity guarantee. Binding can fail if actual backing cannot be allocated. In automatic snapshot mode, a pristine GPU-only native `VkDeviceMemory` can be adopted when its first eligible promoted storage buffer binds, even at a nonzero offset or when smaller than the allocation. The layer adopts the whole allocation as one child and preserves its native memory type, flags, priority, and allocation callbacks. Later eligible, nonoverlapping storage-buffer ranges can share it. This path adds no synthetic backing and permits a native child larger than 256 MiB. Allocations already bound to an ordinary buffer, image, or sparse binding are not adoptable. Once an adopted pool is cold, later ordinary buffer/image binds are refused without waking it. `vkQueueWaitIdle` and `vkDeviceWaitIdle` keep cold snapshots asleep. Binding a cold allocation restores only that allocation; an overlapping bind is rejected before wake. By default, queue submissions restore all cold allocations. Opt-in `--vulkan-selective-restore` restores only whole allocations referenced by recognized compute or transfer submissions; unknown commands and unsupported descriptor or shader paths retain restore-all behavior. Restored allocations remain resident until a later snapshot. Opt-in `--vulkan-active-eviction` lets the worker compress whole allocations with no outstanding tracked use while other tracked queue work is pending. Restoration writes become visible to each app queue on its next intercepted submit through deferred waits, without a device-wide idle. Private queue epoch markers track safe completion, and unknown accesses conservatively keep every candidate resident until their markers retire. A five-case synthetic/native focused gate passed, including restoration before a host-blocked queue was released and an unknown-command guard. Without the separate range option below, this is allocation-granularity eviction. Fault-driven paging remains unsupported. The 40 GiB result verifies transfer integrity for one allocation; it does not establish fast compression of a 40 GiB pool or VRChat/app compatibility. Graphics/presentation and explicit application sparse submissions remain outside the active path. Unknown or mixed pools, images, external/protected memory, capture/replay, synthetic host mapping, and universal active-working-set paging remain unsupported. In automatic snapshot mode, the layer unbinds app buffers before binding private per-child transfer views, copies through each view, then unbinds it; snapshots preserve the whole allocation, including gaps, losslessly. The layer uses a spare sparse queue when available, avoiding a wait on the app queue during binding; without one it retains the synchronous app-queue fallback. In virtual-only mode, unrelated application queues now forward without device-wide snapshot locks or cold-buffer scans; internal sparse-queue work remains serialized. The earlier queue fast path passed seven focused GPU tests and a 62/62 suite; the latest full CTest run passed 69/69 in 42.79 seconds with zero validation errors or VUIDs. Sparse features can be injected through legacy features or a head `VkPhysicalDeviceFeatures2`; other layouts work when the app already enables sparse binding. BDA storage buffers are supported with the required device-address allocation flag. Native buffer/allocation limits remain unchanged; the 40 GiB check used Vulkan 1.1 on this RADV driver. Ctrl+C stops memtest.

Enable selective restoration with `--vulkan-selective-restore` together with `--vulkan-auto-idle-ms`; this requires automatic idle snapshots. Example focused check:

```sh
./zvram --validate --isolate-layers --vulkan-virtual-mib 64 \
  --vulkan-auto-idle-ms 100 --vulkan-cold-mib 64 --vulkan-selective-restore -- \
  ./build/zvram-vulkan-auto-check --selective-submit
```

Add `--vulkan-active-eviction` to test active allocation-granularity eviction. It requires both automatic snapshots and selective restore:

```sh
./zvram --validate --isolate-layers --vulkan-virtual-mib 64 \
  --vulkan-auto-idle-ms 100 --vulkan-cold-mib 64 \
  --vulkan-selective-restore --vulkan-active-eviction -- \
  ./build/zvram-vulkan-auto-check --active-submit --two-queues
```

Opt in to chunk-range residency with `--vulkan-range-mib 32`; it requires automatic snapshots, selective restore, and active eviction. On hardware exposing `sparseResidencyBuffer`, the layer partially binds sparse buffers and selects chunks from narrow descriptor or transfer ranges. A pristine native allocation is segmented into aligned chunks of the configured size, with a smaller final chunk on first eligible bind; default behavior continues to snapshot and evict whole allocations. Global `robustBufferAccess`, explicit pipeline robustness, and dynamic storage descriptors conservatively widen tracking to whole buffers; unknown commands and BDA shaders retain restore-all behavior. The hardware feature and a private queue are required. Graphics and presentation remain unsupported. See [range-residency evidence and limits](VALIDATION.md#vulkan-range-residency).

```sh
./zvram --validate --isolate-layers --vulkan-virtual-mib 64 \
  --vulkan-auto-idle-ms 100 --vulkan-cold-mib 64 \
  --vulkan-selective-restore --vulkan-active-eviction --vulkan-range-mib 32 -- \
  ./build/zvram-vulkan-auto-check --range-submit
```

The range path passed a 64 MiB two-chunk integrity check and one unchanged small-model cold-restore run. This does not establish sustained model serving, 40 GiB compression speed, VRChat compatibility, or graphics/presentation support. See [range-residency evidence and limits](VALIDATION.md#vulkan-range-residency) and [active eviction evidence](VALIDATION.md#active-vulkan-eviction).

An unchanged SmolLM2-135M F16 run with active mode enabled also passed all 12 checks, retained 31/31 layer offload, and produced byte-identical native and wrapped output. This is one integration and cold-restore result, not a speed claim.

Choose the heap size at each launch with `--vulkan-virtual-gib 96`, or use `--vulkan-virtual-mib 98304` for the same 96 GiB budget. Use one size option per launch. There is no artificial upper cap; only positivity and Vulkan's 64-bit byte-size overflow are checked. The heap remains a logical budget. Full 40 GiB backing is verified; both 48 GiB and 96 GiB settings passed bounded 2 GiB memtest checks. Full 48 GiB or 96 GiB backing remains unverified.

### Automatic idle snapshots (narrow experimental path)

`--vulkan-auto-idle-ms 100 --vulkan-cold-mib 512` opts eligible app-created storage buffers into lossless idle snapshots. This was validated with one 320 MiB buffer, two native backing chunks, and a 32 MiB coherent staging buffer. The first idle snapshot retained 12,263,515 bytes (96.345% smaller); after GPU mutation to randomized data, the next snapshot used raw 335,544,320-byte backing. Two GPU XOR wake cycles verified every byte while the `VkBuffer` handle stayed stable. Cold metadata queries and freeing a cold buffer did not restore it. A 1 MiB cold-store budget refused eviction and preserved the original contents. Budget refusals stay suppressed until new queue work or cold-store release, while other transient snapshot errors remain retryable. A 14 MiB release test froze a 192 MiB buffer, freed its cold backing, then froze and byte-verified a 256 MiB buffer without another GPU submission.

The first cold transition released exactly 335,544,320 bytes of process DRM resident VRAM (345,059,328 to 9,515,008 bytes); resident GTT stayed at 69,210,112 bytes. The standalone tests now also cover partial child release and retry after an injected partial-restore OOM; see the validation logs. A separate unchanged 135M F16 llama.cpp Vulkan run passed with automatic snapshots enabled: 31/31 layers offloaded, native and wrapped stdout matched exactly, and 307,998,976 cold logical bytes were stored in 207,039,004 bytes before input. The tested path covers multiple queues/families and BDA for this app's native GPU-only allocations. Native-pool adoption remains limited to pristine GPU-only allocations and eligible nonoverlapping storage-buffer ranges. Images, external/protected memory, capture/replay, and active fault-driven paging remain unsupported. Active work still needs native VRAM plus GTT.

```sh
# 96 GiB logical heap, narrow 320 MiB idle-snapshot integrity check
./zvram --verbose --isolate-layers --vulkan-virtual-gib 96 \
  --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- ./build/zvram-vulkan-auto-check

# Compare an unchanged Vulkan llama-completion run with automatic idle snapshots
python3 check_vulkan_idle_model.py --binary /path/to/llama-completion \
  --model /path/to/SmolLM2-135M-F16.gguf
```

See [hardware evidence and limits](VALIDATION.md#automatic-vulkan-idle-snapshots).

In automatic mode, eligible exact-size GPU-only native allocations use stable app-facing memory tokens and can be adopted into snapshots. A private sparse/transfer queue performs copies. In the default mode, completion markers postpone snapshots while app work is pending; opt-in active eviction can snapshot whole allocations unused by tracked pending submissions. Unknown access protects all candidates. The tested path supports ordinary and internally synchronized queues, BDA, and cross-family storage-buffer barriers. App presentation or explicit sparse submissions stop future automatic snapshots after restoring cold data. Active eviction is allocation-granularity, not frame-by-frame compression or general image paging.

Steam launch options can chain CPU affinity, using the installed `zvram` command:

```text
zvram --vulkan-virtual-gib 96 -- taskset -c 1-7,16-23 %command%
```

## Managed Vulkan buffer pool

Include [`managed_pool.hpp`](managed_pool.hpp) and link `zvram_pool`. The pool owns each Vulkan buffer and allocation. `upload` creates a stable logical ID; `acquire` returns the current `VkBuffer` and pins it; `release` is valid only after the caller has synchronized all GPU work using that buffer. By default a restored allocation can have a different `VkBuffer`, so refresh descriptors and other references after every acquire. With `Config::stableSparseBuffers = true`, each buffer keeps its handle while physical backing is removed and restored through sparse binding. That mode requires `sparseBinding` and `sparseResidencyBuffer` enabled at device creation, plus a sparse-binding transfer queue; physical support checks cannot verify what the caller enabled on an existing device. It still requires the same explicit acquire/release boundaries. Serialize all calls on a pool instance. The supplied queue must be externally synchronized with pool calls.

The pool assumes resident data can be modified by GPU work and reads it back before eviction. Host storage uses zstd when smaller and raw bytes otherwise. Snapshot encoding and restoration stream by staging chunk; temporary codec scratch is bounded by the configured chunk and zstd compression bound, while caller-owned readbacks are outside the pool budget. The pool is an explicit application integration API. Sparse mode rebinds memory at synchronized application boundaries; it does not supply fault-driven paging or discover arbitrary application buffer use. After an uncertain queue operation the pool retains backing and stops further acquire/upload operations.

## HIP allocation layer

Builds when HIP/ROCm development files are available. The `--hip` shim covers `hipMalloc`/`hipFree`; it is not a general HIP memory manager, and asynchronous free of a mapped-host fallback is rejected. `--hip-local-mib` caps native device allocation bytes; without `--hip-vmm`, overflow uses mapped pinned host memory, while `--hip-host-mib` sets its cap. Mapped host is system RAM, not compressed storage.

With `--hip-vmm`, overflow is backed by AMDGPU GTT buffer objects exported through libdrm and imported into one HIP VMM virtual address range. This experimental path requires the `libdrm_amdgpu` development files in addition to ROCm/HIP. One 40 GiB synthetic integrity check passed with 20 GiB each of local VRAM and GTT backing. The native full-GPU load of a 39.73 GB InternLM2.5-20B F16 model OOMed; its VMM/GTT run allocated the full model buffer and generated output. An earlier HIP host-location VMM provider failed and consumed VRAM in a separate probe; that superseded path is not the current GTT provider. See the [model capacity evidence](VALIDATION.md#internlm25-20b-f16-model-capacity).

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

The current hardware evidence and exact commands are in [VALIDATION.md](VALIDATION.md). An official 39.73 GB InternLM2.5-20B F16 model passed a VMM/GTT inference run with a 36,798.77 MiB GPU model buffer; native full-GPU allocation OOMed. A separate native CPU/GPU-offload run matched VMM output and measured 1.38 versus 1.09 tokens/s. This is not a full-GPU baseline or a 40 GiB GPU-weight test. A separate full-file roundtrip of the 270,885,952-byte SmolLM2 F16 GGUF stored 207,311,475 bytes (23.47% saved) under a 16 MiB resident budget, in both default and sparse modes. That is one measured file, not a prediction for other models or proof of universal application compatibility.

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
