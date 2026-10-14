# Dedicated sparse queue compatibility

The bundled stable-diffusion.cpp Vulkan backend on the container RADV driver exposed queue families (flags/count): 7/1, 6/4, 32/1, 64/1, 8/1. Applications requested families 1 and 0. No family supported both command execution and sparse binding. The old layer rejected automatic paging with `VK_ERROR_FEATURE_NOT_PRESENT` before image generation.

The fix retains combined private queues where available, otherwise reserves an unused command queue and a distinct unused sparse queue. Existing before/after binary semaphores order copy -> sparse bind -> copy, and completion waits on the copy queue. A missing sparse queue fails closed. CPU regression traces enforce the queue/semaphore sequence.

A separate real-loader device-only probe demonstrated an additional Features2-chain issue: loader nodes preceding Features2 prevented safe core-feature injection. The installed layer failed; a patched layer preserving cloned loader prefixes succeeded. Unknown prefix structures still fail closed.

## Live evidence

- SD 1.5 Q4 GGUF, stable-diffusion.cpp 1d0a58a35050d285b1d6fb1bc68a92924d35825e, RX 7900 XTX, isolated port 18124.
- zVram recovery enabled, 512 MiB resident ceiling, 1536 MiB native headroom; existing services were preserved during tests.
- Neutral prompt: a red ceramic teapot on a wooden table.
- 512x512, 4 steps: HTTP 200, PNG 301592 bytes, server generation 12.03 seconds.
- 512x512, 20 steps: HTTP 200, PNG 334213 bytes, server generation 19.07 seconds.
- Test servers stopped after each run. Evidence retained locally under /tmp/nx-zvram-feature-chain-qa.
- No FLUX inference with the new paging path was performed: the user requested no further image generation and freed VRAM for gaming. This is not a broad workload/performance claim.
