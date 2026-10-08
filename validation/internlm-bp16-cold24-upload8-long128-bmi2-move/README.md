# BP16 exact-storage ownership-transfer long run

The 92-token InternLM2.5-20B F16 run used commit `6b35007`, which transfers
BP16 encoded storage only when vector size and capacity exactly equal the
encoded size; otherwise it retains the copy path. It completed in **200,387.71
ms** (**0.459110 tokens/s**) with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, **49/49**
layers, and zero GPU fallback. Minimum available RAM was **19,786 MiB** and
swap grew by **2,689 MiB**.

The preceding same-binary FIRST-policy 92-token run completed in **199,795.01
ms** (**0.460472 tokens/s**) with the same exact output. This run showed no
throughput gain. The runs were sequential with unlocked clocks and uncontrolled
background activity, so this is not a controlled performance comparison.

Final restoration counters were 56,365 GPU decodes, **1,860,641,423,360**
decoded bytes, zero GPU fallback, 46,794 clean reuses, 9,158 invalidations,
and **331,481,481,216** copied bytes. The allocated-host input cache reported
1,660 allocations, 14,746 reuses, and **8,588,754,416 live bytes** under its
8 GiB cap; cumulative allocated bytes are logged separately and are not live
memory.

The layer binary SHA-256 was
`86b7eddf61b7a00a4610243a6f650797913d4cb0ee144d94e736ae36107736d0`; the BP16
shader remained `246b5e7f5d5893a1137e31141e7ba41b9bc2b109fae91d671e2cea3b89eff854`.
The focused CPU/full/async correctness evidence for the code change is archived
in [BP16 exact-storage checks](../bp16-exact-storage-move/README.md).

Run artifacts: [command](automatic-command.json), [result](automatic-result.json.gz),
[resources](automatic.resources.json), [memory samples](memory.jsonl),
[stderr](automatic.stderr.txt.gz), [stdout](automatic.stdout.txt.gz), and
[source/runtime provenance](runtime-source-context.json).
