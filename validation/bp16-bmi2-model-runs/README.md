# BP16 BMI2 model-run observations

The 12-token run measured **0.59393 tokens/s** and the 92-token run measured **0.57273 tokens/s**. Both matched the established byte-exact output and offloaded 49/49 layers with zero fallback. The 92-token run used the same child-only 128 KiB mmap/trim thresholds as its earlier comparison. Its cumulative profiles also recorded fewer copies and cache invalidations than the earlier run, so the rate difference cannot be assigned to the packer change alone. These are separate sequential workloads with unlocked clocks; they do not establish a model-level causal speed gain from BMI2. The short run remains below the separate **0.61020 tokens/s** best short run. CPU component encoding improved in its benchmark, but end-to-end causality remains unproven.

[12-token run](../internlm-bp16-cold24-upload8-bmi2/README.md), [92-token run](../internlm-bp16-cold24-upload8-long128-bmi2-mmap/README.md), [exact metrics/provenance](summary.json).
