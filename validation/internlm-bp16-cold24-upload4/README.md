# BP16 cold-quota run with four upload-copy workers

This full InternLM2.5-20B F16 run used a 24 GiB cold quota, 19 GiB tracked
cap, 2.5 GiB headroom reserve, 8 GiB allocated-host input-cache bound, and 32
BP16 encoding workers. It used four upload-copy workers. Twelve decode runs took
**20,113.26 ms** (**0.5966213 tokens/s**), with exact output, 49/49 layers, and
zero diagnostics or GPU fallback. Minimum available RAM was **17,895 MiB** and
swap grew by **570 MiB**.

This is one sequential run; it is not a controlled worker-count comparison.
The source checkout was `85c3ed6a5c409578aa6f404a57039ea247cc5837`. The runtime
layer library SHA-256 was
`d745d704135f7af4e3a57c3df1180c5bee9af3ff42d2343dedc9b191981829f8`.

[Command](command.json), [result](result.json.gz),
[stderr](automatic.stderr.txt.gz), [stdout](automatic.stdout.txt.gz),
[resource samples](automatic.resources.json), and
[runtime hashes](runtime-binary-sha256.json).
