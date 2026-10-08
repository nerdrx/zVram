# Long BP16 generation stopped by the RAM guard

This run attempted the story prompt from the earlier 92-token comparison, with
`--predict 128`, the 24 GiB cold quota, eight upload-copy workers, and the BP16
settings recorded in `command.json`. The helper stopped the child after
**156.05 s** when system-wide `MemAvailable`
reached **16,342 MiB**, below the **16,384 MiB** floor. The prompt had been sent,
but the reply was incomplete. There is no completed decode result, accepted
throughput, or exact native-output comparison. The child cleanup return code
was **-9**. Sampled swap grew by **1,692 MiB**; the Ollama GPU detector found no
GPU process.

The last partial host profile covered **26,992** calls. BP16 upload profiling
recorded **40,715,641 ns** of buffer preparation and **17,707,770,943 ns** of
direct copies; direct copies account for **99.77%** of those two cumulative
subphase totals. These accumulated timings are partial instrumentation data,
not process elapsed time or a completed-workload comparison. The final snapshot
before the stop recorded **28,371 restores**, **27,203 GPU decode calls**, and
zero GPU fallbacks.

The launch source was `4d1b704827535061e9c686b62513edaa01343c73`; the recorded
per-file source hashes match that tree. The runtime layer binary SHA-256 was
`dc3fb518c5df65cc4b302abf89f2a8d502fb96d5b122ee2e4485ec0b68b1f46f`.
The profiling test context is preserved separately: [selected CTest log](profile-tests/device-profile-checks.log)
(20/20 passed, including seven BP16 GPU cases) and [pressure profile log](profile-tests/device-profile-pressure.log)
(1/1 passed). These tests do not turn this guard-stopped attempt into a completed
model run.

[Guard result](result.json), [resource samples](automatic.resources.json),
[stderr](stderr.txt.gz), [stdout](stdout.txt.gz),
[command](command.json), [runtime hashes](runtime-binary-sha256.json), and
[source revision](source-commit.txt).
