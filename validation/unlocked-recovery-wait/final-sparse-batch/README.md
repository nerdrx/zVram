# Final sparse transition batching

After the copy fence proves completion, private source/destination view unbinds
and all application alias rebinds can share one `VkBindSparseInfo`. Its signal
scope covers all sparse operations in that structure, as documented by the
[Vulkan specification](https://docs.vulkan.org/refpages/latest/refpages/source/VkBindSparseInfo.html).
The existing sparse completion wait remains. Plans use an explicit replacement
backing without publishing the child/type before successful completion.
Failures retain both backings and views behind the device gate. Visibility
and generation handling still precede freeing the old backing last.

The preserved pre-change hook backend is a test-only copy with its own manifest
path and library hash, recorded in `before-backend.json`. It is not installed or
packaged. Before/after measurement uses unlocked waiting in both, the same
natural cold-peer fixture, and unchanged caps/quiet/cadence.

Existing profile field names/order are retained for consumers. **Their final
phase meanings change:** before, `private-unbind-us` is the private unbind and
`app-rebind-us` includes the app rebind; after, `private-unbind-us` includes the
combined private/app sparse batch and `app-rebind-us` is the child/type commit.
Only their **sum** is compared across revisions. It includes plan construction,
driver work and completion waits, not pure GPU execution. `compare.py` requires
five complete unlocked transactions and five cold-peer proofs on each side.

Independent implementation review passed. Normal/hook layer and bootstrap builds passed.
The original root 13-test log used a stale normal build directory and is not current harness proof. The corrected authoritative normal `build/` suite passes 13/13 in 5.50 seconds, including final-batch gates; see validation/read-only-transfer-barriers/cpu-ctest.txt. Failure gates cover
plan refusal before driver submission, final sparse submission/completion errors,
and post-bind visibility failure; they preserve old/new resources and gate access.
The multi-alias fixture uses disjoint application ranges consistent with runtime
admission. All six test API strings are absent from the normal library.

Four bounded GPU regressions passed in 2.58 seconds: BDA integrity, blocked two
queues, disjoint hot progress with an unrelated cold peer, and late-cap rollback.
Raw details are in `gpu-details.txt`; no device-fault injection was performed.

One matched before/after pair passed all five original 32 MiB and cold-peer 4 MiB
integrity/validation/cleanup checks on each side. Final-transition sums were
82/71/67/88/88 us before and 101/70/71/76/97 us after; medians 82 and 76 us.
This small, variable observation does not establish a stable latency improvement.
The implementation removes one final sparse submission/completion wait while
preserving completion proof and freeing the old backing last. See `summary.json`
and both raw logs. No repeated measurement was used to select a favorable result.
No default, release, install or game-performance claim changes.
