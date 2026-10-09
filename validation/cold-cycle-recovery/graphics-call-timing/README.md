# Graphics API-call timing, separate from CPU verification

Six offscreen frames per mode, RX 7900 XTX / RADV, Vulkan validation on. zVram modes use installed v0.4.10; native baseline loads no zVram layer. The instrumented source fixture measures only frame vkQueueSubmit and vkWaitForFences calls, excluding upload, CPU full32MiB comparison and pixel checks. All byte/pixel checks remain and pass. Implicit layers are disabled for every mode; an initial non-isolated baseline hit the installed broken liblsfg-vk-layer.so loader entry and is preserved separately, not used below.

| Mode | Submit p50 / max | Fence wait p50 / max |
| --- | --- | --- |
| Native | 0.016 / 0.058 ms | 1.903 / 2.053 ms |
| zVram warm 32 MiB | 0.104 / 0.129 ms | 2.018 / 2.080 ms |
| zVram forced-cold 32 MiB | 11.705 / 13.714 ms | 1.348 / 1.366 ms |
| zVram forced-cold 4 MiB | 8.303 / 10.316 ms | 1.357 / 1.420 ms |

Cold modes deliberately idle-evict all32MiB between frames; warm mode uses pressure-only retention. Both modes verify the complete32MiB buffer and every output pixel on every frame. These are CPU wall durations of API calls, not GPU timestamps, present latency, game frame times or FPS. The readback-heavy fixture differs from a game. The original draw-readback metric remains dominated by CPU validation (roughly180–220ms), so it must not be used to infer GPU frame rate. Smaller ranges still restore all eight4MiB children when this full-buffer draw needs them; reducing individual worker stalls does not eliminate total restore work. These bounded observations support testing the existing4MiB tuning option, not changing defaults. Raw logs and summary.json retain results.
