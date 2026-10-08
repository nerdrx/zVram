# Live userspace control smoke (v0.4.0)

On RX 7900 XTX / RADV, `python3 test_live_control_vulkan.py` launched an external
Vulkan fixture with 64 MiB tracked backing. Manager requests for 96 MiB and
64 MiB received matching sequence acknowledgments and reported the requested
runtime cap during a hold after device creation, before buffer allocation.
The existing range fixture then verified every byte across two
cold cycles with zero validation errors. See [run log](vulkan.log).

Thirteen Python checks cover discovery, external pidfd Stop isolation, private
control parsing, symlink rejection, sequence generation, identity checks,
packaging and model commands. Thirteen CPU CTest checks passed, including
pending reductions, immediate increases, replay rejection, bounds, owner
reserves, permissions and endpoint cleanup. GUI widgets were exercised in
headless gamescope; external live/stop controls and managed PTY TUI checks passed.
The actual Hub engine installed an older release, updated to the new package,
kept an owned CPU job alive and stopped it through the new manager; uninstall
preserved user state and foreign files.

This is bounded API and buffer-integrity evidence, not a game or large-model
performance benchmark. Lowering stays pending until current tracked residency
fits. Subsequent working sets can still exceed a chosen cap. Old processes and
virtual-only launches need a restart with `--live-control` for live residency
controls. HIP live controls and cross-application driver scheduling are not
implemented.

## Default-mode follow-up (v0.4.1)

The same bounded hardware gate passed without `--live-control`: both live cap
requests were acknowledged and range-buffer integrity passed with zero Vulkan
validation errors. See [default-mode log](default-vulkan.log). Fourteen Python
checks and thirteen CPU CTest checks passed. Headless Tk and PTY TUI checks
passed. `--no-live-control` preserves low-level test/benchmark configurations;
HIP and explicit noncompressed model spill mode retain their behavior.
