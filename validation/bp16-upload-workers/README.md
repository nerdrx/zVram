# BP16 upload-worker regression checks

The opt-in `--vulkan-bp16-upload-workers 1..8` CLI flag (or
`ZVRAM_VULKAN_BP16_UPLOAD_WORKERS` environment variable) selects CPU workers
for copying BP16 encoded bytes into the GPU upload buffer. The CLI flag requires
BP16 GPU restoration and automatic snapshots. GDeflate ignores it. The default
is one worker. Copies below 1 MiB stay serial. If thread allocation or creation
fails, started threads are joined and the copy is retried serially.

At worker count 8, the full CTest suite passed **120/120** in 85.30 seconds
with `ZVRAM_VULKAN_GPU_PROFILE=1`. The focused BP16 GPU suite passed **8/8**
in each of four input modes: normal device input, direct host input, imported
host input, and allocated host input. These results verify the option and
regressions; they make no speed claim.

The tested source commit was `736ef60bc57304848810de1b598702badbe08d30`.
The runtime library SHA-256 was
`d745d704135f7af4e3a57c3df1180c5bee9af3ff42d2343dedc9b191981829f8`.

The raw CTest logs are preserved here: [full suite](full-120-workers8.log),
[device input](gpu-8-workers8.log), [direct host input](gpu-8-direct-workers8.log),
and [imported host input](gpu-8-import-workers8.log). Allocated-host input was
also enabled for its corresponding focused suite. Source, binary, and log
hashes plus the full-suite environment are in [provenance metadata](provenance.json).

A later 20B F16 run enabled eight upload-copy workers and measured
**0.61020197 tokens/s**, with exact output and zero fallback. It was a single
sequential run with uncontrolled clocks/background activity, so it does not
isolate a worker-count speed effect. [Run archive and limits](../internlm-bp16-cold24-upload8/README.md).
