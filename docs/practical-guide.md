# Practical setup and limits

## Choose a path

| Use | Starting point | Limit |
| --- | --- | --- |
| Steam/game experiments | `zvram -- %command%` in Steam launch options | Eligible Vulkan buffers only; not universal texture paging or a guarantee of better FPS. |
| Vulkan model workloads | Manager/model setup and an existing Vulkan-enabled server | The active working set still needs usable VRAM/GTT; compression and restores can reduce throughput. |
| Application-owned buffers | Managed Vulkan pool API | The application explicitly participates. |
| HIP research | Optional source build | Narrow allocation/VMM/idle-hibernation paths; not a production-certified general HIP memory manager. |

See [manager controls](manager.md), [measured results and failure records](VALIDATION.md),
and the [HIP scope and ABI restrictions](../README.md#userspace-hip-hibernation).
There is no universal expected speedup or all-driver compatibility guarantee.

## Manual package use

NX Hub is the supported update/desktop-integration path. For manual use, download
`zvram-<version>-linux-x86_64.tar.gz` and `SHA256SUMS` from the same GitHub release.
Compare the archive's `sha256sum` with its entry in that file before extraction.
Extract into a new directory, then run the included launcher directly:

```sh
tar -xzf zvram-0.4.31-linux-x86_64.tar.gz
./usr/share/zvram/0.4.31/zvram --version
./usr/share/zvram/0.4.31/zvram gui
```

This example uses 0.4.31; substitute the version actually downloaded. Keep the
whole versioned directory together: the launcher needs its sibling modules and
`build/` payload. Direct execution does not create a desktop entry or put `zvram`
on PATH. Do not overwrite the backend files of a running app. Existing processes
keep their loaded version; new launches use the launcher you select.

The published package requires Linux x86_64, glibc 2.39+, Python 3.9+, Tk, libzstd,
and a working Vulkan driver. Older glibc systems need a compatible source build;
copying a newer libc into the package is not a supported workaround. HIP and
models are not included in the Vulkan runtime archive.

## Source-build options

Start with CMake 3.20+, a C++17 compiler, Python 3, Vulkan headers/loader,
`glslc`, pkg-config and zstd development files. A Vulkan-only build is:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DZVRAM_BUILD_HIP=OFF
cmake --build build -j2
```

| CMake setting | Default | Purpose |
| --- | --- | --- |
| `ZVRAM_ENABLE_GDEFLATE` | OFF | Experimental codec and optional GPU restore; fetches a pinned dependency when enabled. |
| `ZVRAM_ENABLE_BP16_GPU_TESTS` | OFF | Registers opt-in hardware integrity tests; not the switch for ordinary manager operation. |
| `ZVRAM_BUILD_GDEFLATE_RESEARCH` | OFF | Standalone research host, separate from production integration. |
| `ZVRAM_BUILD_HIP` | ON | Builds the optional HIP backend when development dependencies are available. |
| `ZVRAM_TEST_ASYNC_HOOK` | OFF | Deterministic test-only hooks. Keep OFF for distributable packages. |

`--vulkan-virtual-gib` is a launcher/runtime setting, not a missing CMake feature.
It changes the logical capacity exposed by the experimental path, not physical
VRAM. The synthetic heap currently requires a discrete RADV device, sparse
binding and a sparse-capable queue; otherwise native memory properties remain
unchanged. Building against the Vulkan loader does not itself require RADV,
but that does not make the synthetic heap available on other drivers.

## Limits and troubleshooting

- Start with defaults and one workload. Leave native VRAM headroom for the display,
  other apps, and temporary copies. A resident cap is not a machine-wide VRAM cap.
- Cold storage consumes system RAM, including raw fallback when data compresses
  poorly. Increasing its quota can increase host pressure. Preserve RAM/swap guards.
- A high recovery priority changes eligibility; it does not override in-flight
  work, allocation compatibility, quotas or native-budget checks.
- If usage does not move, confirm the layer loaded in the manager's Details view.
  A configured launch is not proof of a loaded layer. Native images, unsupported
  buffers and kernel-transparent GTT placement remain outside direct control.
- If performance worsens, compare the same workload without the wrapper. Snapshot
  copies, compression, waits and restores have costs; historical throughput in
  [VALIDATION.md](VALIDATION.md) is workload-specific, not a promised gain.
- For launch failures, check `zvram --version`, PATH, package dependencies and the
  selected profile log. Capture launcher output when reporting a failure:
  `zvram -- your-command > /tmp/zvram-app.log 2>&1`.
- A restore error is not permission to reuse unavailable data. The layer propagates
  failures and gates unsafe/ambiguous paths; see the restoration and teardown tests
  and limits in [VALIDATION.md](VALIDATION.md). Save the log and reproduce with a
  small workload rather than disabling safety checks.

## Saved profiles and upgrades

Manager state defaults to `~/.local/state/zvram`; `XDG_STATE_HOME` and
`ZVRAM_MANAGER_HOME` can change that location. Back it up before manual edits or
trying an older manager. `profiles.json` is an object keyed by profile name:

```json
{
  "example": {
    "name": "example",
    "mode": "vulkan",
    "priority": "normal",
    "command": ["/absolute/path/to/application", "argument"],
    "env": {}
  }
}
```

Commands are argument arrays, not shell command strings. Supported modes are
`native`, `vulkan`, and `wrapped`; priorities are `high`, `normal`, and `low`.
Optional limit keys are `resident_mib`, `cold_mib`, `min_available_mib`, and
`max_swap_growth_mib`; `ignore_swap_guard` is boolean. Prefer saving through the
manager, which validates names, values and environment keys. Saving retains only
recognized launch fields; process identity and telemetry are not profile fields.

Live process priorities and remembered Steam priorities use separate
`live-priorities.json` and `steam-priorities.json` files. Job/status files are
runtime state, not portable launch profiles. Package updates preserve the state
directory, but there is currently no versioned profile-schema migration protocol
or guarantee that future fields survive a downgrade. Do not describe state
preservation as a formal migration guarantee.
