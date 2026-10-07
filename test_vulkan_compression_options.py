#!/usr/bin/env python3
"""CPU-only CLI checks for Vulkan snapshot compression options."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


SOURCE = Path(__file__).with_name("zvram")
KEY = "ZVRAM_VULKAN_MIN_SAVINGS_PERCENT"
BASE = ["--vulkan-virtual-gib", "96", "--vulkan-cold-mib", "64"]


def run(launcher, *arguments):
    return subprocess.run([sys.executable, str(launcher), *arguments],
                          text=True, capture_output=True, check=False)


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    launcher = root / "zvram"
    shutil.copyfile(SOURCE, launcher)
    build = root / "build"
    build.mkdir()
    (build / "VK_LAYER_NX_zvram.json").touch()
    (build / "libzvram_layer.so").touch()

    for value in ("-1", "101", "not-an-int"):
        result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100",
                     "--vulkan-min-savings-percent", value, "--", sys.executable, "-c", "pass")
        assert result.returncode == 2, (value, result.stderr)

    missing_auto = run(launcher, *BASE, "--vulkan-min-savings-percent", "5",
                       "--", sys.executable, "-c", "pass")
    assert missing_auto.returncode == 2 and "requires Vulkan automatic snapshots" in missing_auto.stderr

    hip = run(launcher, "--hip", "--hip-vmm", *BASE, "--vulkan-auto-idle-ms", "100",
              "--vulkan-min-savings-percent", "5", "--", sys.executable, "-c", "pass")
    assert hip.returncode == 2, hip.stderr

    child = "import os; print(os.environ.get('" + KEY + "', ''))"
    for value in ("0", "5", "100"):
        result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100",
                     "--vulkan-min-savings-percent", value,
                     "--", sys.executable, "-c", child)
        assert result.returncode == 0, result.stderr
        assert result.stdout == value + "\n", result.stdout
