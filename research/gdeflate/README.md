# Bounded GDeflate shader research fork

This folder contains the standalone Vulkan shader research fork of Microsoft DirectStorage commit `c53f1499d5f67a61b69a1a348d22dcd2b4cb4ede`. It preserves the upstream Apache-2.0 notices in both HLSL files and includes `APACHE-2.0.txt`. The shader is not integrated into the layer. Separately, an optional CPU GDeflate codec adapter can be built into the layer; it is disabled by default, and Zstd remains the default codec. CMake exposes the standalone shader host only with `-DZVRAM_BUILD_GDEFLATE_RESEARCH=ON` (default off).

## Optional CPU snapshot codec

Configure a separate build with `-DZVRAM_ENABLE_GDEFLATE=ON` to fetch the pinned NVIDIA libdeflate fork and enable the CPU encoder/decoder. The source is fetched from `https://codeload.github.com/NVIDIA/libdeflate/tar.gz/8ba9502fb30d2bf728592d121f0d402e40c8cb05` with SHA-256 `d1b4c38dce43e68a5f4c28d0fbb3f81a01953039a3dea63f4bd1a84d7ff80592`; the local [`LIBDEFLATE-COPYING.txt`](LIBDEFLATE-COPYING.txt) preserves the upstream MIT and Apache-2.0 notices. To compile and run CPU-only tests without dispatching Vulkan GPU tests:

```sh
cmake -S . -B build/gdeflate-codec -DCMAKE_BUILD_TYPE=Release \
  -DZVRAM_ENABLE_GDEFLATE=ON -DZVRAM_BUILD_GDEFLATE_RESEARCH=OFF
cmake --build build/gdeflate-codec -j2
ctest --test-dir build/gdeflate-codec -R '^cpu-' --output-on-failure
```

The launcher selects the snapshot codec with `--vulkan-codec zstd|gdeflate`; omitting it keeps Zstd. Codec selection requires Vulkan automatic snapshots. GDeflate selection is rejected before launching the application if the optional codec was not built, and it cannot be combined with `--vulkan-byte-shuffle`. A chunk's codec tag is retained through direct restore and decode-ahead dispatch. The optional adapter runs GDeflate encoding and decoding on the CPU; the shader described below is not used for layer restore.

One CPU-only 32 MiB F16 slice encoded and round-tripped exactly with the optional codec. In a single CPU component run, GDeflate produced **27,219,160 bytes** versus Zstd's **26,673,213 bytes**; GDeflate encode/`decodeOne` took **355.970/114.457 ms** versus **19.194/13.907 ms** for Zstd. This was slightly larger and much slower, so it is format/correctness groundwork, not a speedup. It does not measure model throughput or token speed. The [codec model result](../../validation/gdeflate-codec-model.json), [text summary](../../validation/gdeflate-codec-model.txt), and [codec-enabled CPU CTest log](../../validation/gdeflate-codec-cpu-ctest.txt) record the checks.

A separate synthetic Vulkan-layer test exercised this CPU codec with actual GPU-backed cold snapshots: **320 MiB** was frozen and restored over two wake/compute cycles, with all bytes verified. One cold pass stored **10 compressed chunks** in **17,352,796 bytes**, and another stored **10 RAW chunks** in **335,544,320 bytes**; failures and cleanup counts were zero. This is a narrow synthetic layer check, not a model run or general app-compatibility claim; restore decompression remains on the CPU and this test does not use the shader below. Use `--build-dir` to select the optional build:

```sh
./zvram --build-dir build/gdeflate-codec --validate --isolate-layers \
  --vulkan-virtual-gib 96 --vulkan-auto-idle-ms 100 --vulkan-cold-mib 512 \
  --vulkan-codec gdeflate -- build/gdeflate-codec/zvram-vulkan-auto-check
```

[Layer test log](../../validation/gdeflate-layer-auto-restore.txt).

## Shader ABI

Bindings are set 0: `input` 0, `control` 1, `output` 2, `scratch` 3. `control` has three uint32 words: `[1, streamInputByteOffset, streamOutputByteOffset]`. Dispatch exactly one workgroup per tile, with `GroupID.x` equal to the tile index. `scratch[0]` must be zeroed before dispatch; any nonzero bit after dispatch means the shader rejected or exhausted part of the input. The output buffer must be padded to four bytes, as the decoder writes bytes using aligned 32-bit atomics.

The kernel obtains descriptor byte lengths with `GetDimensions`, validates the stream header and table extent, checks each tile's compressed input range and output range, and requires each compressed tile to contain the 128-byte initial interleaved read. Refills mask bytes beyond a tile end; `BitReader` tracks actual available bits separately from zero-filled lookahead and sets the error bit only if the decoder consumes unavailable bits. Odd logical output tails are supported when the descriptor is padded to four bytes. Input and output byte-address descriptors must fit the shader's 32-bit byte offsets.

The wave-intrinsic variant of `CSMain` rejects subgroup widths other than 32, requires exactly one stream, and processes one tile per workgroup. There is no global work-stealing loop. Dynamic code-length expansion is capped at 318 rounds, compressed-symbol expansion at 65,536 rounds, outer block parsing at 4,096 rounds, and each copy operation at 64 KiB. Shared-memory producer/consumer points use unconditional workgroup barriers in this specialization; output initialization and ordered byte-copy visibility use `AllMemoryBarrierWithGroupSync`. A groupshared error flag lets all lanes leave the decoder at a uniform barrier boundary, while `scratch[0]` records the error for the host.

`scratch[0]` is a sticky OR of categorized failure bits: `0x001` header/stream ABI or subgroup, `0x002` tile table/payload range, `0x004` input bit exhaustion, `0x008` output bounds, `0x010` symbol/table index, `0x020` code-length expansion, `0x040` copy source/distance/length, `0x080` loop budget, and `0x100` final decoded size. Multiple bits may be set together. The bit identifies a failure class, not necessarily the first cause; the checked corpus does not establish coverage of every valid or malformed stream.

The generic bounded nibble writer replaces the upstream optimized writer as a malformed-input defense. This is not evidence of an upstream valid-stream bug: valid DEFLATE symbols 17/18 emit zero code lengths and bypass `set4b`; nonzero repeat symbol 16 is limited to 3–6 entries. The 128-byte minimum tile and loop caps are conservative research constraints, not proof that every valid GDeflate stream is accepted.

`scan` and `scan16` call the shuffle collectively for every lane and use a clamped self-lane index for lanes that do not consume the shuffle result. This is required by the shared-memory fallback, whose shuffle implementation has workgroup barriers; a divergent ternary around that call could deadlock a CPU subgroup-8 implementation. The `WaveGetLaneCount` check is compiled only for the wave-intrinsic variant.

## CPU-only compile and validation

From the repository root:

```sh
dxc \
  -T cs_6_6 -E CSMain -spirv -fspv-target-env=vulkan1.2 \
  -D USE_WAVE_INTRINSICS=1 -D SIMD_WIDTH=32 -I research/gdeflate \
  -fvk-bind-register t0 0 0 0 -fvk-bind-register u0 0 1 0 \
  -fvk-bind-register u1 0 2 0 -fvk-bind-register u2 0 3 0 \
  research/gdeflate/GDeflate-bounded.hlsl -Fo research/gdeflate/GDeflate-bounded-wave32.spv
spirv-val --target-env vulkan1.2 \
  research/gdeflate/GDeflate-bounded-wave32.spv
```

For the CPU subgroup-8 fallback, use the same compile line but replace the two defines with `-D SIMD_WIDTH=8` (omit `-D USE_WAVE_INTRINSICS=1`) and write `GDeflate-bounded-subgroup8.spv`; then run `spirv-val` on that file. The wave32 proof is retained as `GDeflate-bounded-wave32.spv`.

The wave32 module was compiled with local DXC 1.9 using `-D USE_WAVE_INTRINSICS=1 -D SIMD_WIDTH=32` and passes `spirv-val` for Vulkan 1.2. The CPU subgroup-8 fallback was compiled with `-D SIMD_WIDTH=8` and no `USE_WAVE_INTRINSICS`; it also passes `spirv-val`. Both modules declare a 32×1×1 workgroup and descriptor bindings 0–3 as listed above. Fallback SPIR-V contains no `OpGroupNonUniform` instructions; its collective shuffles lower to workgroup control barriers. Compilation validates SPIR-V structure. The separate CPU-software checks below validate decoded bytes for their fixtures; neither check establishes AMD GPU performance or watchdog safety.


## Bounded host and CPU-only correctness gate

```sh
cmake -S . -B build/research -DZVRAM_BUILD_GDEFLATE_RESEARCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/research --target zvram-gdeflate-research -j 2
python3 research/gdeflate/test_software.py --binary build/research/zvram-gdeflate-research
```

The runner requires exactly one system lavapipe ICD (`lvp_icd*.json`), or an explicit `--icd /absolute/path/to/lvp_icd.json`. It selects that single driver, removes inherited alternative-driver settings, disables implicit layers, and limits software workers to two. The host requires a CPU Vulkan device in `--software-smoke` mode; it rejects a GPU device. Validation must be enabled, all output bytes must match, and validation errors/VUIDs must be zero. The runner verifies the HLSL, module and fixture hashes in [`modules.json`](modules.json) before execution. Checked-in SPIR-V was compiled with DXC **v1.9.2602.24**; rebuilding requires that compiler and the commands above.

The two published synthetic streams decode to 65,536 bytes and 131,195 bytes, including a 123-byte final tile. Their expected bytes are generated by `((i % 251) * 13 + (i // 65536) * 29) & 255`. If the separately downloaded local F16 fixtures exist under ignored `build/`, their pinned hashes must match before the runner also checks the first 64 KiB tile and the full 32 MiB stream. Model weights are not distributed in this folder.

On October 7, 2026, all four cases passed on isolated **llvmpipe (LLVM 23.1.1, 256 bits), CPU device type 4**, with full-byte comparisons and zero validation errors/VUIDs. The 32 MiB F16 sample contained 512 tiles; an initial standalone run took approximately 5.62 seconds of CPU-software decode time. This is a correctness result, not GPU throughput or a token-speed result. [Runner report and logs](../../validation/gdeflate-software-tests/results.json), [driver provenance](../../validation/gdeflate-software-driver-provenance.json), [standalone 32 MiB run](../../validation/gdeflate-software-f16-32m.txt).

A software-driver check exposed a header predicate that rejected the valid tile-size index `1`. The predicate now checks for `1` rather than `0`. Earlier failure logs are retained; this explains the software rejection and does not establish the cause of the preceding AMDGPU timeout.

The standalone host modes have separate limits:

- `--preflight-only SHADER ENCODED EXPECTED`: reads bounded files and validates SPIR-V magic and the stream envelope before any Vulkan API call. It does not validate compressed symbols.
- `--software-smoke SHADER ENCODED EXPECTED`: explicit CPU-only driver, at most 32 MiB/512 tiles, one iteration, 30-second fence wait.
- `--gpu-smoke SHADER ENCODED EXPECTED`: one tile and one iteration only, native device-local non-host-visible buffers, wave32 with subgroup-size control and full-subgroup features, 5-second fence wait.
- `--gpu-bounded-smoke SHADER ENCODED EXPECTED`: up to 32 MiB / 512 tiles, one iteration, the same device-local buffers and wave32 controls, and a 5-second fence wait.

After reboot, all four bounded RX 7900 XTX wave32 cases passed exact-byte checks with zero validation errors/VUIDs: 64 KiB / 1 tile, 1 MiB / 16 tiles, 32 MiB / 512 tiles, and a synthetic 131,195-byte odd-tail stream / 3 tiles. The real-slice GPU decode timestamps were 5.267, 5.361, and 5.792 ms respectively; the synthetic odd-tail case took 0.782 ms. These are one-iteration standalone shader component checks, not model inference or token throughput. [64 KiB log](../../validation/gdeflate-gpu-64k-fp16.txt), [1 MiB log](../../validation/gdeflate-gpu-1m-fp16.txt), [32 MiB log](../../validation/gdeflate-gpu-32m-fp16.txt), [odd-tail log](../../validation/gdeflate-gpu-multi-tail.txt), and [results](../../validation/gdeflate-gpu-results.json).

File limits are 4 MiB for SPIR-V, 64 MiB for encoded bytes, and 32 MiB for expected bytes. Host envelope checks bound offsets, tile table ranges and padded output lengths. The shader additionally bounds payload reads, decoded writes and loops. Any nonzero scratch error rejects the result before comparison. An outer runner timeout terminates its owned software process; a fence timeout cannot cancel work already dispatched to a hardware GPU. These constraints reduce the scope of a research test and are not a proof against hangs or driver bugs.

The earlier unbounded real-sample shader experiment triggered an AMDGPU ring timeout. The bounded wave32 tests above passed after reboot; this does not establish general watchdog safety or application integration. The standalone Vulkan shader remains outside zVram's layer. Separately, the optional CPU codec passed the narrow synthetic layer restore test above; real model restore, general app compatibility, active compressed paging, and token-speed gains remain unverified.
