# Admission-snapshot launch skipped by preflight

The attempted 92-token InternLM2.5-20B F16 run used the 26 GiB cold/owner
ceiling, 19 GiB raw hard cap, 1.5 GiB headroom reserve, and admission-budget
snapshots enabled. The launch stopped before starting the model because Ollama
reported `qwen3.5:9b-local` using 6,113,858,682 bytes of VRAM. This is a
preflight skip, not a snapshot test or a throughput result.

The captured command, preflight result, resources, memory trace, runtime
commit, binary hash, and helper scripts are preserved here.
