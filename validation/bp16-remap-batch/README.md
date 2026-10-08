# BP16 batched restore/remap prototype evidence

Historical four-frame BP16 restore/remap fixtures passed CPU, synthetic, and
native full-byte gates. Each required one submitted sparse remap transaction for
four child frames, zero GPU restore fallback, and no validation diagnostics.
The feature was later removed from the current runtime because its model runs
showed no observed speed gain; the historical flags are not current CLI options.

The combined implementation is reproducible by checking out commit `40b3a0d`
(base `f882a74`). `combined-batch-implementation.patch.gz` contains the selected
source diff; `combined-batch-implementation.json` records its original byte
length and SHA-256. The full-model results are in the [batch-only archive](../internlm-bp16-gpu-batch-cold26-owner26-resident19-lfu/README.md)
and [combined-remap archive](../internlm-bp16-gpu-remap-batch-cold26-owner26-resident19-lfu/README.md).
