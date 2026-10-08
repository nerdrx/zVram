# BP16 upload-worker full-model observations

Four full InternLM2.5-20B F16 runs kept the same runtime layer binary
(`d745d704…`), 24 GiB cold quota, 19 GiB tracked-residency cap, 2.5 GiB reserve,
8 GiB allocated-host input-cache bound, 32 BP16 encoding workers, model, and
decode settings. They varied the BP16 upload-copy worker count:

| Upload workers | Decode time (12 tokens) | Observed rate | Output / layers |
| ---: | ---: | ---: | --- |
| 1 | 22,508.02 ms | 0.5331433 tokens/s | Exact / 49 of 49 |
| 4 | 20,113.26 ms | 0.5966213 tokens/s | Exact / 49 of 49 |
| 8 | 19,665.62 ms | 0.6102020 tokens/s | Exact / 49 of 49 |
| 8 repeat | 19,766.73 ms | 0.6070807 tokens/s | Exact / 49 of 49 |

All runs reported zero GPU fallback or diagnostics and the same output SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`. The two
8-worker observations are close. These runs were sequential with unlocked GPU
clocks and uncontrolled desktop/background activity, so the table documents
the observed pattern and does not establish that worker count alone caused it.
The production shader was unchanged.

The launch source commits and runtime-binary provenance are recorded per run.
The repeat's launch checkout was commit `4d1b704`, but its library had not been
rebuilt after that source-only profiling commit: it loaded runtime code commit
`736ef60` with the same library SHA-256 as the other runs. See its
[`runtime-source-context.json`](../internlm-bp16-cold24-upload8-repeat/runtime-source-context.json).

- [One worker](../internlm-bp16-cold24-upload1/README.md)
- [Four workers](../internlm-bp16-cold24-upload4/README.md)
- [Eight workers, first run](../internlm-bp16-cold24-upload8/README.md)
- [Eight workers, repeat](../internlm-bp16-cold24-upload8-repeat/README.md)
