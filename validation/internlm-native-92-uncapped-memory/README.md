# Native 92-token run after VRAM clock-cap removal

This native (no zVram paging or compression) run completed 92 decode tokens in 62,902.69 ms: 1.46258 tokens/s. It produced the expected stdout hash `b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, with 49/49 layers offloaded, exit code 0, minimum available RAM 17,926 MiB, and swap growth 1,181 MiB.

The monitor sampled AMD hwmon `freq2_input` at 1,300 MHz for the memory clock throughout this run; `freq1_input` (core clock) varied from 1,898 to 3,022 MHz. The DPM listing still shows its 772 MHz step, so the hwmon frequency is the relevant observed clock value here. This was run after the user removed the memory-clock cap. It measured slower than the earlier capped native result (1.53286 tokens/s), so there was no observed speed gain in this sequential comparison. The runs were not a controlled clock-only experiment; do not infer that uncapping caused the difference.

The archived command, runtime binary hash, resources, controller output, and 138-sample memory/clock trace are included. `native.stderr.txt` and `memory.jsonl` are gzip-compressed; `compressed-originals.json` records original byte counts and SHA-256 hashes.
