# Steam pressure-vessel live-control export check

Validated 2026-10-09 with the source `zvram` launcher and installed SteamLinuxRuntime_4 pressure-vessel. CPU-only IPC test: no Vulkan instance/device was created, no GPU work was submitted, and no game was launched.

The launcher exported its private `ZVRAM_CONTROL_DIR` through `PRESSURE_VESSEL_FILESYSTEMS_RW`. A temporary C++ helper using `live_control.hpp` ran inside pressure-vessel, initialized `Endpoint` for device token 42, and published a capable status. Host-side `zvram_control.status()` discovered the endpoint and matched its PID/start tick to `/proc`; `zvram_control.request()` wrote sequence 1 for 64 MiB. The in-container helper read that request and exited successfully.

Observed host result:

```json
{"discovered":[{"ack":0,"capable":true,"current_limit_mib":128,"device":"42","max_limit_mib":2048,"min_limit_mib":32,"pid":3194715,"reason":0,"requested_mib":0,"resident_mib":64,"result":0,"seq":0,"start":11465610,"version":1}],"pid":3194715,"requests":[{"device":"42","resident_mib":64,"seq":1}],"start":11465610}
```

Observed helper result:

```text
published
request seq=1 resident_mib=64
```

The helper was compiled outside the repository with:

```sh
g++ -std=c++17 -O2 -I. /tmp/zvram-pv-control.1NOj0I/control_roundtrip.cpp -o /tmp/zvram-pv-control.1NOj0I/control_roundtrip
```

It was launched through the source launcher and real pressure-vessel runtime:

```sh
ZVRAM_CONTROL_DIR=/tmp/zvram-pv-control.1NOj0I/control ./zvram --build-dir build --no-live-control -- \
  /home/nerdrx/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4/pressure-vessel/bin/pressure-vessel-wrap \
  --variable-dir=/tmp/zvram-pv-control.1NOj0I/pv \
  --runtime=/home/nerdrx/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4/steamrt4_platform_4.0.20260805.254769 -- \
  /tmp/zvram-pv-control.1NOj0I/control_roundtrip
```

The host request used the repository's `zvram_control.status()` and `zvram_control.request()` functions with a minimal in-memory manager adapter. All helper, runtime fixture, and control files were temporary and removed after the check. This verifies the control-directory mount and file protocol; it does not verify live Vulkan behavior or a VRChat session.
