#!/usr/bin/env python3
"""CPU-only CLI checks for Vulkan snapshot compression options."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


SOURCE = Path(__file__).with_name("zvram")
KEY = "ZVRAM_VULKAN_MIN_SAVINGS_PERCENT"
BASE = ["--vulkan-virtual-gib", "96", "--vulkan-cold-mib", "64"]


def run(launcher, *arguments, env=None):
    return subprocess.run([sys.executable, str(launcher), *arguments],
                          text=True, capture_output=True, check=False, env=env)


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    launcher = root / "zvram"
    shutil.copyfile(SOURCE, launcher)
    build = root / "build"
    build.mkdir()
    (build / "VK_LAYER_NX_zvram.json").touch()
    (build / "libzvram_layer.so").touch()

    codec_child = "import os; print(os.environ.get('ZVRAM_VULKAN_CODEC', ''))"
    missing_auto_codec = run(launcher, *BASE, "--vulkan-codec", "zstd", "--", sys.executable, "-c", "pass")
    assert missing_auto_codec.returncode == 2, missing_auto_codec.stderr
    missing_codec = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", "gdeflate", "--", sys.executable, "-c", "pass")
    assert missing_codec.returncode == 2 and "GDeflate codec missing" in missing_codec.stderr
    missing_gpu_codec = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-gdeflate-gpu", "--", sys.executable, "-c", "pass")
    assert missing_gpu_codec.returncode == 2 and "requires the gdeflate codec" in missing_gpu_codec.stderr
    (build / "zvram-codecs.json").write_text('{"gdeflate": true}')
    missing_gpu = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", "gdeflate", "--vulkan-gdeflate-gpu", "--", sys.executable, "-c", "pass")
    assert missing_gpu.returncode == 2 and "GPU GDeflate decoder missing" in missing_gpu.stderr
    (build / "zvram-codecs.json").write_text('{"gdeflate": true, "gdeflate_gpu": true}')
    (build / "gdeflate-wave32.spv").touch()
    for workers in (1, 2, 4):
        result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", "gdeflate",
                     "--vulkan-gdeflate-workers", str(workers), "--", sys.executable, "-c",
                     "import os; print(os.environ['ZVRAM_VULKAN_GDEFLATE_WORKERS'])")
        assert result.returncode == 0 and result.stdout == str(workers) + "\n", (result.stdout, result.stderr)
    for extra in (("--vulkan-gdeflate-workers", "0"), ("--vulkan-gdeflate-workers", "5"),
                  ("--vulkan-gdeflate-workers", "2", "--vulkan-codec", "zstd")):
        result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", *extra, "--", sys.executable, "-c", "pass")
        assert result.returncode == 2, result.stderr
    lazy_base = [*BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-selective-restore",
                 "--vulkan-active-eviction", "--vulkan-range-mib", "32", "--vulkan-resident-mib", "32"]
    result = run(launcher, *lazy_base, "--vulkan-lazy-backing", "--", sys.executable, "-c",
                 "import os; print(os.environ['ZVRAM_VULKAN_LAZY_BACKING'])")
    assert result.returncode == 0 and result.stdout == "1\n", (result.stdout, result.stderr)
    for arguments in ((*BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-lazy-backing"),
                      (*lazy_base, "--vulkan-lazy-backing", "--vulkan-resident-after-cold"),
                      ("--hip", "--vulkan-lazy-backing")):
        result = run(launcher, *arguments, "--", sys.executable, "-c", "pass")
        assert result.returncode == 2 and "requires immediate" in result.stderr, result.stderr
    result = run(launcher, *lazy_base, "--vulkan-lazy-backing", "--vulkan-headroom-mib", "2048",
                 "--", sys.executable, "-c", "import os; print(os.environ['ZVRAM_VULKAN_HEADROOM_MIB'])")
    assert result.returncode == 0 and result.stdout == "2048\n", (result.stdout, result.stderr)
    for extra in (("--vulkan-headroom-mib", "2048"),
                  ("--vulkan-lazy-backing", "--vulkan-headroom-mib", "0"),
                  ("--vulkan-lazy-backing", "--vulkan-headroom-mib", "-1"),
                  ("--vulkan-lazy-backing", "--vulkan-headroom-mib", "17592186044416")):
        result = run(launcher, *lazy_base, *extra, "--", sys.executable, "-c", "pass")
        assert result.returncode == 2 and "requires lazy backing" in result.stderr, result.stderr
    gpu_child = "import os; print(os.environ['ZVRAM_VULKAN_GDEFLATE_GPU']); print(os.environ['ZVRAM_GDEFLATE_SHADER_PATH'])"
    result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", "gdeflate", "--vulkan-gdeflate-gpu", "--", sys.executable, "-c", gpu_child)
    assert result.returncode == 0 and result.stdout == "1\n" + str(build / "gdeflate-wave32.spv") + "\n", (result.stdout, result.stderr)
    for codec in ("zstd", "gdeflate"):
        result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", codec, "--", sys.executable, "-c", codec_child)
        assert result.returncode == 0 and result.stdout == codec + "\n", (result.stdout, result.stderr)
    alternate = root / "alternate-build"
    alternate.mkdir()
    (alternate / "VK_LAYER_NX_zvram.json").touch()
    (alternate / "libzvram_layer.so").touch()
    result = run(launcher, "--build-dir", str(alternate), *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", "gdeflate", "--", sys.executable, "-c", "pass")
    assert result.returncode == 2 and "GDeflate codec missing" in result.stderr
    (alternate / "zvram-codecs.json").write_text('{"gdeflate": true}')
    clean = os.environ.copy()
    clean.pop("VK_LAYER_PATH", None)
    clean.pop("VK_ADD_LAYER_PATH", None)
    path_child = "import os; print(os.environ['VK_ADD_LAYER_PATH'])"
    result = run(launcher, "--build-dir", str(alternate), *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", "gdeflate", "--", sys.executable, "-c", path_child, env=clean)
    assert result.returncode == 0 and result.stdout == str(alternate) + "\n", (result.stdout, result.stderr)
    conflicting_filter = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", "gdeflate", "--vulkan-byte-shuffle", "2", "--", sys.executable, "-c", "pass")
    assert conflicting_filter.returncode == 2 and "requires the zstd codec" in conflicting_filter.stderr
    (build / "zvram-codecs.json").write_text('{"gdeflate": false}')
    disabled_codec = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", "gdeflate", "--", sys.executable, "-c", "pass")
    assert disabled_codec.returncode == 2, disabled_codec.stderr
    inherited = os.environ.copy()
    inherited["ZVRAM_VULKAN_CODEC"] = "gdeflate"
    result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--", sys.executable, "-c", "pass", env=inherited)
    assert result.returncode == 2 and "GDeflate codec missing" in result.stderr
    result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", "zstd", "--", sys.executable, "-c", codec_child, env=inherited)
    assert result.returncode == 0 and result.stdout == "zstd\n", (result.stdout, result.stderr)
    (build / "zvram-codecs.json").write_text('{"gdeflate": true}')
    inherited["ZVRAM_VULKAN_BYTE_SHUFFLE"] = "2"
    result = run(launcher, *BASE, "--vulkan-auto-idle-ms", "100", "--vulkan-codec", "gdeflate", "--", sys.executable, "-c", "pass", env=inherited)
    assert result.returncode == 2 and "requires the zstd codec" in result.stderr

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
