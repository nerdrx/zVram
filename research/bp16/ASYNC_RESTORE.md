# Asynchronous restore: constraints and next experiment

The current decoder submits one restore and waits before reading its error mask.
The layer then releases the private view, remaps application buffers, and commits
cold/resident accounting. A later application-queue semaphore wait alone cannot
replace that fence: it orders GPU accesses but cannot make the CPU error decision.
The decoder also reuses one command pool, descriptor set, scratch buffer, and
error readback. Queuing another restore would overwrite live state.

A useful overlap implementation needs bounded per-flight state, rather than a
missing wait. Each flight must retain its encoded owner, backing allocation,
private view, command resources, error readback, and resident-budget charge until
completion. Application work may use the restored range only after successful
error validation and the mapping/visibility transition. Timeout or uncertain
submission must gate the device and retain resources; it must not retry into or
free potentially live memory. Unknown accesses retain conservative handling.

A narrower experiment can prepare the alias-remap plan during a single decode,
then complete and validate before committing it. This overlaps CPU preparation,
not model execution. Prior four-frame restore/remap batching and bounded fence
polling showed no meaningful end-to-end gain, so this remains a hypothesis.

Before enabling an asynchronous path, validate exact bytes for synthetic and
native backing, partial restore failures, multi-queue visibility, host-blocked
queues, unknown accesses, owner lifetime, and budget accounting. Repeat the
unchanged 92-token F16 model with exact output and zero fallback, alongside a
fresh native reference. Keep it opt-in until those gates pass.

## Measure before changing synchronization

With `ZVRAM_VULKAN_GPU_PROFILE=1`, the new optional diagnostic build reports
`GPU restore host split queue-submit-ns=... fence-wait-ns=...`. This separates
downstream submission time from fence waiting while retaining the old combined
metric. Hardware validation is pending; use the next guarded same-profile run
to locate the delay before choosing an overlap implementation.
