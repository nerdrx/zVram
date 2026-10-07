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

    for value in ("-1", "0", "4294967296", "not-an-int"):
        result = run(launcher, *BASE, "--vulkan-auto-idle-ms", value,
                     "--", sys.executable, "-c", "pass")
        assert result.returncode == 2, (value, result.stderr)

    missing_auto = run(launcher, *BASE, "--vulkan-min-savings-percent", "5",
                       "--", sys.executable, "-c", "pass")
    assert missing_auto.returncode == 2 and "requires Vulkan automatic snapshots" in missing_auto.stderr

    hip = run(launcher, "--hip", "--hip-vmm", *BASE, "--vulkan-auto-idle-ms", "100",
              "--vulkan-min-savings-percent", "5", "--", sys.executable, "-c", "pass")
    assert hip.returncode == 2, hip.stderr

    for value in ("0", "3", "-1", "not-an-int"):
        result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100",
                     "--vulkan-byte-shuffle", value,
                     "--", sys.executable, "-c", "pass")
        assert result.returncode == 2, (value, result.stderr)

    missing_auto_shuffle = run(launcher, *BASE, "--vulkan-byte-shuffle", "2",
                               "--", sys.executable, "-c", "pass")
    assert missing_auto_shuffle.returncode == 2, missing_auto_shuffle.stderr

    hip_shuffle = run(launcher, "--hip", "--hip-vmm", *BASE, "--vulkan-auto-idle-ms", "100",
                      "--vulkan-byte-shuffle", "2", "--", sys.executable, "-c", "pass")
    assert hip_shuffle.returncode == 2, hip_shuffle.stderr

    child = "import os; print(os.environ.get('" + KEY + "', ''))"
    for value in ("0", "5", "100"):
        result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100",
                     "--vulkan-min-savings-percent", value,
                     "--", sys.executable, "-c", child)
        assert result.returncode == 0, result.stderr
        assert result.stdout == value + "\n", result.stdout

    idle_child = "import os; print(os.environ.get('ZVRAM_VULKAN_AUTO_IDLE_MS', ''))"
    result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "300000",
                 "--vulkan-min-savings-percent", "5",
                 "--", sys.executable, "-c", idle_child)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "300000\n", result.stdout

    shuffle_child = "import os; print(os.environ.get('ZVRAM_VULKAN_BYTE_SHUFFLE', ''))"
    for value in ("2", "4"):
        result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100",
                     "--vulkan-byte-shuffle", value,
                     "--", sys.executable, "-c", shuffle_child)
        assert result.returncode == 0, result.stderr
        assert result.stdout == value + "\n", result.stdout
