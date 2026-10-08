# Guard-stopped uncapped-clock zVram run

No prompt was sent and no throughput result exists. The run stopped after 77.50 seconds when `MemAvailable` reached 16,376 MiB, below the 16,384 MiB guard floor. Return code was -9; swap growth was 466 MiB and the Ollama GPU check was empty.

Configuration: 18 GiB cold/owner, 17 GiB raw resident hard cap, 2 GiB compressed local owner within a 19 GiB shared cap, and 1,536 MiB headroom estimate. Snapshotting was enabled. The 78-sample memory log reports an AMD hwmon memory clock of 1,300 MHz in the first, middle, and final samples.

A contemporaneous post-run snapshot reported about 32 GiB available system memory versus about 42 GiB initially. This is background context only, not evidence of a cause. This archive records a guard stop, not a performance or code-regression result.

The captured command, source/runtime provenance, and resource report are included. `automatic.stderr.txt` and `memory.jsonl` are gzip-compressed; `compressed-originals.json` records their original byte counts and SHA-256 hashes.
