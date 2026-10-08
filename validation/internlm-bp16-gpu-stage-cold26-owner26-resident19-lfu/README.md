# BP16 owned-input staging: full-model experiment (removed)

This historical prototype staged each owned encoded BP16 frame into a
device-local VRAM buffer before decode. It completed 92 decode runs in
**181,172.59 ms** (`92,000 / 181,172.59 = 0.50780308 tokens/s`), with exact
stdout SHA-256 `b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`,
**49/49** layers offloaded, and zero GPU restore fallback. Minimum available RAM
was **17,758 MiB** and swap grew by **3,776 MiB**. Final telemetry recorded
**55,328 staging calls / 1,430,646,733,584 bytes**, **147.43346 s** transfer,
**7.26184 s** decode, and **170.88023 s** host restore time.

The repeated GPU-encoder run measured **1.10252711 tokens/s** under the same
model workload, versus **0.50780308** here. This sequential comparison is not a
controlled causal test, but the observed result was substantially slower; the
staging prototype was removed from the current runtime and is not a supported
CLI mode. The archived runtime/source metadata describes commit `2ddefdb`; the
implementation patch is also saved with the focused correctness logs in
[the prototype checks](../bp16-owned-input-staging/).

The run used 26 GiB cold/owner ceilings, 19 GiB tracked residency, a 2.5 GiB
reserve, LFU, 32 encoder workers, eight upload workers, and immutable-owner
validation caching. Large JSON and stderr files are gzip compressed;
`original-bytes-sha256.json` records original sizes and SHA-256 values before
compression.
