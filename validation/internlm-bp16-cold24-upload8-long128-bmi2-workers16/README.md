# BP16 BMI2 long run with 16 encoding workers

This 92-token InternLM2.5-20B F16 run used the BMI2-inline BP16 encoder, 16
encoding workers, eight upload-copy workers, a 24 GiB cold quota, a 19 GiB
tracked cap, 2.5 GiB reserve, and child-only 128 KiB glibc mmap/trim thresholds.
It completed in **190,601.02 ms** (**0.48268367 tokens/s**),
with exact output (SHA-256 `b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`), **49/49** layers, and zero GPU
fallback. Minimum available RAM was **18,668 MiB** and
swap grew by **3,120 MiB**.

This sequential run was slower than the 32-worker BMI2 long run
(**0.5727251 tokens/s**). Clocks and background activity were uncontrolled;
this one pair does not establish a general worker-count effect. The short-run
headline remains unchanged. Runtime source and binary hashes are recorded
separately.

[Command](command.json), [result](result.json.gz),
[resource samples](automatic.resources.json), [process-memory samples](memory.jsonl),
[monitor script](monitor_restore_memory.py), [stderr](automatic.stderr.txt.gz),
[stdout](automatic.stdout.txt.gz), [runtime hashes](runtime-binary-sha256.json),
and [source revision](source-commit.txt).
