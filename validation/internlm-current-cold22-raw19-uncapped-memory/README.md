# Guard-stopped uncapped-clock zVram run

No accepted throughput result was produced. The run stopped after 110.07 seconds when `MemAvailable` reached 16,377 MiB, below the 16,384 MiB guard floor. The prompt had been sent, but no completed token count or output hash is available. Return code was -9; swap growth was 2,932 MiB and the Ollama GPU check was empty.

Configuration: 22 GiB cold/owner ceiling, 19 GiB raw resident hard cap, and 1,536 MiB headroom estimate, with snapshotting enabled. The memory log contains 111 samples. AMD hwmon reported 1,300 MHz memory clock in the first, middle, and final samples. This remains a guard-stop, not a performance result.

The contemporaneous report put system-wide available memory around 35.6 GiB after the run. That background snapshot is context only and does not establish why the in-run guard threshold was crossed. No causal performance or memory-pressure conclusion is drawn.

The captured command, source/runtime provenance, and resource report are included. `automatic.stderr.txt` and `memory.jsonl` are gzip-compressed; `compressed-originals.json` records original byte counts and SHA-256 hashes.
