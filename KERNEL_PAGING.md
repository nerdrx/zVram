# Linux TTM paging research

**Conclusion:** Linux TTM already has a lossless backing path for eligible buffer objects. It can preserve a BO's system-memory TT pages in shmem and restore them later. On AMDGPU, the path is plausible after a BO has moved from VRAM to GTT/system memory; this does not make arbitrary VRAM allocations transparently pageable or compressed.

This review follows upstream Linux source and documentation as of 2026-10-07; eligibility and reclaim behavior can differ by kernel and driver version.

## What the kernel path does

TTM's swapout walk skips pinned BOs and BOs rejected by the device driver's eviction policy. It also skips missing/unpopulated TT state and external or already-swapped TT pages. For an eligible BO, it moves the resource to SYSTEM, waits for outstanding GPU work, then invokes TT swapout. [`ttm_bo_swapout`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/ttm/ttm_bo.c#L1299-L1358)

TT swapout copies pages into a shmem file and frees the populated TT pages. A later TT population allocates pages and copies the data back from shmem. When the system has swap configured, shmem pages may in turn be reclaimed through the host's ordinary swap policy; that is a kernel-memory-management opportunity, not a guarantee that a particular BO is written to swap or zram. TTM does not zstd-compress these pages. [`ttm_tt_swapout` and `ttm_tt_populate`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/ttm/ttm_tt.c#L295-L402) · [AMDGPU memory-domain definitions](https://docs.kernel.org/gpu/amdgpu/driver-core.html#memory-domains)

TTM invokes global swapout when its TT page-pool limit is exceeded. That walk considers only resource managers whose `use_tt` flag is set. AMDGPU's GTT manager sets it; the VRAM manager does not set it during initialization. Thus a BO still resident in VRAM is not directly selected by this generic page-pool swapout walk. [`ttm_tt_populate`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/ttm/ttm_tt.c#L349-L391) · [`ttm_device_swapout`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/ttm/ttm_device.c#L159-L175) · [AMDGPU GTT manager](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_gtt_mgr.c#L352-L365) · [AMDGPU VRAM manager](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c#L887-L922)

The likely chain is therefore:

```text
VRAM pressure → driver moves eligible BO to GTT/system memory
              → TTM backs TT pages in shmem and unpopulates them
              → host memory reclaim may swap shmem pages
              → later GPU command submission validates and restores BOs
```

AMDGPU validates VM BOs before command dispatch. Its CS parser calls `amdgpu_vm_validate` with `amdgpu_cs_bo_validate`, which uses `ttm_bo_validate` for unpinned BOs. This coarse pre-submit validation can restore BOs without relying on hardware XNACK/page faults. Moves still depend on the BO's allowed/preferred domains, driver policy, available memory, and the per-submit migration allowance; a validation failure remains possible. [`amdgpu_cs_bo_validate`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L767-L817) · [CS BO validation before dispatch](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L919-L949)

This is not universal: pinned BOs, external/userptr-backed pages, BOs without populated TT pages, and driver-specific non-evictable objects are outside the generic path. Current AMDGPU policy has additional cases, including KFD BO restrictions for the owning process and preemptible BO handling. [`amdgpu_ttm_bo_eviction_valuable`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1420-L1454)

## What userspace can safely probe

The kernel exposes cgroup-v2 `dmem.max`, `dmem.current`, and `dmem.capacity` for device-memory regions. AMDGPU registers VRAM with a dmem reclaim callback that asks TTM to reclaim charged resources. This is a scoped cgroup policy knob, not a per-BO eviction request; reclaim may move BOs to GTT without swapping their backing pages. [cgroup-v2 dmem interface](https://docs.kernel.org/admin-guide/cgroup-v2.html#dmem) · [AMDGPU VRAM dmem reclaim](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c#L852-L922)

The AMDGPU debugfs `amdgpu_evict_vram` file evicts all buffers from the VRAM pool. It is useful for driver debugging, but too broad to use as an application-level pressure API. No generic userspace per-BO request to swap a Vulkan allocation was identified in this review. [AMDGPU DebugFS](https://docs.kernel.org/next/gpu/amdgpu/debugfs.html#amdgpu-evict-vram)

[`dmem_probe.py`](dmem_probe.py) provides the next bounded experiment:

```sh
python3 dmem_probe.py --inspect
sudo python3 dmem_probe.py --run
```

`--run` requires the root cgroup to already expose and enable `dmem` and `memory`. It creates a unique child cgroup, sets a 16 MiB VRAM limit by default (configurable from 1 to 256 MiB), a 512 MiB memory limit, and a 256 MiB swap limit, then runs the 64 MiB capacity integrity check by default. The child drops to the invoking sudo user; a 30-second timeout kills only that child cgroup/process group, and cleanup removes the temporary cgroup. It does not alter global swap, TTM parameters, or other cgroups.

The probe reports cgroup `dmem.current`, memory, and swap peaks plus the check's integrity result. These readings can show that pressure and migration occurred; they cannot by themselves prove TTM shmem swapout, zram use for those pages, or compression. A later probe would need correlated kernel/driver evidence of BO placement and swap activity to establish that path. The privileged run has not yet been performed.

## Research boundary

This kernel capability does not remove the need for application- or driver-level correctness. Applications must still respect queue synchronization and BO lifetimes; pinning, external memory, BO usage flags, placement limits, GPUVM updates, and migration budgets affect eligibility. zVram's managed Vulkan pool remains a separate explicit buffer API with its own zstd host snapshots. Its checks do not establish that arbitrary Vulkan or HIP allocations can use TTM paging transparently.

The HIP `--hip-report-capacity` option is separate from Linux TTM paging: it changes the reported logical capacity only for `hipMemGetInfo`, `hipDeviceTotalMem`, and the installed `hipGetDeviceProperties` ABI when explicitly enabled with HIP VMM and configured limits. Default physical queries, older property ABIs, and external interfaces remain outside that reporting hook. Small three-query consistency and 40 GiB single-pointer checks passed, but a larger reported logical value does not itself establish TTM swapout, RAM availability beyond the tested backing, or universal allocation compatibility.

## Scoped application pressure experiment

The same helper accepts an explicit command after `--`, executed as the original unprivileged sudo user without a shell. `--gpu-limit-mib`, `--memory-high-mib`, `--memory-max-mib`, `--swap-max-mib`, and `--timeout-sec` set only that temporary child cgroup. Timeout is bounded to 300 seconds. With multiple AMD GPU regions, arbitrary commands require an exact `--dmem-region` token from `--inspect`; the built-in check retains its largest-region selection. SIGINT, SIGTERM, and SIGHUP trigger scoped cleanup.

```sh
sudo python3 dmem_probe.py --run --dmem-region drm/0000:03:00.0/vram \
  --gpu-limit-mib 16 --memory-high-mib 384 --memory-max-mib 512 \
  --swap-max-mib 256 --timeout-sec 30 -- /path/to/application arguments
```

These example caps suit a small probe, not a large model or normal gaming session. Ordinary credential-like variables, Python startup variables, and loader variables are filtered; `--keep-library-path` explicitly forwards only an existing `LD_LIBRARY_PATH` after dropping privilege. The helper also reports available cgroup peak/events/stat/pressure fields and before/after system-wide zram `mm_stat`. A zram change is not attributable to this child, and none of these counters alone proves GPU-backed pages were compressed. This extension passed syntax, CLI, region-selection, read-only inspection, and safe non-root refusal checks; its privileged execution and signal cleanup have not been validated live.
