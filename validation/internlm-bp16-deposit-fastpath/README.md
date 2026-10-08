# BP16 deposit-fastpath candidate full-model run

This experimental candidate completed 12 decode runs for InternLM2.5-20B F16
in 27,346.93 ms (**0.4388061 tokens/s**). Output SHA-256 matched the existing
BP16 runs (`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`),
all **49/49** layers were offloaded, and no diagnostics or GPU fallback were
reported. Minimum available RAM was **19,995 MiB** and swap grew by **381 MiB**.

This does not establish that the candidate improved speed over the preceding
sampled-profile result (**0.3967118 tokens/s**). The runs had different profile
sampling/software-pipeline state and RAM conditions; GPU decode durations were
similar (about **7.75 s** versus **7.74 s**). The candidate remains unadopted;
the best measured rate remains **0.4626948 tokens/s**.

The isolated shader also had a **33.6% slower** device-input result on the exact
16 MiB all-mask component fixture; the bounded component evidence is in the
[deposit-fastpath validation archive](../bp16-deposit-fastpath/README.md).

The run used source commit `a8d113bac4dc2bce8867286f10199d56c09ca2f5`. The
runtime manifest records the layer, BP16 shader (`43e49ac7…e1445ab4`) and llama
binary hashes. The final profile recorded **8,409** calls, matching the final
snapshot's **8,409** GPU restores.

See [command](command.json), [result](result.json.gz),
[resources](automatic.resources.json), [stderr](automatic.stderr.txt.gz),
[stdout](automatic.stdout.txt.gz), and [runtime hashes](runtime-binary-sha256.json).
