# zVram 0.4.29

- Recover eligible GPU buffers by recent known-submission frequency, with recency breaking ties. The bounded frequency hint decays so old activity does not keep priority indefinitely; unknown accesses do not inflate frequency.
- Continue pressure paging sooner after a successful move, using a 1 ms worker pause instead of the normal up-to-10 ms poll. Blocked work retains the slower retry, and zero idle settings no longer create a zero-wait polling loop.
- Preserve application-use timestamps and frequency history through synchronous and asynchronous compression, including reads accepted during encoding.
- Add CPU coverage for ranking, decay, duplicate references, metadata preservation, pressure backlog draining, busy-buffer protection, and worker shutdown. Ensure hook-enabled CPU builds actually compile their hook checks.

In-flight, compatibility, quiet-time and memory-budget checks remain in force. Pressure eviction remains oldest-first. Recovery controls eligible buffers, not arbitrary files/images or kernel-transparent GTT placement. Scheduling intervals are not migration-latency or game-FPS guarantees.

Validation: 18 CPU/submission checks passed; genuine hook-enabled bootstrap checks passed; bounded native buffer-device-address GPU recovery preserved every byte and cleaned up all tracked resources. No game FPS improvement has been established.
