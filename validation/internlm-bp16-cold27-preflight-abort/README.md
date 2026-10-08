# BP16 27 GiB preflight abort

The run was rejected before prompting because the preflight detected Ollama's
`qwen3.5:9b-local` using 6,113,858,682 bytes of VRAM. The child exited with
signal status -9; no model throughput, output, or GPU restore result was
produced. This is a safety-preflight record only, not a failed inference or
performance result.

The command requested 27 GiB cold/owner limits, 19 GiB tracked residency, a
2.5 GiB headroom reserve, LFU, 32 BP16 encoder workers, and 16 GiB available-RAM
and swap-growth guards. See the captured command, resource report, logs, and
runtime/source provenance in this directory.
