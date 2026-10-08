# BP16 LFU long run with a 16 GiB allocated-host input-owner cap

The 92-token InternLM2.5-20B F16 run used the same `5fcd4f3` runtime binary as
the preceding 8 GiB-owner LFU run, while increasing the allocated-host input
owner cap to 16 GiB. It completed in **134,742.22 ms** (**0.682785 tokens/s**)
with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, **49/49**
layers, and zero GPU fallback. Minimum available RAM was **19,683 MiB** and
swap grew by **3,057 MiB**.

The 8 GiB-owner LFU run measured **0.574915 tokens/s**. Both runs were
sequential with unlocked clocks and uncontrolled background activity, so the
observed **18.8%** rate difference does not establish a causal owner-cap effect.
The upload profile's direct-copy time was **20.279 s** here versus **41.055 s**
in the preceding run, a supporting profile observation rather than a complete
explanation of end-to-end rate.

Final counters recorded **4,912** raw copies / **162,115,420,160 bytes** /
**17.021 s**, **51,157** clean reuses, and **3,945** invalidations. The input
owner ended at **17,176,013,360 live bytes** under its **17,179,869,184-byte**
cap; cumulative allocation counters are in the raw log and are not resident
memory.

Runtime layer SHA-256:
`3b5915ddc1b36560ff765f6efb5917b20ffa64c3a632ee81a586586060859b5e`. BP16
shader SHA-256:
`246b5e7f5d5893a1137e31141e7ba41b9bc2b109fae91d671e2cea3b89eff854`.
See the preceding [8 GiB-owner LFU run](../internlm-bp16-cold24-upload8-long128-bmi2-lfu/README.md).

Run artifacts: [command](automatic-command.json), [result](automatic-result.json.gz),
[resources](automatic.resources.json), [memory samples](memory.jsonl),
[stderr](automatic.stderr.txt.gz), [stdout](automatic.stdout.txt.gz), and
[source/runtime provenance](runtime-source-context.json).
