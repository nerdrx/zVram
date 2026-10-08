# BP16 22 GiB cold/cache quota run

The full InternLM2.5-20B F16 run changed the total cold/clean-cache quota to
22 GiB; the 8 GiB allocated-host input cache and 19 GiB tracked-residency cap
were unchanged. With a 2.5 GiB reserve and 32 BP16 encoding workers, it
completed 12 decode runs in 27,371.66 ms (**0.4384097 tokens/s**). Output was
exact (SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), all
49/49 layers were offloaded, and there was zero GPU fallback. Minimum available
RAM was 19,104 MiB; swap grew by 2,104 MiB.

The final snapshot reported 1,269 cache invalidations, 6,884 clean reuses, and
70,919,913,472 copied bytes. This sequential result does not establish a speed
improvement over the best **0.4626948 tokens/s** run; clocks and background
conditions were uncontrolled.

See [command](command.json), [result](result.json.gz),
[resources](automatic.resources.json), [stderr](automatic.stderr.txt.gz),
[stdout](automatic.stdout.txt.gz), and [runtime hashes](runtime-binary-sha256.json).
