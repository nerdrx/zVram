# Combined LFU cache-cap trial: GPU out-of-memory abort

This 92-token InternLM2.5-20B F16 trial combined immutable BP16 owner
validation with a 28 GiB cold/allocated-owner cap, 21 GiB tracked-resident cap,
1 GiB Vulkan headroom reserve, LFU clean-cache policy, 32 encoder workers, and
eight upload workers. The cold and allocated-owner caps are separate limits,
not additive.

The child exited **-6** after prompt submission. stderr reports
`vk::Queue::submit: ErrorOutOfDeviceMemory`; no `ErrorDeviceLost` message was
found. There is no completed 92-token result, output hash, or throughput rate.
System `MemAvailable` stayed at or above **30,620 MiB**, above the **16,384 MiB**
safety floor, and swap growth was zero. Snapshot-tracked resident backing
peaked at **22,509,125,632 bytes**; sampled DRM VRAM residency was
**20,796,704 KiB**. These measurements record the failure context but do not
establish a single cause.

The trial used commit `dba6a62` and the BP16 binary SHA-256
`69b48069b2d60a0110b8d8297f3f9bfc6c47c9b8c9b55fa5948549cb983e4779`.
This is a diagnostic failure only; it is not a performance result.

Artifacts: [result](automatic-result.json), [command](automatic-command.json),
[resources](automatic.resources.json), [memory samples](memory.jsonl),
[stderr](automatic.stderr.txt.gz), [fdinfo](automatic-pressure.fdinfo.txt),
and [source/runtime provenance](runtime-source-context.json).
