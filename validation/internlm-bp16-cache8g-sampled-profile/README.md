# BP16 allocated-host cache run with sampled profiling

This full InternLM2.5-20B F16 run used BP16, 32 encoding workers, MRU, async
compression, a 19 GiB tracked cap, a 2.5 GiB reserve, and the 8 GiB allocated
host-input cache. It completed 12 decode runs in 30,248.66 ms
(**0.3967118 tokens/s**), with exact output SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`, 49/49
layers, and zero GPU fallback. Minimum available RAM was 18,651 MiB; swap grew
by 4,316 MiB. The final recorded live cache charge was 8,586,139,232 bytes of
8,589,934,592; cumulative accepted-use bytes are not resident memory.

This sequential sampled-profile run does not replace the best measured
0.4626948 tokens/s result or establish a speed improvement. The last profile
record covered 8,304 calls, while the final snapshot recorded 8,363 GPU
restores; there is no device-destruction profile record, so phase totals are
partial and are not final totals.

Captured source commit: `3b9c7bd9f9c697d0db0f31ad847bd3561cf4f7cd`. The source
hashes and runtime binary hashes are in the result and runtime-hash files.
`run.py` records the cache/profile environment and guard settings.

See [command](command.json), [result](result.json.gz),
[resources](automatic.resources.json), [stderr](automatic.stderr.txt.gz),
[stdout](automatic.stdout.txt.gz), and [runtime hashes](runtime-binary-sha256.json).
