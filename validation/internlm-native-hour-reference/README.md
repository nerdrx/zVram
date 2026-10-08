# Fresh native 92-token reference attempt stopped at RAM guard

This attempted native Vulkan reference produced **no accepted decode rate and
no completed tokens**. The run monitor stopped before application work when
`MemAvailable` reached 16,365 MiB, below the configured 16,384 MiB floor; the
result has no completed runs. Process return code was -9. Sampled system swap
grew by 319 MiB (31,821 to 32,140 MiB).

This guard stop does not replace the earlier accepted native 92-token result at
1.46258 tokens/s, which remains the separate prior reference. The guard-stop
result, command, resource sample, provenance, runner, compressed runtime logs,
and memory trace are preserved here.

`SHA256SUMS` covers every archive file except itself; verify with
`sha256sum -c SHA256SUMS` from this directory.
