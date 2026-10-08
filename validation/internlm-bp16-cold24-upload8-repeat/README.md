# BP16 cold-quota repeat with eight upload-copy workers

This full InternLM2.5-20B F16 repeat used the same 24 GiB cold quota, 19 GiB
tracked cap, 2.5 GiB headroom reserve, 8 GiB allocated-host input-cache bound,
and 32 BP16 encoding workers. Twelve decode runs took **19,766.73 ms**
(**0.6070807 tokens/s**), with exact output, 49/49 layers, and zero diagnostics
or GPU fallback. Minimum available RAM was **17,927 MiB**; swap grew by
**832 MiB**.

The source checkout at launch was `4d1b704827535061e9c686b62513edaa01343c73`;
the recorded per-file source hashes match that checkout. The runtime library
was not rebuilt after the profiling-only source commit. Its loaded runtime code
commit was `736ef60`, and its SHA-256 was
`d745d704135f7af4e3a57c3df1180c5bee9af3ff42d2343dedc9b191981829f8`. See
[`runtime-source-context.json`](runtime-source-context.json) for the distinction.

This repeat supports the observed 8-worker rate but does not isolate a causal
worker-count effect; system load and GPU clocks were uncontrolled.

[Command](command.json), [result](result.json.gz),
[stderr](automatic.stderr.txt.gz), [stdout](automatic.stdout.txt.gz),
[resource samples](automatic.resources.json), and
[runtime hashes](runtime-binary-sha256.json).
