# BP16 26 GiB repeat: Vulkan allocation failure

A repeat of the completed 26 GiB/19 GiB-resident/2.5 GiB-reserve configuration
reached the prompt but terminated with `vk::Queue::submit:
ErrorOutOfDeviceMemory` after **69.67 seconds**. It had minimum available RAM
of **27,077 MiB**, zero swap growth, and no Ollama GPU model detected. No
throughput result or completed output comparison exists. Root reported no
kernel GPU reset. The failing Vulkan allocation stage is unknown.

The launched runtime was the unchanged `dba6a62` binary from the successful
run; source changes visible on disk during the repeat were still unbuilt and
are recorded separately in `automatic-result.json`. This single failure does
not establish that the successful configuration is unstable, but the 0.889
result has not yet been reproduced. This archive is a failure diagnostic only.

The captured runtime binary hash is copied from the original safe26 run and
confirms the same `dba6a62` layer. The source-context record preserves the
separate dirty on-disk source hashes; those edits were not in the loaded binary.
