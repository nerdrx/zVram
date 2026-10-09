# Userspace manager and Novum bridge smoke

The current SmolLM2 135M F16 GGUF loaded through the BP16 Vulkan range profile
at a 256 MiB eligible resident cap, 1 GiB cold ceiling and 256 MiB clean-cache
cap. Its loopback llama-server answered `Hello!` (three output tokens) and
reported healthy. The dedicated `zvram-smollm2-135m` alias was registered and
verified visible through Novum Xenium's authenticated model picker API.

Numeric paging logs show compression and GPU restores. This is one tiny-model
server smoke, not a throughput benchmark or proof for larger model servers,
long chats, simultaneous games, or universal VRAM priority. The Qwen 27B server
profile was prepared but not launched in this task.

Six CPU unit checks passed, covering owned-process stop, unrelated process
isolation, literal argument handling, pressure cleanup, cookie parsing and
endpoint registration guards. UI field checks and a real PTY TUI interaction
passed. Actual Tk widget checks ran in headless gamescope; the manager's
current live status was captured separately without starting or stopping jobs.
The screenshot records transient hardware/system state, not a model memory
baseline. Existing system swap was already present before this smoke.

The GUI uses Novum's dark/violet palette. GPU physical totals are distinct
from per-process DRM allocation accounting. Priority presets apply on next
launch and explicit MiB caps override them. No privileged service or global
GPU scheduling change was made.
