# Read-only finite transfer barrier cache reuse

A barrier with only zero/TRANSFER_READ source and destination masks describes
read access scopes; it does not introduce a buffer write. The conservative
classification is limited to the existing finite, valid promoted-buffer range
with a null extension chain and both queue-family indices IGNORED. Every other
access bit, including HOST_WRITE, other read bits and unknown bits, stays write
classified. Whole ranges, ownership transfers and unknown extensions retain
fallback behavior. Range references remain selected; actual fill/copy-destination
writes and unknown commands still invalidate cached snapshots and async epochs.
See the official [Vulkan barrier access scopes](https://docs.vulkan.org/refpages/latest/refpages/source/VkBufferMemoryBarrier.html).

The nine-line shared helper change applies to all four legacy/synchronization2
barrier wrappers. Current normal and hook layer/bootstrap builds pass. The actual
normal build is `build/`; its full 13-test CPU suite passes in 5.50 seconds,
including wrapper/reference/cache/async-epoch and later-write/unknown gates.
All six test API strings are absent from that library (production-build.json).

One bounded 64 MiB native two-range fixture uses a 32 MiB resident cap, exact
finite read-only barrier/copy cycles, then a real fill and full changed/unchanged
range checks. Existing final layer telemetry proves four clean-cache reuses,
zero read-phase invalidations and one actual-write invalidation before cleanup.
Final resident/cold/cache/failure counters and driver backing bytes are zero.
The production normal CTest passes in 0.39 seconds. It runs check_log.py --run;
byte-only fixture PASS alone cannot satisfy the mandatory cache telemetry proof.
No timing, FPS or game-smoothness conclusion is inferred.

The same fixture with the preserved c3761ea hook backend verifies bytes but
records zero clean-cache reuses; check_log.py rejects it as expected. Before and
after logs are untouched. The initial old fixture wording claimed cache reuse
without telemetry; it was corrected to a byte/cleanup-only PASS, with external
telemetry mandatory in CTest. The parser recognizes the archived old marker but
still rejects its missing cache reuse. The copied backend is test-only and is
never installed or packaged.

An initial comparison accidentally used the stale gdeflate-codec backend, which
predated finite range tracking: it refused the read at the working-set cap.
That log and initial provenance remain archived and are not the cache comparator.
Initial root CPU invocations also selected stale/other build directories; those
outputs are preserved, but only the current `build/` 13-test suite is counted.
This corrects the earlier final-sparse-batch root suite provenance too: current
normal coverage includes those final-batch gates. Agent normal/hook bootstrap
gates had used current build directories throughout.

Installed v0.4.19 and all feature defaults remain unchanged. No release, tag,
installation or GPU-fault reproduction is part of this change.
