# Failed 13 GiB local-owner / 6 GiB shared-raw model attempt

This 92-token InternLM2.5-20B F16 run did **not** complete and has no accepted
throughput or output result. The process exited **-6** after prompting with
`vk::Queue::submit: ErrorOutOfDeviceMemory`. Minimum available RAM was
**28,833 MiB** and swap growth was **100 MiB**; the Ollama GPU guard detected no
other model.

The experiment set a 13 GiB local-owner quota plus 6 GiB shared/raw quota, with
a 32 GiB cold/owner ceiling and 6 GiB resident limit. Telemetry shows the local
owner tier was active and nearly full (peak **13,958,403,872 / 13,958,643,712
bytes**), with **205,698,410,288 cumulative local-owner encoded-input bytes**.
It also recorded zero GPU decoder fallback before the failure. This demonstrates
that the physical local tier was exercised, but the raw 6 GiB allowance did not
make the attempted model run complete. These cumulative bytes are not resident
memory.

The captured layer binary hash is
`bd021861754b40285381cb3cda814347c8cf79ff0c819b5244b922e3b2bf4494`; its
runtime commit was `782685685acb71d698f12faa14a30acb8cf99b0a`. The controller
captured source hashes at run start. Large stderr is gzip-compressed; original
file sizes and hashes are in `original-bytes-sha256.json`.
