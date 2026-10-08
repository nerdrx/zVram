# Qwen 27B Vulkan forced-residency pressure run

This archive records a same-prompt native-versus-zVram run of the unchanged
Qwen 27B Q4 model. Both processes completed 94 decode tokens, offloaded 66/66
layers, and produced the same stdout SHA-256:

`ab00fd0d30f1e0c2eb8dd044f34a85228adc47951e04517bd24d53092b0a5b9d`

| Run | Configuration | Decode rate | Minimum available RAM | Swap growth |
| --- | --- | ---: | ---: | ---: |
| Native Vulkan | 66/66 layers; about 15,088 MiB model buffers | 34.56 tokens/s | 27,147 MiB | 1 MiB |
| zVram BP16 | Forced 12 GiB tracked-residency ceiling; 8 GiB owner and cold ceilings; 1.5 GiB headroom reserve | 1.78 tokens/s | 23,697 MiB | 0 MiB |

The BP16 profile reported 237 GPU encoder calls over 293 GiB of raw input.
The final cumulative runtime counters recorded 342,325,002,240 bytes copied
(35.758 s), 334,474,575,872 bytes CPU-decoded (20.660 s), and 1,884,422,144
bytes GPU-decoded (91.95 ms). These are component counters for this run, not
end-to-end token-rate estimates or isolated speedups. The run used a forced
residency limit and spilled to host backing; it does not establish a general
optimization, prove a 40 GiB model fits, or establish broad workload behavior.

The runs were sequential. Desktop/background activity was uncontrolled, and
system swap was already about 20 GiB in use at baseline; the reported swap
figure is only growth during each run. The resource monitor sampled system-wide
state and was not a hard allocation cap. A smaller earlier 32 GiB/12 GiB
profile run is not included here and is not used as a comparison.

## Reproduction and provenance

`native-command.json` and `automatic-command.json` preserve the exact argument
vectors; their matching `*-env.json` files contain the filtered environment.
The forced run used 32 BP16 workers, GPU encoding, eight upload workers,
allocated-host input up to 8 GiB, MRU residency eviction, and clean-first
behavior. `source.patch` and `provenance.json` preserve the recorded source
change and source/build hashes. `run.py`, `result.json`, resource samples, and
fdinfo snapshots retain the runner and measured details.

Large logs and the memory trace are gzip-compressed. `SHA256SUMS` covers every
archive file except itself; verify with `sha256sum -c SHA256SUMS` from this
directory.

The complete structured result is compressed in `result.json.gz`; `summary.json` keeps the rates, resource guards, and output hashes readable.
