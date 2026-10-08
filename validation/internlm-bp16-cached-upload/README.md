# BP16 cached direct-host upload run

The run enabled `ZVRAM_VULKAN_BP16_CACHED_UPLOAD=1` and
`ZVRAM_VULKAN_BP16_HOST_INPUT=1` for a 39.73 GB InternLM2.5-20B F16 model. It
completed 12 decode runs in 30,446.86 ms (**0.3941293 tokens/s**), with exact
output SHA-256 `8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`,
49/49 layers, no diagnostics, and zero GPU fallback. Minimum available RAM was
21,700 MiB; swap grew by 711 MiB. This used direct host input, not the allocated
host cache.

The result is below the separately measured 0.4626948 tokens/s BP16 result.
Runs were sequential with uncontrolled clocks and background load; this is not
a speed improvement claim. Cached upload preference is experimental and off by
default.

The captured source commit is `1161e7dfb7cf516cf04382f99e993cabae35ea16`.
The run's source hashes are in `result.json.gz`; the BP16 decoder and layer
source hashes match that commit. Runtime binary hashes are in
`runtime-binary-sha256.json`. `run.py` records environment and guard settings.
The system-wide RAM floor was 16,384 MiB and the swap-growth guard was 16 GiB;
no Ollama GPU process was detected.

See [command](command.json), [result](result.json.gz),
[resources](automatic.resources.json), [stderr](automatic.stderr.txt.gz),
[stdout](automatic.stdout.txt.gz), and [runtime hashes](runtime-binary-sha256.json).
