# BP16 clean-first repeat

A repeated 92-token InternLM2.5-20B F16 run used the clean-first resident-victim
preference with synchronous GPU BP16 encoding. It completed in **78,586.29 ms**
(`92,000 / 78,586.29 = 1.17068766 tokens/s`), with exact stdout SHA-256
`b8803c0156cf91c4c8f6af68959d503e61206de7c8962f197d479950f146587b`, **49/49**
layers offloaded, and zero GPU restore fallback. Minimum available RAM was
**28,435 MiB (27.77 GiB)**; swap growth was **0 MiB**.

The run recorded **932 GPU encodes / 30,762,008,576 raw bytes / 2.865278391 s
encoder host time**, zero encoder fallback, and zero RAW snapshots. It accepted
**1,450,967,326,064 encoded input bytes**; dividing by the **52.6678616 s**
device decode phase gives **27.55 GB/s logical encoded payload per decode
second**, not measured PCIe wire throughput.

The earlier clean-first run measured **1.16969830 tokens/s**; both produced the
same exact output and had zero swap growth. The pair is about **6.1–6.2% higher**
than the prior repeated GPU-encoder run at **1.10252711 tokens/s**. These are
sequential observations with uncontrolled clocks and background activity, so
they do not establish clean-first as the cause.

This repeat used the same 26 GiB cold/owner ceilings, 19 GiB tracked residency,
2.5 GiB reserve, LFU, 32 encoder workers, eight upload workers, and
immutable-owner validation cache. Captured source/runtime metadata corresponds
to compiled lean runtime commit `a19bd5d`. Large JSON and stderr files are
gzip-compressed; `original-bytes-sha256.json` records the pre-compression sizes
and hashes.
