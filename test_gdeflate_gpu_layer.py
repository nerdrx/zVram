#!/usr/bin/env python3
"""Require full-byte application checks and observed GPU decode, without fallback."""
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
profiles = [tuple(map(int, row)) for row in re.findall(
    r"GPU GDeflate restore calls=(\d+) bytes=(\d+) host-ns=(\d+) fallbacks=(\d+)",
    result.stdout)]
if (result.returncode != 0 or "PASS:" not in result.stdout or
        "GPU GDeflate restore enabled" not in result.stdout or
        not profiles or profiles[-1][0] <= 0 or profiles[-1][1] <= 0 or
        profiles[-1][3] != 0 or "VUID-" in result.stdout or
        "Validation Error" in result.stdout):
    raise SystemExit("FAIL: full-byte check, observed GPU decode, or zero-fallback validation gate failed")
print("PASS: application byte checks, observed GPU decoding, zero fallback and validation diagnostics")
