# Local full-codec release candidate

Candidate `0.4.20-dev.1d4938e` packages runtime source `1d4938e` locally. It is
not published or installed. Installed v0.4.19 and all defaults remain unchanged.

The full-codec Release configuration (`build/gdeflate-codec`) enables GDeflate,
GDeflate GPU, BP16 and BP16 GPU with test hooks OFF. Every executable used by
its 15 CPU tests was rebuilt before the suite; all 15 passed in 6.49 seconds.
Build/configuration/test evidence is in `build/` and `cpu/`. Package tests
passed 3/3. No HIP wrapper is in the userspace Vulkan package.

`payload.json` records the exact archive and library SHA256 values. All archive
checksums verify, launcher/manager/control/model/UI bytes match current source,
the packaged library matches the full-codec build, its manifest is relocatable,
and all six compile-only API names are absent. The capability metadata has
`test_hooks=false`; GDeflate/BP16 capabilities remain present.

Root ran the UNPACKED package with recovery, zero quiet delay and asynchronous
compression enabled, under synchronization validation and a 15-second outer
bound. The small live-cap fixture evicted exactly one 32 MiB range, raised the
cap and verified all original bytes. Final resident/cold/stored/cache/failures
and driver live-local/live-nonlocal were zero. `packaged-live-cap.txt` preserves
actual output. No validation errors/VUIDs were emitted. This verifies packaged
paging behavior, not selective graphics, warm-recovery timing or gaming FPS.

The release-readiness review found no additional default/config/test-API blocker.
Unlocked recovery remains experimental and opt-in; sparse transitions still
wait synchronously. Unknown graphics still conservatively restores all relevant
buffers. Both real device/queue idle waits returning ambiguous errors deliberately
retain uncertain resources; native-child cleanup remains an abnormal teardown
limitation, not a global safety claim. No fault reproduction was performed.

The README's old no-cold-data restriction was corrected to the tested finite,
resident, disjoint HOT contract with unrelated cold peers. Publication/install
is a separate step and must verify the eventual CI-built archive, not substitute
this locally built candidate under an existing release tag.
