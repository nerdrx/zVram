"""Small offscreen correctness/timing probe; not a game FPS benchmark."""
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
out = Path(__file__).resolve().parent
common = ["build/zvram-vulkan-graphics-check", "--native-allocation", "--frames", "12", "--frame-delay-ms", "150"]
wrap = ["./zvram", "--build-dir", "build", "--no-live-control", "--validate", "--isolate-layers",
        "--vulkan-virtual-mib", "128", "--vulkan-auto-idle-ms", "100", "--vulkan-cold-mib", "64",
        "--vulkan-selective-restore", "--vulkan-active-eviction", "--vulkan-range-mib", "32", "--vulkan-resident-mib", "64"]
env = {k: v for k, v in os.environ.items() if not k.startswith("ZVRAM_") and k != "VK_INSTANCE_LAYERS"}
env["VK_LOADER_LAYERS_DISABLE"] = "~implicit~"
results = []
for name, command in [("native", common + ["--native"]),
                      ("idle", wrap + ["--vulkan-eviction-trigger", "idle", "--"] + common),
                      ("pressure", wrap + ["--vulkan-eviction-trigger", "pressure", "--"] + common + ["--warm-residency"])]:
    result = subprocess.run(command, cwd=root, env=env, text=True, capture_output=True, timeout=30)
    (out / (name + "-graphics.stdout.txt")).write_text(result.stdout)
    (out / (name + "-graphics.stderr.txt")).write_text(result.stderr)
    item = {"mode": name, "returncode": result.returncode,
            "timing": [line for line in result.stdout.splitlines() if "draw-readback-ms" in line]}
    results.append(item)
    print(json.dumps(item), flush=True)
    if result.returncode:
        print(result.stderr[-2500:])
        break
(out / "graphics-summary.json").write_text(json.dumps(results, indent=2) + "\n")
raise SystemExit(any(item["returncode"] for item in results))
