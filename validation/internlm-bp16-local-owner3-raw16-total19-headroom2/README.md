# Failed local-owner 3+16 GiB run with 2 GiB headroom reserve

This model-loading attempt exited **-6** with `vk::Queue::submit:
ErrorOutOfDeviceMemory` after about 23,000 restores. It has no accepted
throughput result. At the final captured point, driver budget, driver usage, and
tracked residency had each fallen by 32 MiB after a free, followed by an
incoming 40 MiB request. A stale budget/free-credit hypothesis is plausible but
not confirmed; do not treat this trace as proof of the cause.

The configuration was 3 GiB local-owner + 16 GiB shared/raw under a 19 GiB
combined cap, with a 2 GiB reserve. Original run files and hashes are retained;
large stderr and memory samples are compressed.
