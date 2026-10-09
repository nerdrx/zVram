# Pressure-triggered residency validation

Tested on 2026-10-09 with AMD RX 7900 XTX / RADV NAVI31, after confirming the GPU was idle and LACT performance mode was `auto`.

Automatic live launches and manager Vulkan profiles now retain eligible backing while it fits the native admission budget and configured resident cap. The background worker samples the effective budget once per pass, selects the oldest completed eligible chunk, and attempts at most one chunk per pass. Explicit idle experiments retain their previous policy; `--vulkan-eviction-trigger pressure|idle` selects it explicitly.

Validation:

- 18 Python launcher, manager, model and packaging tests passed.
- All 13 local CPU CTests passed.
- `vulkan-pressure-only-idle-regression` passed with Vulkan validation: a written 32 MiB allocation remained resident for 500 ms with a 100 ms idle interval; a separate 64 MiB allocation created pressure under a 64 MiB cap; after freeing that allocation, the original restored and its full-byte pattern matched. Cleanup retained no backing or snapshot errors.
- Both graphics cold-restore tests and all four synthetic/native single/two-queue pressure tests passed.

The fixture initially created its pressure allocation before the below-cap assertion; that fixture ordering was corrected before the passing run. Upload uses one staging chunk repeatedly, keeping application fixture allocations to 128 MiB including staging. Layer helper buffers are additional.

These results prove retention and cold restoration correctness for these fixtures, not a measured VRChat FPS improvement. A configured residency cap still applies even when more VRAM is free. Warm nonlocal backing is not migrated by this change, and application staging/GTT allocations remain application-managed.
