# Initial sparse transition batching

The candidate has no active references and the device/queue locks exclude new
accesses. The single VkBindSparseInfo contains application alias unbinds
plus both private source/old-backing and destination/replacement bindings. All
plans must be stable before the driver call; child metadata remains unchanged.
Copy starts only after the existing completion wait succeeds. No ordering among
individual binds is assumed. Pre-driver plan failures can clean newly allocated
resources; ambiguous downstream failures retain both allocations and private
views behind the device gate. The final rebind/visibility and free-old-last
ordering stay intact.

The Vulkan sparse-resource specification documents ordinary aliasing rules and
unordered batches: https://docs.vulkan.org/spec/latest/chapters/sparsemem.html .
This uses a no-access transition, not data-consistent concurrent sparse aliases.

`before-backend.json` identifies a preserved hook backend with its own manifest;
it is test-only, never installed. `before.txt` is one five-transaction natural
cold-peer sampler run, unlocked wait enabled. All original 32 MiB and peer 4 MiB
bytes, pristine peer retention, synchronization validation and cleanup passed.
Strict analysis confirms five transactions. Initial-phase sums were
137/129/64/65/634 us. These include planning/driver/wait time and are variable;
they do not establish a latency gain.

`compare.py` compares ONLY the sum of alias-unbind-us and private-bind-us across
revisions. Existing field names are preserved: after batching the first field
includes the combined transition and the second marks no additional driver work.
Independent implementation review passed. Normal and hook layer/bootstrap builds
and CPU gates pass, including initial plan refusal without driver mutation,
initial sparse-submit and completion failures retaining both backings/views,
copy rollback, final-batch/visibility failures and disjoint multi-alias offsets.
Root authoritative normal `build/` final CPU suite passes 13/13 in 5.35 s;
see `final-cpu-ctest.txt`. All six test API strings are absent from the production library; see
`production-hooks.json`. Existing diagnostic record formats stay intact; the
app sparse-batch count includes the two private-view binds in this setup call.

Four bounded GPU regressions passed in 2.65 seconds: BDA integrity, blocked two
queues, disjoint HOT progress with unrelated cold, and late-cap rollback. All
bytes and zero final snapshot/driver live bytes are preserved with clean
synchronization validation. `gpu-ctest.txt` and `gpu-details.txt` retain raw proof.

The matched after run also passed all five original 32 MiB and peer 4 MiB checks.
Initial-transition sums were 150/65/53/60/60 us after, compared with
137/129/64/65/634 us before; medians 60 and 129 us. This is ONE order/load-sensitive
component comparison, including host planning and queue waits. It does not
establish stable overall/game latency improvement. The mechanism removes one
sparse submission/completion wait; no wait or lifetime guard is bypassed.
See `before.txt`, `after.txt`, `summary.json` and the strict five-transaction parser.
Installed v0.4.19/defaults remain unchanged. No FPS claim.
