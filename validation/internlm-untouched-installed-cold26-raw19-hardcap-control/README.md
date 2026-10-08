# Untouched installed binary: guarded stop

This run has no accepted throughput result. It stopped after 115.9 seconds when system-wide `MemAvailable` reached 16,379 MiB, below the 16,384 MiB guard floor. The prompt had been sent, but there is no completed token count or output hash. The process was stopped by the guard (`returncode: -9`); swap growth was 1,554 MiB, and the Ollama GPU guard found no active model.

The run used the previously validated installed binary from commit `6d310fbcb3ba7714c85e2c4f8a8ff7d8610ea31a` (layer SHA-256 `49c5119a8bd87a153f17c9f4a4dad37b4fc551079f41c8ff2473a5afba28b19a`). It omitted dynamic headroom estimation while retaining the 19 GiB resident hard cap. The current worktree's source hashes in `automatic-result.json` are recorded separately from the compiled runtime provenance; they are not claims about the binary's build inputs.

The run overlapped removal of the GPU memory-clock cap around 07:26 UTC, according to the incident record. The monitor log records changing clock states during the run, so this is mixed-clock evidence. It is not a valid speed comparison. This old binary also hit the RAM guard under the newer machine conditions; that does not establish a code regression or a Vulkan allocation failure.

`automatic.stderr.txt` and `memory.jsonl` are gzip-compressed. `compressed-originals.json` records each original file's byte count and SHA-256 for integrity checks. The remaining JSON and logs preserve the command, guard outcome, and binary/source provenance.
