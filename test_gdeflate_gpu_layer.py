#!/usr/bin/env python3
"""Require full-byte application checks and observed GPU decode, without fallback."""
import os
import re
import subprocess
import sys

if len(sys.argv) < 2:
    raise SystemExit("supply the zVram application command")
try:
    result = subprocess.run(sys.argv[1:], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=45)
except subprocess.TimeoutExpired as error:
    output = error.stdout or b""
    print(output.decode(errors="replace") if isinstance(output, bytes) else output)
    raise SystemExit("FAIL: GPU restore application timed out")
print(result.stdout, end="")
profiles = [(codec, *map(int, row)) for codec, *row in re.findall(
    r"GPU (GDeflate|BP16) restore calls=(\d+) bytes=(\d+) host-ns=(\d+) fallbacks=(\d+)",
    result.stdout)]
selected = re.findall(r"Vulkan snapshot codec=(gdeflate|bp16)", result.stdout)
expected = selected[-1].lower() if selected else "gdeflate"
if (result.returncode != 0 or "PASS:" not in result.stdout or
        f"GPU {expected.upper() if expected == 'bp16' else 'GDeflate'} restore enabled" not in result.stdout or
        not profiles or profiles[-1][0].lower() != expected or
        profiles[-1][1] <= 0 or profiles[-1][2] <= 0 or profiles[-1][4] != 0 or "VUID-" in result.stdout or
        "Validation Error" in result.stdout):
    raise SystemExit("FAIL: full-byte check, observed GPU decode, or zero-fallback validation gate failed")
if os.environ.get("ZVRAM_VULKAN_BP16_IMPORT_HOST_INPUT") == "1":
    imports = re.findall(r"GPU BP16 imported input imports=(\d+) reuses=(\d+) bytes=(\d+)", result.stdout)
    if expected != "bp16" or not imports or int(imports[-1][0]) <= 0 or int(imports[-1][2]) <= 0:
        raise SystemExit("FAIL: imported BP16 input requested but no actual imports observed")
print("PASS: application byte checks, observed GPU decoding, zero fallback and validation diagnostics")
