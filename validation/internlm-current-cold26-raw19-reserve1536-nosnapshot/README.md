# 19 GiB raw, 1.5 GiB reserve, snapshots-off load failure

The current binary attempted to load the 92-token InternLM2.5-20B F16
workload with a 26 GiB cold/owner ceiling, 19 GiB raw hard cap, and 1.5 GiB
headroom reserve. It exited after about 7 seconds with Vulkan
`ErrorOutOfDeviceMemory` during model loading, before input or a throughput
result. Logs show the available driver budget shrank between admission and
restore again; this is a failed load, not a performance measurement.

The captured command, stderr, resources, memory trace, runtime commit, binary
hash, and helper scripts are preserved here.
