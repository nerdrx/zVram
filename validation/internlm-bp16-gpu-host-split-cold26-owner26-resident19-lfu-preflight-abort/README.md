# Preflight abort before restore timing run

The initial launch was stopped before prompting because Ollama had
`qwen3.5:9b-local` active with **6,113,858,682 bytes** of VRAM. It has no
inference result or throughput measurement. The model was unloaded before the
successful retry in the sibling archive. Original files and hashes are retained
for the preflight record.
