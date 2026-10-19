## Fixed

- Prevent unrelated GPU submissions from repeatedly retrying a buffer whose snapshot cannot fit the cold-store quota, allowing other eligible buffers to proceed.
- Retry a refused child after accepted writes that may change its contents or after cold-store space changes. Unknown accepted access remains conservative.

## Validation and limits

- Reproduced the prior starvation in a deterministic CPU fixture; the fix permits a smaller snapshot while preserving the refused buffer.
- Normal and hook-enabled bootstrap checks pass, including scoped writes, reads, unknown access, quota changes, and epoch saturation. Full CPU/submission suite: 18/18 passed. Production layer build passed.
- Existing in-flight safety guards, quota accounting, and recovery defaults are preserved. No game FPS or migration-latency claim.
