# BP16 24 GiB cold/cache quota run

This full InternLM2.5-20B F16 run used a **24 GiB** cold/clean-cache quota,
with the 8 GiB allocated-host input cache, 19 GiB tracked-residency cap, and
2.5 GiB headroom reserve unchanged. With 32 BP16 encoding workers, it completed
12 decode runs in **22,746.94 ms** (**0.5275435 tokens/s**). Output was exact
(SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), all
49/49 layers were offloaded, and GPU fallback was zero. Minimum available RAM
was **18,578 MiB**; swap grew by **1,232 MiB**.

The sampled live allocated-host cache peaked at **8,589,891,104** of
**8,589,934,592 bytes**. Final snapshot counters were 735 invalidations, 7,309
clean reuses, and 56,357,421,056 copied bytes. The final GPU profile reported
8,458 calls, matching the final snapshot's 8,458 GPU restores.

This was one sequential run with unlocked clocks and uncontrolled background
conditions. It is the current observed best at the time of this record, not a
repeatable or causal claim that the larger cold quota caused the rate change.

The run marker records source commit `b8905208025aa9672d704e48f0dbe2226f2ebc6e`.
The runtime layer binary hash is `210495673549320b99351682e3678db23b9f973a29063bed083d288d8515beed`,
identical to the previous 10 GiB and 22 GiB runs; that library was built from
the `a8d113b` runtime code. The result also preserves the contemporaneous
`gdeflate_gpu.hpp` source hash (`2db3a70a…`), which differs from the tracked
header at those commits (`518eaa94…`) due to concurrent source editing. The
archive therefore does not claim that the recorded header snapshot exactly
matches the runtime binary.

See [command](command.json), [result](result.json.gz),
[resources](automatic.resources.json), [stderr](automatic.stderr.txt.gz),
[stdout](automatic.stdout.txt.gz), and [runtime hashes](runtime-binary-sha256.json).
