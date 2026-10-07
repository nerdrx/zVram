#!/usr/bin/env python3
"""Build a pinned upstream HIP copy example; optionally run native and zVram checks."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request

REVISION = "959b2d4d0459abfd1f67f3fb9cce20cd88a7785a"
FILES = {
    "primbench.hpp": "db6a521b8f5e656156e116a0d1da38e59fee8b4c9bc7e6ba85a29716bccdc6ca",
    "examples/hip/copy_benchmark.cpp": "e6564e289876f6e18039013e2bd776019d08b8f7689b0576f0ec8af472225979",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="run two small GPU integration checks after building")
    parser.add_argument("--arch", default="gfx1100", help="HIP compiler target (default: tested RX 7900 XTX)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    directory = root / "build/third-party/primbench"
    directory.mkdir(parents=True, exist_ok=True)
    for name, expected in FILES.items():
        path = directory / name
        if path.exists():
            contents = path.read_bytes()
        else:
            url = f"https://raw.githubusercontent.com/ROCm/rocm-libraries/{REVISION}/shared/primbench/{name}"
            with urllib.request.urlopen(url, timeout=30) as response:
                contents = response.read(5 * 1024 * 1024 + 1)
        if hashlib.sha256(contents).hexdigest() != expected:
            raise RuntimeError(f"Upstream source hash mismatch: {name}")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
    compiler = shutil.which("hipcc") or "/opt/rocm/bin/hipcc"
    binary = directory / "copy_benchmark"
    subprocess.run([compiler, "-O2", "-std=c++17", f"--offload-arch={args.arch}",
                    "-DPRIMBENCH_NO_MONITORING", f"-I{directory}",
                    str(directory / "examples/hip/copy_benchmark.cpp"), "-o", str(binary)],
                   check=True, timeout=120)
    (directory / "UPSTREAM_COMMIT.txt").write_text(REVISION + "\n")
    print(f"Built unchanged ROCm primbench at {REVISION}", flush=True)
    if not args.run:
        return
    import os
    environment = os.environ.copy()
    environment.pop("LD_PRELOAD", None)
    for key in list(environment):
        if key.startswith("ZVRAM_"):
            del environment[key]
    application = [str(binary), "--size", "32MiB", "--min-secs", "0.1", "--noise-timeout-secs", "1"]
    wrapped = [str(root / "zvram"), "--hip", "--hip-vmm", "--hip-report-capacity",
               "--hip-local-mib", "32", "--hip-host-mib", "512", "--"] + application
    for name, command in (("native", application), ("vmm", wrapped)):
        result = subprocess.run(command, cwd=directory, env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=30)
        log = root / f"build/primbench-{name}.txt"
        log.write_text(f"Upstream commit: {REVISION}\nCommand: {command!r}\n"
                       + result.stdout + f"\nExit={result.returncode}\n")
        result.check_returncode()
        if any(value not in result.stdout for value in ("type: char", "type: long long")):
            raise RuntimeError(f"Both upstream specializations did not run; inspect {log}")
        if name == "vmm" and any(value not in result.stdout for value in
                                  ("tracked=0 ", "host_pinned_backing_bytes_current=0 pending=0 ",
                                   "orphaned_vmm_cleanup=0 ", "failures=0;")):
            raise RuntimeError(f"zVram cleanup check failed; inspect {log}")
        print(f"PASS {name}: upstream's three-value copy assertion; log: {log}")
    print("This is an integration check, not full-buffer integrity or a performance comparison.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"primbench check: {error}", file=sys.stderr)
        sys.exit(1)
