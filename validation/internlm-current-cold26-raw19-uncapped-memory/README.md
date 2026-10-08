# Guard-stopped uncapped-clock zVram run

This run produced no accepted throughput result. After 124.98 seconds, the RAM guard stopped the process when `MemAvailable` reached 16,378 MiB, below its 16,384 MiB floor. The prompt had been sent, but the run has no completed token count/output hash. It ended with return code -9; swap grew by 2,865 MiB, and the Ollama GPU check was empty.

The run used a 26 GiB cold/owner ceiling, 19 GiB raw resident hard cap, and 1,536 MiB headroom estimate. It sampled 126 system snapshots. The AMD hwmon memory-clock reading was 1,300 MHz in the first, middle, and last samples; core clock varied. This is a guard-stop, not a comparable performance measurement.

The user had removed the VRAM clock cap before this run. Krita was also active; the contemporaneous report noted about 1.2 GB used by Krita and roughly 38 GiB available system memory after the run, versus about 42 GiB earlier. These are background conditions, not proof of why the guard was reached.

`runtime-commit.txt` and `runtime-binary-sha256.txt` identify the compiled binary. Source hashes in `automatic-result.json` describe the captured source context separately. `automatic.stderr.txt` and `memory.jsonl` are gzip-compressed; `compressed-originals.json` records original byte counts and SHA-256 hashes.
