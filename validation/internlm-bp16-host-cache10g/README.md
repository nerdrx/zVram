# BP16 allocated-host cache at 10 GiB

This full InternLM2.5-20B F16 run changed the allocated-host cache budget to
10,240 MiB (`ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB=10240`); the rest of the
tracked-residency configuration remained at a 19 GiB cap with a 2.5 GiB
reserve. It completed 12 decode runs in 29,532.11 ms
(**0.4063374 tokens/s**), with exact output SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`, 49/49
layers, and zero fallback. Minimum available RAM was 19,199 MiB; swap grew by
1,976 MiB.

Telemetry sampled a maximum live allocated-host charge of **10,736,501,008**
of **10,737,418,240 bytes**. The cache-size change alone did not produce a
speed win: this sequential run was slower than the best 8 GiB result
(**0.4626948 tokens/s**), and clocks/background conditions were uncontrolled.
The 10 GiB budget is not recommended as a performance improvement.

Source commit: `5d9f8686037f8ef95b33160a7a7ac09a971c0b02`. Runtime hashes,
environment, guard settings, and source hashes are retained in the run
metadata.

See [command](command.json), [result](result.json.gz),
[resources](automatic.resources.json), [stderr](automatic.stderr.txt.gz),
[stdout](automatic.stdout.txt.gz), and [runtime hashes](runtime-binary-sha256.json).
