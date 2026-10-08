#!/usr/bin/env python3
"""CPU-only CLI checks for the isolated Vulkan research host's worker option."""

import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXE = Path(sys.argv[1]) if len(sys.argv) == 2 else ROOT / "build/bp16-codec/zvram-gdeflate-research"
if len(sys.argv) > 2:
    raise SystemExit("usage: check_host_copy_workers.py [research-host-executable]")
if not EXE.is_absolute():
    EXE = ROOT / EXE


def run(args):
    return subprocess.run([str(EXE), *args], cwd=ROOT, text=True, capture_output=True)


def expect(condition, message, result):
    if not condition:
        raise SystemExit(f"FAIL: {message}: rc={result.returncode}\n{result.stdout}{result.stderr}")


shader = "research/bp16/decode.spv"
frame = "build/bp16-research/gpu-mixed-pattern.bp16"
raw = "build/bp16-research/gpu-mixed-pattern.raw"
preflight = ["--codec", "bp16", "--preflight-only", shader, frame, raw]

default = run(preflight)
expect(default.returncode == 0 and "CPU-only BP16 frame preflight" in default.stdout,
       "omitting worker option must keep CPU preflight usable", default)

with tempfile.TemporaryDirectory(prefix="bp16-worker-cli-") as temp:
    for workers, mode in (("1", "--host-input"), ("8", "--allocated-host-input")):
        result = run(["--codec", "bp16", mode, "--host-copy-iterations", "1",
                      "--host-copy-workers", workers, "--gpu-bounded-smoke",
                      f"{temp}/missing.spv", f"{temp}/missing.bp16", f"{temp}/missing.raw"])
        expect(result.returncode == 1 and "cannot open" in result.stderr and
               "Vulkan" not in result.stderr,
               f"valid worker count {workers} must parse before missing-file failure", result)

    for workers in ("0", "9", "text", "999999999999"):
        result = run(["--codec", "bp16", "--host-copy-workers", workers,
                      "--gpu-bounded-smoke", f"{temp}/missing.spv",
                      f"{temp}/missing.bp16", f"{temp}/missing.raw"])
        expect(result.returncode == 2 and "--host-copy-workers" in result.stderr and
               "cannot open" not in result.stderr,
               f"invalid worker count {workers} must be rejected before file access", result)

    result = run(["--codec", "bp16", "--host-copy-workers", "--gpu-bounded-smoke",
                  f"{temp}/missing.spv", f"{temp}/missing.bp16", f"{temp}/missing.raw"])
    expect(result.returncode == 2 and "expects an integer" in result.stderr,
           "missing worker value must be rejected", result)

missing_iterations = run(["--codec", "bp16", "--host-input", "--host-copy-workers", "2",
                          "--preflight-only", shader, frame, raw])
expect(missing_iterations.returncode == 1 and "requires --host-copy-iterations" in missing_iterations.stderr,
       "worker option without copy iterations must fail before Vulkan", missing_iterations)

missing_mode = run(["--codec", "bp16", "--host-copy-workers", "2", "--host-copy-iterations", "1",
                    "--preflight-only", shader, frame, raw])
expect(missing_mode.returncode == 1 and "requires --gpu-bounded-smoke" in missing_mode.stderr,
       "worker option outside bounded smoke must fail before Vulkan", missing_mode)

print("PASS: default, worker bounds, missing value/context, and no-GPU CLI paths")
