# Short BP16 runs with allocator thresholds

Two sequential 12-token full-model runs used the same launch settings, loaded
runtime (`dc3fb…`), production shader, and child-only 128 KiB glibc
mmap/trim thresholds. The normal reusable upload path measured
**0.54232386 tokens/s** (22,127.00 ms);
the cached-upload preference measured **0.52496056
tokens/s** (22,858.86 ms). Both produced the established exact
output, offloaded 49/49 layers, and had zero GPU fallback. The cached preference
did not improve this pair. The runs were sequential with unlocked clocks and
background activity, so the difference is not a causal estimate.

Both rates are below the separate **0.61020197 tokens/s** 12-token result,
which used a different runtime build (before the later profiling-only change);
this is not a matched comparison. The 92-token threshold run measured
0.4652449 tokens/s and completed exact output, while the threshold-unset long
run stopped at the RAM guard without a rate. These observations do not justify
changing defaults or attributing the longer-run RAM behavior to allocator
thresholds.

[Normal upload run](../internlm-bp16-cold24-upload8-mmap/README.md),
[cached upload run](../internlm-bp16-cold24-upload8-cached-mmap/README.md),
[summary and hashes](summary.json).
