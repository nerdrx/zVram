# BP16 CPU-synchronous encoder comparator

This 92-token InternLM2.5-20B F16 run used the same compiled runtime
(`23787c6`, layer SHA-256 `cbd97596f9202aaeb699fb40e78229893a7a337193f83e7667dd1246e6d4c6c4`)
and same 26 GiB cold/owner ceilings, 19 GiB tracked residency, 2.5 GiB
reserve, LFU, 32 workers, eight upload workers, and immutable-owner cache as
the GPU-encoder repeat. GPU encoding was disabled and async compression was not
requested, so snapshot encoding used the synchronous CPU path.

It completed in **107,649.86 ms** (`92,000 / 107,649.86 = 0.85462257
tokens/s`) with the same exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, **49/49**
layers, and zero GPU restore fallback. Minimum available RAM was **18,494 MiB**
and swap grew by **2,786 MiB**. The GPU-encoder repeat measured **1.10252711
tokens/s**, about **1.29x** this observed rate. Runs were sequential with
uncontrolled clocks and desktop activity, so this does not isolate a causal
performance difference. No output or fallback regression was observed.

The raw command, result, resource samples, stdout/stderr, fdinfo, runtime binary
hash, and source-context record are preserved here. The BP16 decoder and both
encoder SPIR-V hashes match the GPU-encoder repeat.
