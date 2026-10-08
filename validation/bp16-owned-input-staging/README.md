# BP16 owned-input staging prototype checks (removed)

The staged-input prototype passed CPU and GPU correctness checks: CPU12, two
full-byte synthetic/native fixtures, full CTest125/125, and focused staged GPU
checks 8/8. These checks verify correctness only. Its full-model experiment was
substantially slower than the GPU-encoder baseline, so the staging mode was
removed from the current runtime; do not use its environment variable with the
current CLI.

The original prototype is preserved by commit `2ddefdb`;
`implementation.patch` contains that commit patch. The model performance result,
full logs, and source/runtime hashes are in the [staging model archive](../internlm-bp16-gpu-stage-cold26-owner26-resident19-lfu/README.md).
