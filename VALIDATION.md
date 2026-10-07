# zVram v0.2.0 — hardware validation

Measured on **2026-10-07**. These are prototype integrity and allocation-path checks, not performance benchmarks.

| Environment | Value |
|---|---|
| GPU | AMD Radeon RX 7900 XTX, RADV NAVI31 / HIP gfx1100 |
| Driver | Mesa 26.2.4-arch3.1 |
| OS | Linux, kernel 7.2.8-2-cachyos |
| Physical VRAM | 25,753,026,560 bytes (approximately 24 GiB) |
| AMD GTT limit | 32,411,705,344 bytes (approximately 30 GiB) |
| Host RAM | Approximately 60 GiB usable |
| Build | CMake Release, C++17 |

## Vulkan capacity integrity

The native run and Vulkan-layer run each retained **640 × 64 MiB device-local allocations** simultaneously: **42,949,672,960 bytes (40 GiB)**. The transfer queue uploaded deterministic data to every allocation, then read all allocations back and compared every 64-bit word.

| Run | Data verified | Transfer and verification time | Exit |
|---|---|---:|---:|
| Native RADV baseline | 40 GiB, exact match | 16.0968 s | 0 |
| zVram Vulkan layer | 40 GiB, exact match | 16.1779 s | 0 |

Driver-wide accounting after readback showed approximately 25.42 GB VRAM and 22.67 GB GTT used in each run, including other applications. The layer recorded a 40 GiB peak of requested local allocations, 64 MiB nonlocal allocations, and zero failures. These are allocation counters, not physical residency counters.

**Interpretation:** this RADV setup already preserved and verified more allocation data than physical VRAM by using native system-RAM spillover. The matching baseline and layer results do not show extra capacity caused by zVram, model compatibility, shader random-access behavior, or a performance gain.

```sh
# Native baseline
VK_LOADER_LAYERS_DISABLE='~implicit~' \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
VK_VALIDATION_VALIDATE_SYNC=1 \
./build/zvram-capacity-check --mib 40960

# Same check through the layer
./zvram --validate --isolate-layers -- \
  ./build/zvram-capacity-check --mib 40960
```

Both runs exited 0 without core or synchronization validation diagnostics. Raw logs: [native baseline](validation/native-40gib.txt), [zVram layer](validation/zvram-40gib.txt). The 40 GiB test can pressure the desktop; the utility defaults to 64 MiB.

### Pooled synthetic storage-buffer ranges

The focused Vulkan pool check placed three eligible promoted storage buffers in one 512 MiB synthetic `VkDeviceMemory`: A was 192 MiB at offset 128 MiB, B was 128 MiB at 352 MiB, and C was 32 MiB at offset 0. A crosses the 256 MiB native-child boundary. Full-byte compute/readback checks passed in virtual-only and automatic-snapshot modes through both `vkBindBufferMemory` and `vkBindBufferMemory2`; the BDA automatic-snapshot case also passed. Overlapping ranges were rejected. Tests destroyed and rebound buffers, including after all buffers were destroyed and cold backing remained; contents persisted across rebind. BDA addresses remained stable over cold/restore cycles.

Automatic snapshots covered the full 512 MiB allocation, including gaps. The first cold snapshot stored 336,776,617 bytes with zero resident bytes. After mutations and buffer destruction, it stored 369,103,967 bytes, still with zero resident bytes. Cleanup returned resident, cold-logical, and stored-byte counters to zero, and the full 512 MiB quota was reusable. This section documents the synthetic range path; native-pool adoption is tested separately below. Images and universal active-working-set paging remain unsupported. These checks make no speed or gaming-application claim.

Focused log: [Vulkan pooled-range check](validation/vulkan-pool-focused.txt). The final GPU CTest suite passed **44/44** in **26.98 seconds**, with zero validation errors or matching VUIDs: [full CTest log](validation/vulkan-pool-44-ctest.txt).

### Native Vulkan memory-pool adoption

Automatic snapshot mode can adopt a pristine GPU-only native `VkDeviceMemory` when its first eligible promoted storage buffer binds. The first buffer can use a nonzero offset and cover less than the allocation; the layer adopts the full native allocation as one child, without allocating duplicate synthetic backing. The original memory type, allocation flags, and priority carry across restore; original allocation callbacks stay attached to the borrowed backing until it is released. Native children can exceed the 256 MiB limit used for synthetic children. Later eligible nonoverlapping buffer ranges can share the adopted allocation.

The 512 MiB pool check used A=192 MiB at offset 128 MiB, B=128 MiB at offset 352 MiB, and C=32 MiB at offset 0. Five cases passed full-byte checks: legacy bind, `vkBindBufferMemory2`, `vkBindBufferMemory2` with BDA, two queues, and two queue families with BDA. Initial automatic cold storage retained 336,776,617 bytes with zero resident bytes; after mutation and orphaning, it retained 369,103,967 bytes, still with zero resident bytes. All bytes restored correctly, including after every buffer was destroyed. Freeing the native allocation waited until its last bound buffer was destroyed. Adoption charged no duplicate synthetic heap quota.

The focused probe also verified exclusion and refusal behavior. A native allocation already bound to an ordinary 256-byte transfer buffer, or to one 2-by-2 image, could not be adopted later for a 1 MiB storage buffer at offset 1 MiB. After adoption, ordinary buffer and image binds were rejected while the pool was cold, and pool statistics remained unchanged, so these calls did not wake the allocation. Unknown or mixed pools remain unsupported, as do images, external/protected memory, and universal active-working-set paging. These checks make no speed or gaming-application claim.

Focused log: [native-pool adoption checks](validation/vulkan-native-pool-focused.txt). The native-pool milestone GPU CTest suite passed **49/49** in **36.36 seconds**, with zero validation errors or matching VUIDs: [full CTest log](validation/vulkan-native-pool-49-ctest.txt).

### Selective cold-pool binding

`vkQueueWaitIdle` and `vkDeviceWaitIdle` keep cold snapshots asleep. Binding a cold synthetic or adopted native allocation restores only that allocation; an overlapping range is rejected before any wake. The one-group restore-failure guard does not falsely fail a completed selected restore because a different allocation remains cold. Without the selective-restore opt-in, queue submissions restore all cold allocations.

Four focused regressions used two independent 32 MiB allocations and full-byte patterns. They cover synthetic and adopted-native backing through legacy bind and `vkBindBufferMemory2`, including that restore-failure guard. See [focused selective-bind checks](validation/vulkan-selective-bind-focused.txt). The preceding selective-bind milestone suite passed **53/53** in **37.37 seconds**, with zero validation errors or matching VUIDs: [full CTest log](validation/vulkan-selective-bind-53-ctest.txt).

After native-pool adoption was extended, unchanged SmolLM2-135M F16 inference passed again with **31/31 layers**, identical nonempty native/wrapped stdout, and zero snapshot errors. Before input, **308,084,736 cold logical bytes** were stored in **207,033,581 bytes**; the whole-allocation total includes buffer-pool gaps. Process resident VRAM fell from **319,938,560** to **12,115,968 bytes** (307,822,592 bytes released), while resident GTT stayed at 94,248,960 bytes. The short 44-token decode measured 359.51 versus 357.50 tokens/s; this is regression evidence, not a speedup claim. See the [summary](validation/vulkan-native-pool-model-summary.json), [native stdout](validation/vulkan-native-pool-model-native.stdout.txt), [automatic stdout](validation/vulkan-native-pool-model-automatic.stdout.txt), [native stderr](validation/vulkan-native-pool-model-native.stderr.txt), [automatic stderr](validation/vulkan-native-pool-model-automatic.stderr.txt), [hot fdinfo](validation/vulkan-native-pool-model-hot.fdinfo.txt), and [cold fdinfo](validation/vulkan-native-pool-model-cold.fdinfo.txt).

After selective binding and passive idle waits were added, the unchanged SmolLM2-135M F16 regression passed all nine checks: identical nonempty native/wrapped output, 31/31 layers, lossless wake, and zero snapshot errors. Cold logical backing was 308,084,736 bytes with 207,031,772 stored bytes; process resident VRAM again fell by 307,822,592 bytes. Short decode rates were 316.78 native and 303.41 automatic tokens/s; this is regression evidence, not a throughput benchmark. See the [model summary](validation/vulkan-selective-bind-model-summary.json), [native output](validation/vulkan-selective-bind-model-native.stdout.txt), and [automatic output](validation/vulkan-selective-bind-model-automatic.stdout.txt).

### Selective Vulkan submission restore (opt-in)

`--vulkan-selective-restore` requires `--vulkan-auto-idle-ms`. With it enabled, tracked compute and transfer submissions restore only whole allocations referenced by the command buffer. Descriptor writes/copies, secondary command buffers, and command-buffer or pool resets update tracking. Generated wrappers cover 309 Khronos-registry commands. The default path still restores every cold allocation on submit. Unknown commands, unsupported descriptor layouts, update-after-bind, descriptor buffers, push descriptors, graphics, and `PhysicalStorageBuffer` shaders use restore-all fallback; an unknown future device-proc lookup also forces fallback. Restored allocations stay resident until a later snapshot. This whole-allocation selective path does not provide fault-driven or partial-range paging; the later opt-in range path is documented below.

Descriptor-based skipping is valid only when skipped bindings are not accessed. Descriptors accessed by a dispatch must meet Vulkan's descriptor validity rules, as specified by [`vkCmdDispatch`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDispatch.html).

Eight focused GPU tests passed across synthetic/native memory and legacy/API2 binding. They include unknown-`vkCmdSetEvent` and BDA shader fallback cases. CPU helper test passed. The full CTest suite passed **62/62** in **40.20 seconds**, with zero validation errors or matching VUIDs. See [focused test log](validation/vulkan-selective-submit-focused.txt) and [62-test CTest log](validation/vulkan-selective-submit-62-ctest.txt).

An unchanged SmolLM2-135M F16 Vulkan run passed all **11 checks**, retained **31/31 layer offload**, and produced byte-identical nonempty native/wrapped output. It stored **308,084,736 cold logical bytes** in **207,035,398 bytes**, while process resident VRAM fell from **319,938,560** to **12,115,968 bytes**. Selective event counters `(selected, restored, still cold)` were `(2,1,2)`, `(2,1,1)`, and `(3,1,0)`; fallback count was zero. These checks show tracked whole-allocation restoration for this unchanged app, not gaming compatibility or a speedup. See the [summary](validation/vulkan-selective-submit-model-summary.json), [native stdout](validation/vulkan-selective-submit-model-native.stdout.txt), [wrapped stdout](validation/vulkan-selective-submit-model-automatic.stdout.txt), [automatic stderr](validation/vulkan-selective-submit-model-automatic.stderr.txt), and [cold fdinfo](validation/vulkan-selective-submit-model-cold.fdinfo.txt).

## Automatic Vulkan idle snapshots

The opt-in layer path was tested with a 320 MiB eligible sparse storage buffer, two native backing chunks, and a 32 MiB coherent staging buffer. With a 512 MiB cold-store cap, the first idle snapshot retained 12,263,515 bytes (96.345% smaller than the payload). After a GPU XOR mutation produced randomized contents, the next snapshot correctly used raw 335,544,320-byte backing. Two GPU XOR/wake cycles compared every byte and kept the same `VkBuffer` handle. Cold metadata queries did not wake the allocation, freeing it while cold did not restore it, and the 1 MiB budget refusal retained the original data.

Process fdinfo measured resident DRM VRAM falling from 345,059,328 bytes hot to 9,515,008 bytes cold, exactly the 320 MiB buffer; GTT stayed at 69,210,112 bytes. The logical heap was configured to 96 GiB, but this was a 320 MiB integrity test, not 96 GiB backing.

The updated 37-test suite includes incremental freeze and partial-restore recovery. With an 11 MiB cold budget, the 320 MiB segmented test froze a 256 MiB child (9,809,248 bytes stored) and kept 64 MiB resident, then restored and checked all bytes. A second test injected OOM after restoring the first child; the remaining 64 MiB stayed cold, and a retry restored the full buffer. See [37-test CTest log](validation/vulkan-incremental-ctest.txt), [partial freeze](validation/vulkan-automatic-partial-freeze.txt), and [partial restore retry](validation/vulkan-automatic-partial-restore-retry.txt). These tests cover segmented synthetic allocations; adopted native allocations still freeze atomically, and this is not active fault-driven paging.

The earlier **39-test** GPU CTest suite passed in **23.84 seconds**; the prior [38-test run](validation/vulkan-fast-38-ctest.txt) remains archived. Budget refusal stays suppressed during idle and retries after new queue work; other transient snapshot errors remain retryable, and the 1 MiB test preserved its original data. A second test used a **14 MiB** cold-store budget with two children: it froze a 192 MiB child to 7,360,298 bytes, then freed that cold allocation so the remaining 256 MiB child could freeze to 9,809,248 bytes without another GPU submission. The latter woke and passed full-byte verification. See the [39-test suite](validation/vulkan-queue-fastpath-39-ctest.txt), [stable refusal log](validation/vulkan-automatic-budget-refusal-stable.txt), [budget release log](validation/vulkan-automatic-budget-release.txt), [combined test detail](validation/vulkan-automatic-budget-details.txt), and [verbose budget tests](validation/vulkan-automatic-budget-tests.txt). The exclusive queue-family test passed three additional targeted runs (1.98, 1.98, and 2.01 seconds); an earlier no-error 0.48-second abort remains unexplained. A separate host-thread resource warning occurred during CLI testing and does not establish the cause. See [repeat log](validation/vulkan-automatic-exclusive-repeat.txt).

```sh
# Bounded capacity probe with a 96 GiB logical heap (2 GiB payload)
./zvram --verbose --isolate-layers --vulkan-virtual-gib 96 -- \
  memtest_vulkan 1 2147483648

# Narrow idle snapshot integrity check
./zvram --verbose --isolate-layers --vulkan-virtual-gib 96 \
  --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 -- ./build/zvram-vulkan-auto-check
```

### Unchanged Vulkan model check

An unchanged `llama-completion` Vulkan run with SmolLM2-135M F16 also passed automatic idle snapshots. The app offloaded **31/31 layers** and reported a **256.63 MiB** Vulkan model buffer. Before input, the latest snapshot state showed **307,998,976 logical cold bytes** and **207,039,004 stored bytes** (about 32.8% less). Native and wrapped runs produced identical nonempty stdout (SHA-256 `c05dace16a4bff716e5fa37134cbc15a1033586c4d37385592351b1707af7ef0`). The run exercised native GPU-only allocations, BDA, two queue families, and the tested synchronization path; it demonstrates one real model integration path, not large-model capacity, broad Vulkan compatibility, or a performance gain. The expanded CTest suite passed all 37 tests.

For this model run, process DRM resident VRAM fell from 319,922,176 to 12,099,584 bytes (307,822,592 bytes released); resident GTT stayed at 94,248,960 bytes. This includes model and working buffers.

After the incremental snapshot changes, the model harness passed again with identical native/automatic stdout and full 31/31 layer offload. The fresh report records 307,998,976 cold logical bytes and 207,042,626 stored bytes before input; see [incremental model summary](validation/vulkan-incremental-model-summary.json). The earlier run's values above remain linked to its original evidence.

After the budget-release fix, the model check passed again with identical stdout, 31/31 layers, zero snapshot errors, and 307,998,976 logical cold bytes stored in 207,035,382 bytes; process resident VRAM fell by 307,822,592 bytes. See the [latest post-fix model summary](validation/vulkan-fast-model-summary.json). The reported 536.47/540.26 tokens/s readings are a short run, not a stable performance comparison.

The earlier one-queue restriction is lifted for the tested multi-queue path, which coordinates completion before snapshotting. Segmented virtual allocations can freeze incrementally when each child's snapshot fits the cold budget; adopted native allocations still freeze atomically. Images, external/protected memory, unknown or mixed native pools, capture/replay, and active fault-driven paging remain unsupported.

Reproduce with `python3 check_vulkan_idle_model.py --binary /path/to/llama-completion --model /path/to/SmolLM2-135M-F16.gguf`. The script captures native/automatic output, cold-state evidence, and hot/cold DRM fdinfo. The model summary and full evidence are in [summary JSON](validation/vulkan-automatic-model-summary.json), [native stdout](validation/vulkan-automatic-model-native.stdout.txt), [automatic stdout](validation/vulkan-automatic-model-automatic.stdout.txt), [native stderr](validation/vulkan-automatic-model-native.stderr.txt), [automatic stderr](validation/vulkan-automatic-model-automatic.stderr.txt), [hot fdinfo](validation/vulkan-automatic-model-hot.fdinfo.txt), and [cold fdinfo](validation/vulkan-automatic-model-cold.fdinfo.txt). Additional gates: [expanded CTests](validation/vulkan-expanded-ctest.txt), [two-family validation](validation/vulkan-automatic-native-two-families.txt), and [pending-bind/timeline guard](validation/vulkan-virtual-pending-bind.txt).

Evidence: [idle integrity log](validation/vulkan-automatic-idle-integrity.txt), [budget refusal](validation/vulkan-automatic-budget-refusal.txt), [backing report](validation/vulkan-automatic-backing.json), [hot fdinfo](validation/vulkan-automatic-hot.fdinfo.txt), [cold fdinfo](validation/vulkan-automatic-cold.fdinfo.txt), and [CTest output](validation/vulkan-automatic-ctest.txt).

### Active Vulkan eviction

Opt-in `--vulkan-active-eviction` requires automatic snapshots and `--vulkan-selective-restore`. It allows the worker to compress an eligible whole allocation with no tracked in-flight use while another tracked queue submission is pending. Private binary epoch markers and semaphores establish per-queue completion. A later submit that touches cold memory restores that allocation without a device-wide idle wait; restoration writes become visible to each app queue on its next intercepted submit through deferred waits. If command-buffer use is unknown, every candidate remains resident until the outstanding markers retire. This is whole-allocation eviction; one promoted allocation is still the minimum eviction unit.

The focused gate passed **5/5 cases** across CPU submission-reference checks and synthetic/native GPU pools. In the tracked case, B compressed and was read back/restored while A remained blocked on a host-signalable timeline semaphore; the 2 s watchdog did not fire, and byte patterns matched, including a fresh B readback on the second queue after restore. With an unknown event command, both pools stayed resident for 300 ms until the host released the wait, then both passed byte verification. See the [focused log](validation/vulkan-active-eviction-focused.txt). This is a narrow two-queue correctness result. It does not show fast compression for a 40 GiB allocation, VRChat or broad application compatibility, or per-range residency. Eviction remains constrained by whole allocation sizes and the configured cold-store budget; private queue availability and synchronization path affect progress. Graphics/presentation and explicit app sparse submissions are outside this active mode.

An unchanged SmolLM2-135M F16 active-mode run passed all **12 checks**, kept **31/31 layers** offloaded, and produced byte-identical native and wrapped stdout (SHA-256 `40af802dfb2b6042c7e4ec011b0d82ed8057f634f9c724e68f23ca1917fef76b`). It recorded **308,084,736 cold logical bytes** and **207,022,698 stored bytes**; process resident VRAM fell from **319,938,560** to **12,115,968 bytes**, while GTT stayed at **94,248,960 bytes**. Three selective events reported `(selected, restored, still cold)` values `(2,1,2)`, `(2,1,1)`, and `(3,1,0)`, with zero fallbacks. The final short 44-run decode measured 319.47 tokens/s native and 305.93 tokens/s active mode; prompt processing took 7.44 ms versus 157.06 ms after cold restore. This is output and cold-restore correctness for one small model run, not evidence of a speedup or sustained active-eviction benefit. See the [summary](validation/vulkan-active-eviction-model-summary.json), [native stdout](validation/vulkan-active-eviction-model-native.stdout.txt), [automatic stdout](validation/vulkan-active-eviction-model-automatic.stdout.txt), [hot fdinfo](validation/vulkan-active-eviction-model-hot.fdinfo.txt), and [cold fdinfo](validation/vulkan-active-eviction-model-cold.fdinfo.txt).

The preceding full CTest run passed **67/67 tests in 41.54 seconds**, with zero validation errors or matching VUIDs. The earlier 62/62 queue-fast-path result remains historical; see the [latest full-suite log](validation/vulkan-active-eviction-67-ctest.txt).

## Vulkan range residency (opt-in)

`--vulkan-range-mib 32` requires automatic snapshots, selective restore, active eviction, `sparseResidencyBuffer`, and an available private queue. Eligible sparse buffers use [partial binding](https://docs.vulkan.org/spec/latest/chapters/sparsemem.html); narrow descriptor and transfer ranges select chunks for restore and eviction. A pristine native GPU-only allocation is split into aligned chunks of the configured size, with a smaller final chunk on its first eligible bind. Without this option, native allocations retain whole-allocation snapshot and eviction behavior. Global `robustBufferAccess`, explicit pipeline robustness, and dynamic storage descriptors conservatively widen tracking to whole buffers; unknown commands and BDA shaders fall back to restore-all. Graphics and presentation remain unsupported.

The focused synthetic/native check used a 64 MiB buffer split into two 32 MiB chunks. Each chunk restored independently across two cold cycles, with every byte verified. The focused gate passed **4/4** in **0.82 seconds**; the full CTest suite passed **69/69** in **42.79 seconds**, with zero validation errors or matching VUIDs. See the [focused log](validation/vulkan-range-residency-focused.txt) and [full CTest log](validation/vulkan-range-residency-69-ctest.txt).

An unchanged SmolLM2-135M F16 run passed **13/13 checks**, retained **31/31 layers**, and produced identical native and wrapped stdout (SHA-256 `40af802dfb2b6042c7e4ec011b0d82ed8057f634f9c724e68f23ca1917fef76b`). It stored **308,084,736 cold bytes** in **207,046,305 bytes**; process resident VRAM fell from **328,347,648** to **20,525,056 bytes**, while GTT remained **94,248,960 bytes**. Range events `(selected chunks, restored chunks, cold pools left)` were `(1,1,2)`, `(10,9,1)`, and `(11,1,0)`, with zero fallbacks. One submission selected ten chunks together, so this does not demonstrate streaming a 40 GiB working set or a speedup. See the [result summary](validation/vulkan-range-residency-model-result.json), [native output](validation/vulkan-range-residency-model-native.stdout.txt), and [automatic output](validation/vulkan-range-residency-model-automatic.stdout.txt).

## Vulkan resident-pressure admission

`--vulkan-resident-mib N` requires `--vulkan-range-mib` and bounds admission of tracked sparse-buffer chunks. Before restoring an eligible tracked range, completed tracked chunks can be snapshotted and evicted to make room. This is not a global VRAM cap: accounting covers tracked buffer backing only, not images, graphics, or other untracked allocations. With `--vulkan-resident-after-cold`, admission is armed only after the first complete cold pass; startup and initial model binding can exceed the configured limit. A known submission whose complete tracked working set exceeds the limit is refused and may OOM. Unknown/full submissions can likewise exceed the limit or OOM, so this is not a general application memory guarantee.

The focused pressure gate passed **6/6** in **3.42 seconds**, covering synthetic and native backing, single and two-queue work, and robustness2 alignment cases. The latest full CTest suite passed **75/75** in **46.19 seconds**. The logs are [focused pressure checks](validation/vulkan-pressure-focused.txt) and [full CTest run](validation/vulkan-pressure-75-ctest.txt).

The unchanged SmolLM2-135M-Instruct F16 check passed **21/21 checks**, retained **31/31 layer offload**, and produced identical nonempty native and wrapped output. The model used 293.8125 MiB of eligible backing. After a full cold pass, the 192 MiB limit held tracked resident backing to **165.8125 MiB** peak and triggered **354 evictions**, with no admission refusals. Both runs used `GGML_VK_MAX_NODES_PER_SUBMIT=1` and Vulkan core/synchronization validation. Native decode measured **102.46 tokens/s** versus **2.82 tokens/s** wrapped over 44 decode runs; this is one short workload with substantial overhead, not a speed claim or evidence of fast large-pool compression. Both native and wrapped streams had zero Vulkan validation diagnostics. The tested model binary was unchanged. See [result summary](validation/vulkan-pressure-model-result.json), [native stderr](validation/vulkan-pressure-model-native.stderr.txt), [wrapped stderr](validation/vulkan-pressure-model-automatic.stderr.txt), [native output](validation/vulkan-pressure-model-native.stdout.txt), [wrapped output](validation/vulkan-pressure-model-automatic.stdout.txt), and hot/cold [native](validation/vulkan-pressure-model-native-hot.fdinfo.txt) and [wrapped hot](validation/vulkan-pressure-model-automatic-hot.fdinfo.txt)/[cold](validation/vulkan-pressure-model-automatic-cold.fdinfo.txt) fdinfo captures.

To reproduce the model run from the repository root:

```sh
python3 check_vulkan_idle_model.py --binary build/third-party/llama-vulkan-build/bin/llama-completion --model build/third-party/models/SmolLM2-135M-Instruct-f16.gguf --tokens 64 --idle-ms 100 --range-mib 32 --resident-mib 192 --resident-after-cold --strict-robustness --max-nodes-per-submit 1 --validate --output-dir build/vulkan-pressure-model-normal --timeout 120
```

`--strict-robustness` opts into supported `VK_EXT_robustness2` and includes descriptor alignment in range tracking. Explicit weaker robustness1 remains whole-buffer tracked; dynamic storage descriptors also use conservative whole-buffer tracking. Without this option, core `robustBufferAccess` keeps whole-buffer tracking. This result tests tracked buffer chunks in one small model run. It does not prove a global VRAM cap, image/graphics eviction, a 40 GiB model, or fast 40 GiB compression. The optional `--serialize-submissions` mode is not part of this clean result: a separate native run with that llama.cpp option emitted `VUID-vkResetCommandPool-commandPool-00040` on stdout before zVram was involved ([native stdout](validation/vulkan-pressure-serialized-native-native.stdout.txt)). Normal submissions passed validation in both streams.

## Managed Vulkan pool

### Mixed-data lifecycle check

The small compute check creates eight 8 MiB buffers (64 MiB total) with a 16 MiB resident budget and 128 MiB host-snapshot budget. It performs GPU mutations and exact readback checks across **two full eviction/restore passes**. It also checks pin refusal, raw fallback, recovery after dirty data exceeds the host budget, and a 3-byte upload/restore case.

Latest run, [`validation/managed-compute.txt`](validation/managed-compute.txt):

| Measurement | Result |
|---|---:|
| Vulkan layer peak local allocation | 16,777,216 B (16 MiB) |
| Vulkan layer peak nonlocal allocation | 1,056,768 B |
| Host snapshots stored | 58,721,120 B |
| Evictions / restores / raw fallbacks | 38 / 30 / 33 |
| Integrity result | Both 64 MiB cycles passed; dirty-data recovery passed |

```sh
./zvram --validate --isolate-layers -- ./build/zvram-managed-check
```

### 40 GiB synthetic-data check

The capacity mode creates **1,280 × 32 MiB buffers (40 GiB total)**. Each buffer starts with a constant value and the GPU mutates it to another constant value; the check verifies every 32-bit word after eviction and restore. It uses a 256 MiB resident budget and an 8 MiB staging chunk.

| Measurement | Result |
|---|---:|
| Vulkan layer peak local allocation | 268,435,456 B (256 MiB) |
| Vulkan layer peak nonlocal allocation | 8,388,608 B (8 MiB) |
| Host-store budget | 67,108,864 B (64 MiB) |
| Host snapshots stored | 3,998,720 B |
| Evictions / restores | 2,553 / 1,273 |
| Elapsed time | 31.264 s, one check only |
| Integrity result | 40 GiB GPU-mutated data verified; exit 0 |

```sh
./zvram --validate --isolate-layers -- \
  ./build/zvram-managed-check --capacity-mib 40960
```

Raw output: [`validation/managed-40gib.txt`](validation/managed-40gib.txt). The per-buffer constant pattern is highly compressible and not representative of model weights. The stored-byte result is a synthetic example, not an expected model compression ratio or a benchmark. This demonstrates the explicit buffer pool lifecycle on this device; it is not transparent application paging.

Both pool checks stream snapshot encoding/restoration by staging chunk. Temporary zstd scratch is bounded by chunk size and the compression bound; caller-owned readback vectors and metadata are outside the host-store budget.

## HIP allocation fallback

One opt-in run kept **40 GiB** of allocations live in 8 MiB chunks. The GPU wrote a deterministic pattern and the CPU verified all **5,368,709,120 64-bit words**. The HIP layer reported a 20 GiB peak for native `hipMalloc` device allocations and a 20 GiB peak for mapped host fallback; all allocations were freed. Elapsed time was 19.163 s for this single check, not a benchmark. [`validation/hip-40gib.txt`](validation/hip-40gib.txt)

```sh
./zvram --hip --hip-local-mib 20480 --hip-host-mib 20480 -- \
  ./build/hip_check --mib 40960
```

Mapped fallback is pinned host RAM, not compressed VRAM. The test establishes this allocation and pointer path; there is no native HIP baseline here, so it does not establish a capacity gain over native HIP behavior. The current shim focuses on `hipMalloc`/`hipFree`; `hipFreeAsync` for mapped-host fallback is rejected. It does not establish compatibility with an unmodified application or model.

A separate allocation-only concurrency check passed with four threads, eight rounds, and nine live 32 KiB allocations per worker. It observed 256 native and 32 mapped-host allocations, with no GPU kernels launched. This checks the wrapper's bookkeeping under concurrent allocation/free calls, not GPU concurrency or workload safety. [`validation/hip-concurrency.txt`](validation/hip-concurrency.txt)

## HIP VMM research probe

The current `--hip-vmm` provider obtains AMDGPU GTT BOs with libdrm, exports them as DMA-BUFs, imports them into HIP VMM, and maps the segments into one virtual address range. A 64 MiB physical-placement probe passed a GPU copy and CPU verification of every byte; measured GTT rose by 64 MiB while VRAM remained unchanged at BO creation.

The integrated report-enabled single-pointer check kept **40 GiB** mapped at once: **20 GiB of local VRAM plus 20 GiB of driver GTT/RAM backing across 160 physical handles**. `hipMemGetInfo` reported total/free capacity as 40/40 GiB before allocation, 40/0 GiB while live, and 40/40 GiB after free. The GPU wrote the allocation and the CPU verified all **5,368,709,120 64-bit words**. Pointer attributes reported memory type `2` (device memory), device ordinal `0`, and unmanaged memory. All allocations were freed; the run exited 0 with no failures or orphaned VMM cleanup. Elapsed time was 8.555 s for this single check, not a benchmark. Raw output: [`validation/hip-vmm-40gib.txt`](validation/hip-vmm-40gib.txt).

Driver-wide snapshots include other applications. VRAM/GTT usage measured 1,263,779,840/3,536,683,008 B before, 23,005,634,560/25,017,876,480 B while live, and 16,471,797,760/3,547,287,552 B immediately after free. Thus the driver's GTT use returned near its starting snapshot, while VRAM use remained elevated at the immediate post-run sample; this run does not show that all VRAM is returned instantly. `MemAvailable` was 40,764,276 KiB before, 20,025,816 KiB while live, and 40,860,676 KiB after.

### Opt-in HIP capacity reporting

By default, `hipMemGetInfo`, `hipDeviceTotalMem`, and the installed `hipGetDeviceProperties` ABI continue to return native physical capacity. `--hip-report-capacity` is an explicit mode that reports the configured local limit plus the bounded GTT-backed tier through only those three HIP entry points. It requires `--hip-vmm`, an explicit local cap, and a positive host/GTT cap. It does not alter physical capacity, older property-query ABIs, or queries made outside these intercepted HIP APIs.

The small query check passed with an 80 MiB configured logical total: free capacity reported 80 MiB before a 32 MiB allocation, 48 MiB during it, and 80 MiB after free. `hipMemGetInfo`, `hipDeviceTotalMem`, and the installed `hipGetDeviceProperties` ABI agreed. Raw output: [`validation/hip-capacity-report.txt`](validation/hip-capacity-report.txt). The 40 GiB run above separately verified `hipMemGetInfo` transitions with the reporting option enabled. These checks validate reporting consistency and allocation integrity for the tested path, not universal availability beyond the configured backing limits.

The all-GTT descriptor test kept **256 × 4 KiB allocations** live, filled each with `hipMemset`, and verified every byte on readback. Open descriptor count moved from **8 to 9 to 8** before, during, and after the allocations, showing the shared GTT provider reused a bounded descriptor set and cleaned up on free. The HIP summary reported zero tracked, pending, local, host, orphaned, and failed allocations. Raw output: [`validation/hip-vmm-descriptors.txt`](validation/hip-vmm-descriptors.txt).

```sh
./zvram --hip --hip-vmm --hip-report-capacity --hip-local-mib 16 --hip-host-mib 64 -- \
  ./build/zvram-hip-capacity-check

./zvram --hip --hip-vmm --hip-report-capacity \
  --hip-local-mib 20480 --hip-host-mib 20480 -- \
  ./build/hip_check --single-allocation --mib 40960
```

An earlier HIP host-location VMM provider is a separate, superseded path: its 40 GiB attempt failed during host-segment creation, and a host-only probe showed each 256 MiB allocation consuming matching VRAM with GTT unchanged at creation. Its historical failure log is [`validation/hip-vmm-40gib-failed.txt`](validation/hip-vmm-40gib-failed.txt); those findings do not describe the current GTT BO provider. For comparison, the non-VMM `hipMalloc` mapped-host fallback also passed a separate 40 GiB test using pinned host memory. These are synthetic pointer/data-integrity checks. A native HIP baseline requested one 40 GiB allocation on the same discrete GPU and returned `hipErrorOutOfMemory` (2), exit 1, in 0.033 s; it reported 24 GiB total capacity. The VMM/GTT single-allocation pass therefore demonstrates extra usable allocation capacity on this stack. This baseline does not test 40 GiB across multiple native HIP allocations. [Native single-allocation log](validation/hip-native-single-40gib.txt). These checks do not establish model workloads, compression, or transparent paging. A separate unmodified-application check is documented below. The VMM/GTT build needs ROCm/HIP and libdrm AMDGPU development files.

## Linux TTM paging probe

`dmem_probe.py --inspect` reads cgroup, RAM, and swap state. The privileged `sudo python3 dmem_probe.py --run` path is prepared but **has not been run**. It uses a temporary child cgroup with a 16 MiB default AMDGPU VRAM cap, 512 MiB RAM cap, 256 MiB swap cap, and a 30-second limit around only the 64 MiB capacity check. It makes no global swap or TTM changes and cleans up its cgroup. Even a successful run would not, by itself, prove that TTM shmem pages reached swap or zram, or that GPU data was compressed. See [KERNEL_PAGING.md](KERNEL_PAGING.md).

## Earlier compression check and routine commands

The standalone compression demo verified a 16 MiB repetitive input round trip (58,748 bytes stored) and seeded-random input raw fallback (16,777,216 bytes stored), with exact final GPU readback. It retains correctness copies and is not a process-memory savings measurement. [Raw output](validation/compression.txt).

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Tests require compatible GPU hardware and the relevant runtime. The near-40GB F16 model below passed through VMM/GTT with a 36,798.77 MiB GPU model buffer; a 40 GiB GPU model buffer, broad application compatibility, performance gains, larger-model compression ratios, and transparent paging remain unverified.

## Unmodified ROCm application

The unchanged official ROCm [primbench HIP copy example](https://github.com/ROCm/rocm-libraries/blob/959b2d4d0459abfd1f67f3fb9cce20cd88a7785a/shared/primbench/examples/hip/copy_benchmark.cpp) was built at commit `959b2d4d0459abfd1f67f3fb9cce20cd88a7785a`, targeting gfx1100, with monitoring disabled and upstream assertions enabled. `check_primbench.py` pins both source hashes and reproduces the build and runs.

Both the native process and `./zvram --hip --hip-vmm --hip-report-capacity --hip-local-mib 32 --hip-host-mib 512 -- copy_benchmark --size 32MiB --min-secs 0.1 --noise-timeout-secs 1` exited 0 for `char` and `long long`. The example allocates two 32 MiB data buffers plus its library's internal 256 MiB cache buffer. Under zVram these exceeded the 32 MiB local cap: peak tracked backing was 32 MiB VRAM and 288 MiB GTT, with six VMM allocations over the run. Tracked/local/host/pending/orphan/failure counters returned to zero.

The upstream copy assertion verifies only the first three values (`0, 1, 2`), so this does not replace the full-word integrity checks above. Both short runs reached primbench's statistical noise timeout, which is distinct from the subprocess timeout and assertion failure; no stable performance comparison is claimed. An initial 256 MiB host cap correctly refused another allocation because the internal cache had already consumed most of it; the successful run used 512 MiB. Logs: [native](validation/primbench-native.txt), [VMM/GTT](validation/primbench-vmm.txt).

## Small llama.cpp model check

An unmodified `llama-completion` from llama.cpp commit `c479922ac520a08969b4c1dc154d7bbb3c386d85` ran the F16 GGUF `SmolLM2-135M-Instruct` (270,885,952 bytes; SHA-256 `f535f83ec568d040f88ddc04a199fa6da90923bbb41d4dcaed02caa924d6ef57`) on the RX 7900 XTX. The model was not additionally quantized. Both native and VMM/GTT runs exited 0, offloaded 31/31 model layers, and produced byte-identical stdout (SHA-256 `087087260916ca2af13b0c97b12bd4cd9945c05fa3149c1a484667d130873cc8`). The run used `-ngl 999`, context 512, batch 128, 32 generated tokens, greedy sampling (`temp 0`, seed 1); it is an application compatibility check, not a benchmark.

The VMM/GTT run used a 64 MiB local cap and 2 GiB GTT cap. zVram tracked a 64 MiB peak local allocation, 240,599,040 bytes peak host/GTT backing, and three VMM allocations; tracked allocations, pending frees, and orphaned cleanup returned to zero, with no failures. Both actual model loads logged a 256.63 MiB ROCm0 model buffer and a 54.00 MiB CPU_Mapped buffer. The earlier 0.00 MiB lines belong to llama.cpp's preliminary no-allocation fit pass, not the actual model load. zVram's model allocation mapped 269,103,104 bytes: 67,108,864 local and 201,994,240 GTT. This establishes a real model-weight allocation through the wrapper, but the small native run also fit; it does not establish a model capacity or performance gain. See the near-40GB model test below; a 40 GiB GPU model buffer and broad model/application behavior remain untested.

The exact model source, hash, build configuration, and command metadata are in [`validation/llama-small-model.json`](validation/llama-small-model.json); captured logs are [native](validation/llama-small-native.txt) and [VMM/GTT](validation/llama-small-vmm.txt), with [native stdout](validation/llama-small-native.stdout.txt) and [VMM/GTT stdout](validation/llama-small-vmm.stdout.txt).

## InternLM2.5-20B F16 model capacity

The official **39,725,643,136-byte (39.73 GB)** InternLM2.5-20B F16 GGUF passed a VMM/GTT inference run on the RX 7900 XTX. Its verified SHA-256 is `9e4f99bab5aed46cf2a960530d8fef060b635bb92b81bb8b21e3c89b3e6d9623`. The run offloaded **49/49 layers**, reported a **36,798.77 MiB** ROCm0 model buffer and **1,084.50 MiB** ROCm_Host buffer, and generated 32 tokens. The VMM allocation mapped **38,586,310,656 bytes**, split into **19,327,352,832 bytes local VRAM** and **19,258,957,824 bytes host/GTT**. This is a real near-40GB model test, but the GPU model buffer is below 40 GiB.

The full-GPU native run OOMed on a **38,586,310,656-byte** allocation request. The paired helper's overall result remains false because native full-GPU loading failed; its separate VMM capacity checks passed, all 49 layers loaded, and allocation/pending/orphan/failure counters returned to zero. A separate native CPU/GPU-offload run placed **24/49 layers** on the GPU and generated the same 32-token output byte-for-byte as VMM under the same prompt and settings. Native partial-offload decode measured **1.38 tokens/s** over 31 runs; VMM measured **1.09 tokens/s** over 31 runs after a 70.91-second load. This is one partial-native comparison, not a full-GPU baseline or a general performance claim. No automatic compression flags were used. Process DRM-client samples polled every 0.5 seconds recorded peak resident VRAM of **19,534,401,536 bytes** and peak resident GTT of **19,473,006,592 bytes**; these peaks can occur at different samples.

Reproduce the single paired attempt with the existing model and binary:

```sh
python3 check_model.py --binary build/third-party/llama-build/bin/llama-completion \
  --model build/third-party/models/internlm2_5-20b-chat-fp16.gguf \
  --local-mib 18432 --host-mib 24576 --min-model-mib 24576 \
  --tokens 32 --timeout 120 --output-dir build/internlm2-5-20b-check
```

The paired report deliberately records native full-GPU OOM and VMM capacity separately: [summary](validation/hip-internlm2-5-20b-capacity-summary.json), [native stdout](validation/hip-internlm2-5-20b-native.stdout.txt), [native stderr](validation/hip-internlm2-5-20b-native.stderr.txt), [VMM stdout](validation/hip-internlm2-5-20b-vmm.stdout.txt), [VMM stderr](validation/hip-internlm2-5-20b-vmm.stderr.txt), and [sampled DRM backing](validation/hip-internlm2-5-20b-backing-samples.json). The separate partial-native comparison is available as [native partial stdout](validation/hip-internlm2-5-20b-native-partial.stdout.txt) and [stderr](validation/hip-internlm2-5-20b-native-partial.stderr.txt). This establishes one model capacity path without compression and one partial-native output/performance comparison; a 40 GiB GPU model buffer, full-GPU native inference comparison, and broad model compatibility remain unverified.

The same model also completed through the zVram Vulkan virtual heap: all 49 layers were offloaded, output matched native Vulkan byte-for-byte, and decode measured **1.63 tokens/s** over 31 runs versus **1.41 tokens/s** native. These were short sequential runs, not a controlled or stable speed comparison. An earlier wrapped attempt was stopped with SIGTERM during load after VRChat was started; it produced no inference result and is not a failure. Native Vulkan success alongside native HIP full-GPU OOM shows API/backend-specific capacity behavior; this does not establish zVram expansion for every GPU API. Vulkan output is compared only against Vulkan, not HIP. See the [combined Vulkan run summary](validation/internlm2-5-20b-vulkan-speed-summary.json), [resumed run summary](validation/internlm2-5-20b-vulkan-resumed-summary.json), [native stdout](validation/internlm2-5-20b-vulkan-native.stdout.txt), [native stderr](validation/internlm2-5-20b-vulkan-native.stderr.txt), [resumed zVram stdout](validation/internlm2-5-20b-vulkan-zvram-resumed.stdout.txt), [resumed zVram stderr](validation/internlm2-5-20b-vulkan-zvram-resumed.stderr.txt), and [earlier stopped wrapper stderr](validation/internlm2-5-20b-vulkan-zvram.stderr.txt).

### llama.cpp n-gram self-drafting

An unchanged llama.cpp `llama-cli` used `--spec-type ngram-simple` with the same 39.73 GB F16 target, 19 GiB local cap, and 24 GiB GTT cap. Both runs offloaded 49/49 layers into a 36,798.77 MiB GPU model buffer and finished with zero allocator failures or outstanding allocations. On an intentionally repetitive prompt asking for one sentence twelve times, ordinary decoding measured **1.10 tokens/s** and self-drafting **6.33 tokens/s**; 103 of 121 draft tokens were accepted, and the transcripts matched byte-for-byte. On a 392-token code-explanation prompt, rates were **1.43** and **1.45 tokens/s**, only 4 of 25 drafts were accepted, and the transcripts differed despite greedy sampling. The reason for that output divergence is unknown. These short sequential runs show that llama.cpp self-drafting can help repeated text; they do not demonstrate a generic zVram or gaming speedup, stable throughput, or exact output for arbitrary prompts. Model weights and the lossless allocator were unchanged.

The optional self-drafting flags are existing llama.cpp CLI options; no model loader or zVram source changes are needed:

```sh
./zvram --hip --hip-vmm --hip-report-capacity \
  --hip-local-mib 19456 --hip-host-mib 24576 -- \
  ./build/third-party/llama-build/bin/llama-cli \
  --model build/third-party/models/internlm2_5-20b-chat-fp16.gguf \
  --gpu-layers 999 --ctx-size 512 --batch-size 128 --predict 128 \
  --temp 0 --seed 1 --load-mode none --fit off --flash-attn off \
  --verbose --simple-io --no-display-prompt --single-turn \
  --prompt "Output this sentence exactly twelve times, with no introduction or explanation: The copper fox jumps over the sleepy dog." \
  --spec-type ngram-simple --spec-ngram-simple-size-n 3 \
  --spec-ngram-simple-size-m 16 --spec-ngram-simple-min-hits 1
```

The exact model hash, commands, rates, draft acceptance, and captured outputs are in the [repeated-text summary](validation/internlm2-5-20b-drafting-summary.json) and [ordinary-prose summary](validation/internlm2-5-20b-prose-drafting-summary.json); each summary links its stdout, stderr, and transcript artifacts.

### Synthetic HIP local/GTT read diagnostic

A GPU reduction read a checksummed 256 MiB buffer 32 times after warmup. The short local/GTT/local sequence measured **819.39**, **24.36**, and **490.22 GB/s**, with matching checksums and zero GTT cleanup failures. This indicates substantially slower reads from GTT for this access pattern; clocks, caches, and reduction overhead were uncontrolled. It is not model or gaming throughput and cannot predict token rates or establish a bandwidth limit. See the [summary](validation/hip-spill-read-bandwidth-summary.json) and [diagnostic source](validation/hip-spill-read-bandwidth.cpp).

## Stable sparse Vulkan buffers

The optional `ManagedBufferPool::Config::stableSparseBuffers` path retained three 1 MiB logical buffers under a 2 MiB resident budget. The GPU mutated every word across two cycles; full readback verified the values after compression and restoration while `VkBuffer` handles stayed identical. The run recorded 15 evictions and 12 restores. It also verified that pinned buffers cannot be evicted/erased, an incompressible GPU-mutated buffer remains intact when a 128-byte host-store cap refuses eviction, and a 3-byte logical buffer survives aligned sparse binding with a stable handle. Final pool accounting was zero.

The feature-enabled sparse compute run exited 0 without core or synchronization validation diagnostics; the layer recorded a 2 MiB peak local allocation and 512 KiB peak nonlocal allocation across sequential pool instances. [Raw sparse validation](validation/sparse-compute.txt). This is a cooperative API with synchronized acquire/release boundaries, not an unmodified Vulkan application test or fault-driven paging. The default pool path and HIP backend remain separate.

## Real F16 model-file compression

The same 270,885,952-byte SmolLM2 F16 GGUF documented above was streamed through the pool in 8 MiB logical buffers, with a 16 MiB resident budget, 512 MiB host-store cap, and 1 MiB staging buffer. All chunks were evicted, restored, read back from the GPU, and compared byte for byte against the source file, then erased. Both default and stable sparse modes passed without Vulkan core or synchronization validation diagnostics. Sparse handles stayed identical.

Both modes retained **207,311,475 bytes** in cold snapshots: **23.47% saved**, or about **1.307:1**, for this one file. Each recorded 66 evictions, 33 restores, zero raw fallbacks, and zero resident/host bytes after cleanup. The layer measured peak local allocation at 16,777,216 bytes and peak nonlocal staging at 1,048,576 bytes. Codec scratch, metadata, and caller readback vectors are outside these storage budgets. This tests real file bytes, including model metadata; it does not execute inference from compressed weights, predict other model ratios, or demonstrate automatic application compression.

```sh
./zvram --validate --isolate-layers -- \
  ./build/zvram-file-pool-check --file /path/to/SmolLM2-135M-Instruct-f16.gguf
./zvram --validate --isolate-layers -- \
  ./build/zvram-file-pool-check --file /path/to/SmolLM2-135M-Instruct-f16.gguf --sparse
```

The checker accepts nonempty files up to 512 MiB. Raw logs: [default](validation/model-file-default.txt), [sparse](validation/model-file-sparse.txt). Single-run elapsed times (0.876 s / 0.795 s) are integrity-check observations, not a performance benchmark.

## HIP procedure lookup

The `hipGetProcAddress` path now returns zVram wrappers for the currently supported allocation/free and capacity-query entry points, only when the native resolver returns the matching installed ABI. Default flags and nonnegative versions are required; unknown functions, legacy property ABIs, invalid inputs, and other lookup modes retain native results. Direct lookups against an explicitly opened native HIP library can still bypass preloading.

Both procedure checks passed. The physical-query run reported 24 GiB and used a native 32 MiB allocation. The VMM/query run verified every byte of a 32 MiB allocation with 16 MiB local + 16 MiB GTT backing; resolver-obtained capacity calls agreed on 80 MiB total and free capacity moved 80 → 48 → 80 MiB. Both cleanup summaries were zero. Unwrapped copy/memset procedures and unsupported queries matched the native resolver. [Physical-query log](validation/hip-proc-native.txt), [VMM/query log](validation/hip-proc-vmm.txt). All 16 current CTests passed on the documented hardware; this adds one dynamic lookup route, not every possible HIP allocator or library-binding route.

## Reproducible model comparison

`check_model.py` repeated the same small F16 model with fit disabled and `--load-mode none`, using the existing llama.cpp build and model provenance above. Both native and VMM runs exited 0, offloaded 31/31 layers, and logged an actual 256.63 MiB ROCm model buffer. Their greedy 32-token output matched byte for byte (SHA-256 `d798f86733d30a691d3907c380ff89fcb95f5022a3e927c921e82e2b0345f0f1`). The VMM run recorded eight hybrid allocations, with cleanup/failure counters zero. A separate one-token `--vmm-only` check also passed; that mode explicitly omits native comparison. Neither small check proves large-model capacity.

[Reproducer summary](validation/llama-repro-summary.json), [native log](validation/llama-repro-native.stderr.txt), [VMM log](validation/llama-repro-vmm.stderr.txt), [native stdout](validation/llama-repro-native.stdout.txt), [VMM stdout](validation/llama-repro-vmm.stdout.txt). The report keeps the measured actual model-buffer threshold separate from the configured logical capacity.

## Existing Odysseus models

The live local Ollama server stores two larger, unchanged Q4_K_M GGUF weight files. Their local aliases reuse the same blobs as their upstream model names; they are not four independent models. Both complete file SHA-256 values were checked against the local manifests. The 27B model's Ollama package size also includes a vision projector; the inference checks below use its text weight blob.

| Model | Weight file bytes | Verified SHA-256 | Actual ROCm model buffer |
|---|---:|---|---:|
| `huihui-qwen3-coder:30b-local` | 18,556,688,736 | `9fddd9b57b678ca9f9f7b07b02c6f7107bc0f8b307e2383a68cf7a513e6ae0f5` | 17,524.43 MiB |
| `huihui-qwen3.8:27b-local` | 16,810,714,400 | `6c2c13cef89238c3604d756b07b3ef5fafebbd61095feb8553ff449c95e4c1c6` | 15,088.32 MiB |

Each first passed a separate `check_model.py --vmm-only` run with a 4,096 MiB local cap, 20,000 MiB GTT cap, context 512, batch 128, greedy sampling, and the same unmodified llama.cpp build documented above. Full layer offload was 49/49 and 66/66. The 30B run generated one token and the 27B run two; these are short compatibility checks. The 30B weight allocation used 4 GiB local + 14,080,733,184 bytes GTT; the 27B allocation used 4 GiB local + 11,526,283,264 bytes GTT. Both exited 0, with cleanup and failure counters zero. [30B summary](validation/odysseus-coder-summary.json), [27B summary](validation/odysseus-27b-summary.json).

A separate native 30B run using the same model and application arguments returned exit 1: native `hipMalloc` could not allocate its 18,375,698,432-byte weight buffer with the desktop's current memory use. This is a capacity improvement under that observed headroom, not evidence that an otherwise idle 24 GiB card cannot fit this model. [Native log](validation/odysseus-coder-native.stderr.txt).

### Concurrent inference

The two VMM processes then ran concurrently, with the same per-process 4 GiB local / 20,000 MiB GTT caps, generating 64 and 32 tokens. Both exited 0, fully offloaded their layers, and reported zero live allocations, pending operations, orphaned cleanup, or failures on exit. A 100 ms sampler observed **116 samples with both processes alive and both outputs nonempty**, approximately 11.6 seconds of overlap. Their actual GPU model buffers sum to **32,612.75 MiB (31.85 GiB, about 34.2 GB)**, exceeding the physical 24 GiB card. File sizes are not used as the GPU-memory measurement.

The two processes each peaked at 4 GiB local backing. Host/GTT peaks were 14,257,631,232 and 11,891,027,968 bytes including context/compute allocations. Driver-wide VRAM/GTT peaks were 19,551,813,632 / 28,863,438,848 bytes; these include other applications and need not occur in the same sample. `MemAvailable` stayed at or above 14,349,968 KiB. GTT accounting was 2,689,425,408 bytes before and 2,685,231,104 after. The per-process caps are independent, not a global VRAM reservation policy.

[Concurrent report and samples](validation/odysseus-pair-summary.json), [30B log](validation/odysseus-pair-coder30b.stderr.log), [27B log](validation/odysseus-pair-dense27b.stderr.log), [hardware-specific runner](validation/odysseus-pair-runner.py), [file provenance](validation/odysseus-models.json). The runner reproduces this host's paths and caps; adjust them for another machine. These checks establish concurrent unchanged-model inference through GPU-accessible GTT backing. They do not demonstrate compressed inference, a 40 GiB GPU model buffer, performance improvement, or arbitrary application compatibility; the separate InternLM test above documents the near-40GB model run.

## HIP error-state compatibility

A successful mapped-host fallback previously left its internal native OOM visible through `hipGetLastError`. The shim now preserves prior caller errors, hides errors from successful internal fallback steps, and exposes rejected wrapped calls through thread-local error queries. The installed error-query ABIs also participate in the supported `hipGetProcAddress` lookup path.

The mapped-host and VMM checks each injected a real native 64 GiB allocation failure inside a successful allocation path, then verified clean peek/get/ext-get results and a 64 KiB byte-for-byte copy. They also verified that a prior native error survives successful allocation/capacity queries, quota failures remain visible until cleared, rejected async free retains the allocation, newer native errors take precedence, and errors remain isolated between two threads. Three intentional quota rejections appear in each failure counter; final tracked/pending/orphaned allocation counters were zero. [Mapped-host log](validation/hip-fallback-error-state.txt), [VMM log](validation/hip-vmm-error-state.txt). All 18 CTests passed after this change on the documented GPU.

## Userspace HIP hibernation

The explicit API in [`hip_hibernate.hpp`](hip_hibernate.hpp) was tested on October 7, 2026, without root or a service. It snapshots zVram-owned VMM allocations to lossless Zstd/raw CPU storage, releases physical handles/BOs, and reserves their GPU virtual addresses. The caller must serialize all application HIP/HSA work across hibernate/resume and must not use cold pointers. Native and mapped-host allocations do not participate. Logical cold bytes count whole allocations requiring resume; stored bytes count retained payload, excluding metadata and bounded staging/codec scratch.

| Check | Allocation | Layout | Stored cold payload | Result |
| --- | ---: | --- | ---: | --- |
| Integrity and resident-cap reuse | 33,554,432 B | 16 MiB local + 16 MiB GTT | 16,779,392 B | PASS |
| Multiple GTT segments | 301,989,888 B | 16 MiB local + 256 MiB GTT + 16 MiB GTT | 151,014,528 B | PASS |
| Injected remap failure and retry | 33,554,432 B | 16 MiB local + 16 MiB GTT | 16,779,392 B retained on refusal | PASS |

Each allocation contains half repeated 64-bit values and half seeded pseudo-random values. The GPU mutates both halves over two cycles. A separate GPU kernel checks every word, and native `hipMemcpy` reads back the entire allocation for CPU verification before and after hibernation. A one-byte cold budget refuses without eviction or data changes. A second allocation consumes the released resident cap, causing resume to refuse until that allocation is freed; recovery then verifies the original data and pointer. The remap fixture fails the fourth map after the first segment has been remapped, retains snapshots, then succeeds after removing the injection. Free while cold, empty resume, pending caller-error preservation, and final zero allocation/cold counters also pass.

Per-process `/proc/self/fdinfo` measurements separate these resources from other desktop applications. In the larger check's first cycle, DRM client 17584 reported resident VRAM **159,916 → 143,532 KiB** and GTT **286,772 → 8,244 KiB** hot-to-cold: exactly **16 MiB VRAM + 272 MiB GTT** released. On restore, GTT returned to 286,772 KiB and VRAM to 159,920 KiB, with a 4 KiB runtime difference. GPU-wide sysfs counters are also logged; they include other applications and need not immediately reflect client releases. These measurements establish backing release for this synthetic check, not total process-RAM savings or a model compression ratio.

Internal snapshot copies use a GPU kernel and a 1 MiB mapped staging allocation with its explicit device alias. An earlier native-copy staging attempt returned incorrect readbacks after remapping; the final path passes both GPU checks and native application readbacks. Access is granted on the full mapped range because ROCm 7.2's [`hipMemSetAccess` implementation](https://github.com/ROCm/clr/blob/rocm-7.2.0/hipamd/src/hip_vm.cpp) validates child sizes from the parent range. Resume maps all missing segments before granting access and restoring chunks. It can require active backing plus retained snapshots, and can fail with recoverable cold data if another allocation occupies capacity or RAM admission fails. These explicit checks do not establish fault paging, compressed active inference, or multi-GPU behavior. Automatic HIP dispatch checks are documented below.

All **21 CTests passed** after this feature, including the existing allocation, lookup, error-state, and Vulkan regressions. Raw logs: [32 MiB integrity](validation/hip-hibernation-integrity.txt), [288 MiB and process residency](validation/hip-hibernation-multisegment.txt), [failed remap recovery](validation/hip-hibernation-remap-recovery.txt).

A separate 1-to-8 MiB snapshot-chunk candidate passed the same 288 MiB mixed-data, two-cycle integrity check before/after/before. The short sequential hibernate/resume timings overlapped, so no reliable gain was established; the candidate was reverted and the default remains 1 MiB. See the [decision summary](validation/hip-snapshot-8mib-summary.json) and its linked logs.

```sh
cmake --build build -j2
ctest --test-dir build -R hip-hibernation --output-on-failure
# Tests select the discrete gfx1100 device and run without privilege.
```

A separate allocator regression reran the unchanged small F16 llama.cpp model for eight tokens through native and VMM/GTT paths. Both exited 0, offloaded 31/31 layers, and produced identical output (SHA-256 `93f6b895a3f0448b3fa0a4299639533448ddafe1a235a21dc059de633fa4fcb3`). VMM reported eight allocations and zero cleanup/failure counters. This run did **not** invoke hibernation; it checks the refactored allocator only. [Summary](validation/hip-hibernation-model-summary.json), [native log](validation/hip-hibernation-model-native.stderr.txt), [VMM log](validation/hip-hibernation-model-vmm.stderr.txt).

## Automatic userspace HIP idle hibernation

On the installed HIP 7.2.53211 ABI, the opt-in ROCm dispatch bridge wraps 506 slots. It gates new HIP entry while snapshotting, drains earlier GPU work, and restores all cold tracked VMM allocations before work proceeds. Direct calls, `hipGetProcAddress`, and native-library `dlsym` synchronization all wake cold allocations. Metadata queries retain cold state, and cold free releases snapshots. Two threads with separate nonblocking streams passed two simultaneous wake cycles, GPU verification, and full readback of both 32 MiB allocations. Stream capture and host callbacks issued while cold safely restore data and disable subsequent automatic snapshots. [Raw integration and guard output](validation/hip-automatic-guards-and-integrity.txt). All 26 CTests passed, including an all-local automatic VMM case with no GTT allocation allowance. [Full test output](validation/hip-automatic-26-tests.txt).

Automatic mode routes intercepted nonzero `hipMalloc` allocations through VMM even when they fit entirely in VRAM; native allocations cannot be hibernated. Other dispatch ABIs do not enable the worker. Raw HSA/direct GPU submissions, multiple-runtime coexistence, and general Vulkan application compression are outside this mode's validated scope. IPC, external memory, raw VMM/memory-pool mutations, and callback registrations disable future snapshots. HIP graph capture is disabled in the model checks using llama.cpp's existing `GGML_CUDA_DISABLE_GRAPHS=1` setting. No application source changes or privileged service are used.

A subsequent all-local small-model check used a 512 MiB local cap and retained all model/work buffers in VRAM while active. It offloaded 31/31 layers, hibernated 307,704,064 logical bytes to 206,994,149 stored bytes, and restored in 169.422 ms. Native versus automatic output was byte-identical over 108 decode runs, with zero cleanup/failure counters. Single-run decode rates were 287.57 versus 281.75 tokens/s (2.02% lower with automatic mode); this short run is not a general overhead estimate. [Summary](validation/hip-automatic-local-model-summary.json), [native stderr](validation/hip-automatic-local-model-native.stderr.txt), [automatic stderr](validation/hip-automatic-local-model-automatic.stderr.txt), [hot fdinfo](validation/hip-automatic-local-model-hot.fdinfo.txt), [cold fdinfo](validation/hip-automatic-local-model-cold.fdinfo.txt).

### Existing Odysseus 27B model

The existing Q4_K_M Qwen 27B model offloaded 66/66 layers, with a 15,088.32 MiB ROCm model buffer. Both runs used an 8,192 MiB local cap and 12,000 MiB GTT cap; the baseline used VMM without automatic mode. The automatic run hibernated before receiving interactive input, restored on the first inference call, and produced byte-identical stdout. Final allocation, backing, cleanup, and failure counters were zero.

| Measurement | Result |
|---|---:|
| Cold logical bytes | 16,150,707,328 |
| Retained cold payload | 15,701,249,373 |
| Payload savings | 2.78% |
| Background hibernation | 41,844.201 ms |
| Wake/restore | 11,488.008 ms |
| Baseline decode, 45 runs | 2.91 tokens/s |
| Automatic decode, 45 runs | 2.94 tokens/s |
| Baseline prompt evaluation | 686.94 ms |
| Automatic prompt evaluation, including restore | 12,228.92 ms |

The approximately 1% decode difference is not evidence of a speed improvement: these are single runs on an active desktop. It isolates automatic-mode overhead at the same constrained placement, **not** slowdown relative to fully GPU-resident Odyssey/Ollama inference. A native all-GPU attempt failed allocating the 15,088.32 MiB buffer under current desktop memory use. The user's roughly 25 tokens/s Odyssey result is not reproduced by this forced-spill benchmark.

Deduplicated process AMD DRM-client memory accounting fell from **8,756,654,080 to 155,070,464 bytes VRAM** and **7,579,774,976 to 18,993,152 bytes GTT** while cold. The captured clients include a small integrated-GPU runtime client. These are process driver counters, not global desktop totals or complete process-RAM usage. [Hot fdinfo](validation/hip-automatic-27b-hot.fdinfo.txt), [cold fdinfo](validation/hip-automatic-27b-cold.fdinfo.txt).

[Summary](validation/hip-automatic-27b-summary.json), [baseline stderr](validation/hip-automatic-27b-baseline.stderr.txt), [automatic stderr](validation/hip-automatic-27b-automatic.stderr.txt), [baseline stdout](validation/hip-automatic-27b-baseline.stdout.txt), [automatic stdout](validation/hip-automatic-27b-automatic.stdout.txt), [native OOM](validation/hip-automatic-27b-native-oom.stderr.txt).

Reproduce using existing binaries/models with `check_idle_model.py --binary /path/to/llama-completion --model /path/to/model.gguf --vmm-baseline --local-mib 8192 --host-mib 12000 --cold-mib 20000 --idle-ms 5000 --tokens 64`. Interactive llama.cpp subtracts input tokens from its prediction budget; the script requires at least 64 to avoid a negative remaining count. Actual decode counts are recorded in the logs.

A later matched **fully VRAM-resident 27B** comparison succeeded after the other GPU workload ended. Native decode was **24.41 tokens/s**, versus **24.33 tokens/s** through automatic VMM hibernation, over 45 decode runs each (0.33% lower in this short desktop run). Both offloaded 66/66 layers and produced byte-identical output. Automatic mode used a 20,000 MiB local cap and 512 MiB GTT cap; captured active GTT usage was only 19,005,440 bytes. Cold storage retained 15,689,006,140 bytes from 16,150,707,328 logical bytes, a 2.86% saving. Hibernation took 39,970.256 ms and wake/restore took 12,866.416 ms. Process VRAM fell from 16,304,242,688 to 154,677,248 bytes while cold, with zero cleanup/failure counters. This reproduces the user's roughly 25 tokens/s fully local result; the earlier ~3 tokens/s benchmark forced substantial GTT spillover. [Summary](validation/hip-automatic-27b-local-summary.json), [native log](validation/hip-automatic-27b-local-native.stderr.txt), [automatic log](validation/hip-automatic-27b-local-automatic.stderr.txt), [hot counters](validation/hip-automatic-27b-local-automatic-hot.fdinfo.txt), [cold counters](validation/hip-automatic-27b-local-automatic-cold.fdinfo.txt).

## Unchanged memtest_vulkan smoke check

Installed `memtest_vulkan` 0.5.0 ran on the RX 7900 XTX with a 2 GiB explicit limit, through the Vulkan layer and natively, sequentially. Each run was interrupted after 12 seconds with SIGINT and exited 65. Neither emitted a memory-error report. The last five-second reports checked about 449.6 GB/s through zVram versus 437.7 GB/s natively. This short check is not a complete five-minute stability test or a performance improvement claim.

Both runs emitted the same SPIR-V `AtomicIAdd` memory-semantics validation warning during shader creation. zVram logged the 2,147,483,648-byte local allocation plus a 432-byte allocation; memtest still reported the native 24 GiB card. This path exercises telemetry and native allocation behavior, not automatic Vulkan compression or capacity expansion. [Wrapped log](validation/memtest-vulkan-zvram-2gib.txt), [native log](validation/memtest-vulkan-native-2gib.txt).

The upstream CLI uses positional device and byte-limit arguments: [`memtest_vulkan` source](https://github.com/GpuZelenograd/memtest_vulkan/blob/main/src/main.rs). The bounded local command was `./zvram --verbose --isolate-layers -- memtest_vulkan 1 2147483648`, with the RADV ICD selected and SIGINT sent after 12 seconds.

## Scoped kernel VRAM reclaim

A temporary root-created cgroup limited the RX 7900 XTX VRAM region to 16 MiB while the unprivileged Vulkan check uploaded and verified all 64 MiB in eight allocations. The check exited successfully and the temporary cgroup was removed. Sampled child dmem peak was 204,800 bytes, host memory.peak was 17,620,992 bytes, and swap usage and OOM events were zero. Global driver counters showed additional GTT backing during the workload. These results establish data integrity with scoped VRAM reclaim; they do not establish TTM swapout or compression. [Raw result](validation/dmem-scoped-first-run.txt).

The helper now checks working `cgroup.kill` before launching any child and uses a random cgroup suffix. A separate two-second timeout test killed both its parent and a detached `setsid()` descendant, returned 124 as expected, and removed the cgroup. [Cleanup test](validation/dmem-detached-cleanup.txt). This keeps detached descendants within bounded cleanup and avoids collisions when a helper runs in a PID namespace. No global swap, TTM, or other application settings were changed.

## Userspace Vulkan segmented allocations

The opt-in `--vulkan-virtual-mib 40960` mode exposes a separate GPU-only logical heap/type and creates eligible storage buffers with sparse binding. The application retains its ordinary buffer handle. Bind calls materialize native backing in aligned segments of at most 256 MiB; synthetic handles are consumed by the layer and never submitted as native bind allocations. Initial bindings complete synchronously. In virtual-only mode, unrelated application queues now forward without taking device-wide snapshot locks or scanning cold buffers; internal sparse-queue operations remain serialized. Automatic snapshot mode is unchanged. This phase provides segmented backing and native migration, not compression.

The virtual-only queue fast path passed **7/7** focused GPU tests, including a concurrent pending-wait case where queue 0 remained blocked until queue 1 signaled a timeline semaphore, plus the explicit fault-arm regression. The earlier failed log is retained for context: its test hook fired during upload setup, before the intended fault point; it is not evidence of data being replaced with zeroes. See the [verified 7-test run](validation/vulkan-virtual-queue-fastpath-verified-tests.txt) and [earlier failed run](validation/vulkan-virtual-queue-fastpath-tests.txt). The full CTest suite is still pending and is not claimed here.

The Vulkan 1.1 transfer check allocated memory before creating its final buffer, used `vkGetBufferMemoryRequirements2` and `vkBindBufferMemory2`, and verified every word of **one 40 GiB allocation and one buffer**, backed by **160 native chunks**. Generation, upload, readback, and verification took **17.454 seconds**. At full upload, global driver counters were 25,161,117,696 bytes VRAM and 24,989,626,368 bytes GTT; these include desktop allocations. No validation error was emitted. [Full log](validation/vulkan-virtual-single-40gib.txt). A smaller 320 MiB check verified the two-chunk boundary through the same API2 calls. [Small log](validation/vulkan-virtual-single-api2-320mib.txt).

Unchanged installed memtest displayed **40 GB** and completed a 12-second, 2 GiB compute smoke check through eight chunks. It was stopped with SIGINT and exited 65 as expected; no memory-error report appeared. The same pre-existing SPIR-V memory-semantics warning appeared as in the earlier native check. Its five-second report checked 443.3 GB/s; this is not a controlled speed comparison or a full five-minute stability run. [Memtest log](validation/memtest-vulkan-virtual-2gib.txt).

Native heaps/types and Vulkan buffer/allocation limit properties are preserved. This driver advertises maintenance4 limits near 4 GiB; the large successful check requested Vulkan 1.1 and did not enable maintenance4. It does not establish that every API version, driver, or buffer usage supports a single 40 GiB buffer. Synthetic allocations currently allow one zero-offset bind, and sparse promotion excludes image expansion, external/protected memory, capture/replay, and synthetic host mapping; eligible BDA storage buffers now pass the dedicated address-integrity checks. Backing capacity remains limited by actual VRAM, GTT, RAM, and driver admission; configured heap capacity alone proves none of these.

A follow-up 48 GiB launch setting displayed **48 GB** in unchanged memtest and passed its bounded 2 GiB check, then exited 65 on SIGINT. [48 GiB setting log](validation/memtest-vulkan-virtual-48gib-2gib.txt). The launch setting now has no artificial upper cap; positivity and 64-bit byte-size overflow are checked. Full 48 GiB use remains unverified.

The same 40 GiB single-allocation check with virtual memory disabled failed at native `vkAllocateMemory` with `VK_ERROR_OUT_OF_DEVICE_MEMORY`. [Native single-allocation failure](validation/vulkan-native-single-40gib.txt). Earlier native 40 GiB success used many separate allocations; the segmented provider enables this one logical allocation by owning smaller native allocations.

The 96 GiB launch setting also displayed **96 GB** and passed an eight-second 2 GiB unchanged memtest check with eight native chunks, ending on SIGINT. [96 GiB setting log](validation/memtest-vulkan-virtual-96gib-2gib.txt). All **28 CTests passed**, including Vulkan legacy/API2 segmented integrity and existing HIP regression checks. The API2 check used a 96 GiB configured heap and verified 320 MiB; it does not establish full 96 GiB backing. [CTest log](validation/vulkan-segmented-ctest.txt).
