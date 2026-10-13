# Compatibility work until 08:00 Berlin

User authorized compatibility improvements until 2026-10-14 08:00 Europe/Berlin (06:00 UTC). Stop new work at that deadline; pause heartbeat `zvram-game-and-app-performance-five-hours` after reading its actual TOML. Earlier continue-until-stopped instructions are superseded. No merge, release, installation, or experimental default changes in this task.

## Current state

- Repo: `/home/nerdrx/Documents/ChatGPT/NX/zvram`.
- Branch: `fix/compatibility-oct14`, based on main `333b9ec` plus completed fixture commit `834f386`.
- Installed/published v0.4.21 includes merged PR1 and PR2. PR3 (`fix/ollama-vision-projectors`) is separate; do not duplicate it.
- Recovery-churn fixture and raw evidence committed as `834f386`; both quiet settings passed. Do not repeat unchanged GPU timing runs.
- Preserve untracked `memtest_vulkan.log`.
- Current GPU settings observed manual on discrete GPU and auto on integrated GPU; preserve exactly, do not reset to older remembered settings. Two existing llama-server processes and ollama daemon belong to the user; never stop them.
- Last fresh test health: ~27 GiB available, memory PSI avg10 zero, discrete GPU idle with ~12 GiB VRAM headroom. Refresh before any new GPU run.
- No root-owned running build/GPU jobs.

## Current progress and next work

- Launcher diagnostic committed as `c757cb7`; root reviewed and all 16 package/manager/control tests passed.
- Maintenance4 core/KHR query fix and standalone consistency checker completed; normal build, 14 CPU tests, before/after real-driver query comparison, and automatic-range query case all passed. See README.md and raw evidence. Committed `3442397`, pushed; draft PR https://github.com/nerdrx/zVram/pull/4. Check CI once on a future pass.
- Map2 audit complete: no fix needed. Valid map2/unmap2 memory is host-visible and remains an unwrapped native handle; synthetic/wrapped memory is GPU-only and cannot validly use mapping APIs.
- Maintenance6 fix completed and root-reviewed: enabled feature tracks KHR/core1.4, valid status chain accepted for promoted buffers, all enabled batch entries attempted with per-bind results. All 14 normal CPU tests passed in 5.09s; test-only older-header mock guard corrected and bootstrap rebuilt/passed afterward. Tiny same-checker old-backend expected failure/current normal PASS 0.05s; both bind statuses SUCCESS, validation clean, final live local/nonlocal zero. Normal hooks OFF/six APIs absent. See maintenance6 evidence and README. Ready to commit/push in this pass.
- Previous 3442397 PR4 CI both SUCCESS: 38482501131 and 38482508048 checked 23:21 UTC.
- `/root/auto_docs_update` now has a bounded READONLY CPU-only launcher Steam/Proton command/env/exit propagation audit. Other owners holding. No new production edits or GPU authorized for that audit.
- Other agents completed/holding. No root-owned running jobs. No GPU timing reruns needed.

## Operating constraints

Use Ponytail minimal changes, concise Caveman communication, and RTK for commands. Root GPU only; agents CPU only. Small focused tests, synchronization validation, fresh PSI/RAM/GPU/apps first. Presentation tests hidden/headless Gamescope. Never heavy models, RAM/swap fill, swapoff, disruptive stall reproduction, app termination, or GPU setting changes. Report exact coverage; no game FPS or broad compatibility claims from fixtures. Save real failures honestly. Update this file after meaningful progress so heartbeat resumes without duplicate work.

## Completed checks

- Baseline package, manager, and control tests: 15 passed in 3.878 s. Raw log `python-baseline.txt`.
- Reference: https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceBufferMemoryRequirements.html and https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceBufferMemoryRequirements.html.
