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

## Lossless byte-plane filter

The experimental `--vulkan-byte-shuffle 2` / `4` option permutes byte planes before Zstd and restores their original order after decompression. It is disabled by default and requires automatic snapshots. Compressed chunks carry their stride; RAW fallback remains unfiltered. The filter preserves incomplete trailing elements. x86 uses baseline SSE2, with a portable fallback on other targets. This feature currently has **CPU-only validation**; its Vulkan freeze/restore integration and model/game performance have not been tested on the GPU. The preceding 101/101 GPU suite predates this feature.

The corpus is a 32 MiB slice at file offset 67,108,864 of the previously tested InternLM2.5-20B F16 GGUF, covering parts of `blk.0.attn_output.weight` and `blk.0.attn_q.weight`. Input SHA-256: `8a67e5cb4721587afbd3c22104e5cbac52f6460f5db9e9117e939f5186a80895`. No model weights are included in the repository. Each measurement uses Zstd 1.5.7 level 1, one CPU worker, and three exact full-byte round trips.

| Preallocated-buffer CPU measurement | Plain Zstd | Stride 2 | Stride 4 |
|---|---:|---:|---:|
| Stored bytes | 26,673,213 | 23,452,077 | 23,465,723 |
| Median decode + reverse filter | 13.605 ms | 11.737 ms | 12.020 ms |
| Median full filter/compress/decompress/reverse cycle | 30.766 ms | 32.652 ms | 34.129 ms |

Stride 2 saves about 12.1% of compressed storage on this sample. The preallocated restore measurement is faster, while the full CPU cycle is slower. These timings exclude GPU copies and initially exclude the production decoder's scratch allocation. A separate full `decodeOne` measurement, including a zero-filled scratch vector, was slower than plain decoding; the implementation now allocates scratch without zero-fill and still verifies the exact decompressed size before reading it. Five full-call repetitions of the current implementation measured **13.867 ms plain**, **14.246 ms stride 2**, and **14.443 ms stride 4**, with exact 32 MiB output every time. Including allocation, filtered decoding remains about 2.7–4.1% slower here. This is a storage experiment, with no measured token-rate gain.

Filtered decoding adds up to 32 MiB temporary memory per worker, at most 128 MiB across four workers, in addition to existing staging. Allocation or decoding failure returns failure before filtered bytes are restored to the GPU. CPU tests cover mixed RAW/plain/filtered batches, stride 2/4, odd lengths, canaries, corrupt/truncated frames, invalid metadata, and caller/worker failures. Both SSE2 and forced portable implementations passed 1,080 reference comparisons and round trips. AddressSanitizer/UndefinedBehaviorSanitizer passed the mixed snapshot test.

Four synthetic/native Vulkan byte-shuffle tests are registered for strides 2 and 4, but remain **unrun**; byte-shuffle GPU integration is still unverified. Their focused selector is `ctest --test-dir build -R '^vulkan-byte-shuffle-' --output-on-failure`.

Evidence: [SSE2 component results](validation/byte-shuffle-internlm-f16-32m-offset64m.zstd-shuffle-bench-sse2.json), [portable component results](validation/byte-shuffle-internlm-f16-32m-offset64m.zstd-shuffle-bench-portable.json), [component benchmark source](validation/byte-shuffle-cpu_zstd_shuffle_bench.cpp), [CPU CTest](validation/byte-shuffle-cpu-ctest.txt), [test details](validation/byte-shuffle-cpu-details.txt), and [sanitizer output](validation/byte-shuffle-sanitizer-final.txt). Compile the component source from the repository root with `g++ -std=c++17 -O3 -Wall -Wextra -Werror -I. validation/byte-shuffle-cpu_zstd_shuffle_bench.cpp -lzstd -o /tmp/zvram-shuffle-bench`; pass the exact local 32 MiB input slice as its argument. It writes compressed fixtures beside that input. Add `-U__SSE2__` to exercise the portable path.

Full-call evidence: [current scratch allocation](validation/byte-shuffle-internlm-f16-32m-offset64m.snapshot-decode-one-uninitialized.json), [historical zero-filled scratch](validation/byte-shuffle-internlm-f16-32m-offset64m.snapshot-decode-one-zeroed-historical.json), and [benchmark source](validation/byte-shuffle-snapshot_decode_one_bench.cpp). Compile with `g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Werror -I. validation/byte-shuffle-snapshot_decode_one_bench.cpp -lzstd -o /tmp/zvram-decode-one-bench`; arguments are the raw slice followed by plain, stride-2, and stride-4 compressed fixtures.

### Optional CPU GDeflate snapshot codec

An optional snapshot codec adapter uses the NVIDIA libdeflate GDeflate API on the **CPU**. It is disabled by default with `ZVRAM_ENABLE_GDEFLATE=OFF`, so Zstd remains the default and only built-in codec. The optional dependency is fetched from the pinned NVIDIA fork at commit `8ba9502fb30d2bf728592d121f0d402e40c8cb05` (`https://codeload.github.com/NVIDIA/libdeflate/tar.gz/8ba9502fb30d2bf728592d121f0d402e40c8cb05`) with SHA-256 `d1b4c38dce43e68a5f4c28d0fbb3f81a01953039a3dea63f4bd1a84d7ff80592`; its upstream `COPYING` carries MIT and Apache-2.0 notices. The launcher accepts `--vulkan-codec zstd|gdeflate`; codec selection requires automatic Vulkan snapshots. It rejects GDeflate before launch if the optional build is absent and rejects byte-shuffle with GDeflate. Codec tags are retained for both direct restore and decode-ahead. This adapter handles CPU encode/decode; the opt-in layer GPU restore path is documented separately below.

A 32 MiB F16 model slice encoded and round-tripped exactly with the new CPU API; its stream SHA-256 `a6bb608fbc0099811e5d3e35717b590f6e66e149c0834262d990a9a3e4c3e602` matches the previously verified stream. In a single CPU component run, GDeflate produced **27,219,160 bytes** and took **355.970 ms encode / 114.457 ms decodeOne**, versus Zstd's **26,673,213 bytes** and **19.194 / 13.907 ms**. GDeflate was slightly larger and slower; this is format-boundary and correctness groundwork, not a speed gain. Fresh CPU CTest runs passed **8/8** with the default build and **10/10** with the optional codec enabled: [default log](validation/gdeflate-codec-default-cpu-ctest.txt) and [codec log](validation/gdeflate-codec-cpu-ctest.txt). No model weights are published. See [model result](validation/gdeflate-codec-model.json), [text summary](validation/gdeflate-codec-model.txt), and the [build and reproduction notes](research/gdeflate/README.md#optional-cpu-snapshot-codec).

A bounded CPU scaling test measured the production encoder on the saved 32 MiB F16 slice with one, four, eight, sixteen, and thirty-two workers. Three interleaved runs per setting gave median encode times of **345.990 / 91.849 / 54.909 / 33.635 / 29.777 ms**, respectively. Thirty-two workers improved this component by **11.62×** versus one and **1.13×** versus sixteen. Every stream was byte-identical to the previously verified stream and passed exact production CPU decoding. The host has sixteen physical cores and thirty-two logical CPUs; CPU clocks and competing desktop work were not controlled. Runs used `nice -n 19`, a 1 GiB address-space limit, and a ten-second per-run timeout; peak child RSS was 138,704 KiB. The initial thirty-two-worker attempt failed cleanly under that address-space limit because allocator arenas reserve virtual space; reruns used process-local `MALLOC_ARENA_MAX=2`, without changing any system setting. The opt-in worker limit is now `1..32`; default remains `1`. Thread creation or allocation failure joins started workers and returns encoding failure. These elapsed-time CPU measurements do not prove GPU restore speed, token throughput, or frame-time improvement. [Commands, hashes, all samples, and scope](validation/gdeflate-workers-32-model.json).

A separate synthetic Vulkan-layer application check exercised the CPU codec with GPU-backed snapshots: **320 MiB** was frozen and restored across two wake/compute cycles, with full-byte verification each time. The first cold pass stored **10 compressed chunks** in **17,352,796 bytes**; the second stored **10 RAW chunks** in **335,544,320 bytes**. Snapshot failures and cleanup counts were zero. This validates one synthetic layer path only; it is not model inference or a general application compatibility result, and the GDeflate shader was not used. Reproduce it with the separate codec-enabled build directory:

```sh
./zvram --build-dir build/gdeflate-codec --validate --isolate-layers \
  --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 \
  --vulkan-codec gdeflate -- build/gdeflate-codec/zvram-vulkan-auto-check
```

Evidence: [layer restore log](validation/gdeflate-layer-auto-restore.txt).

### Opt-in GPU GDeflate restore

The CPU encoder now retains caller output capacity across chunks instead of allocating a fresh result buffer for each call. A regression check preserves a preallocated 1 MiB buffer across changing input sizes and exact round trips; it fails against the prior encoder. The optional CPU suite passed **10/10**. Encoding the same 32 MiB F16 slice still produced the identical 27,219,160-byte stream already verified by the GPU decoder. This removes avoidable allocation work without changing the format; no throughput benefit has been measured, and the GPU suite predates this allocation-only change. [CPU reuse checks](validation/gdeflate-output-reuse-cpu-ctest.txt), [stream identity and regression evidence](validation/gdeflate-output-reuse.json).

The optional `--vulkan-gdeflate-gpu` path uses the wave32 decoder during Vulkan automatic restore. It requires a codec-enabled build, `--vulkan-codec gdeflate`, and automatic snapshots; Zstd remains the default, and the CPU codec remains the default GDeflate restore path. `--vulkan-gdeflate-workers N` sets CPU encoding parallelism from **1 to 32** and defaults to **1**. For eligible compressed chunks up to 32 MiB, the layer uploads compressed input and decodes directly into private sparse backing views. RAW chunks stay on the CPU/copy path. A recoverable GPU decode error is counted and falls back to CPU; an unsafe fence or device failure stops reuse to protect backing contents. Temporary GPU input, upload, and scratch allocations are outside tracked resident admission and can increase peak memory.

One unchanged SmolLM2-135M F16 run passed **14/14** checks, retained matching output and **31/31** layers, and recorded **11 GPU decode calls / 308,084,736 bytes**, with zero fallback and validation diagnostics. Its timings overlapped focused synthetic GPU tests, so they are not a speed comparison. A later short sequential pair, with validation disabled and no owned GPU tests running, produced matching output hashes in both modes. The GPU restore pair measured **258.95 vs 269.69 tokens/s** wrapped/native, with **169 ms vs 7 ms** first output; the CPU GDeflate pair measured **312.49 vs 315.97 tokens/s**, with **1,078 ms vs 8 ms** first output. These first-output delays are about six times different, but the sample is short, the desktop remained active, clocks were not fixed, and native generation timing varied between pairs; this does not establish a robust steady-state gain, gameplay speed, or 40 GiB behavior. Evidence: [benchmark summary](validation/gdeflate-clean-benchmark-summary.json), [GPU pair](validation/gdeflate-gpu-smollm-clean-benchmark/result.json), and [CPU-codec pair](validation/gdeflate-cpu-smollm-clean-benchmark/result.json).

The strict focused layer suite passes **5/5**. Its synthetic and adopted-native range fixtures each recorded two GPU calls totaling **67,108,864 bytes**, with zero fallback; the following RAW restore cycle used the CPU path as expected. The earlier RAW-only fixture attempt is retained as superseded evidence: [initial range-fixture attempt](validation/gdeflate-gpu-focused-raw-range-attempt.txt). The existing standalone 32 MiB shader measurements above are component tests, not layer-restore evidence.

### InternLM2.5-20B GPU GDeflate pressure attempt

A guarded run used the unchanged **39,725,643,136-byte** F16 model with GPU GDeflate restore, a 20 GiB tracked-residency limit, and a 24 GiB cold budget. Native completed 12 decode runs at **1.47 tokens/s** with 49/49 layers. The wrapped process timed out after 240 seconds while waiting for model/snapshot readiness; it produced no output, so there is no wrapped token rate, output comparison, or completed-inference result. During repeated weight-loading pressure it recorded **388 GPU restore calls / 12,882,149,376 bytes**, with zero GPU fallback; the last state showed 232 freezes, 99 restores, zero snapshot failures, and 98 cache invalidations. This is startup-pressure evidence only, not inference proof or a speed result. [Run result](validation/gdeflate-gpu-internlm-quick-pressure/result.json), [wrapped stdout](validation/gdeflate-gpu-internlm-quick-pressure/automatic.stdout.txt.gz), [wrapped stderr](validation/gdeflate-gpu-internlm-quick-pressure/automatic.stderr.txt.gz), [native stdout](validation/gdeflate-gpu-internlm-quick-pressure/native.stdout.txt.gz), and [native stderr](validation/gdeflate-gpu-internlm-quick-pressure/native.stderr.txt.gz).

A separate retry with a **900-second timeout** was intentionally stopped after the user reported a transient game stall; the game recovered after the stop. Follow-up inspection found no AMDGPU ring timeout/reset, VRAM-loss, or OOM evidence. The stall's cause is unproven, and this is not evidence that the GDeflate path caused or fixed it. No accepted 40 GiB wrapped inference throughput is available. Startup backing is not yet fully covered by the configured 20 GiB admission accounting; a CPU-only correction is underway, with no retry result claimed here. [Stopped-run manifest](validation/gdeflate-gpu-internlm-extended-pressure/result.json).

Build and run with the separate codec-enabled directory:

```sh
./zvram --build-dir build/gdeflate-codec --vulkan-codec gdeflate \
  --vulkan-gdeflate-gpu --validate --isolate-layers --vulkan-virtual-gib 96 \
  --vulkan-auto-idle-ms 1000 --vulkan-cold-mib 512 -- path/to/vulkan-app
```

Evidence: [14-check model result](validation/gdeflate-gpu-smollm-idle/result.json), [focused GPU layer suite, 5/5](validation/gdeflate-gpu-focused-ctest.txt), [run scope and hashes](validation/gdeflate-gpu-layer-results.json), [superseded RAW-only range-fixture attempt](validation/gdeflate-gpu-focused-raw-range-attempt.txt), and [small-model run logs](validation/gdeflate-gpu-smollm-idle/).

### Research-only GDeflate decoder

The standalone bounded Vulkan shader tests, distinct from the optional CPU codec above, are documented under [`research/gdeflate/README.md`](research/gdeflate/README.md). The wave32 decoder is also used by the opt-in layer restore path documented in the preceding section; these component tests do not validate that path. Its CPU subgroup-8 SPIR-V passed exact full-byte verification on **llvmpipe**, a CPU Vulkan device (`LLVM 23.1.1`, Mesa `26.2.4-arch3.1`, two-worker limit). The full real F16 slice decoded **32 MiB across exactly 512 tile workgroups**, with zero validation errors or VUIDs. The same run set also passed synthetic 64 KiB and multi-tail streams and an independently wrapped first F16 tile. This is shader correctness on a CPU software driver only; its timings are not GPU, model, or token-throughput evidence. The software driver was extracted locally from a signature-verified package, not installed system-wide. No model weights are included. [Full CPU run log](validation/gdeflate-software-f16-32m.txt), [run manifest and hashes](validation/gdeflate-software-tests/results.json), [driver provenance](validation/gdeflate-software-driver-provenance.json), [first-tile log](validation/gdeflate-software-f16-first-tile.txt), and [multi-tail log](validation/gdeflate-software-multi-tail.txt).

After a reboot, the bounded **wave32 GPU shader** passed exact full-byte comparisons on the AMD Radeon RX 7900 XTX (RADV NAVI31) for all three inputs below. Validation reported zero errors and zero VUIDs in each one-iteration test. The 32 MiB compressed input is the same stream produced by the optional CPU codec above.

| Decoded bytes | Tiles | Upload | GPU decode | Readback copy |
|---:|---:|---:|---:|---:|
| 65,536 | 1 | 0.001 ms | 5.267 ms | 0.006 ms |
| 1,048,576 | 16 | 0.037 ms | 5.361 ms | 0.019 ms |
| 33,554,432 | 512 | 1.038 ms | 5.792 ms | 1.133 ms |
| 131,195 (synthetic odd tail) | 3 | 0.001 ms | 0.782 ms | 0.002 ms |

This is bounded standalone shader correctness and single-iteration component timing, not application restore, full-model inference, or token throughput. `--gpu-smoke` remains limited to one tile; `--gpu-bounded-smoke` permits up to 32 MiB / 512 tiles, one iteration, and the same five-second fence timeout. The earlier unbounded real-sample experiment remains historical: it triggered an AMDGPU ring timeout, ring reset, and device-wedged report; its diagnostic handler crashed, and no devcoredump was captured. The passing bounded cases do not establish general watchdog safety. The optional layer codec encodes on CPU; GPU restore is a separate opt-in path, and these shader timings do not measure it. Zstd remains the default; byte-shuffle stays off by default. The last full **101/101** GPU suite predates optional codec integration. Evidence: [64 KiB GPU log](validation/gdeflate-gpu-64k-fp16.txt), [1 MiB GPU log](validation/gdeflate-gpu-1m-fp16.txt), [32 MiB GPU log](validation/gdeflate-gpu-32m-fp16.txt), [synthetic odd-tail GPU log](validation/gdeflate-gpu-multi-tail.txt), [GPU results](validation/gdeflate-gpu-results.json), [earlier GPU logs](validation/gdeflate-gpu-vram-single-64k.log), and [historical failure record](validation/gdeflate-real-f16-gpu-failure.txt).

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

### Offscreen graphics fixture on software Vulkan

The graphics fixture passed on CPU llvmpipe (`type=4`) with validation enabled: three offscreen fullscreen-triangle frames rendered a 16×16 RGBA8 image from an SSBO and matched both exact pixels and a full 32 MiB buffer readback. A separate hardware CTest passed **3/3** in **2.96 seconds** on synthetic cold draw, selective/active cold draws, and adopted-native cold draws, with exact pixels, full 32 MiB readback, and no validation callbacks. This validates only the fixture paths; SDL2 presentation remains compile-checked but not run. It is not gameplay graphics or performance evidence. The hardware cold-draw gate passes, while headless-gamescope presentation remains pending. The production presentation guard is unchanged. [Hardware CTest log](validation/graphics-hardware-ctest.txt) and [full details](validation/graphics-hardware-details.txt.gz).

```sh
python3 test_graphics_software.py --binary build/zvram-vulkan-graphics-check \
  --icd build/third-party/lavapipe/local_lvp_icd.json \
  --log build/graphics-software.txt
```

Evidence: [software-native log](validation/graphics-software-native.txt) and [result summary](validation/graphics-software-results.json).

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

The focused pressure gate passed **6/6** in **3.42 seconds**, covering synthetic and native backing, single and two-queue work, and robustness2 alignment cases. The resident-admission milestone CTest suite passed **75/75** in **46.19 seconds**. The logs are [focused pressure checks](validation/vulkan-pressure-focused.txt) and [full CTest run](validation/vulkan-pressure-75-ctest.txt).

The unchanged SmolLM2-135M-Instruct F16 check passed **21/21 checks**, retained **31/31 layer offload**, and produced identical nonempty native and wrapped output. The model used 293.8125 MiB of eligible backing. After a full cold pass, the 192 MiB limit held tracked resident backing to **165.8125 MiB** peak and triggered **354 evictions**, with no admission refusals. Both runs used `GGML_VK_MAX_NODES_PER_SUBMIT=1` and Vulkan core/synchronization validation. Native decode measured **102.46 tokens/s** versus **2.82 tokens/s** wrapped over 44 decode runs; this is one short workload with substantial overhead, not a speed claim or evidence of fast large-pool compression. Both native and wrapped streams had zero Vulkan validation diagnostics. The tested model binary was unchanged. See [result summary](validation/vulkan-pressure-model-result.json), [native stderr](validation/vulkan-pressure-model-native.stderr.txt), [wrapped stderr](validation/vulkan-pressure-model-automatic.stderr.txt), [native output](validation/vulkan-pressure-model-native.stdout.txt), [wrapped output](validation/vulkan-pressure-model-automatic.stdout.txt), and hot/cold [native](validation/vulkan-pressure-model-native-hot.fdinfo.txt) and [wrapped hot](validation/vulkan-pressure-model-automatic-hot.fdinfo.txt)/[cold](validation/vulkan-pressure-model-automatic-cold.fdinfo.txt) fdinfo captures.

To reproduce the model run from the repository root:

```sh
python3 check_vulkan_idle_model.py --binary build/third-party/llama-vulkan-build/bin/llama-completion --model build/third-party/models/SmolLM2-135M-Instruct-f16.gguf --tokens 64 --idle-ms 100 --range-mib 32 --resident-mib 192 --resident-after-cold --strict-robustness --max-nodes-per-submit 1 --validate --output-dir build/vulkan-pressure-model-normal --timeout 120
```

`--strict-robustness` opts into supported `VK_EXT_robustness2` and includes descriptor alignment in range tracking. Explicit weaker robustness1 remains whole-buffer tracked; dynamic storage descriptors also use conservative whole-buffer tracking. Without this option, core `robustBufferAccess` keeps whole-buffer tracking. This result tests tracked buffer chunks in one small model run. It does not prove a global VRAM cap, image/graphics eviction, a 40 GiB model, or fast 40 GiB compression. The optional `--serialize-submissions` mode is not part of this clean result: a separate native run with that llama.cpp option emitted `VUID-vkResetCommandPool-commandPool-00040` on stdout before zVram was involved ([native stdout](validation/vulkan-pressure-serialized-native-native.stdout.txt)). Normal submissions passed validation in both streams.

### Lazy Vulkan backing (opt-in)

`--vulkan-lazy-backing` requires `--vulkan-range-mib` and an immediate `--vulkan-resident-mib N` limit; it rejects `--vulkan-resident-after-cold`. Eligible pristine chunks begin without backing and are allocated/bound before admitted use, without copying their undefined initial contents. Once initialized, they follow the regular lossless snapshot path. Central restore preflight refuses work that would exceed the tracked resident cap, including paths that could otherwise bypass admission. A CPU production-path harness passed checks for zero-allocation startup, exact-cap full restore, pre-allocation refusal one byte over cap, disjoint bind while cold, initial-bind rollback, allocation retry, and sticky sparse-failure accounting, plus conservative unknown-submit admission/restore. The narrow hardware and small-model gates below passed; game and 40 GiB behavior remain unverified. Allocation or bind failure can prevent paging. The default presentation path remains conservative; opt-in null-`pNext` base presentation has only the narrow X11 fixture proof below. Swapchain images and explicit application sparse submissions remain unsupported. This is not a global VRAM cap. The optional CPU suite passed **11/11** in **2.08 seconds**; a focused rerun also passed the added unknown-submit gate. [CPU results and source hashes](validation/lazy-backing-cpu-results.json), [focused production-path log](validation/lazy-backing-cpu-details.txt).

### Lazy backing and budget hardware checks

A bounded RX 7900 XTX/RADV check in hidden Gamescope X11 verified zero tracked backing and zero stored bytes before initializing an ordinary native 32 MiB pool. Lazy/async Zstd then passed three exact-pixel/full-buffer cold-restore/present cycles. Lazy backing with a 2 GiB budget margin, 32-worker GDeflate encoding, async snapshots, and direct GPU restore passed the same gate. An intentionally impossible 256 GiB reserve reduced the effective limit to zero: the first upload was refused before backing allocation, and cleanup reported zero resident/cold bytes, freezes, restores, or snapshot failures. Synchronization validation reported no errors. This tests budget sampling and refusal, not a reservation or protection against another process racing the estimate.

An unchanged SmolLM2-135M F16 run with a **192 MiB** backing cap passed **32/32 checks**: matching nonempty native/wrapped output, 31/31 GPU layers, lazy initial backing, observed native-budget queries, eleven async commits, clean-cache reuse/invalidation, and zero snapshot/validation errors. GPU restoration performed 124 calls over 3,603,365,888 bytes with zero fallback. This proves the combined path on one small model; the 40 GiB workload and games remain unverified.

Short sequential runs without validation produced matching output but failed the full scenario gate because no async commit or clean-cache invalidation occurred. Their wrapped rates were 3.16 tokens/s for GPU GDeflate and 6.44 for Zstd; respective native rates were 45.09 and 115.00. These diagnostic samples do not establish an async speedup or a controlled codec comparison. Clocks and desktop activity were uncontrolled; urgent admission remains synchronous.

[Hardware and model summary](validation/lazy-hardware/summary.json), [accepted small-model report](validation/lazy-hardware/smollm-lazy-async-headroom/result.json), [budget-refusal report](validation/lazy-hardware/present-headroom-refusal-verified/result.json). Reproduce the tiny refusal with `python3 test_graphics_presentation.py --binary build/zvram-vulkan-graphics-check --icd /usr/share/vulkan/icd.d/radeon_icd.json --prefer-device 1002:744c --native-allocation --lazy-backing --headroom-mib 262144 --expect-headroom-refusal`; the runner uses a 20-second watchdog and checks the expected refusal rather than counting it as presented frames.

### Base buffer presentation (opt-in)

`--vulkan-buffer-presentation` retains buffer paging across a base `VkPresentInfoKHR` with a null `pNext`. It requires initialized automatic snapshots and active eviction; otherwise device creation fails. The layer forwards the original presentation arguments and semaphore waits under its device/queue locks, without restoring unrelated cold buffers, injecting a copy bridge, or switching paging off. Native swapchain images remain outside compressed buffer backing. Device loss poisons the reuse gate; ordinary presentation statuses are returned unchanged.

This follows [Vulkan's presentation semantics](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html): base present reads swapchain images after its semaphore waits. A non-null `pNext` uses the existing conservative full-restore/paging-disable path, including extensions that could reference buffers. Explicit app sparse submissions also retain that fallback. No metadata extension chain is enabled by this first implementation.

CPU production mocks verify unchanged arguments/semaphore pointers, cold buffers left unallocated even above the resident cap, retained paging, suboptimal/out-of-date forwarding, poisoned-gate refusal, device-loss poisoning, and default/extension-chain fallback. The default CPU suite passed **10/10 in 1.82 seconds**, and the optional codec CPU suite passed **12/12 in 4.23 seconds**, including 32-worker exactness checks ([default CPU log](validation/presentation-async/default-cpu-ctest.txt), [optional CPU log](validation/presentation-async/gdeflate-cpu-ctest.txt)). In hidden Gamescope **X11**, the native baseline and wrapped synthetic/native-pool paths passed three presented draw/readback frames each with exact pixels, full **32 MiB** checks, and synchronization validation; async Zstd and async GPU GDeflate restore also passed the same three-frame gate. This is a narrow X11 fixture, not a game, general swapchain, or frame-time result. A native headless Gamescope **Wayland** control (no layer) timed out after `VK_ERROR_OUT_OF_DATE_KHR`, so current presentation evidence is X11-only. Native swapchain images remain unpaged; non-null extension chains and explicit app sparse submissions retain the conservative fallback. [Native X11 baseline](validation/presentation-async/present-native-sync-after/result.json), [synthetic-pool X11](validation/presentation-async/present-zvram-sync-after/result.json), [native-pool X11](validation/presentation-async/present-native-pool-sync-after/result.json), [async Zstd](validation/presentation-async/present-async-sync-after/result.json), [async GPU GDeflate](validation/presentation-async/present-async-gdeflate-gpu/result.json), and [native Wayland control](validation/presentation-async/present-native-wayland-sync-after/result.json).

### Asynchronous idle-range compression (opt-in)

`--vulkan-async-compression` requires active range paging (`--vulkan-range-mib`) and is disabled by default. A worker processes at most one eligible idle-range snapshot, capped at **32 MiB**. Admission-triggered compression remains synchronous. The worker captures GPU bytes and restores application aliases while holding device and queue locks, then releases both locks for CPU encoding. Before publishing cold bytes it rechecks allocation identity, binding/child generations, accepted-write epochs, pending references, the paging gate, and the cold-store budget; stale candidates are discarded.

Hidden Gamescope X11 checks passed three exact-pixel/full-32-MiB presented draw/readback frames for async Zstd and async GDeflate with **32 CPU encoding workers** and direct GPU restore. In the GDeflate run, four snapshots each encoded **33,554,432 → 1,744,520 bytes**; three cold restores used the GPU for **100,663,296 bytes** with zero fallback. The optional GDeflate CPU suite passed **12/12 in 4.23 seconds**. These are correctness results only, with no speed or frame-time claim. [Async Zstd result](validation/presentation-async/present-async-sync-after/result.json), [async GDeflate/GPU result](validation/presentation-async/present-async-gdeflate-gpu/result.json), and [codec CPU CTest log](validation/presentation-async/gdeflate-cpu-ctest.txt).

To reproduce the hidden Gamescope X11 GDeflate case from the repository root:

```sh
python3 test_graphics_presentation.py \
  --binary build/zvram-vulkan-graphics-check \
  --build-dir build/gdeflate-codec \
  --icd /usr/share/vulkan/icd.d/radeon_icd.json \
  --prefer-device 1002:744c --native-allocation \
  --async-compression --gdeflate-gpu \
  --output-dir build/present-async-gdeflate-gpu
```

### Vulkan memory-budget headroom (opt-in)

`--vulkan-headroom-mib N` subtracts a reserve from the tracked backing cap using `VK_EXT_memory_budget` for exactly one native device-local heap. It requires `--vulkan-lazy-backing`, immediate `--vulkan-resident-mib` admission, an application-enabled Vulkan 1.1 path or `VK_KHR_get_physical_device_properties2` (including its KHR function alias for Vulkan 1.0), and device support for `VK_EXT_memory_budget`. The layer enables the device extension when supported; unsupported configurations or failure to initialize immediate lazy paging are rejected rather than silently continuing without the requested mode.

At admission, the layer samples the raw native heap, sets `otherUsage = max(heapUsage - trackedLocalBacking, 0)`, and estimates the backing cap as `min(configuredCap, max(min(heapBudget, heapSize) - reserve - otherUsage, 0))`. Subtracting only tracked local backing leaves private helper and other untracked use charged against available headroom. The heap-derived limit is compared against total tracked backing, so nonlocal fallback can conservatively over-evict or refuse work. Simultaneous admissions across logical devices can race on the estimates; this is not a shared reservation. Driver budgets can change after sampling, including through another process or device sharing physical memory, so this is a conservative estimate rather than a reservation, global VRAM cap, or guarantee against allocation failure.

The CPU helper, production-path mock, and CLI checks passed **10/10** in **1.76 seconds**. The mock exercises budget queries in process without calling Vulkan. Later narrow hardware and small-model checks are documented above; no game or 40 GiB result is claimed. [Headroom CPU CTest log](validation/headroom-cpu-ctest.txt).

## Vulkan clean-snapshot cache

`--vulkan-clean-cache` requires `--vulkan-range-mib`. It keeps verified compressed backing available after a tracked chunk is restored, so a later cold transition can reuse that snapshot instead of recompressing the chunk. Reuse is limited to read-only accesses the layer can prove: SPIR-V `NonWritable` descriptor ranges and copy-source ranges. Any accepted write invalidates the affected chunks; an accepted access the layer cannot classify invalidates the entire cache. A failed queue submission does not invalidate cached contents.

Clean backing shares `--vulkan-cold-mib` with cold snapshots; their combined stored size remains within that quota. Clean cached snapshots are expendable and trimmed when quota space is needed. This cache uses bounded host memory, not an unbounded second copy of the working set. Access tracking remains conservative across the aggregate bound sets and pipelines, and explicitly ordered dispatch support is not implemented.

The full CTest suite passed **81/81** in **53.69 seconds**, with zero validation errors or VUIDs ([full suite](validation/vulkan-clean-cache-81-ctest.txt)). The focused synthetic/native gate passed **6/6** in **7.69 seconds**, with zero Vulkan validation diagnostics. Alternating readbacks reused **18** clean snapshots, shader writes invalidated stale snapshots while preserving every byte, and the three-chunk quota cases trimmed **28** expendable clean copies. The maximum combined cold-plus-cache payload was **67,108,864 bytes**, exactly the configured **64 MiB** quota. A separate accepted-unknown event-plus-copy submission modified one chunk; both chunks survived a complete cold/restore cycle with the updated and untouched patterns verified. Cleanup reported zero resident, cold, and cached bytes. See the [focused cache checks](validation/vulkan-clean-cache-focused.txt).

A paired unchanged SmolLM2-135M-Instruct F16 check used the 192 MiB resident limit, 32 MiB ranges, strict robustness, admission after cold startup, and one node per submit. The pressure-only baseline passed **21/21** checks; clean-cache mode passed **25/25**, with identical stdout in both (SHA-256 `40af802dfb2b6042c7e4ec011b0d82ed8057f634f9c724e68f23ca1917fef76b`). Tracked resident peak stayed **165.8125 MiB** in both runs. The cache recorded **405 clean reuses** and **3 invalidations**; the final cold-plus-clean payload was **206,787,490 bytes** (about **197 MiB**) under the shared **512 MiB** quota. In separate paired runs with 44 decode iterations, pressure-only throughput measured **2.82 tokens/s** and clean-cache mode **6.23 tokens/s**; these are short, workload-specific observations, not a general speed claim. Native reference runs measured **123.74** and **123.25 tokens/s**, respectively, showing no material change to the native baseline. This does not establish a 40 GiB result or game workload behavior. See the [clean-cache summary](validation/vulkan-clean-cache-model-result.json), [pressure-baseline summary](validation/vulkan-clean-cache-baseline-result.json), clean-cache [native stdout](validation/vulkan-clean-cache-model-native.stdout.txt), [wrapped stdout](validation/vulkan-clean-cache-model-automatic.stdout.txt), [native stderr](validation/vulkan-clean-cache-model-native.stderr.txt), [wrapped stderr](validation/vulkan-clean-cache-model-automatic.stderr.txt), and [hot fdinfo](validation/vulkan-clean-cache-model-automatic-hot.fdinfo.txt)/[cold fdinfo](validation/vulkan-clean-cache-model-automatic-cold.fdinfo.txt), with corresponding `vulkan-clean-cache-baseline-` files.

Reproduce the clean-cache model check from the repository root with the same helper and flags as the resident-pressure check, adding `--clean-cache` and a distinct output directory:

```sh
python3 check_vulkan_idle_model.py --binary build/third-party/llama-vulkan-build/bin/llama-completion --model build/third-party/models/SmolLM2-135M-Instruct-f16.gguf --tokens 64 --idle-ms 100 --range-mib 32 --resident-mib 192 --resident-after-cold --strict-robustness --max-nodes-per-submit 1 --validate --clean-cache --output-dir build/vulkan-clean-cache-model --timeout 120
```

The reuse rule follows SPIR-V's `NonWritable` decoration; see the [SPIR-V specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#_decoration). This current model result is a narrow validation of cache retention and reuse, not proof of general active-working-set caching, unbounded memory savings, or faster large-pool compression.

## Vulkan range eviction policy

`--vulkan-eviction-policy lru|mru` requires `--vulkan-resident-mib`; the default is `lru`. LRU evicts the least-recently-used eligible completed chunk first. MRU evicts the newest eligible completed, unselected chunk first. Both policies retain the existing protection for selected or in-flight chunks and the same unknown-access fallback.

The matched SmolLM2-135M F16 scan runs used a 1,000 ms idle interval, clean cache, 192 MiB resident limit, 32 MiB ranges, strict robustness, one node per submit, and full validation. Each passed **26/26** checks and the LRU/MRU outputs were identical. LRU measured **6.23 tokens/s**; MRU measured **8.36 tokens/s** in one run each, a **34.2%** higher observed rate. Their native references measured **122.04** and **119.43 tokens/s**, respectively. Tracked peak resident backing was **165.8125 MiB** for LRU and **185.875 MiB** for MRU, below the configured limit. MRU restored **8,640 MiB** across 270 chunk restores versus LRU's **13,056 MiB** across 408 restores (33.8% fewer bytes/calls). Clean reuses were 262 for MRU and 401 for LRU; the lower MRU count reflects fewer restore cycles, not a lower cache hit ratio. These are single-run, small-workload observations with throughput variance, not a general speedup or evidence for 40 GiB or game workloads.

The focused policy gate passed **4/4** in **2.86 seconds**, and the full CTest suite passed **86/86** in **56.75 seconds**, both with zero Vulkan validation errors or VUIDs. See the [focused log](validation/vulkan-mru-focused.txt) and [full-suite log](validation/vulkan-mru-86-ctest.txt).

The helper's `--eviction-policy lru|mru` selects the same policy. Optional `--min-available-mib N` aborts its child process group when system-wide Linux `MemAvailable` falls below the configured floor. It is a safety stop, not a hard allocation cap. Summaries and raw run evidence: [LRU](validation/vulkan-lru-scan-model-result.json), [MRU](validation/vulkan-mru-scan-model-result.json), and their `validation/vulkan-{lru,mru}-scan-model-*` output and fdinfo files.

The paired result differs from the earlier clean-cache model check, which did not select a residency policy and established the historical LRU-path measurement. See [clean-snapshot cache evidence](#vulkan-clean-snapshot-cache) for its configuration and limits.

A larger unchanged downloaded 27B Q4 model completed both runs with identical stdout and **66/66** layers offloaded, with no Vulkan validation diagnostics. With a 1,000 ms idle interval, 32 MiB chunks, clean cache, MRU, and a 12 GiB resident limit, wrapped throughput was only **0.09 tokens/s**, versus **13.87** for its native Vulkan reference (45 decode iterations). The helper correctly reports **failure: 24/26 checks**: no pressure admissions or pressure evictions occurred. The idle worker kept tracked residency below about 3.004 GiB, so this run does not exercise MRU pressure admission or establish a speedup. It recorded 23,104 restores and 23,524 freezes. Both runs used mmap loading, one node per submit, strict robustness, and validation; these settings differ from the earlier HIP/Ollama measurements. The wrapped run's minimum system `MemAvailable` was **25,375 MiB**, above the **16,384 MiB** guard. See the [failed pressure-check summary](validation/vulkan-27b-short-idle-result.json), [compressed wrapped diagnostic log](validation/vulkan-27b-short-idle-automatic.stderr.txt.gz), [native log](validation/vulkan-27b-short-idle-native.stderr.txt), and matching [native](validation/vulkan-27b-short-idle-native.stdout.txt)/[wrapped](validation/vulkan-27b-short-idle-automatic.stdout.txt) output. The longer-idle run below tests pressure admission separately.

With a **60,000 ms** idle interval, the same downloaded **16.8107 GB Q4** model then passed **26/26** checks. Native and wrapped output matched (SHA-256 `61c5375468005f6c1436b328f039ef4a5fbb6b73687461b4cfe4d7f5e6d20a5b`), all **66/66** layers offloaded, and validation reported zero VUIDs. MRU pressure admission occurred **4,952** times; tracked backing peaked at **12,884,508,672 bytes** under the **12,884,901,888-byte** cap, with zero refusals or fallbacks. Across 45 decode runs, wrapped throughput was **0.28 tokens/s** versus **6.00 tokens/s** native. This confirms the larger model can complete with long-idle MRU admission, but it ran slowly. The native baseline changed from the earlier 13.87 tokens/s run, so these results do not isolate an idle-policy effect; no large-model LRU comparison was run. This is a 16.8107 GB model test, not a 40 GB model or game result. Minimum system `MemAvailable` was **28,404 MiB**, above the **16,384 MiB** process-group safety floor; post-cleanup available RAM was about **42 GiB**.

Reproduce with the same helper options and model loading settings:

```sh
python3 check_vulkan_idle_model.py --binary build/third-party/llama-vulkan-build/bin/llama-completion --model /path/to/27B-Q4_K_M.gguf --tokens 64 --idle-ms 60000 --cold-mib 20000 --range-mib 32 --resident-mib 12288 --resident-after-cold --strict-robustness --max-nodes-per-submit 1 --validate --clean-cache --eviction-policy mru --min-available-mib 16384 --app-arg=--load-mode --app-arg=mmap --output-dir build/vulkan-27b-long-idle-model --timeout 600
```

Evidence: [summary](validation/vulkan-27b-long-idle-result.json), [wrapped diagnostics](validation/vulkan-27b-long-idle-automatic.stderr.txt.gz), [native diagnostics](validation/vulkan-27b-long-idle-native.stderr.txt), [wrapped output](validation/vulkan-27b-long-idle-automatic.stdout.txt), [native output](validation/vulkan-27b-long-idle-native.stdout.txt), and [hot fdinfo](validation/vulkan-27b-long-idle-automatic-hot.fdinfo.txt)/[cold fdinfo](validation/vulkan-27b-long-idle-automatic-cold.fdinfo.txt)/[native hot fdinfo](validation/vulkan-27b-long-idle-native-hot.fdinfo.txt).

### InternLM2.5-20B F16 under Vulkan tracked-residency pressure

The unchanged **39,725,643,136-byte** (39.73 GB, about 37 GiB) InternLM2.5-20B F16 model completed native and wrapped Vulkan inference with identical nonempty output and **49/49** layers offloaded in both runs. The wrapped run passed **28/28** checks, including cold restore, clean-cache reuse/invalidation, range residency, and admission accounting; it emitted no validation diagnostics or VUIDs, restore fallbacks, or admission refusals. After its first complete cold pass, MRU admission kept tracked backing below the **20 GiB** limit (peak **21,471,690,752 bytes**) and recorded **1,212** admissions. Bootstrap was unbounded by this admission limit. The run stored **38,749,798,400** cold logical bytes in **29,595,160,469** bytes (**23.62% saved**). The helper's minimum available memory was **13,110 MiB**, above its **12,288 MiB** safety floor; both processes returned 0.

Across **12 actual decode runs**, wrapped throughput was **0.09 tokens/s** versus **1.44 tokens/s** native. This demonstrates a slow compressed-pressure inference path for this exact model and setup; it is not evidence for a 40 GiB GPU model buffer, game behavior, universal compatibility, or fast/general inference. The cap covers eligible tracked buffer backing only, not total VRAM. The run used the layer from `9f7536957e52dd1c6fd3639fc9b8c71e90a25dfc` with the 32-token helper.

Separately, reusing the compression output scratch vector across chunks in one range removes repeated allocation without changing lossless payloads or queue synchronization. The rebuilt layer and helper passed **93/93** CTest cases in **58.81 seconds**, with zero validation errors or VUIDs. [Full suite log](validation/vulkan-scratch-reuse-93-ctest.txt). This is correctness evidence; no scratch-reuse speed gain was measured. The helper now accepts token budgets of 32–128 and requires positive decode runs and throughput from both processes, so a prompt-only run cannot pass as inference evidence.

Reproduce on a machine with the same model and Vulkan binary:

```sh
python3 check_vulkan_idle_model.py --binary build/third-party/llama-vulkan-build/bin/llama-completion --model build/third-party/models/internlm2_5-20b-chat-fp16.gguf --tokens 32 --idle-ms 60000 --cold-mib 32768 --min-savings-percent 5 --selective-restore --active-eviction --range-mib 128 --resident-mib 20480 --resident-after-cold --strict-robustness --clean-cache --validate --max-nodes-per-submit 1 --eviction-policy mru --min-available-mib 12288 --app-arg=--load-mode --app-arg=mmap --output-dir build/vulkan-internlm-f16-pressure-20g --timeout 600
```

Evidence: [result](validation/vulkan-internlm-f16-pressure-20g-result.json), [native stdout](validation/vulkan-internlm-f16-pressure-20g-native.stdout.txt), [native stderr](validation/vulkan-internlm-f16-pressure-20g-native.stderr.txt), [wrapped stdout](validation/vulkan-internlm-f16-pressure-20g-automatic.stdout.txt), [compressed wrapped stderr](validation/vulkan-internlm-f16-pressure-20g-automatic.stderr.txt.gz), and [native hot](validation/vulkan-internlm-f16-pressure-20g-native-hot.fdinfo.txt)/[wrapped hot](validation/vulkan-internlm-f16-pressure-20g-automatic-hot.fdinfo.txt)/[wrapped cold](validation/vulkan-internlm-f16-pressure-20g-automatic-cold.fdinfo.txt) fdinfo.

### Bounded parallel decode

The last full layer and helper suite before optional GDeflate codec integration passed **101/101** in **68.13 seconds**, with zero Vulkan validation errors or VUIDs: [suite log](validation/vulkan-first-pressure-long-idle-101-ctest.txt) and [diagnostic log](validation/vulkan-first-pressure-long-idle-101-details.txt.gz). These changes add first-submit helper startup, bootstrap-state logging, a positive-uint32 Vulkan idle interval, and explicit cold-budget refusal diagnostics; the compression codec is unchanged. The previous **101/101** run in **66.49 seconds** remains available as [historical suite evidence](validation/vulkan-pipeline-101-ctest.txt). Earlier bootstrap and sparse-app-bind checks are in the [99-test suite](validation/vulkan-bootstrap-bind-99-ctest.txt) and [bootstrap regression](validation/vulkan-cache-bootstrap-focused.txt).

The current focused suite passed **4/4** in **4.31 seconds**: CPU guard and model-budget checks (including a fake-child prompt-readiness gate), plus pipeline and bootstrap GPU checks. [Focused log](validation/vulkan-first-pressure-focused.txt), [diagnostics](validation/vulkan-first-pressure-focused-details.txt.gz).

For ranges larger than 32 MiB, compression decode can use up to three background workers plus the calling thread to fill the already allocated staging buffer, bounded to 128 MiB; snapshot frames remain 32 MiB. Freeze and restore batch up to four frames into one GPU copy/wait. RAW frame decoding, single compressed frames, and 32 MiB ranges remain serial, though the next cold child can still be prefetched. Worker launch failure joins started workers before serial fallback, and a failed decode retains its cold copy. This does not duplicate the output buffers.

The bounded pipeline uses two host staging slots of at most **128 MiB** each and at most four CPU decode workers per restoration. While the GPU copies one admitted cold child, CPU workers can decode the next admitted child into the other slot. It joins workers on completion or error; failure to allocate the second slot falls back to the serial path. Whole-pool and oversized snapshots remain serial, and the pipeline makes no GPU allocations outside resident admission. A **3/3** focused suite passed in **1.86 seconds**: compressed synthetic data, native RAW data, and partial-failure/full-retry cases all exercised two actual prefetches. The failure-state case uses a test-only grace period to make state observable; production idle eviction is unchanged. The test observed **128 MiB resident** with **192 MiB cold**. [Focused summary](validation/vulkan-pipeline-focused.txt), [diagnostics](validation/vulkan-pipeline-focused-details.txt.gz).

### Model-helper pressure from the first submit

The helper's `--pressure-on-first-submit` mode requires `--no-warmup`, `--range-mib`, and `--resident-mib`. It skips warmup for both native and wrapped runs, waits for prompt readiness without requiring a complete cold pass, and arms admission on the first submit. Its cap check covers all admission records and completed-restore states; native allocation or binding may transiently exceed the cap. The helper reports the other observed residency peaks separately, so this is not a global allocation cap.

An unchanged SmolLM2-135M F16 run passed **27/27** checks with identical output, **31/31** layers in both runs, and zero validation errors, restore fallbacks, or admission refusals. Across 12 decode runs, native measured **123.38 tokens/s** and wrapped measured **8.40 tokens/s**. Under the **192 MiB** setting, the highest completed-restore state was **201,326,592 bytes** (192 MiB); the largest observed post-admission peak was **207,421,440 bytes** (about 197.81 MiB) during a later compute allocation. The pressure fdinfo sample was captured during the first post-prompt restore, not after evaluation. This small-model result validates helper behavior, not a global allocation cap or 40 GiB inference. Evidence: [result](validation/vulkan-smollm-first-pressure-scoped-result.json), [native stdout](validation/vulkan-smollm-first-pressure-scoped-native.stdout.txt), [native stderr](validation/vulkan-smollm-first-pressure-scoped-native.stderr.txt.gz), [wrapped stdout](validation/vulkan-smollm-first-pressure-scoped-automatic.stdout.txt), [wrapped stderr](validation/vulkan-smollm-first-pressure-scoped-automatic.stderr.txt.gz), [native hot fdinfo](validation/vulkan-smollm-first-pressure-scoped-native-hot.fdinfo.txt), [wrapped hot fdinfo](validation/vulkan-smollm-first-pressure-scoped-automatic-hot.fdinfo.txt), and [pressure fdinfo](validation/vulkan-smollm-first-pressure-scoped-automatic-pressure.fdinfo.txt).

The production long-idle setting also passed a **27/27** unchanged SmolLM2-135M F16 check at **300,000 ms**, with identical native/wrapped output, **31/31** layers, and zero validation diagnostics, restore fallbacks, or admission refusals. Native measured **112.81 tokens/s** and wrapped **8.59 tokens/s** across 12 decode runs. This verifies the long-idle helper path on the small model; separate full-model evidence follows below. [Result](validation/vulkan-smollm-first-pressure-long-idle-result.json), [native stdout](validation/vulkan-smollm-first-pressure-long-idle-native.stdout.txt), [native stderr](validation/vulkan-smollm-first-pressure-long-idle-native.stderr.txt.gz), [automatic stdout](validation/vulkan-smollm-first-pressure-long-idle-automatic.stdout.txt), [automatic stderr](validation/vulkan-smollm-first-pressure-long-idle-automatic.stderr.txt.gz), [native hot fdinfo](validation/vulkan-smollm-first-pressure-long-idle-native-hot.fdinfo.txt), [automatic hot fdinfo](validation/vulkan-smollm-first-pressure-long-idle-automatic-hot.fdinfo.txt), and [pressure fdinfo](validation/vulkan-smollm-first-pressure-long-idle-automatic-pressure.fdinfo.txt).

The focused CPU and GPU checks passed **5/5** in **6.14 seconds** with zero validation errors or VUIDs. Four synthetic/native Vulkan cases checked full-byte integrity on 320 MiB across 128+128+64 MiB chunks, with minimum savings set to 0% and 100%. A separate CPU probe measured **39.4666 ms** serial versus **13.4031 ms** for three-worker direct staging (**2.94×**) on only three 32 MiB F16 slices, over five repeats; SHA-256 and exact-size checks were outside the timed intervals. Peak resident memory was **236,793,856 bytes**. This CPU-only probe is not GPU or model throughput evidence. [CPU probe result](validation/vulkan-parallel-decode-cpu-probe.json), [reproducible CPU probe](validation/vulkan-parallel-decode-cpu-probe.py), [focused log](validation/vulkan-parallel-decode-focused.txt).

A separate **64 MiB** bootstrap regression passed **1/1**: a pre-arm restore left the allocation resident; after the complete cold pass and arming admission, restore and clean-cache reuse completed, followed by a cold transition. The wrapper explicitly rejects VUIDs, and none occurred. A **20/20** focused suite in **24.73 seconds** also observed app sparse-child bind batches of two and three across queues, with 22 bind/unbind events. These are correctness and batching checks, with no speed claim.

The large-model run below tests **batching only**, before parallel decode was enabled: the same model completed **28/28** checks with identical output, **49/49** layers, 1,234 admissions, and no VUIDs, restore fallbacks, or admission refusals. It retained the same **38,749,798,400** cold logical bytes in **29,595,160,469** bytes. Native measured **1.50 tokens/s** over 12 decode runs; wrapped measured **0.09 tokens/s**. A previous run also measured 0.09 tokens/s, so this does not isolate a speed gain.

A prior attempt with the parallel decoder stopped during automatic warmup: the helper observed **12,240 MiB** available below its **12,288 MiB** safety floor. The native run completed at **1.50 tokens/s** over 12 decode runs, but the wrapped run never became interactive-ready and produced no stdout, cold fdinfo, or decode throughput. Before the guard stopped it, tracked resident backing was **32,574,734,336 bytes**; there were **6,175,064,064 cold logical bytes** (**4,734,386,339 stored**) and **18,042,643,793 clean-cache bytes**. The cache-retention condition was then tightened to keep restored snapshots only when both clean-cache and resident admission are armed, avoiding redundant successful-restore snapshots during bootstrap. The failed attempt remains a guard-stopped warmup, not an inference result. See [failure metadata](validation/vulkan-internlm-f16-parallel-decode-20g-failure-metadata.json), [native stdout](validation/vulkan-internlm-f16-parallel-decode-20g-native.stdout.txt), [native stderr](validation/vulkan-internlm-f16-parallel-decode-20g-native.stderr.txt.gz), [automatic stdout](validation/vulkan-internlm-f16-parallel-decode-20g-automatic.stdout.txt), [automatic stderr](validation/vulkan-internlm-f16-parallel-decode-20g-automatic.stderr.txt.gz), [native hot fdinfo](validation/vulkan-internlm-f16-parallel-decode-20g-native-hot.fdinfo.txt), [automatic hot fdinfo](validation/vulkan-internlm-f16-parallel-decode-20g-automatic-hot.fdinfo.txt), and [helper traceback](validation/vulkan-internlm-f16-parallel-decode-20g-run.log.gz).

The retry completed **28/28** checks with identical **67-byte** native and wrapped output (SHA-256 `8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), **49/49** layers, and return code 0 for both runs. It recorded no VUIDs, restore fallbacks, or admission refusals. Cold storage held **38,749,798,400 logical bytes** in **29,595,160,469 stored bytes**; tracked backing peaked at **21,471,690,752 bytes** under the 20 GiB limit, with **1,199 admissions** and **1,843 compressed / 0 RAW** chunks. Minimum available memory was **13,155 MiB**, above the **12,288 MiB** guard. Across 12 actual decode runs, native measured **1.50 tokens/s** and wrapped measured **0.19 tokens/s**. This is a completed but still slow large-model inference result, about eight times slower than native. It predates the two-slot pipeline above, so it does not measure that pipeline. The prior batch-only run measured 0.09 tokens/s wrapped; because cache retention and decoder behavior both changed, the difference does not isolate a parallel-decode benefit or any other individual change. [Result](validation/vulkan-internlm-f16-bootstrap-cache-20g-result.json), [native stdout](validation/vulkan-internlm-f16-bootstrap-cache-20g-native.stdout.txt), [native stderr](validation/vulkan-internlm-f16-bootstrap-cache-20g-native.stderr.txt.gz), [wrapped stdout](validation/vulkan-internlm-f16-bootstrap-cache-20g-automatic.stdout.txt), [wrapped stderr](validation/vulkan-internlm-f16-bootstrap-cache-20g-automatic.stderr.txt.gz), [native hot fdinfo](validation/vulkan-internlm-f16-bootstrap-cache-20g-native-hot.fdinfo.txt), [wrapped hot fdinfo](validation/vulkan-internlm-f16-bootstrap-cache-20g-automatic-hot.fdinfo.txt), and [wrapped cold fdinfo](validation/vulkan-internlm-f16-bootstrap-cache-20g-automatic-cold.fdinfo.txt).

A later full-model attempt with the two-slot pipeline reached interactive mode and offloaded **49/49** layers, then stopped at the memory guard while waiting for the **first complete cold pass**, before prompt input: available memory was **12,262 MiB**, below the **12,288 MiB** floor. Native completed 12 decode runs at **1.59 tokens/s**. Automatic stdout held only the three-byte interactive prompt (newline, `> `); no cold fdinfo, generated response, or wrapped throughput was produced. Before stopping, the layer recorded **6,862,733,312 resident bytes**, **31,887,065,088 cold logical bytes** (**24,333,045,029 stored**), **47 actual prefetches**, and no validation errors or VUIDs. The benchmark exited 1 and its processes were freed. This records pipeline activity but no generated model output or speed result. [Failure metadata](validation/vulkan-internlm-f16-pipeline-20g-failure-metadata.json), [native stdout](validation/vulkan-internlm-f16-pipeline-20g-native.stdout.txt), [native stderr](validation/vulkan-internlm-f16-pipeline-20g-native.stderr.txt.gz), [automatic stdout](validation/vulkan-internlm-f16-pipeline-20g-automatic.stdout.txt), [automatic stderr](validation/vulkan-internlm-f16-pipeline-20g-automatic.stderr.txt.gz), [native hot fdinfo](validation/vulkan-internlm-f16-pipeline-20g-native-hot.fdinfo.txt), [automatic hot fdinfo](validation/vulkan-internlm-f16-pipeline-20g-automatic-hot.fdinfo.txt), [GPU clock before](validation/vulkan-internlm-f16-pipeline-20g-gpu-clock-before.json), and [GPU clock after](validation/vulkan-internlm-f16-pipeline-20g-gpu-clock-after.json).

A subsequent `--pressure-on-first-submit` attempt avoided waiting for a full cold pass before prompt readiness and completed 12 decode runs, producing the same 67-byte output as native with **49/49** layers offloaded and no VUIDs. However, the result failed the `successful_restore` and `model_buffer_coverage_before_prompt` checks: it recorded **420 failures before prompt input** and **646 by the final state**. Its **0.24 tokens/s** versus **1.50 native** is therefore a failed diagnostic run, not an accepted speed result; the prior **0.19 tokens/s** run remains the last accepted large-model result above. The tracked completed-restore peak was **21,466,906,624 bytes**, within the **20 GiB** admission cap; the helper reports all-state observed peaks separately because the cap check covers admission and completed-restore states, not every transient allocation. Root-cause work on the idle ceiling remains unvalidated. [Result](validation/vulkan-internlm-first-pressure-20g-24g-result.json), [native stdout](validation/vulkan-internlm-first-pressure-20g-24g-native.stdout.txt), [automatic stdout](validation/vulkan-internlm-first-pressure-20g-24g-automatic.stdout.txt), [native stderr](validation/vulkan-internlm-first-pressure-20g-24g-native.stderr.txt.gz), [automatic stderr](validation/vulkan-internlm-first-pressure-20g-24g-automatic.stderr.txt.gz), [native hot fdinfo](validation/vulkan-internlm-first-pressure-20g-24g-native-hot.fdinfo.txt), [automatic hot fdinfo](validation/vulkan-internlm-first-pressure-20g-24g-automatic-hot.fdinfo.txt), [automatic pressure fdinfo](validation/vulkan-internlm-first-pressure-20g-24g-automatic-pressure.fdinfo.txt), and [GPU clock before](validation/vulkan-internlm-first-pressure-20g-24g-gpu-clock-before.json).

The long-idle retry passed **27/27** checks after production idle-ceiling handling was changed to support the **300,000 ms** setting. Native and wrapped output matched byte-for-byte (**67 bytes**, SHA-256 `8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), with **49/49** layers offloaded and no admission refusals, restore fallbacks, or VUIDs. Across 12 decode runs, wrapped throughput was **0.19 tokens/s**, the same as the prior accepted run; this shows no speed gain. Wrapped minimum available memory was **22,552 MiB**. The completed-restore peak was **21,471,363,072 bytes**, below the **20 GiB** tracked-residency limit, while the separate all-state observed peak was **21,569,667,072 bytes** (about **20.089 GiB**) during a transient allocation; the cap checks admission and completed-restore states, not every allocation. Final state retained **17,387,225,088 cold logical bytes** in **13,330,257,346 bytes**, with **2,131 compressed / 0 RAW** chunks. Transfer profiling recorded **34.979 seconds** CPU decode and **41.760 seconds** GPU copy, which overlap and should not be summed. [Result](validation/vulkan-internlm-first-pressure-long-idle-20g-24g-result.json), [native stdout](validation/vulkan-internlm-first-pressure-long-idle-20g-24g-native.stdout.txt), [automatic stdout](validation/vulkan-internlm-first-pressure-long-idle-20g-24g-automatic.stdout.txt), [native stderr](validation/vulkan-internlm-first-pressure-long-idle-20g-24g-native.stderr.txt.gz), [automatic stderr](validation/vulkan-internlm-first-pressure-long-idle-20g-24g-automatic.stderr.txt.gz), [native hot fdinfo](validation/vulkan-internlm-first-pressure-long-idle-20g-24g-native-hot.fdinfo.txt), [automatic hot fdinfo](validation/vulkan-internlm-first-pressure-long-idle-20g-24g-automatic-hot.fdinfo.txt), [automatic pressure fdinfo](validation/vulkan-internlm-first-pressure-long-idle-20g-24g-automatic-pressure.fdinfo.txt), and [GPU clock during native run](validation/vulkan-internlm-first-pressure-long-idle-20g-24g-gpu-clock-during-native.json) and [GPU clock after](validation/vulkan-internlm-first-pressure-long-idle-20g-24g-gpu-clock-after.json).

The wrapped decode time fell from **132,131.67 ms** in the batching-only run to **63,951.55 ms** in this retry (**2.07 times faster** in these single runs). After the first complete cold transition, cumulative copy/wait time was **41.284 seconds** and CPU decode wall time was **34.635 seconds**, against **82.609 seconds** of prompt plus decode time. These cover the full inference phase, not per-token isolated components.

A bounded 128 MiB transfer probe compared direct cached staging with decode into cached staging followed by an extra CPU copy into uncached coherent upload memory. Across five repeats per case, direct staging measured **34.373 ms** end-to-end versus **43.262 ms** for the extra-copy path; GPU copy alone was **18.445 ms** versus **18.701 ms**. Every upload was read back and compared byte-for-byte outside the timing. The extra-copy path was slower, so the layer keeps direct cached staging. This is a component probe on RX 7900 XTX, not model or game throughput; synchronization validation recorded zero VUIDs/errors and one settings-deprecation warning. [Probe source](validation/vulkan-staging-copy-probe.cpp), [result](validation/vulkan-staging-copy-probe-result.json), [raw log](validation/vulkan-staging-copy-probe.txt).

The successful retry used the same 32-token workload options in a separate output directory to preserve the guard-stopped attempt:

```sh
python3 check_vulkan_idle_model.py --binary build/third-party/llama-vulkan-build/bin/llama-completion --model build/third-party/models/internlm2_5-20b-chat-fp16.gguf --tokens 32 --idle-ms 60000 --cold-mib 32768 --min-savings-percent 5 --selective-restore --active-eviction --range-mib 128 --resident-mib 20480 --resident-after-cold --strict-robustness --clean-cache --validate --max-nodes-per-submit 1 --eviction-policy mru --min-available-mib 12288 --app-arg=--load-mode --app-arg=mmap --output-dir build/vulkan-internlm-f16-bootstrap-cache-20g --timeout 600
```

Earlier batching-only evidence: [result](validation/vulkan-internlm-f16-transfer-batch-20g-result.json), [native stdout](validation/vulkan-internlm-f16-transfer-batch-20g-native.stdout.txt), [native stderr](validation/vulkan-internlm-f16-transfer-batch-20g-native.stderr.txt), [wrapped stdout](validation/vulkan-internlm-f16-transfer-batch-20g-automatic.stdout.txt), [compressed wrapped stderr](validation/vulkan-internlm-f16-transfer-batch-20g-automatic.stderr.txt.gz), and [native hot](validation/vulkan-internlm-f16-transfer-batch-20g-native-hot.fdinfo.txt)/[wrapped hot](validation/vulkan-internlm-f16-transfer-batch-20g-automatic-hot.fdinfo.txt)/[wrapped cold](validation/vulkan-internlm-f16-transfer-batch-20g-automatic-cold.fdinfo.txt) fdinfo. The preceding suite before bootstrap-cache and sparse-bind updates passed **98/98** in **65.73 seconds**: [suite log](validation/vulkan-parallel-decode-98-ctest.txt).

## Vulkan compression-savings cutoff

`--vulkan-min-savings-percent N` accepts **0–100** and requires automatic Vulkan snapshots. The default `0` preserves the existing behavior of keeping a compressed snapshot whenever compression saves space. When the saved percentage falls below `N`, zVram stores the snapshot losslessly as RAW instead. At `100`, it skips Zstd compression and decompression entirely and always retains RAW backing. This does not quantize or discard data. RAW snapshots use the same bounded cold and clean-cache quota, so they need more RAM and can be refused if the stored data will not fit. Accepted writes still invalidate affected clean-cache snapshots.

The model helper exposes the cutoff as `--min-savings-percent N`. CPU checks in `compression_policy.hpp` passed the exact ceiling and `uint64_t` boundary cases. The focused gate passed **6/6** in **2.19 seconds**: two CPU cases and four synthetic/native Vulkan cases. Across 30 fresh GPU snapshots, all RAW entries had stored size equal to logical size, compressed bytes were zero, full-byte checks passed, and accepted writes invalidated cached copies as expected. The full CTest suite passed **92/92** in **58.81 seconds**, with zero validation errors or VUIDs. See the [focused checks](validation/vulkan-compression-cutoff-focused.txt) and [full-suite log](validation/vulkan-compression-cutoff-92-ctest.txt). These are correctness and accounting results, not a throughput benchmark or evidence for 40 GiB or game workloads.

### Matched 135M model run

The unchanged SmolLM2-135M F16 pair used the same binary, model, 1,000 ms idle interval, 32 MiB ranges, 192 MiB resident limit, MRU, clean cache, strict robustness, one node per submit, and validation; only the minimum savings cutoff changed. Both runs produced the same stdout (SHA-256 `40af802dfb2b6042c7e4ec011b0d82ed8057f634f9c724e68f23ca1917fef76b`), offloaded **31/31** layers, and had zero VUIDs. At **0%**, checks passed **27/27** with 15 compressed and 0 RAW fresh snapshots. At **100%**, checks passed **28/28** with 0 compressed and 32 RAW fresh snapshots. Each measured 44 decode runs: **8.26 tokens/s** at 0% and **17.53 tokens/s** at 100% in this single pair. Native references measured **121.84** and **125.63 tokens/s**, respectively, so neither mode approached native throughput. Stored cold backing increased from **207,000,847 bytes** (about **197.4 MiB**) at 0% to **308,084,736 bytes** (**293.8125 MiB**) at 100%; tracked peaks were **185.875 MiB** and **186.5625 MiB**, both below 192 MiB. This one small-model pair is a workload-specific observation, not a general speed claim or evidence for large models, 40 GiB, or games.

See the [0% summary](validation/vulkan-cutoff-0-small-model-result.json), [100% summary](validation/vulkan-cutoff-100-small-model-result.json), and `validation/vulkan-cutoff-{0,100}-small-model-*` output and fdinfo captures.

### Matched 27B run at a 5% cutoff

The unchanged **16.8107 GB Q4** model also passed **27/27** checks with a 5% minimum-savings cutoff, 60,000 ms idle interval, 20,000 MiB cold quota, 32 MiB ranges, 12 GiB resident cap, MRU, clean cache, strict robustness, and one node per submit. It matched the previous 27B native/wrapped stdout (SHA-256 `61c5375468005f6c1436b328f039ef4a5fbb6b73687461b4cfe4d7f5e6d20a5b`), offloaded **66/66** layers, and had zero VUIDs, admission refusals, or restore fallbacks. The layer recorded **4,891 pressure admissions** and reached the exact **12 GiB** tracked cap. The full job recorded **1,774 compressed** and **1,715 RAW** fresh snapshot chunks; before input, cold storage held **16,151,085,056 logical bytes** in **15,826,134,806 stored bytes** (about **2.01%** saved). Minimum system `MemAvailable` was **28,056 MiB**, above the **16,384 MiB** guard. Across 45 decode runs, wrapped throughput was **0.41 tokens/s**, versus **13.30 native**. The prior no-cutoff run measured **0.28 wrapped** and **6.00 native**; because the native baseline changed, the 47% wrapped difference cannot be attributed to the cutoff alone. This confirms mixed encoding and pressure admission on this model, but remains very slow and is not evidence for a 40 GiB model, game behavior, or a general speed benefit.

Reproduce with the model helper and the same loading options:

```sh
python3 check_vulkan_idle_model.py --binary build/third-party/llama-vulkan-build/bin/llama-completion --model /path/to/27B-Q4_K_M.gguf --tokens 64 --idle-ms 60000 --cold-mib 20000 --range-mib 32 --resident-mib 12288 --resident-after-cold --strict-robustness --max-nodes-per-submit 1 --validate --clean-cache --eviction-policy mru --min-savings-percent 5 --min-available-mib 16384 --app-arg=--load-mode --app-arg=mmap --output-dir build/vulkan-cutoff-5-27b-model --timeout 600
```

See the [summary](validation/vulkan-cutoff-5-27b-model-result.json), [wrapped diagnostics](validation/vulkan-cutoff-5-27b-model-automatic.stderr.txt.gz), [native diagnostics](validation/vulkan-cutoff-5-27b-model-native.stderr.txt), [wrapped output](validation/vulkan-cutoff-5-27b-model-automatic.stdout.txt), [native output](validation/vulkan-cutoff-5-27b-model-native.stdout.txt), and [wrapped hot fdinfo](validation/vulkan-cutoff-5-27b-model-automatic-hot.fdinfo.txt)/[wrapped cold fdinfo](validation/vulkan-cutoff-5-27b-model-automatic-cold.fdinfo.txt)/[native hot fdinfo](validation/vulkan-cutoff-5-27b-model-native-hot.fdinfo.txt).

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


## InternLM lazy loading and swap-growth guard

A new loading attempt used the 39,725,643,136-byte InternLM2.5-20B F16 model, 32 MiB ranges, an immediate 18 GiB tracked residency limit, lazy backing, asynchronous Zstd compression and a 4 GiB budget headroom estimate. The native direct-I/O (`--load-mode dio`) reference completed with 49/49 layers offloaded and a 36,798.77 MiB Vulkan model buffer: 12 decode runs took 7,533.88 ms, or **1.59 tokens/s**. This single run does not control desktop activity or GPU clocks.

The wrapped run failed during loading before any prompt or decoded output. Pristine model chunks started without resident backing, but an unknown command-buffer access forced conservative whole-model admission and exceeded the effective resident limit. A follow-up loading probe narrowed the reason to `untracked-command`. There is **no wrapped token rate** for this lazy-loading configuration. Command diagnostics now preserve named unsupported commands; CPU checks verify unchanged conservative fallback decisions. Core `vkCmdSetEvent` and `vkCmdResetEvent`, plus debug-utils begin/end/insert labels, no longer invalidate buffer tracking. Event commands change synchronization state and debug labels annotate commands; neither directly accesses application buffer contents. Wait-side buffer dependencies remain tracked. See the primary [event specification](https://github.khronos.org/Vulkan-Site/refpages/latest/refpages/source/vkCmdSetEvent.html) and [debug-label specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBeginDebugUtilsLabelEXT.html). A CPU production-hook regression forwards all five event/debug commands through fake driver dispatch and verifies that `commandMemories` still selects exactly one recorded sparse child. An unsupported command still forces unknown-access fallback with its specific name. A subsequent full lazy-backing model run completed with matching native output; see the newer 16 GiB evidence below.

The earlier mmap native reference was manually stopped during loading after logical swap usage rose sharply. The helper now accepts optional `--max-swap-growth-mib N`, a positive uint64 threshold measured against each run's initial system-wide used swap. It samples during loading, generation and immediately before sending the prompt, and terminates only its owned child process group if the threshold is exceeded. CPU tests cover loading and generation aborts, retained logs, and survival of an unrelated process. Every run now retains `<label>.resources.json`, including on guard abort or failed loading, with observed RAM minimum, logical swap baseline/peak, prompt state, child return code and configured thresholds. These are sampled observations rather than continuous peaks. This guard is off by default and is not a hard allocation cap or a measure of physical zram consumption; compressed swap's logical stored bytes can exceed the RAM it uses. No zram or swappiness settings were changed.

[Loading evidence and native timing](validation/internlm-lazy-loading/summary.json). The direct-I/O run used a 16 GiB available-memory floor and a 4 GiB logical swap-growth threshold. The short diagnostic used the same memory floor and a 1 GiB swap-growth threshold; it exited before input. GPU retests were deferred following a reported VRChat lag spike; causality is unconfirmed.


## Batched restore mapping transition

After a compressed child's decode or staging copy completes, restoration now retires the private sparse view and binds the application aliases in one sparse-bind batch. This replaces the separate view-unbind and app-bind operations with one completion wait. The decoder fence or copy completion still occurs first; layer locks and subsequent application-queue restore visibility remain unchanged. Pristine lazy children keep their existing admission path.

A failed merged transition leaves the GPU gate in an error state and retains the backing, private-view state and cold metadata; it does not assume which bindings completed or free potentially referenced memory. The batch operates on distinct buffer ranges; Vulkan permits multiple sparse buffer binding operations in a batch and requires a resource range not be bound more than once in that batch. See [the primary sparse-binding specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueBindSparse.html).

CPU production-path regression results are recorded in [restore-transition-cpu.json](validation/restore-transition-cpu.json). The subsequent full 16 GiB Zstd run below exercised restored mappings and matched native output. The subsequent completed GPU GDeflate and BP16 runs below also matched native output through this transition. This reduces required mapping operations; an isolated token-throughput benefit has not been measured.


## Full InternLM lazy backing under 16 GiB

The unchanged 39,725,643,136-byte InternLM2.5-20B F16 GGUF completed inference with 49/49 layers offloaded, a 36,798.77 MiB GPU model buffer, lazy backing, automatic async Zstd snapshots, selective restore, active LRU eviction, 32 MiB ranges, a 16 GiB tracked cap and 4 GiB driver-budget headroom. This run used commit `250acc7943f011245275c8c10c4dc9ecacc6484c`; source and runtime binary hashes are retained. The 67-byte output matched a subsequent native direct-I/O run byte for byte, SHA-256 `8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`.

Twelve actual decode runs took **361,534.38 ms**, calculated **0.03319 tokens/s**, versus **1.69 tokens/s** for the fresh native reference (7,088.30 ms). Prompt evaluation took 67,111.33 ms versus 1,339.69 ms native. The `--predict 32` interactive budget included 20 input tokens; this was 12 generated tokens, not 32. Desktop activity and GPU clocks were not locked, and this is one short inference comparison. Native Vulkan already spills into driver-managed GTT; native is not a VRAM-only reference.

Tracked backing peaked at **17,177,772,032 bytes**, below the 17,179,869,184-byte cap throughout recorded state events. Driver observations also include staging and other allocations outside that cap. The final state recorded 16,916 freezes, 17,433 restores and zero snapshot failures. CPU decoding processed 539,687,518,208 bytes with 238,657,956,509 ns accumulated decode time; copy, prefetch and wait counters can overlap, so they must not be summed as independent elapsed time. This Zstd path works but is extremely slow. GPU GDeflate plus MRU was evaluated later; see the completed run below.

Available system RAM stayed at or above 18,107 MiB in sampled observations. Logical used swap grew by 6,450 MiB; the configured guard was 16,384 MiB growth with a 16,384 MiB available-RAM floor. These are system-wide samples, not continuous peaks or physical zram consumption. No tracking fallback or admission-refusal diagnostic occurred, and the journal check found no new GPU fault/reset during the run. This establishes this model's compressed-pressure inference, not game compatibility, a 40 GiB GPU model buffer, or universal paging.

[Compact result and native reference](validation/internlm-lazy-16g/summary.json), [exact command](validation/internlm-lazy-16g/command.json), [runtime hashes](validation/internlm-lazy-16g/runtime-binary-sha256.json), [full wrapped result](validation/internlm-lazy-16g/result.json.gz), [wrapped log](validation/internlm-lazy-16g/automatic.stderr.txt.gz), [native log](validation/internlm-lazy-16g/native.stderr.txt.gz), and [resource samples](validation/internlm-lazy-16g/automatic.resources.json.gz).

## Bounded admission retry for changing driver estimates

A separate 18 GiB loading attempt observed the native driver estimate fall about 30 MiB between admission and the next child restore. The first selected child had restored; a subsequent 32 MiB child then failed the fresh budget preflight. Separately, an 18 GiB limit with 4 GiB headroom left only 24 MiB after the reserve—less than one 32 MiB child—so that abort was a structural sizing shortfall, not evidence of a budget-estimate race. The queue path now permits one re-admission attempt only for an explicitly identified budget-preflight refusal before application work is submitted. It keeps already restored selected children, evicts other eligible children if needed, then retries restoration once. A persistent refusal remains out-of-memory. Allocation, sparse-binding, copy/decode and sticky GPU-gate errors are not retried through this path.

CPU production-path fixtures cover successful retry, persistent refusal, allocation failure, sticky sparse failure, and a partial restore where the reduced budget stays low and an unselected resident child must be evicted. Both default and optional GDeflate CPU suites were rebuilt for this change and passed 10/10 and 12/12 respectively. [Fixture results and source hashes](validation/budget-preflight-retry-cpu.json). This is not a guarantee against driver-budget changes or global contention.


## Dispatch-scoped compute write attribution

A command buffer can bind several compute pipelines whose storage binding numbers overlap. Previously every recorded pipeline's write proof was applied to every bound descriptor set; a writer of output binding 0 could therefore invalidate a different set's read-only weight binding 0. The tracker now records the active compute pipeline and bound set handles at each direct, base or indirect dispatch, and attributes writes using scopes that reference the same set index and handle. Descriptor contents remain resolved at submission, preserving supported live updates. The conservative accumulated resource union remains unchanged, and any referenced range written by one scope is still writable.

Command buffers with non-compute pipeline or descriptor bindings retain the prior conservative attribution. Sets with no matching dispatch scope also retain the prior fallback. Unknown shaders, unsupported commands, descriptor/layout errors and dynamic/wide range handling remain conservative. CPU fixtures cover distinct read-only weight and writable output buffers, read/write use of the same buffer, rebinding, live updates, indirect arguments, secondary dispatches and mixed graphics/compute.

Fresh Release compilation also exposed alias-unsafe Vulkan `pNext` common-header traversal under C++20 optimization. Header reads now use `memcpy` before matching `sType` and accessing the concrete structure. Optimized C++17, C++20 and compiler-default builds pass without disabling strict aliasing or optimization. The production default/optional CPU suites passed **11/11** and **13/13**; **3/3** GPU buffer tests passed with full-byte application comparison, observed GPU decompression, zero fallback and no validation diagnostics. [Results, source hashes and check logs](validation/dispatch-scoped-writes/summary.json).

The GPU decoder also avoids zeroing encoded bytes immediately before overwriting them, clearing only the final 0–3 padding bytes. At unchanged input capacity it updates only output descriptor binding 2; input/control/scratch descriptors are refreshed when buffers grow. Output clearing, synchronization, validation, error readback and unsafe-fence lifetime handling remain unchanged. These reduce redundant host work; no isolated inference speed gain has been established.

## GPU full-model attempt stopped by RAM guard

A subsequent full-model GPU GDeflate/MRU attempt used 16 GiB tracked residency, 32 MiB ranges, four CPU encoding workers, a 24 GiB cold/cache quota and the budget-preflight retry. It reached inference but the owned helper stopped it after **322.64 seconds** when sampled `MemAvailable` fell to **16,336 MiB**, below its **16,384 MiB** floor. Logical swap grew by **6,133 MiB**. It produced no completed output comparison or accepted token rate.

The last state reported **10,315 GPU restores / 340,599,767,040 bytes**, zero GPU fallback and zero snapshot failures, with **142,752,635,735 ns** accumulated GPU-restore host time. It retained 17,047,056,624 cold bytes and 8,709,529,388 clean-cache bytes; these are not all process/system RAM costs. This is partial restore evidence, not full-model correctness or a throughput result. A later full-model trial completed under the configuration below. [Guard result and counters](validation/internlm-gpu-16g-ram-stop/summary.json), [wrapped log](validation/internlm-gpu-16g-ram-stop/automatic.stderr.txt.gz), [resource samples](validation/internlm-gpu-16g-ram-stop/automatic.resources.json.gz), and [source/runtime hashes](validation/internlm-gpu-16g-ram-stop/runtime-binary-sha256.json).

## Full InternLM GPU GDeflate with MRU under 18 GiB

The unchanged **39,725,643,136-byte** InternLM2.5-20B F16 GGUF completed 12 actual decode runs with **49/49** layers offloaded and output matching the native run byte for byte (SHA-256 `8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`). This run used GPU GDeflate restore, 32 CPU encoding workers, MRU eviction, 32 MiB ranges, lazy backing, and an 18 GiB tracked-residency limit with a 3 GiB native-budget reserve. It recorded **8,883 GPU decode calls / 294,139,330,560 bytes**, zero GPU fallbacks, and zero snapshot failures.

Wrapped decode throughput was **0.09 tokens/s**. A separate native reference configured with `GGML_VK_MAX_NODES_PER_SUBMIT=4` measured **1.11 tokens/s**; it was not a simultaneous paired run. The earlier 16 GiB Zstd run measured **0.03319 tokens/s versus 1.69 native**, but its configuration and native reference differ, so these results do not form a controlled codec or scheduling comparison. The completed run proves this configuration can finish this model with matching output; it remains very slow and does not establish a general speed benefit, game behavior, or universal compatibility.

A separate paired GPU component test decoded the same 32 MiB model slice exactly with zero validation diagnostics: BP16's median decoder time was **0.11784 ms** versus GDeflate's **5.77616 ms** (about **49×** for decoder-only timing). BP16 stored **29,202,816 bytes (87.0% of raw)**, while GDeflate stored **27,219,160 bytes (81.1%)**. This bounded component result does not predict full restore or inference throughput. [Paired GPU result](validation/bp16-component/paired-gpu-result.json), [BP16 log](validation/bp16-component/bp16-paired.log), and [GDeflate log](validation/bp16-component/gdeflate-paired.log).

[Run summary](validation/internlm-gpu-18g/summary.json), [exact command](validation/internlm-gpu-18g/command.json), [runtime hashes](validation/internlm-gpu-18g/runtime-binary-sha256.json), [source commit](validation/internlm-gpu-18g/source-commit.txt), and [full run result](validation/internlm-gpu-18g/result.json.gz).

## Full InternLM GPU BP16 under 18 GiB

The same 39.73 GB F16 model completed all 49/49 layers with BP16 GPU restore,
MRU, one encoding worker, 32 MiB ranges, lazy backing, an 18 GiB tracked cap
and a 3 GiB budget reserve. Twelve actual decode runs took 85,806.84 ms: **0.14
tokens/s**, versus the earlier GDeflate trial's 0.09. The exact throughput ratio
is about 1.53, far below the decoder component's 49x difference. These sequential
trials did not control desktop activity or GPU clocks. The separate native
nodes=4 reference was 1.11 tokens/s.

Output matched native byte for byte (SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`).
There were 8,797 GPU decode calls / 290,958,082,048 bytes, zero GPU fallback,
zero CPU decode bytes, and zero snapshot failures. Minimum available RAM was
21,065 MiB; logical swap growth was 4,802 MiB. Tracked residency is not a cap
on all application allocations or all physical VRAM usage.

[Summary](validation/internlm-bp16-18g/summary.json),
[command](validation/internlm-bp16-18g/command.json),
[binary and source hashes](validation/internlm-bp16-18g/runtime-binary-sha256.json),
and [full stderr](validation/internlm-bp16-18g/automatic.stderr.txt.gz).

### Eight-worker sequential repeat

A later run used the same full F16 model and 18 GiB tracked cap, with BP16 GPU
restore, MRU, lazy backing, a 3 GiB budget reserve, and eight CPU encoding
workers. All **49/49** layers were offloaded; output matched the native run
byte for byte (SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`). The
12 decode runs took **65,471.04 ms**, giving the recorded rate
`12 * 1000 / 65471.04 = 0.18328 tokens/s`. This remains slow full-model
inference. It followed the one-worker 0.14 trial sequentially, without
controlled GPU clocks or desktop activity, so it does not establish a worker
speedup. The earlier one-worker result above is retained as its own run.

The last snapshot state recorded **10,016 restores**, **9,436 freezes**, and
zero snapshot failures. The cumulative GPU restore profile recorded **8,848
calls / 293,193,777,152 bytes**, **62.968 s** host elapsed inside GPU
restoration calls, and zero GPU fallbacks. Host profiling recorded 8,848 calls, **1.308 s** validation,
**13.949 s** input preparation, and **47.590 s** submit/wait. The run sampled a
minimum of **22,847 MiB** available RAM and **2,229 MiB** swap growth. The
Decoder-only component timings do not measure model throughput or establish a
full-model speedup.

[Summary](validation/internlm-bp16-workers8-18g/summary.json),
[command](validation/internlm-bp16-workers8-18g/command.json),
[runtime binary hashes](validation/internlm-bp16-workers8-18g/runtime-binary-sha256.json),
[source commit](validation/internlm-bp16-workers8-18g/source-commit.txt),
[full result](validation/internlm-bp16-workers8-18g/result.json.gz), and
[full stderr](validation/internlm-bp16-workers8-18g/automatic.stderr.txt.gz).

### Nineteen-gibibyte sequential profile run

A subsequent run used the same 39,725,643,136-byte InternLM2.5-20B F16 model,
BP16 GPU restore, MRU, lazy backing, async compression, and eight CPU encoding
workers, with a **19 GiB** tracked cap and **2.5 GiB** headroom reserve. All
**49/49** layers were offloaded, output matched the earlier BP16 runs
byte-for-byte (SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), and the
run had zero diagnostics or GPU fallback. Twelve decode runs took **61,489.52
ms**: `12 * 1000 / 61489.52 = 0.195155 tokens/s` (reported as **0.20**).
This is a sequential result at a different cap, not a controlled codec or
worker-count comparison; clocks and desktop activity were uncontrolled.

The final device profile had **8,341 samples**: **42.281 s transfer**,
**1.092 s compute**, and **0.061 s finish**. Host profiling recorded **1.499 s
validation**, **13.494 s input preparation**, and **45.005 s submit/wait**.
Backing profiling recorded **0.164 s allocation**, **0.103 s free**, and
**0.198 s sparse binding** cumulatively. These counters point to the transfer
stage as the largest profiled device component; they do not establish an
isolated cause or model speedup. The final snapshot had **9,509 restores**,
**8,896 freezes**, and zero failures. Minimum available RAM was **21,751 MiB**;
swap grew by **2,514 MiB**.

The run used source commit `a02c7e635cf07f854586998fcc2ceb3510dbbad8` with
working-tree profiling additions. The compiled profiling source revision was
`09fcc57`, verified against the recorded source hashes.

[Summary](validation/internlm-bp16-workers8-19g/summary.json),
[command](validation/internlm-bp16-workers8-19g/command.json),
[run harness](validation/internlm-bp16-workers8-19g/run.py),
[controller result](validation/internlm-bp16-workers8-19g/controller.log),
[runtime binary hashes](validation/internlm-bp16-workers8-19g/runtime-binary-sha256.json),
[source commit](validation/internlm-bp16-workers8-19g/source-commit.txt),
[full result](validation/internlm-bp16-workers8-19g/result.json.gz), and
[full stderr](validation/internlm-bp16-workers8-19g/automatic.stderr.txt.gz).

### Nineteen-gibibyte no-prefill follow-up

The next run kept the same **19 GiB** tracked cap, **2.5 GiB** headroom reserve,
model, eight BP16 encoding workers, and restore settings. It completed 12 decode
runs in **47,571.45 ms**, or `12 * 1000 / 47571.45 = 0.252252 tokens/s`
(reported as **0.25**). All **49/49** layers were offloaded, output matched the
previous runs byte-for-byte (SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), and
there were zero diagnostics or GPU fallbacks.

This build skipped the redundant full-range output fill for canonical BP16
frames; the exact four-line source patch and matching source hash are archived.
The device profile recorded **8,348 samples**: **22.252 s transfer**, **1.085 s
compute**, and **0.064 s finish**. The prior same-cap run recorded 42.281 s
transfer and 1.092 s compute. This is consistent with removing a transfer
operation, but the trials were sequential with uncontrolled GPU clocks and
background activity, so the full elapsed-time change cannot be attributed only
to that edit. The direct-host-input prototype was not part of this run and has
no performance result here.

Host profiling recorded **1.538 s validation**, **13.699 s input preparation**,
and **24.987 s submit/wait**. Backing profiling recorded **0.142 s allocation**,
**0.097 s free**, and **0.198 s sparse binding** cumulatively. The final snapshot
had **9,516 restores**, **8,903 freezes**, and zero failures. Minimum available
RAM was **16,882 MiB** against a **16,384 MiB** guard; swap grew by **3,679 MiB**.

[Summary](validation/internlm-bp16-workers8-19g-no-prefill/summary.json),
[command](validation/internlm-bp16-workers8-19g-no-prefill/command.json),
[run harness](validation/internlm-bp16-workers8-19g-no-prefill/run.py),
[controller result](validation/internlm-bp16-workers8-19g-no-prefill/controller.log),
[runtime hashes](validation/internlm-bp16-workers8-19g-no-prefill/runtime-binary-sha256.json),
[source note](validation/internlm-bp16-workers8-19g-no-prefill/source-commit.txt),
[output-fill patch](validation/internlm-bp16-workers8-19g-no-prefill/bp16-output-prefill.patch),
[full result](validation/internlm-bp16-workers8-19g-no-prefill/result.json.gz), and
[full stderr](validation/internlm-bp16-workers8-19g-no-prefill/automatic.stderr.txt.gz).

### Interrupted BP16 direct host-input trial

An opt-in direct coherent host-input attempt ended at
`vk::Queue::submit: ErrorOutOfDeviceMemory` after **353 GPU restores**, with
zero GPU fallback but **10 snapshot failures**. It did not complete inference
and has no accepted rate. The recorded process status is **-9**; PID 1756378 was
later killed while blocked in core-dump handling under disk pressure, so that
status is not the original crash signal.

During the attempt, an external Ollama 9B model reload was observed at 6.1 GB
VRAM; native budget fell to 15.7 GB and effective budget to 13.0 GB from the
planned 19 GB. No GPU reset was seen in the journal. These concurrent changes
do not prove host-input mode caused the submit failure. See the
[incident summary](validation/internlm-bp16-workers8-19g-host-input-interrupted/summary.json),
[incident notes](validation/internlm-bp16-workers8-19g-host-input-interrupted/incident-notes.md),
[command](validation/internlm-bp16-workers8-19g-host-input-interrupted/command.json),
and [full stderr](validation/internlm-bp16-workers8-19g-host-input-interrupted/automatic.stderr.txt.gz).

### BP16 presentation fixture: host and device input

Both BP16 GPU input modes passed the hidden Gamescope presentation fixture for
**3 frames** each, with sync validation enabled, native graphics allocation,
lazy backing, async snapshots, full 32 MiB buffer checks, and exact pixel
readback. Each recorded zero BP16 GPU fallbacks. This exercises the presentation
and restore fixture only; it is not a game compatibility or performance result,
and it does not use the GDeflate shader. The run used source `cefb8fc` and
harness `9b05065`.

[Host-input result](validation/bp16-presentation/host-input/result.json) ·
[host-input log](validation/bp16-presentation/host-input/run.log.gz) ·
[device-input result](validation/bp16-presentation/device-input/result.json) ·
[device-input log](validation/bp16-presentation/device-input/run.log.gz).


### Nineteen-gibibyte direct host-input run

A clean full-model run enabled `ZVRAM_VULKAN_BP16_HOST_INPUT=1`, using BP16 GPU
restore with direct coherent host input, eight encoding workers, MRU, lazy
backing, and async compression. Under the same **19 GiB** tracked cap and
**2.5 GiB** headroom reserve, it completed 12 decode runs in **30,595.01 ms**:
`12 * 1000 / 30595.01 = 0.39222 tokens/s` (reported as **0.39**). All
**49/49** layers were offloaded, output matched the prior BP16 runs byte for
byte (SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), and
there were zero diagnostics or GPU fallbacks.

This is a sequential result using a different input path. The preceding **0.25**
device-input result and the fresh native reference below are not paired
comparisons; GPU clocks and background activity were uncontrolled. Device profiling recorded
**8,341 samples**: **0.041 s transfer**, **7.772 s compute**, and **0.076 s
finish**. The compute phase includes PCIe reads from host memory, so it is not
shader-only timing. Host profiling recorded **1.370 s validation**, **13.352 s
input preparation**, and **9.502 s submit/wait**.

The resource guard detected no Ollama GPU process during the run and enforced a
**16,384 MiB** available-memory floor; minimum available RAM was **20,599 MiB**
and swap grew by **1,910 MiB**. The child core-dump limit was zero. Source commit
was `50fc8b9`. This result does not establish a general application or gaming
speedup.

[Summary](validation/internlm-bp16-workers8-19g-host-input-clean/summary.json),
[command](validation/internlm-bp16-workers8-19g-host-input-clean/command.json),
[run harness](validation/internlm-bp16-workers8-19g-host-input-clean/run.py),
[resource samples](validation/internlm-bp16-workers8-19g-host-input-clean/automatic.resources.json.gz),
[runtime hashes](validation/internlm-bp16-workers8-19g-host-input-clean/runtime-binary-sha256.json),
[full result](validation/internlm-bp16-workers8-19g-host-input-clean/result.json.gz), and
[full stderr](validation/internlm-bp16-workers8-19g-host-input-clean/automatic.stderr.txt.gz).

### Fresh native nodes=4 reference

A fresh native run of the same 39,725,643,136-byte model completed 12 decode
runs in **7,060.86 ms** at **1.6995 tokens/s** (reported as **1.70**), with
**49/49** layers and the same output SHA-256 as the wrapped BP16 runs. The
numerical comparison with the preceding **0.39222 tokens/s** host-input result
is about **23.1%** (or **4.33x slower** for BP16), but the runs were sequential,
GPU clocks were unlocked, and this is not a controlled matched comparison.

Native placement used driver VRAM and GTT spillover: the run recorded
23,155,245,056 resident VRAM bytes and 16,757,374,976 resident GTT bytes for a
36,798.77 MiB model buffer. This differs from zVram's tracked-residency path.
Minimum available RAM was **29,286 MiB**, swap grew by **460 MiB**, and the
Ollama GPU guard detected no process.

[Summary](validation/internlm-native-nodes4-clean/summary.json),
[command](validation/internlm-native-nodes4-clean/command.json),
[resource samples](validation/internlm-native-nodes4-clean/native.resources.json.gz),
[runtime hashes](validation/internlm-native-nodes4-clean/runtime-binary-sha256.json),
[full result](validation/internlm-native-nodes4-clean/native-result.json.gz), and
[full stderr](validation/internlm-native-nodes4-clean/native.stderr.txt.gz).

### Fresh virtual-native-spill reference

A separate run used zVram's **96 GiB virtual heap** without automatic snapshots
or compression, allowing the native Vulkan driver to place backing across VRAM
and GTT. It completed 12 decode runs in **7,061.47 ms**:
`12 * 1000 / 7061.47 = 1.69936 tokens/s` (reported as **1.70**). All **49/49**
layers were offloaded and output matched the other runs (SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`). Minimum
available RAM was **27,289 MiB**; swap grew by **823 MiB**; the Ollama GPU guard
detected no process.

The rate is nearly identical to the fresh native **1.6995 tokens/s** reference.
Compared numerically with compressed BP16 at **0.40653 tokens/s**, it is about
**4.18x higher**, but these were sequential runs with unlocked clocks and
different backing paths. This is not a controlled codec comparison, proof of
compression behavior, or universal RAM/driver-placement guarantee. This mode
recorded no cold snapshots.

Source commit was `aec5330`. [Summary](validation/internlm-virtual-native-spill-clean/summary.json),
[command](validation/internlm-virtual-native-spill-clean/command.json),
[resource samples](validation/internlm-virtual-native-spill-clean/virtual.resources.json.gz),
[runtime hashes](validation/internlm-virtual-native-spill-clean/runtime-binary-sha256.json),
[full result](validation/internlm-virtual-native-spill-clean/result.json.gz), and
[full stderr](validation/internlm-virtual-native-spill-clean/virtual.stderr.txt.gz).

### Nineteen-gibibyte direct host-input run: 32 workers

This later run kept the same model, **19 GiB** tracked cap, **2.5 GiB** reserve,
BP16 direct coherent host input, and restore settings, with CPU encoding set to
32 workers. It completed 12 decode runs in **29,518.12 ms**:
`12 * 1000 / 29518.12 = 0.40653 tokens/s` (reported as **0.41**). All
**49/49** layers were offloaded, output matched the other BP16 runs byte for
byte (SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), and
there were zero diagnostics or GPU fallbacks.

The layer binary hash matches the eight-worker direct-host run, but the
collector source changed. The observed rate is about **3.6%** above the earlier
eight-worker result, which is not an isolated worker-count comparison. This
does not make 32 workers mandatory or change the default of one. The separate
fresh native reference measured **1.6995 tokens/s**, about **4.18x** the BP16
rate; that sequential comparison used different VRAM/GTT placement and
uncontrolled clocks, so it is not a controlled benchmark.

Device profiling recorded **8,405 samples**: **0.042 s transfer**, **7.765 s
compute** (including PCIe reads from coherent host memory), and **0.077 s
finish**. Host profiling recorded **1.537 s validation**, **13.773 s input
preparation**, and **9.482 s submit/wait**. Backing allocation, free, and sparse
binding summed to **0.210 s**, **0.127 s**, and **0.241 s** respectively. The
final snapshot showed **9,573 restores**, **8,962 freezes**, and zero failures.
Minimum available RAM was **22,359 MiB** against a **16,384 MiB** floor; swap
grew by **1,714 MiB**. The Ollama GPU guard detected no process.

Source commit was `df22d6d`; it includes a collector fix. The run used
`ZVRAM_VULKAN_BP16_HOST_INPUT=1` with the same guarded child-core and memory
limits archived alongside the command and run harness.

[Summary](validation/internlm-bp16-workers32-19g-host-input-clean/summary.json),
[command](validation/internlm-bp16-workers32-19g-host-input-clean/command.json),
[run harness](validation/internlm-bp16-workers32-19g-host-input-clean/run.py),
[resource samples](validation/internlm-bp16-workers32-19g-host-input-clean/automatic.resources.json.gz),
[runtime hashes](validation/internlm-bp16-workers32-19g-host-input-clean/runtime-binary-sha256.json),
[full result](validation/internlm-bp16-workers32-19g-host-input-clean/result.json.gz), and
[full stderr](validation/internlm-bp16-workers32-19g-host-input-clean/automatic.stderr.txt.gz).

### Failed 20 GiB BP16 host-input attempt

A later run with a **20 GiB** tracked cap and **2 GiB** reserve aborted after
65.22 seconds at `vk::Queue::submit: ErrorOutOfDeviceMemory` (status **-6**).
The last recorded state showed **235 GPU restores**, zero GPU fallback, and zero
snapshot failures; inference did not complete, so there is no accepted rate or
output. Minimum available RAM was **29,189 MiB**, swap growth was zero, and the
Ollama GPU guard detected no process. The child core-dump limit was zero.

The successful **19 GiB / 2.5 GiB** run used a different cap and reserve. This
failure does not establish a host-input defect or native GPU fault. See the
[summary](validation/internlm-bp16-workers32-20g-host-input-failed/summary.json),
[incident notes](validation/internlm-bp16-workers32-20g-host-input-failed/incident-notes.md),
[command](validation/internlm-bp16-workers32-20g-host-input-failed/command.json),
and [full stderr](validation/internlm-bp16-workers32-20g-host-input-failed/automatic.stderr.txt.gz).

### Latest completed BP16 run: bounded allocated-host cache, 32 workers

The full InternLM2.5-20B F16 run used the 8 GiB allocated-host cache, a 19 GiB
tracked-residency cap, 2.5 GiB reserve, and 32 BP16 encoding workers. It completed
12 decode runs in **25,935.02 ms** (`12,000 / 25,935.02 = 0.4626948427 tokens/s`,
reported as **0.46**), with **49/49** layers and the established exact output
SHA-256 `8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`.
There were zero diagnostics and GPU fallbacks. Prompt processing took **6,929.72
ms**.

The sampled live allocated-host cache peaked at **8,589,301,536 bytes**, under
its **8,589,934,592-byte** limit. It recorded 695 allocations, 3,028 reuses,
and **14,186,299,600 cumulative bytes**; cumulative bytes are not resident
memory. Minimum available RAM was **18,993 MiB**, swap grew by **2,612 MiB**,
and the Ollama GPU guard detected no process. GPU profiling recorded **7.786 s
decode**; host profiling recorded **1.072 s validation**, **7.681 s input
preparation**, and **9.472 s submit/wait**.

This is numerically about **13.8% above** the earlier **0.40653 tokens/s**
direct-host run, but the runs were sequential with unlocked clocks and different
RAM conditions. This is not a controlled paired comparison or evidence that
the cache alone caused the difference. [Run summary](validation/internlm-bp16-allocated-host-cache8g/summary.json),
[command](validation/internlm-bp16-allocated-host-cache8g/command.json),
[resource samples](validation/internlm-bp16-allocated-host-cache8g/automatic.resources.json.gz),
[runtime hashes](validation/internlm-bp16-allocated-host-cache8g/runtime-binary-sha256.json),
[full result](validation/internlm-bp16-allocated-host-cache8g/result.json.gz), and
[full stderr](validation/internlm-bp16-allocated-host-cache8g/automatic.stderr.txt.gz).

### BP16 cached direct-host upload preference

The opt-in `ZVRAM_VULKAN_BP16_CACHED_UPLOAD=1` asks the BP16 direct-host input
buffer selector to prefer `HOST_CACHED` memory while retaining the required
host-visible/coherent properties and non-device-local constraint. If no
compatible cached type exists, it falls back to the first compatible type.
The preference applies only when BP16 host input is enabled; it is off by
default.

A full 39.73 GB InternLM2.5-20B F16 run enabled this preference with direct
host input, 32 BP16 workers, MRU, async compression, a 19 GiB tracked cap and
2.5 GiB reserve. It completed 12 decode runs in **30,446.86 ms**, or
**0.3941293 tokens/s**, with exact output (SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`),
**49/49** layers, no diagnostics and zero GPU fallback. Minimum available RAM
was **21,700 MiB** and swap grew by **711 MiB**. The run used cached direct-host
input, not the allocated-host cache.

This result does not improve the best measured **0.4626948 tokens/s** BP16
result above; runs were sequential and clocks/background load were not
controlled. It shows correctness for the opt-in preference, not a speed win.
The captured source hashes match commit `1161e7dfb7cf516cf04382f99e993cabae35ea16`;
the runtime layer and llama binaries are recorded in the archive. Separate
validation recorded **8/8** GPU tests twice. The first pass enabled
`ZVRAM_VULKAN_BP16_HOST_INPUT=1` and `ZVRAM_VULKAN_BP16_CACHED_UPLOAD=1` at
source `1161e7d`; the sampled-profile pass also enabled
`ZVRAM_VULKAN_GPU_PROFILE=1` at source `870a7dd`. A three-frame hidden
presentation check used `ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT=1` instead, so
it did not test cached-upload preference; it passed exact pixel/buffer checks
with zero validation errors. These are correctness checks, not game or
inference performance proof.

[Full-run archive](validation/internlm-bp16-cached-upload/README.md),
[GPU test logs](validation/bp16-cached-upload-check/README.md),
[run result](validation/internlm-bp16-cached-upload/result.json.gz), and
[full stderr](validation/internlm-bp16-cached-upload/automatic.stderr.txt.gz).

### BP16 allocated-host run with sampled profiling

A separate sampled-profile run used the 8 GiB bounded allocated-host cache with
the same 19 GiB tracked cap, 2.5 GiB reserve, and 32 BP16 workers. It completed
12 decode runs in **30,248.66 ms** (**0.3967118 tokens/s**), with the same exact
output SHA-256, **49/49** layers, and zero GPU fallback. Minimum available RAM
was **18,651 MiB**; swap grew by **4,316 MiB**. The final recorded live cache
charge was **8,586,139,232 bytes** of the **8,589,934,592-byte** cap; cumulative
accepted-use traffic is not resident memory.

The last sampled profile covered **8,304** calls, while the final snapshot
recorded **8,363** GPU restores. The application exited without a device
destruction profile record, so these sampled phase totals are partial and are
not presented as final totals. This sequential run does not replace the best
measured **0.4626948 tokens/s** result or establish a speed improvement.

[Sampled-run archive](validation/internlm-bp16-cache8g-sampled-profile/README.md),
[result](validation/internlm-bp16-cache8g-sampled-profile/result.json.gz), and
[full stderr](validation/internlm-bp16-cache8g-sampled-profile/automatic.stderr.txt.gz).

### BP16 allocation-free profiling marker check

After the allocation-free profiling change, the focused CTest suite passed
**8/8 GPU checks in 7.17 s** with
`ZVRAM_VULKAN_GPU_PROFILE=1` and
`ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT=1`. The final per-test log records
`allocation-free=1` on the relevant profiling outputs and final markers. This
checks only opt-in profile reporting; it makes no speed claim. The tests used
source `a8d113b`; the runtime layer binary hash is archived alongside the
[CTest logs](validation/bp16-allocation-free-profile-check/).

### BP16 deposit-fastpath candidate full-model run

An experimental deposit-fastpath candidate completed 12 decode runs in
**27,346.93 ms** (**0.4388061 tokens/s**), with exact output SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`,
**49/49** layers, and zero GPU fallback. Minimum available RAM was
**19,995 MiB** and swap grew by **381 MiB**. The source was
`a8d113bac4dc2bce8867286f10199d56c09ca2f5`; the isolated BP16 shader hash was
`43e49ac77c0e6adef9db24dadc6df889a285cc0e9505f03dbbf9ba80e1445ab4`.

This does not establish a speed improvement over the prior sampled-profile run
at **0.3967118 tokens/s**: profile sampling/software-pipeline state and RAM
conditions differed, while measured GPU decode duration was similar at about
**7.75 s** versus **7.74 s**. The candidate remains unadopted, and the best
measured rate remains **0.4626948 tokens/s**. The final profile call count
matched the final snapshot at **8,409**; this is profile consistency evidence,
not an end-to-end speed claim.

[Candidate archive](validation/internlm-bp16-deposit-fastpath/README.md),
[result](validation/internlm-bp16-deposit-fastpath/result.json.gz), and
[full stderr](validation/internlm-bp16-deposit-fastpath/automatic.stderr.txt.gz).

### Same-prompt 92-token native/allocated-host comparison

A separate longer generation used the same prompt, temperature 0, seed 1,
context 512, 128-token prediction setting, and four-node Vulkan execution for
native and BP16 allocated-host mode. Both completed 92 decode runs, offloaded
**49/49** layers, and produced identical stdout
(SHA-256 `b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`).
Native measured **54,138.45 ms** (**1.6993468 tokens/s**); allocated-host mode
measured **209,195.45 ms** (**0.4397801 tokens/s**).

The cache stayed within its 8 GiB live limit: sampled peak **8,589,929,408** of
**8,589,934,592 bytes**. Allocated-host counters recorded 1,548 allocations,
19,607 reuses and **26,524,914,384 cumulative bytes**, which are not resident
memory. Minimum available RAM was 28,255 MiB native and 20,876 MiB allocated;
swap grew by 245 MiB and 3,248 MiB respectively, and the Ollama GPU guard found
no process in either run. These runs were sequential
with unlocked clocks and different RAM conditions, so this is not a controlled
performance comparison. Its longer-run BP16 rate is below the separate short-run
**0.4626948 tokens/s** result above and does not replace that as the best
measured short-run rate.

[Comparison summary](validation/internlm-bp16-allocated-long128/comparison-summary.json),
[archive provenance](validation/internlm-bp16-allocated-long128/provenance.json),
[native logs and harness](validation/internlm-bp16-allocated-long128/native/), and
[allocated-host logs and hashes](validation/internlm-bp16-allocated-long128/allocated-host/).

### Bounded imported BP16 host input and current regression checks

The standalone research decoder now has an opt-in `--import-host-input` path using
`VK_EXT_external_memory_host`. On the RX 7900 XTX, a mixed 4,352-byte frame and a
32 MiB F16 slice each passed three exact-byte iterations with zero validation
errors/VUIDs. The 32 MiB GPU decode median was 1.064 ms and includes host reads.
This is component evidence only: the production layer still copies frames into
its coherent upload buffer, and no imported-input inference speed is established.
The prototype owns aligned immutable memory through completion and retains it on
uncertain completion; it never imports arbitrary vector storage.
[Logs and hashes](validation/bp16-import-host-input/summary.json).

The BP16-enabled full suite passed **120/120** checks. Four initially stale unknown
command fixtures used an event that is now classified as resource-free; they now
use a native image clear to exercise conservative restore. After a final
exception-path image-lifetime guard, the four affected checks passed again. The
default build CPU checks passed **12/12**. Archived logs are linked above.

### Cached BP16 imported host input

The production layer has an opt-in `ZVRAM_VULKAN_BP16_IMPORT_HOST_INPUT=1`
path with `--vulkan-codec bp16 --vulkan-bp16-gpu`. Vulkan 1.1+ and suitable
`VK_EXT_external_memory_host` support are required. It copies a frame once into
owned aligned immutable memory, imports that allocation, and can reuse the
import when the clean snapshot cache retains it. The original vector is released
only after quota and savings checks accept the import. Alignment padding is
charged throughout cold/cache transitions. Unsupported imports and quota
pressure retain the existing upload path; default settings are unchanged.

Eight focused GPU checks passed with actual import counters, exact bytes, no
GPU fallback, and no validation diagnostics. The full suite passed **120/120**
with imported mode enabled, and CPU checks passed **12/12**. Two native cache
fixtures preserved bytes and quota cleanup, but did not observe repeated imported
GPU use after their writes/RAW transitions. Three hidden Gamescope frames passed
exact pixel and full-buffer checks with synchronization validation and observed
imports. This is not a game or inference speed claim. A final guard rejecting
invalid driver host/page alignments was followed by another **8/8** focused pass.

Imported owners cannot be copied by value. Completed operations release Vulkan
resources before host memory; uncertain queue submission/completion poisons the
decoder and retains potentially in-flight owners. CPU checks exercise aligned
size bounds, quota overflow/exact fit, owner moves, exactly-once handle cleanup,
and poison retention with fake handles. No real device loss was induced.
[Logs, hashes and limits](validation/bp16-import-host-layer/summary.json);
[hidden presentation result](validation/bp16-import-host-layer/presentation-result.json).

#### Full-model cached-import experiment: correct but slow

The opt-in cached imported-host-input mode completed the full InternLM2.5-20B
F16 check with **49/49** layers and the same output SHA-256 as other BP16 runs,
but took **166,980.73 ms** for 12 decode runs:
`12 * 1000 / 166980.73 = 0.0718646 tokens/s`. There were zero GPU fallbacks.
This is correctness evidence with a substantial slowdown, not a faster mode;
the latest measured compressed result is **0.4626948 tokens/s** in the
separate bounded allocated-host-cache run above. The earlier **0.40653** run
remains a historical direct-host result.

The run recorded **2,120 imports**, **6,495 reuses**, and **43,318,460,416
cumulative imported bytes**. These counters are cumulative use, not current
resident memory. Device profiling recorded **7.801 s compute** (including
coherent host-input reads). Host profiling recorded **1.274 s validation**,
**9.62 ms input preparation**, and **121.592 s submit/wait**. Compared with the
non-import host-input run, input preparation fell from **13.773 s**, while
submit/wait rose from **9.482 s**. A driver buffer-object overhead explanation
is only a hypothesis; causality is unproven.

Minimum available RAM was **19,999 MiB**; swap grew by **1,963 MiB**; the Ollama
GPU guard detected no process. The initial invocation was stopped before loading
because the guard found `qwen3.5:9b-local` using **6,113,858,682 VRAM bytes**.
The parent unloaded it through the API before retrying; no files were deleted.
Source commit was `2502f05`.

[Run summary](validation/internlm-bp16-workers32-19g-import-host-input/summary.json),
[command](validation/internlm-bp16-workers32-19g-import-host-input/command.json),
[run harness](validation/internlm-bp16-workers32-19g-import-host-input/run.py),
[runtime hashes](validation/internlm-bp16-workers32-19g-import-host-input/runtime-binary-sha256.json),
[full result](validation/internlm-bp16-workers32-19g-import-host-input/result.json.gz), and
[full stderr](validation/internlm-bp16-workers32-19g-import-host-input/automatic.stderr.txt.gz).
Initial guard evidence is in the [separate attempt record](validation/internlm-bp16-workers32-19g-import-host-input/initial-guard/result.json).

### BP16 imported-input resident-count component matrix

A 4,352-byte active mixed-pattern frame ran for 16 iterations at resident import
counts 0, 128, 512 and 1,024, with buffer-device-address both disabled and
enabled; a final zero-count case in a fresh process provided a baseline. All
nine cases passed exact bytes with validation/VUID counts at zero. The
1,024-import case reserved about 4 MiB in dummy allocations, plus the active
fixture owner.
Median host submit time rose from about 8 us at zero imports to 38, 130 and
329 us at 128, 512 and 1,024; fence wait
stayed roughly 163–183 us. Imports were retained across the timed submissions,
so this measures count-dependent per-submission host cost, not import creation
cost. It does not establish the cause of the full-model 121.6 s submit/wait
interval. Validation was enabled, so this does not isolate driver-only cost;
runs were sequential, with clocks/background activity uncontrolled; this is
not an inference benchmark.
[Logs, source and hashes](validation/bp16-resident-import-count/summary.json).

### BP16 imported-input dummy allocation-size matrix

A six-case follow-up varied total dummy bytes as well as import count using the
same 4,352-byte exact-output fixture and 16 iterations. All cases passed with
zero validation/VUID errors; the largest total dummy allocation was 256 MiB,
plus the active fixture owner. Median host submit time was about 13 us for
16 × 4 KiB, 100.75 us for 16 × 16 MiB, 333.57 us for 256 × 1 MiB, and 349 us
for 1,024 × 4 KiB. Fence wait stayed
roughly 163–181 us. This supports both a count and allocation-size cost in this
component's per-submission host path with retained imports, not import creation
cost. It does not establish the cause or explain the magnitude of the full-model
delay. Validation was enabled; runs were sequential with uncontrolled
clocks/background activity. [Logs, source and hashes](validation/bp16-resident-import-size/summary.json).

### BP16 allocated-host-input size matrix

A separate six-case 4,352-byte fixture used ordinary `vkAllocateMemory`
HOST_VISIBLE|HOST_COHERENT|HOST_CACHED allocations without DEVICE_LOCAL memory;
it did not use `VK_EXT_external_memory_host`. Across 16 × 4 KiB,
16 × 16 MiB, 256 × 1 MiB, and 1,024 × 4 KiB allocations, all cases passed
exact bytes with zero validation/VUID errors. Median host-submit time stayed
about 6–8 us. The corresponding imported-input matrix measured 13, 100.75,
333.57, and 349 us in a separate sequential run. This is component evidence
that the input/allocation path changes per-submission host cost, not a
controlled comparison or explanation of full-model timing. [Logs, source and
recorded hashes](validation/bp16-allocated-host-size/summary.json).

The follow-up [BDA and 32 MiB cases](validation/bp16-allocated-host-size/extra/summary.json)
also passed exact bytes with zero validation/VUID errors. The 32 MiB frame
used an exact-size **29,202,816-byte** ordinary host-visible allocation
(memory type 5, flags `0xe`) and buffer-device-address. Across three component
iterations, BP16 decode median was **1.05684 ms** and readback-copy median was
**1.13584 ms**. This is decoder-component timing, not end-to-end model
performance.

The production path is opt-in with `ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT=1`
and BP16 GPU restore flags; it remains off by default. It uses ordinary
allocated host memory, not `VK_EXT_external_memory_host`, and caps the live
allocated-input cache at **8 GiB** by default. Set
`ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB` to a decimal MiB value to override the
cap; `0` disables only this cache, leaving direct coherent-host GPU input
available. Existing cold/cache byte quotas still apply. Each owner is charged
its actual Vulkan allocation requirement before allocation and released only
after unmapping and freeing; poisoned/in-flight owners retain their reservation.
The shutdown marker reports `allocations`, `reuses`, cumulative `bytes`,
`live-bytes`, and `limit-bytes`; accepted-use counters are cumulative, while
`live-bytes` reports current cache charge.

On source `23c05842cebf2fe3c7093191ba7f448626505d6e`, focused GPU checks passed
**8/8** in 7.30 s with the default budget and **8/8** in 7.08 s with the
allocated-host cache disabled; CPU budget-parser and ownership checks also
passed. The
earlier **120/120** CTest, **11/11** CPU, and three-frame presentation evidence
was recorded at source `220ae18f3b01c94f7f17fcfa3c3c9257abd9acfc`, before the
new budget, and must not be read as testing this final capped version
([earlier gate summary](validation/bp16-allocated-host-layer/summary.json)).

The earlier uncapped 19 GiB full-model attempt at source
`220ae18f3b01c94f7f17fcfa3c3c9257abd9acfc` did not complete: after the
prompt was sent, the RAM guard stopped it at **16,368 MiB available**, below
its 16,384 MiB floor, after 76.95 s. It had recorded 631 allocations, one reuse
and 15,607,075,264 cumulative allocated-host bytes; there is no completed
output hash or rate. This run predates the 8 GiB cache cap and says nothing
about full-model behavior with the current bound. [Stopped-run record and logs](validation/internlm-bp16-allocated-host-ram-guard/summary.json).

One plausible driver-path explanation is that Mesa 26.2.4 RADV marks ordinary
allocations with `NO_INTERPROCESS_SHARING` and `PREFER_LOCAL_BO`, then treats
BOs in VRAM/GTT domains as local and `VM_ALWAYS_VALID` ([allocation flags](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.2.4/src/amd/vulkan/radv_device_memory.c#L227),
[BO placement](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.2.4/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c#L604)).
The imported-user-pointer path lacks these placement flags and triggers kernel
HMM validation of the userptr range on submission
([AMDGPU CS validation](https://github.com/CachyOS/linux/blob/cachyos-7.2.9-1/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L901)).
This mechanism fits the component timing difference, but does not establish
causality or explain the full-model submit/wait delay.

### BP16 host-copy component check

A smoke-only helper copied the same **29,202,816-byte** encoded BP16 frame 64
times using direct coherent host input and allocated cached host input. All six
runs passed with zero validation errors/VUIDs; the measured copy rates were
22.31–24.11 GB/s for direct host input and 23.51–49.41 GB/s for allocated
cached input. The allocated range is noisy and these component copies do not
show a full-model speedup. The captured source and binary hashes are in the
[matrix summary](validation/bp16-host-copy-matrix/summary.json), alongside all
six logs and the compressed helper source.
