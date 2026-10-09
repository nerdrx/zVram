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

## Small offscreen graphics probe

`graphics-probe.py` compares 12 validated draw/readback frames in native, legacy idle-snapshot, and pressure-retention modes, with 150 ms idle gaps and a 100 ms snapshot timer. It disables unrelated implicit layers for all modes. The pressure mode asserts the 32 MiB allocation remains resident with no freezes before every draw. Every frame checks rendered pixels and the full buffer.

One local run returned median draw/readback times of 179.243 ms native, 183.257 ms idle, and 177.704 ms pressure. These times include CPU verification of 32 MiB per frame and are not FPS measurements; ordering, warmup and noise prevent a reliable percentage speedup claim. The useful gate is residency plus byte/pixel integrity across idle gaps. The first native attempt without implicit-layer isolation encountered an unrelated broken LSFG layer entry point and failed validation; all recorded successful comparisons use the same isolation.

The private live-cap fixture also passed: 64 MiB stayed resident under a 96 MiB cap; a request lowered the cap to 32 MiB without app submissions, leaving exactly one chunk cold. Raising it to 96 MiB allowed byte-correct restoration of both chunks. Pending status was observed in the run, but the test does not require catching that transient between host polls. The three-frame graphics retention CTest passed too.

The plain launcher's automatic ceiling was subsequently changed from half the detected VRAM to all detected VRAM, retaining the 1536 MiB native budget reserve and dynamic budget clamp. Explicit user caps and manager priority presets remain unchanged. This affects new launches and is not a physical reservation or a warm-GTT migration mechanism. Launcher plumbing tests verify the ceiling, explicit override and reserve.
