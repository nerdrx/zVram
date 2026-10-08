# BP16 restore batching: Ollama preflight abort

This attempted the same opt-in four-frame restore batching configuration as
the successful archive, but the harness stopped before prompting because
Ollama had `qwen3.5:9b-local` active with **6,113,858,682 bytes of VRAM**. It
produced no model output or throughput result. This is a preflight diagnostic,
not a batching failure or GPU correctness result.

Files preserve the command, runtime/source metadata, resource sample, and
original-byte hashes.
