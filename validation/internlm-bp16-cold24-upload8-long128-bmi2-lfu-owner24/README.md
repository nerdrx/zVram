# BP16 LFU long run with a 24 GiB allocated-host input-owner cap

The 92-token InternLM2.5-20B F16 run used the same `5fcd4f3` runtime binary as
the 16 GiB-owner run, increasing only the allocated-host input-owner cap to
24 GiB. It completed in **129,950.52 ms** (**0.707962 tokens/s**) with exact
stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, **49/49**
layers, and zero GPU fallback. Minimum available RAM was **20,842 MiB** and
swap grew by **2,914 MiB**.

Against the sequential 16 GiB-owner run, the observed rate was **3.7% higher**.
Clocks and background activity were uncontrolled, so this does not isolate a
cap effect. GPU restore profiles recorded **0** upload-copy time and **0.023 s**
input preparation here, versus **20.279 s** direct-copy time for the 16 GiB run.
Validation took **7.077 s** and GPU decode **51.794 s**. Raw snapshot copies
increased to **6,520 / 215,797,530,624 bytes / 21.887 s** from 4,912 /
162,115,420,160 bytes / 17.021 s; clean reuses were **49,595** versus 51,157,
with **5,554** versus 3,945 invalidations. These counters show other workload
differences and do not support attributing the rate change to one mechanism.

The allocated-host input owner ended at **24,892,151,392 live bytes** under
its **25,769,803,776-byte** cap. Cumulative allocation bytes are recorded in
the run log and are not resident memory.

Runtime layer SHA-256:
`3b5915ddc1b36560ff765f6efb5917b20ffa64c3a632ee81a586586060859b5e`. BP16
shader SHA-256:
`246b5e7f5d5893a1137e31141e7ba41b9bc2b109fae91d671e2cea3b89eff854`.
See the [16 GiB-owner run](../internlm-bp16-cold24-upload8-long128-bmi2-lfu-owner16/README.md).

Run artifacts: [command](automatic-command.json), [result](automatic-result.json.gz),
[resources](automatic.resources.json), [memory samples](memory.jsonl),
[stderr](automatic.stderr.txt.gz), [stdout](automatic.stdout.txt.gz), and
[source/runtime provenance](runtime-source-context.json).
