# Clean-first 20 GiB resident-cap attempt: out-of-device-memory

This attempt raised tracked residency to **20 GiB** while keeping the 26 GiB
cold/owner ceilings and **2.5 GiB** reserve. The process exited **-6** after
prompting with `vk::Queue::submit: ErrorOutOfDeviceMemory`; it produced no
completed model result or throughput rate. Available RAM stayed above the
16 GiB guard (minimum **29,738 MiB**), swap grew by **658 MiB**, and the Ollama
GPU guard detected no model.

The attempt used clean-first and GPU BP16 encoding. Captured runtime commit
`0237548` included the fence-spin prototype, but
`ZVRAM_VULKAN_BP16_SPIN_WAIT=0` kept that behavior off. The failed 20 GiB
setting is rejected for now; retain the previous **19 GiB** tracked-residency
profile. This diagnostic does not establish that every 20 GiB configuration
will fail.

Large stderr is gzip-compressed. `original-bytes-sha256.json` records original
file sizes and SHA-256 values before compression. Captured command,
source/runtime metadata, resources, and memory samples are preserved here.
