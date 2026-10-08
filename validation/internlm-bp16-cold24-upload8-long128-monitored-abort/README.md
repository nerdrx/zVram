# BP16 long generation stopped at the RAM guard

This monitored baseline repeated the 24 GiB cold-quota, eight-upload-worker
long-generation setup with `MALLOC_ARENA_MAX=2` and no mmap/trim threshold
overrides. It sent the prompt, then the RAM guard stopped the run
after `MemAvailable` reached **16,383 MiB**, below the **16,384 MiB** floor.
The process ran for **161.04 s**; swap grew by **2,781 MiB**. The incomplete
reply has no accepted decode rate, output hash comparison, or completed
workload result. Ollama GPU detection was empty.

The run preserves one-second `memory.jsonl` process-tree, system-memory, and
DRM-fdinfo samples, plus the `monitor_restore_memory.py` sampler used for
this record. Runtime library SHA-256 is `dc3fb518c5df65cc4b302abf89f2a8d502fb96d5b122ee2e4485ec0b68b1f46f`;
BP16 shader SHA-256 is `246b5e7f5d5893a1137e31141e7ba41b9bc2b109fae91d671e2cea3b89eff854`.
The llama process peaked at **19.60 GiB RSS** (**19.55 GiB anonymous**), with
about **48 MiB file-backed**; the last pre-exit sample was **18.66 GiB RSS**
and **1.53 GiB swap**. `MemAvailable` rose from **16.01 GiB** to **45.59 GiB**
one second after process exit. Ollama remained empty in the samples. The
process DRM counters peaked at about **19.08 GiB VRAM** and **9.16 GiB GTT**.
These observations do not isolate allocator fragmentation or driver memory
accounting as the cause. The 8 GiB host-input cache is a subset of the 24 GiB
cold quota, not additional to it.
The launch source commit and per-file source hashes are recorded in the result;
they match checkout `a6c2053`. The loaded profiling runtime is separately
identified as code commit `4d1b704`.

[Result](result.json), [command](command.json),
[resource samples](automatic.resources.json), [memory samples](memory.jsonl),
[stderr](automatic.stderr.txt.gz), [stdout](automatic.stdout.txt.gz),
[runtime binary hashes](runtime-binary-sha256.json), and
[launch source](source-commit.txt).

The follow-up with child-only 128 KiB mmap/trim thresholds completed the same
92-token output, but this threshold-unset baseline did not complete and has no
rate; the pair cannot establish an allocator speed effect. [Follow-up archive](../internlm-bp16-cold24-upload8-long128-mmap/README.md).
