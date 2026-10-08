# Opt-in BP16 restore batching

Compiled code: 9565d86. Up to four immutable allocated-host BP16 frames share one private decoder submission/fence. Default off (`ZVRAM_VULKAN_BP16_RESTORE_BATCH=1`).

Full regression: 123/123 passed in 86.11 seconds, including validation-enabled synthetic and native fixtures. Each new fixture reported one four-item GPU restore batch, checked every application byte, and reported zero GPU restore fallback and no Vulkan validation diagnostics. Initial fixture attempt used incompressible compute output, selected raw snapshots, and correctly failed the actual-GPU-decode gate; the corrected fixture preserves partially constant upload data.

The batch path retains serial fallback for noneligible frames. Submit/wait uncertainty poisons the decoder and retains resources. No model throughput claim until separately measured.

With encoder, allocated host input, batching and profiling enabled, existing GPU checks passed 8/8 in 6.29 seconds.
