# Preflight stop before prompt

This earlier attempt did not reach model inference. The Ollama-GPU preflight
detected `qwen3.5:9b-local` using 6,113,858,682 bytes of VRAM and stopped the
child before the prompt was sent. It has no throughput result and is not a
GPU-decode failure.

See the [preflight result](automatic-result.json), [resource record](automatic.resources.json),
[command](automatic-command.json), and [source/runtime hashes](runtime-source-context.json).
