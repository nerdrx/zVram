<p align="center"><img src="assets/banner.svg" alt="zVram — Explore memory beyond VRAM" width="100%"></p>

<p align="center"><b>Experimental GPU memory research · Vulkan · AMD RADV · C++17</b><br><a href="https://nerdrx.github.io/zVram/">Project website</a> · <a href="#quick-start">Quick start</a> · <a href="VALIDATION.md">Measured results</a></p>

zVram explores how GPU workloads can use memory beyond physical VRAM. The first prototype combines an **opt-in Vulkan allocation layer**, a **full-data capacity check**, and a **lossless compression round trip for explicitly managed buffers**.

**Status: experimental v0.1.0.** Transparent compression for arbitrary applications is a research goal. The launcher currently observes allocations and requests a supported driver policy; it does not compress application memory or change reported physical VRAM.

## What works today

| Component | Behavior |
|---|---|
| `zvram` launcher | Enables the local layer for one Vulkan process; no global installation. |
| Vulkan layer | Records live/peak allocation bytes and failures. Requests AMD `ALLOWED` overallocation when supported and the application supplies no policy. Preserves explicit application policies. |
| Capacity check | Keeps all chunks allocated, uploads distinct data, then copies back and verifies every byte. Records physical VRAM and GTT usage. |
| Compression check | Reads a managed buffer, compresses it with CPU zstd, releases its Vulkan allocation, restores it, and verifies the GPU readback. Uses raw storage for incompressible data. |

RADV can already migrate allocations from VRAM into GPU-accessible system RAM. Native spillover remains the driver's work. A successful baseline without zVram demonstrates that native capability, not a capacity gain created by this layer.

Layer counters describe requested allocation bytes by memory heap, not physical residency. The capacity check uses driver-wide sysfs accounting to observe VRAM/GTT placement.

## Quick start

Requires Linux, CMake, a C++17 compiler, Python 3, Vulkan headers/loader, and zstd development files. The GPU checks currently select a discrete AMD GPU; the layer passes other drivers through when the AMD extension is unavailable.

```sh
git clone https://github.com/nerdrx/zVram.git
cd zVram
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Launch a Vulkan application with allocation telemetry.
./zvram -- your-application its-arguments

# Include per-allocation logging.
./zvram --verbose -- your-application

# Small integrity and compression checks.
./zvram -- ./build/zvram-capacity-check --mib 64
./zvram -- ./build/zvram-compression-check
```

When Vulkan validation layers are installed, `--validate` enables core and synchronization validation. `--isolate-layers` disables implicit layers for controlled comparisons.

```sh
./zvram --validate --isolate-layers -- ./build/zvram-capacity-check --mib 64
```

The launcher uses the build directory beside itself. Reconfigure CMake after moving the checkout, so its layer manifest points to the current shared library.

## Measuring capacity

`zvram-capacity-check` defaults to **64 MiB**, uses a single staging buffer, and accepts `--mib 1..40960` and `--chunk-mib 1..256`. It refuses requests beyond an estimate based on the Vulkan budget, free AMD GTT, and host memory with an 8 GiB reserve. This estimate and allocation priority are advisory; a large check can still pressure the desktop.

Compare the same command with and without the launcher. The check verifies transfer data integrity while allocations remain live. It does not establish inference throughput, shader random-access performance, or compatibility with a 40 GB model.

See [VALIDATION.md](VALIDATION.md) for hardware, exact commands, and evidence. Synthetic compressibility is not a model compression ratio.

## Toward compressed GPU memory

- [x] Lossless managed-buffer eviction and restoration proof.
- [x] Opt-in Vulkan layer and allocation telemetry.
- [x] Full-data capacity probe with driver memory accounting.
- [ ] Reusable managed-buffer API and bounded host storage.
- [ ] Resource lifetime and synchronization handling for an eviction scheduler.
- [ ] Broader Vulkan workload compatibility and compute benchmarks.
- [ ] Assess driver support needed for transparent virtual memory across APIs.

Freeing and rebuilding a standalone buffer is straightforward. Preserving arbitrary application's bindings, device addresses, command buffers, images, and in-flight work requires deeper memory management. HIP/CUDA, OpenGL, and DirectX support are outside this prototype.

## Development

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Tests require a discrete AMD GPU. CI checks compilation and launcher syntax; GPU results come from the documented hardware runs. No desktop windows are opened by the transfer checks.

MIT licensed. Contributions should include reproducible workloads and distinguish native driver behavior from zVram changes.
