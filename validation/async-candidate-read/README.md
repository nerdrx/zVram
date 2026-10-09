# Async candidate read interaction

Fixture-only coverage of an exact 4 MiB tracked virtual child while its snapshot
is encoded off-lock. No production change or new test API is introduced. Idle
trigger, a 1000 ms deadline and the existing async encode hook isolate the first
candidate from pressure admission and later idle retries.

The completed-read callback submits a finite TRANSFER_READ barrier and copies the
same candidate to host-coherent staging, then completes its queue. Main-thread
hook removal drains the first encoding decision. A single callback and a decision
within 500 ms of that read exclude a later retry. The candidate commits cold,
restores, and preserves every byte.

The held-read callback submits the same read behind a fresh timeline wait for
value 1. After hook removal drains the encoding decision, the candidate must remain
resident with no freeze, and the read fence must remain NOT_READY for 200 ms. A host
signal releases the read independently of queue tails. Its completed fence permits
byte inspection and resource cleanup. A two-second host-signal watchdog bounds
unwind; it is joined before reading its result. Unknown completion errors retain
participating objects instead of freeing in-flight storage.

Root hardware verification on RX 7900 XTX/RADV, 2026-10-09 18:18 UTC:

- Completed read: PASS, 1.38 s; first candidate froze once and restored once.
- Held read: PASS, 1.28 s; no freeze while its actual reference was pending.
- Both: full 4 MiB integrity, synchronization validation without VUID/errors,
  final resident/cold/stored/failures and driver live-local/live-nonlocal all zero.
- Raw results: `final-gpu-ctest.txt` and `final-gpu-details.txt`.

Both tests have effective CTest TIMEOUT=30 after the global timeout assignment.
The corrected pair additionally ran under a 15-second outer timeout. No CPU suite
rerun was needed for these fixture-only changes. Installed v0.4.19 and defaults
remain unchanged. These are correctness gates, not game or latency measurements.

## Preserved failures and review corrections

Initial static review caught copied submitted-state lifetime risk and missing
first-decision timing. The final guard reads atomic submitted state after hook
drain; it retains resources unless actual fence completion/device loss is proven.

The first completed-read run passed (1.36 s), but the original held fixture used
an unsignaled binary semaphore and was rejected by validation, then hit its
effective 60-second CTest timeout. The individual 30-second timeout had been
replaced by the later global setting. `initial-held-timeout-ctest.txt` and
`initial-held-timeout-details.txt` preserve the failed pair unchanged. No residual
owned fixture process remained. The correction uses timeline host signaling and
a final timeout override; it does not weaken validation or production guards.
