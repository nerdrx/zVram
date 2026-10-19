## Fixed

- A frequently used buffer whose replacement allocation returns device-memory OOM now waits one second before retrying, giving smaller eligible buffers a chance to recover into VRAM.
- Retains the existing 250 ms device-wide failure backoff, in-flight guards, and ambiguous-failure handling. Host-memory OOM behavior is unchanged.

## Validation and limits

- A deterministic CPU allocator-failure fixture reproduces the old repeated selection and verifies the fix, unchanged ownership/accounting on failure, exact retry boundaries, and successful smaller-buffer recovery.
- Normal and hook-enabled bootstrap checks pass. Production build and all 18 CPU/submission checks pass.
- This is fault-injected CPU evidence, not a real-device OOM reproduction or a game performance claim.
