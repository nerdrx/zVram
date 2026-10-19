#!/usr/bin/env python3
"""Validate the offscreen graphics fixture using an explicitly selected CPU ICD."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

from research.gdeflate.test_software import pick_icd


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--icd", help="lavapipe ICD JSON; defaults to the single system lavapipe ICD")
    parser.add_argument("--log", type=Path, default=Path("build/graphics-software.txt"))
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    icd = pick_icd(args.icd)
    env = os.environ.copy()
    for key in ("VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES", "VK_INSTANCE_LAYERS",
                "VK_LAYER_PATH", "VK_ADD_LAYER_PATH"):
        env.pop(key, None)
    env.update(VK_DRIVER_FILES=str(icd), VK_LOADER_LAYERS_DISABLE="~implicit~",
               VK_VALIDATION_VALIDATE_SYNC="1", LP_NUM_THREADS="2")
    result = subprocess.run([str(binary), "--native"], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.log.write_text(result.stdout)
    print(result.stdout, end="")
    if (result.returncode or "type=4" not in result.stdout or "validation=on" not in result.stdout or
            "PASS: 3 offscreen draw/readback frames (native mode)" not in result.stdout or
            any(marker in result.stdout for marker in ("VUID-", "Validation Error", "Vulkan validation error:"))):
        raise RuntimeError(f"CPU device, full graphics readback, or validation gate failed; see {args.log}")
    print("PASS: CPU-only graphics fixture; no physical GPU execution or zVram paging claim")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
