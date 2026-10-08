# BP16 GPU-encoder run: Ollama preflight stop

The first 26 GiB GPU-encoder launch was rejected before prompting because
`qwen3.5:9b-local` was using **6,113,858,682 bytes of VRAM**. It has no inference
or throughput result. This is a separate preflight diagnostic for the later
completed retry in the sibling archive.
