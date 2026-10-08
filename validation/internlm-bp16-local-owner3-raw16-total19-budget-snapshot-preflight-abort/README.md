# Preflight abort before budget-snapshot model run

The initial attempt was correctly rejected before prompting because Ollama had
`qwen3.5:9b-local` active with **6,113,858,682 bytes** of VRAM. No run result or
throughput was produced. Ollama's model list was empty shortly afterward; no
unload action was needed before retry. Original files and hashes are preserved.
