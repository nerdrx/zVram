# zVram Manager

![Standalone NX-themed zVram Manager](../validation/manager-bridge-smoke/manager.png)

The GUI and TUI show managed profiles and detect other same-user zVram launches,
including Steam launch options and terminal commands. Detected external apps
show PID, RSS and available DRM memory accounting, but remain read-only: use
their original launcher to stop them or change the next launch. Ordinary apps
without zVram stay hidden and keep normal driver behavior. No root daemon,
system GPU settings, or Ollama service changes are required.

```sh
zvram gui
zvram tui
zvram manage status
```

High, normal, and low are launch-time eligible-buffer cap presets: 85%, 50%,
and 25% of the largest detected GPU's VRAM. An explicit Resident MiB value overrides
the preset. The TUI priority action clears that override. Profile changes
apply on the next launch, not to running processes. These caps are neither
physical VRAM reservations nor a global priority scheduler. Native mode adds
no layer or cap; wrapped mode uses an existing zVram command.

For Steam, opt into the managed launcher explicitly:

```text
zvram run --name vrchat --priority high -- %command%
```

This selects experimental range paging with a launch cap and native-budget
headroom. Test per game; tracked buffers only, with images and unknown access
outside the narrow paging guarantee. The original `zvram --vulkan-virtual-gib
96 -- %command%` remains available and appears as a detected read-only app.
Discovery refreshes about every two seconds. A configured launch environment
does not prove that the app has loaded the Vulkan layer; process details
distinguish configured launches from a mapped zVram backend. Process inspection
restrictions may prevent discovery or hide memory counters.
Programs must remain in the foreground; launchers that detach their game or
server are not supported for reliable stop/accounting.

GUI telemetry shows driver-reported physical GPU VRAM/GTT totals, system RAM
and swap, process RSS, and process DRM allocation accounting. The latter can
exceed physical VRAM and does not prove local residency. Limits are for tracked
allocations; they exclude driver overhead. Stop sends TERM to an owned worker,
which terminates its child session; SIGSTOP is not used because it retains VRAM.
Existing models and unrelated programs are never stopped automatically.

The worker stops its own process when available RAM falls below its floor or
system swap grows by more than 4 GiB from launch. Defaults: 16 GiB available
RAM for range-paged launches, 4 GiB otherwise. System swap includes unrelated
activity, so the guard is deliberately conservative. Logs show the reason.
This guard cannot guarantee an application or driver will not stall or OOM.

Profiles and logs live under `$XDG_STATE_HOME/zvram` (normally
`~/.local/state/zvram`), privately owned by the current user. Commands are
argument arrays and never invoke a shell. No login autostart is installed.

## Models in Novum Xenium

```sh
zvram model list
zvram model setup --model /path/model.gguf --alias zvram-my-model \
  --name my-model --compressed --resident-mib 12288 --cold-mib 8192 --port 8097
zvram manage start my-model
# Once the server is healthy:
zvram model register --alias zvram-my-model --port 8097
zvram manage stop my-model
```

The setup action saves a profile without launching it. Registration creates a
separate provider in Novum and leaves the current model/default untouched.
Use a different port for each simultaneously running model. The build needs
the optional BP16 GPU codec for compressed profiles. See the
[Novum handoff](novum-xenium-integration.md) for the provider integration API.
