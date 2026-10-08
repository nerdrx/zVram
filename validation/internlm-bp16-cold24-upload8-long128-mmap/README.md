# BP16 92-token run with child-only allocator thresholds

This 92-token InternLM2.5-20B F16 run used the 24 GiB cold quota and eight
BP16 upload workers. It set child-only glibc thresholds
`MALLOC_MMAP_THRESHOLD_=131072` and `MALLOC_TRIM_THRESHOLD_=131072`
(`MALLOC_ARENA_MAX=2` was also set). The run completed in
**197,745.32 ms** (**0.46524489 tokens/s**, reported as
**0.47**), offloaded **49/49** layers, and had zero GPU fallbacks. Its
stdout exactly matched the native 92-token result
(SHA-256 `b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`). Minimum available RAM was
**20,601 MiB** and swap grew by
**2,056 MiB**.

This is one sequential observation with unlocked clocks and background activity.
The monitored baseline using default child allocator thresholds stopped at the
16 GiB RAM floor and has no rate, so the two runs do not establish an allocator
speed effect. Compared with the earlier 0.4397801 tokens/s allocated-host
92-token result and 1.6993468 tokens/s native result, conditions/configurations
differ; this run does not establish a controlled speedup. It remains separate
from the 12-token short-run best **0.61020197 tokens/s**. The allocator settings
are not application defaults.

The launch checkout was `f953ceb`; captured source hashes match that tree.
The loaded profiling runtime code was `4d1b704`; the runtime layer library SHA-256
is `dc3fb518c5df65cc4b302abf89f2a8d502fb96d5b122ee2e4485ec0b68b1f46f`, and
the production BP16 shader SHA-256 is
`246b5e7f5d5893a1137e31141e7ba41b9bc2b109fae91d671e2cea3b89eff854`.
The allocator experiment changed child environment only; no production source
was modified. See the [glibc tunables documentation](https://sourceware.org/glibc/manual/latest/html_node/Memory-Allocation-Tunables.html).

Profile totals: CPU validation **6.947 s**, input preparation **42.440 s**,
host submit/wait **64.083 s**; device transfer **0.276 s**, GPU decode
**51.847 s**, and finish **0.522 s**. These cumulative instrumented phase totals
are not a controlled attribution of throughput.

[Result](result.json.gz), [command](command.json),
[resource samples](automatic.resources.json), [memory samples](memory.jsonl),
[monitor script](monitor_restore_memory.py), [stderr](automatic.stderr.txt.gz),
[stdout](automatic.stdout.txt.gz), [runtime hashes](runtime-binary-sha256.json),
and [source revision](source-commit.txt).
