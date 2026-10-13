# Compatibility fixes, 14 October 2026

## Maintenance4 buffer memory requirements

Vulkan 1.3 / `VK_KHR_maintenance4` applications can query a buffer's memory requirements before creating it. zVram previously forwarded both `vkGetDeviceBufferMemoryRequirements` names without the transformations applied by `vkCreateBuffer`, omitting its virtual memory type and potentially reporting requirements for different sparse/usage/sharing parameters.

The core and KHR wrappers now query the same transformed create info as actual creation, preserve the outer query and output chains, and add the virtual bit only for eligible buffers. Noneligible queries pass through. Each alias remains unavailable if its corresponding downstream entrypoint is unavailable. A shared helper prevents the create and query paths from drifting. If queue-family preparation runs out of host memory, the void query falls back to downstream native requirements without advertising the synthetic bit; actual creation retains its existing out-of-host-memory result.

Reference: [Khronos query specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceBufferMemoryRequirements.html).

### Verification

- Normal layer and CPU harness built. All 14 CPU CTests passed in 5.76 s, including actual-wrapper transformation/alias/pNext/passthrough checks.
- Production library metadata says test hooks disabled; all six compile-only API names are absent.
- The same standalone checker fails with the preserved old layer because the virtual type is absent, then passes with the patched normal layer on RADV / RX 7900 XTX. Both core and enabled KHR queries match subsequent created-buffer size, alignment, type bits, and dedicated-allocation output. Noneligible uniform buffers retain native requirements.
- Automatic range mode also passed the checker (0.06 s). Synchronization validation produced no VUID/error. These tests create unbound 4 MiB buffers; they allocate no GPU backing memory and submit no GPU commands. Final driver live allocations are zero.
- CTests `vulkan-maintenance4-requirements` and `vulkan-maintenance4-auto-requirements` have 15-second limits and return honest skip 77 on unsupported devices. This is API-level driver evidence, not an application/game compatibility guarantee.

Raw evidence lives beside this file. `initial-unconfigured-target.txt` records an initial build invocation before CMake regenerated the newly added target; it is not counted as a successful build.

## Ambiguous launcher paths

Linux `VK_LAYER_PATH` is colon-delimited. A build/install path containing `:` previously split into multiple search entries, allowing the child to launch without discovering zVram. The launcher now refuses that ambiguous path with a relocation instruction before executing the child. Paths containing spaces remain accepted. This adds a clear diagnostic; it does not add support for colon-containing install paths.

Package/manager/control tests pass: 16 checks in 3.886 s. The focused checks prove the colon path cannot execute its child and a space-containing path is preserved. No actual Steam/Proton game was launched.

No release, installation, default-policy change, or user application/GPU-setting change occurred.

## Maintenance6 buffer bind status

The layer rejected the valid VkBindMemoryStatusKHR chain for promoted buffers, and returned immediately on the first buffer bind failure even when the application enabled maintenance6. The fix tracks the requested KHR or Vulkan 1.4 feature, accepts only the status node for promoted binds, attempts all entries when required, populates each result, and preserves an aggregate failure. Unknown promoted-buffer chains remain rejected. Native binds retain their original pNext chain; no feature is enabled for the application.

- Root reviewed the patch and independent review found no blocker. CPU tests exercise actual wrappers, feature detection, promoted status success, unsupported-chain rejection, native failure followed by later success, and disabled-feature early exit. All 14 normal CPU tests passed in 5.09 seconds (`maintenance6-cpu.txt`).
- New optional `--bind-status` checker/CTest uses a 4 MiB virtual eligible buffer and a 4 KiB host-visible native uniform buffer in one batch, with a status output for each. Buffers are destroyed before their memory on normal and exception paths. It skips with code 77 when extension support is unavailable; it submits no rendering or copy commands.
- Same checker with preserved 3442397 normal library failed as expected: return -8, both status sentinels untouched (-13). With the patched normal library, both bind results and batch result were SUCCESS, CTest passed in 0.05 seconds. Synchronization validation reported no VUIDs/errors, and final driver local/nonlocal live allocations were zero in both runs. Raw logs and fresh health snapshots are retained as `maintenance6-before*` and `maintenance6-after*`. This proves the KHR extension path on RADV/RX 7900 XTX, not broad driver compatibility or performance.
- Production library remains hooks OFF with all six compile-only API strings absent; SHA and CPU result are in `maintenance6-normal-build.json`.
- Root caught a test-only preprocessor issue for older headers during review; the owner guarded the maintenance6 mock as a unit before final handoff. No production behavior changed for that correction.

Specification: [vkBindBufferMemory2](https://docs.vulkan.org/refpages/latest/refpages/source/vkBindBufferMemory2.html), [VkBindMemoryStatus](https://docs.vulkan.org/refpages/latest/refpages/source/VkBindMemoryStatus.html).
