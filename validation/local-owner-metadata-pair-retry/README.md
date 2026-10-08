# Local-owner metadata validation: alternating before/current fixture

The one-line source control changed only BP16 metadata validation in
`gdeflate_gpu.hpp`: the before variant inspected the owner bytes, while current
code inspects the encoded metadata prefix. Four alternating runs (before,
current, before, current) all passed exact application byte checks, reported
13 GPU BP16 encodes over 436,207,616 raw bytes, zero GPU restore fallbacks,
and zero Vulkan validation errors/VUIDs.

| Run | Variant | Host encode time | Wall time |
|---|---|---:|---:|
| 0 | Before | 2.344679483 s | 2.889 s |
| 1 | Current | 26.795424 ms | 0.510 s |
| 2 | Before | 2.345212120 s | 2.845 s |
| 3 | Current | 26.815799 ms | 0.554 s |

All four runs recorded a 1,300 MHz memory clock, but desktop background
activity was uncontrolled. These are fixture-level metadata timings, not model
throughput and not a model speedup claim.

`result.json` records each exact command, wall time, clocks, pass/VUID counts,
and runtime binary hashes. `command.json`, `before-command.json`, and
`env.json` preserve the shared launch settings. The one-line
`before-control.patch`, source-control provenance, and `zvram-codecs.json`
manifest are included; the manifest hash is recorded in `provenance.json` and
`original-bytes-sha256.json`. The source control is also documented in
[`local-owner-metadata-control`](../local-owner-metadata-control/README.md).
`source-control-provenance.json` preserves the original setup record; its
`paired_gpu_run: not_run` note predates the completed pair recorded here.
