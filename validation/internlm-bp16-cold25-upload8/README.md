# BP16 cold-quota follow-up with eight upload workers

This full InternLM2.5-20B F16 run used the 25 GiB cold/cache quota and the
experimental eight-worker BP16 upload-copy setting. It completed 12 decode
runs in **20,222.13 ms** (**0.59340930 tokens/s**,
reported as **0.59**). Output matched the established BP16 output exactly
(SHA-256 `8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), all **49/49** layers were offloaded, and there were no GPU
fallbacks or diagnostics. Minimum available RAM was **18,123 MiB**;
swap grew by **621 MiB**.

This sequential run used a different cold quota than the earlier 24 GiB
worker comparison. It was slower than the current best **0.61020197 tokens/s**
observation, so it does not replace that result or show a benefit from the
quota change. GPU clocks and background activity were uncontrolled. The
loaded layer library SHA-256 is recorded separately from the source checkout;
`runtime-source-context.json` identifies runtime code commit `4d1b704`, while
`source-commit.txt` records the launch checkout.

[Command](command.json), [result](result.json.gz),
[resource samples](automatic.resources.json), [stderr](automatic.stderr.txt.gz),
[stdout](automatic.stdout.txt.gz), [runtime hashes](runtime-binary-sha256.json),
and [source revision](source-commit.txt).
