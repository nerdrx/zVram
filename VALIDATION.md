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

Tests require compatible GPU hardware and the relevant runtime. A small llama.cpp model passed, while 40 GiB model loading, broader application compatibility, performance gains, compression ratios for larger models, and transparent paging remain unverified.

## Unmodified ROCm application

The unchanged official ROCm [primbench HIP copy example](https://github.com/ROCm/rocm-libraries/blob/959b2d4d0459abfd1f67f3fb9cce20cd88a7785a/shared/primbench/examples/hip/copy_benchmark.cpp) was built at commit `959b2d4d0459abfd1f67f3fb9cce20cd88a7785a`, targeting gfx1100, with monitoring disabled and upstream assertions enabled. `check_primbench.py` pins both source hashes and reproduces the build and runs.

Both the native process and `./zvram --hip --hip-vmm --hip-report-capacity --hip-local-mib 32 --hip-host-mib 512 -- copy_benchmark --size 32MiB --min-secs 0.1 --noise-timeout-secs 1` exited 0 for `char` and `long long`. The example allocates two 32 MiB data buffers plus its library's internal 256 MiB cache buffer. Under zVram these exceeded the 32 MiB local cap: peak tracked backing was 32 MiB VRAM and 288 MiB GTT, with six VMM allocations over the run. Tracked/local/host/pending/orphan/failure counters returned to zero.

The upstream copy assertion verifies only the first three values (`0, 1, 2`), so this does not replace the full-word integrity checks above. Both short runs reached primbench's statistical noise timeout, which is distinct from the subprocess timeout and assertion failure; no stable performance comparison is claimed. An initial 256 MiB host cap correctly refused another allocation because the internal cache had already consumed most of it; the successful run used 512 MiB. Logs: [native](validation/primbench-native.txt), [VMM/GTT](validation/primbench-vmm.txt).

## Small llama.cpp model check

An unmodified `llama-completion` from llama.cpp commit `c479922ac520a08969b4c1dc154d7bbb3c386d85` ran the F16 GGUF `SmolLM2-135M-Instruct` (270,885,952 bytes; SHA-256 `f535f83ec568d040f88ddc04a199fa6da90923bbb41d4dcaed02caa924d6ef57`) on the RX 7900 XTX. The model was not additionally quantized. Both native and VMM/GTT runs exited 0, offloaded 31/31 model layers, and produced byte-identical stdout (SHA-256 `087087260916ca2af13b0c97b12bd4cd9945c05fa3149c1a484667d130873cc8`). The run used `-ngl 999`, context 512, batch 128, 32 generated tokens, greedy sampling (`temp 0`, seed 1); it is an application compatibility check, not a benchmark.

The VMM/GTT run used a 64 MiB local cap and 2 GiB GTT cap. zVram tracked a 64 MiB peak local allocation, 240,599,040 bytes peak host/GTT backing, and three VMM allocations; tracked allocations, pending frees, and orphaned cleanup returned to zero, with no failures. Both actual model loads logged a 256.63 MiB ROCm0 model buffer and a 54.00 MiB CPU_Mapped buffer. The earlier 0.00 MiB lines belong to llama.cpp's preliminary no-allocation fit pass, not the actual model load. zVram's model allocation mapped 269,103,104 bytes: 67,108,864 local and 201,994,240 GTT. This establishes a real model-weight allocation through the wrapper, but the small native run also fit; it does not establish a model capacity or performance gain. A 40 GiB model and broader model/application behavior remain untested.

The exact model source, hash, build configuration, and command metadata are in [`validation/llama-small-model.json`](validation/llama-small-model.json); captured logs are [native](validation/llama-small-native.txt) and [VMM/GTT](validation/llama-small-vmm.txt), with [native stdout](validation/llama-small-native.stdout.txt) and [VMM/GTT stdout](validation/llama-small-vmm.stdout.txt).

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

[Concurrent report and samples](validation/odysseus-pair-summary.json), [30B log](validation/odysseus-pair-coder30b.stderr.log), [27B log](validation/odysseus-pair-dense27b.stderr.log), [hardware-specific runner](validation/odysseus-pair-runner.py), [file provenance](validation/odysseus-models.json). The runner reproduces this host's paths and caps; adjust them for another machine. These checks establish concurrent unchanged-model inference through GPU-accessible GTT backing. They do not demonstrate compressed inference, a single 40 GiB model, performance improvement, or arbitrary application compatibility.

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

## Unchanged memtest_vulkan smoke check

Installed `memtest_vulkan` 0.5.0 ran on the RX 7900 XTX with a 2 GiB explicit limit, through the Vulkan layer and natively, sequentially. Each run was interrupted after 12 seconds with SIGINT and exited 65. Neither emitted a memory-error report. The last five-second reports checked about 449.6 GB/s through zVram versus 437.7 GB/s natively. This short check is not a complete five-minute stability test or a performance improvement claim.

Both runs emitted the same SPIR-V `AtomicIAdd` memory-semantics validation warning during shader creation. zVram logged the 2,147,483,648-byte local allocation plus a 432-byte allocation; memtest still reported the native 24 GiB card. This path exercises telemetry and native allocation behavior, not automatic Vulkan compression or capacity expansion. [Wrapped log](validation/memtest-vulkan-zvram-2gib.txt), [native log](validation/memtest-vulkan-native-2gib.txt).

The upstream CLI uses positional device and byte-limit arguments: [`memtest_vulkan` source](https://github.com/GpuZelenograd/memtest_vulkan/blob/main/src/main.rs). The bounded local command was `./zvram --verbose --isolate-layers -- memtest_vulkan 1 2147483648`, with the RADV ICD selected and SIGINT sent after 12 seconds.
