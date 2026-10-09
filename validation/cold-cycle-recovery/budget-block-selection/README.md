# Generation-scoped recovery budget refusal

The recovery selector now skips an oldest chunk whose snapshot budget refusal still matches both current budget and submission generations. Previously freeze returned success without creating a snapshot, and recovery retried the same nonlocal chunk every 250 ms. A younger chunk that fits the quota could remain starved.

The selector uses the existing pressure-path predicate. A changed budget generation or submission generation makes the chunk eligible again. No allocation, quota, transaction, rollback or default policy changed. CPU two-pool gates require the younger chunk to win while the older one is blocked, and independently verify retry eligibility after each generation changes. This fixes opt-in scheduling starvation, not game FPS.

All 13 normal CPU checks passed in 5.32 seconds. Both CPU bootstrap builds passed during implementation. The normal shared layer contains neither test setter. The bounded hot-recovery single/two-queue GPU gates retain full-byte integrity and pending-queue deferral checks; their archived result is `gpu.txt`.

## Published and installed v0.4.16

Release 37889013680 and all three Build workflows passed. Version 0.4.16 was already installed when checked. The downloaded archive passed SHA256 verification, its layer matches the installed bytes, scripts match the tag, and test hooks are disabled and absent. See `installed-0.4.16-payload.json`. The installed live-cap fixture with recovery, zero quiet delay and async compression passes full-byte cap-lower/raise behavior and zero resident/cold/errors at teardown; see `installed-0.4.16-live-cap.txt`. This checks the packaged regression path; generation-block selection itself is proven by the CPU gate.
