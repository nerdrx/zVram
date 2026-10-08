# BP16 short run: normal upload allocator thresholds

This full InternLM2.5-20B F16 run completed 12 decode runs in
**22,127.00 ms** (**0.54232386 tokens/s**), with exact output
(SHA-256 `8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), **49/49** layers, and no GPU fallback. It used
normal reusable upload path, 24 GiB cold quota, 19 GiB tracked-residency cap, 2.5 GiB reserve,
32 BP16 encoding workers, and eight BP16 upload-copy workers. The child had
`MALLOC_MMAP_THRESHOLD_=131072`, `MALLOC_TRIM_THRESHOLD_=131072`, and
`MALLOC_ARENA_MAX=2`. Minimum available RAM was
**20,412 MiB**; swap grew by
**749 MiB**.

The launch checkout and per-file source hashes are retained separately from the
loaded runtime. Runtime code commit was `4d1b704`, layer SHA-256
`dc3fb518c5df65cc4b302abf89f2a8d502fb96d5b122ee2e4485ec0b68b1f46f`; the
production BP16 shader SHA-256 was
`246b5e7f5d5893a1137e31141e7ba41b9bc2b109fae91d671e2cea3b89eff854`. These
allocator settings are experimental and child-only; they are not defaults.

[Command](command.json), [result](result.json.gz),
[resource samples](automatic.resources.json), [stderr](automatic.stderr.txt.gz),
[stdout](automatic.stdout.txt.gz), [runtime hashes](runtime-binary-sha256.json),
and [source revision](source-commit.txt).
