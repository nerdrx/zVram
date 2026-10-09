zVram 0.4.20 improves paging synchronization and preserves clean snapshots for narrowly tracked read-only transfers.

- Finite, validated buffer barrier ranges retain precise child references. Transfer-read-only barriers no longer falsely invalidate clean snapshots or advance write epochs; actual writes and unknown accesses remain conservative.
- Initial and final warm-recovery sparse transitions each batch the applicable private and application buffer bindings into one submission and completion wait.
- Experimental unlocked recovery waits permit validated, disjoint resident transfers to proceed while a recovery copy completes, including with unrelated cold data. Matching, cold and unknown accesses still wait; identity, lifetime and live-cap checks remain enforced.
- Teardown requires real driver completion proof. Proven-idle resources clean up exactly once; ambiguous failures retain uncertain resources and suppress late decoder calls.

Recovery, asynchronous compression, buffer presentation and unlocked recovery remain off by default. Unlocked recovery requires local recovery and is enabled only with ZVRAM_VULKAN_UNLOCKED_RECOVERY_WAIT=1. General graphics retain conservative restoration; sparse transitions still wait synchronously. If both real device and queue idle checks fail ambiguously, uncertain resources are deliberately retained and native child cleanup remains an abnormal teardown limitation.

Validation includes the rebuilt full-codec 15-test CPU suite, package checks, bounded Vulkan synchronization validation, full-byte paging/cap rollback, concurrent read/write guards, and zero-resource cleanup checks. Component timing samples do not establish stable overall latency or gaming FPS improvements. No GPU fault reproduction was performed.

Production packages exclude all six compile-only test APIs. This release does not merge PR #1.
