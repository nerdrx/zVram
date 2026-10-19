#!/usr/bin/env python3
"""Exercise bounded restore lookahead with one submission spanning 3 groups."""
import os
import re
import subprocess
import sys


def run_case(launcher, check, *, native=False, partial=False):
    command = [
        launcher, "--validate", "--isolate-layers",
        "--vulkan-virtual-mib", "512", "--vulkan-auto-idle-ms", "100",
        "--vulkan-cold-mib", "512", "--vulkan-selective-restore",
        "--vulkan-active-eviction", "--vulkan-range-mib", "128",
        "--vulkan-resident-mib", "512", "--vulkan-min-savings-percent",
        "100" if native else "0", "--",
        check,
    ]
    if native:
        command.append("--native-allocation")
    command.append("--expect-pipeline-partial-restore" if partial else "--expect-pipeline-restore")
    env = {**os.environ, "VK_LOADER_LAYERS_DISABLE": "~implicit~"}
    if partial:
        env["ZVRAM_TEST_RESTORE_FAIL_AFTER_GROUPS"] = "1"
    else:
        env.pop("ZVRAM_TEST_RESTORE_FAIL_AFTER_GROUPS", None)
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=150, env=env)
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        return False, f"case timed out\n{output}"
    output = result.stdout
    if result.returncode:
        return False, f"case exited {result.returncode}\n{output}"
    errors = [line for line in output.splitlines()
              if "VUID-" in line or "Validation Error" in line]
    if errors:
        return False, "Vulkan validation errors\n" + "\n".join(errors)
    if "PASS: " not in output:
        return False, "missing success marker\n" + output
    if partial and "PIPELINE_PARTIAL_FAULT resident=134217728 cold=201326592" not in output:
        return False, "partial restore did not preserve the expected 128/192 MiB state\n" + output
    prefetches = [int(value) for value in re.findall(
        r"snapshot pipeline prefetches=(\d+)", output)]
    if not prefetches or max(prefetches) == 0:
        return False, "multi-group workload did not launch a decode prefetch\n" + output
    return True, output


def main():
    if len(sys.argv) != 3:
        print("usage: test_vulkan_pipeline.py LAUNCHER CHECK_EXECUTABLE", file=sys.stderr)
        return 2
    launcher, check = sys.argv[1:]
    cases = [("synthetic", False, False), ("native", True, False),
             ("partial-failure-and-retry", False, True)]
    for name, native, partial in cases:
        ok, output = run_case(launcher, check, native=native, partial=partial)
        evidence = [line for line in output.splitlines()
                    if "snapshot pipeline prefetches=" in line or
                    "PIPELINE_PARTIAL_FAULT" in line or line.startswith("PASS:")]
        if not ok:
            print(f"FAIL [{name}]: {output}", file=sys.stderr)
            return 1
        print(f"[{name}] " + " | ".join(evidence))
    print("PASS: synthetic/native lookahead and joined partial-failure retry")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
