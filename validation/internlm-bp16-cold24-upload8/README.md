# BP16 cold-quota run with eight upload-copy workers

This full InternLM2.5-20B F16 run used the 24 GiB cold/clean-cache quota, 19 GiB
tracked-residency cap, 2.5 GiB headroom reserve, 8 GiB allocated-host input-cache
limit, 32 BP16 encoding workers, and the opt-in eight upload-copy workers. It
completed 12 decode runs in **19,665.62 ms** (`12,000 / 19,665.62 =
0.61020197 tokens/s`, reported as **0.61**). Output matched the archived BP16
runs byte-for-byte (SHA-256
`8ac12258546a6f05dd7ff9cab38e38b4e85fdfe918c178ba14bcb38dd0b7f04b`), all
**49/49** layers were offloaded, and there were zero GPU fallbacks or
diagnostics. Minimum available RAM was **18,212 MiB**; swap grew by **1,062 MiB**.

This is one sequential run with uncontrolled clocks and background activity.
It does not establish that eight upload workers caused the rate difference from
the earlier 24 GiB run. The production shader was unchanged.

The marker records source commit `9f2fa795685356bd9eafbe817842338bc97dcd18`.
The per-file source hashes in `result.json.gz` match that committed tree. The
runtime layer library SHA-256, a separate binary identity, is
`d745d704135f7af4e3a57c3df1180c5bee9af3ff42d2343dedc9b191981829f8`. Source
revision and runtime binary hash are recorded separately; the binary hash is
unchanged from the earlier worker-regression build. The record does not claim
that the worker option alone explains this result.

[Command](command.json), [result](result.json.gz),
[resource samples](automatic.resources.json),
[stderr](automatic.stderr.txt.gz), [stdout](automatic.stdout.txt.gz),
[runtime hashes](runtime-binary-sha256.json), and [source revision](source-commit.txt).
