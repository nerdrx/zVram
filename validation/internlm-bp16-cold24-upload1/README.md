# BP16 cold-quota run with one upload-copy worker

This full InternLM2.5-20B F16 run used a 24 GiB cold quota, 19 GiB tracked
cap, 2.5 GiB headroom reserve, 8 GiB allocated-host input-cache bound, and 32
BP16 encoding workers. It used one upload-copy worker. Twelve decode runs took
**22,508.02 ms** (**0.5331433 tokens/s**), with exact output, 49/49 layers, and
zero diagnostics or GPU fallback. Minimum available RAM was **17,947 MiB** and
swap grew by **1,904 MiB**.

This is one sequential run; it is not a controlled worker-count comparison.
The source checkout was `0d7994649f4710a218db2b15f56eb3c1887ce057`. The runtime
layer library SHA-256 was
`d745d704135f7af4e3a57c3df1180c5bee9af3ff42d2343dedc9b191981829f8`.

[Command](command.json), [result](result.json.gz),
[stderr](automatic.stderr.txt.gz), [stdout](automatic.stdout.txt.gz),
[resource samples](automatic.resources.json), and
[runtime hashes](runtime-binary-sha256.json).
