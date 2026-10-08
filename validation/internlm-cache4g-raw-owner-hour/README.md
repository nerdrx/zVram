# 4 GiB clean-cache-cap trial stopped at RAM guard

This run produced **no accepted model result or decode rate**. The monitor
stopped before application work when `MemAvailable` reached 16,374 MiB, below
the configured 16,384 MiB floor. The result record contains no completed runs.
The process return code was -9 and the sampled
system swap grew by 1,405 MiB before the stop;
this is a guard stop, not evidence of a model OOM or throughput regression.

The attempted profile used a 4 GiB clean-cache cap with the same compiled
library SHA-256
`04c6d673e812e7ebee1922e6e4b8fe43580bb663a44ed39b67d14788b2bf87d2`.
It does not establish a 4 GiB cache result, model fit, or raw-owner performance.
The completed 1 GiB-cap run is archived separately at
[validation/internlm-cache1g-raw-owner-hour](../internlm-cache1g-raw-owner-hour/README.md).

The small result, command, resource sample, fdinfo, source patch, provenance,
and `run.py` are preserved. The stderr log and memory trace are included as gzip files. The README/result/resource records remain short text for direct inspection. `SHA256SUMS` covers every file except itself;
verify with `sha256sum -c SHA256SUMS` from this directory.
