# Steam layer import check

Validated 2026-10-09 against installed zVram 0.4.2 runtime files, with the updated source launcher. This is a CPU-only container/import/library-load check, not a Vulkan allocation or game compatibility test. The running VRChat process was left untouched.

The unmodified launcher used `VK_ADD_LAYER_PATH`; a fresh Steam Runtime 4 pressure-vessel fixture discarded that variable and did not import zVram's manifest. Setting `VK_LAYER_PATH` imported the manifest. The live game's existing imported manifest also referenced an older, removed 0.4.1 library. Its process maps contained no zVram backend and it had no live control endpoint.

The updated launcher uses `VK_LAYER_PATH` and preserves existing explicit-layer search paths, including XDG/native defaults when there is no override. A fresh container then imported the manifest and `ctypes.CDLL` loaded its library successfully. The current library is compatible with the tested Steam Runtime 4; the failure was not an ABI mismatch.

Exact command used from the zVram checkout:

```sh
./zvram --build-dir /home/nerdrx/.local/share/zvram/0.4.2/build --no-live-control -- \
  /home/nerdrx/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4/pressure-vessel/bin/pressure-vessel-wrap \
  --variable-dir=/tmp/zvram-pv-fixture \
  --runtime=/home/nerdrx/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4/steamrt4_platform_4.0.20260805.254769 -- \
  /usr/bin/python3 -c 'import ctypes,json,os,pathlib; d=os.environ["VK_LAYER_PATH"].split(os.pathsep)[0]; m=next(json.loads(p.read_text()) for p in pathlib.Path(d).glob("*.json") if "VK_LAYER_NX_zvram" in p.read_text()); lib=m["layer"]["library_path"]; ctypes.CDLL(lib); print("VK_LAYER_PATH="+os.environ["VK_LAYER_PATH"]+"; manifest="+m["layer"]["name"]+"; dlopen=ok")'
```

Observed stdout:

```text
VK_LAYER_PATH=/usr/lib/pressure-vessel/overrides/share/vulkan/explicit_layer.d; manifest=VK_LAYER_NX_zvram; dlopen=ok
```

Temporary fixture files were removed after the check. Restarting an affected game is still required; this does not inject paging into an existing Vulkan device. Full GPU/live-cap behavior through Steam remains a separate validation gate.
