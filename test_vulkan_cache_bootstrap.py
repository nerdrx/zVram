#!/usr/bin/env python3
"""Check clean-cache suppression before resident admission arms, then reuse after."""
import re
import os
import subprocess
import sys


def fail(message, output):
    print(f"FAIL: {message}\n{output}", file=sys.stderr)
    return 1


def main():
    if len(sys.argv) != 3:
        return fail("usage: test_vulkan_cache_bootstrap.py LAUNCHER CHECK_EXECUTABLE", "")
    launcher, check = sys.argv[1:]
    command = [
        launcher, "--validate", "--isolate-layers",
        "--vulkan-virtual-mib", "128", "--vulkan-auto-idle-ms", "500",
        "--vulkan-cold-mib", "128", "--vulkan-selective-restore",
        "--vulkan-active-eviction", "--vulkan-range-mib", "32",
        "--vulkan-resident-mib", "32", "--vulkan-resident-after-cold",
        "--vulkan-clean-cache", "--", check, "--range-cache-bootstrap",
    ]
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=45,
                                env={**os.environ,
                                     "VK_LOADER_LAYERS_DISABLE": "~implicit~"})
    except subprocess.TimeoutExpired as exc:
        return fail("bootstrap cache check timed out", exc.stdout or "")
    output = result.stdout
    if result.returncode != 0:
        return fail(f"GPU check exited {result.returncode}", output)
    validation_errors = [line for line in output.splitlines()
                         if "VUID-" in line or "Validation Error" in line]
    if validation_errors:
        return fail("Vulkan validation reported an error", "\n".join(validation_errors))

    pre_start = output.find("CACHE_BOOTSTRAP_PREARM_BEGIN")
    pre_end = output.find("CACHE_BOOTSTRAP_PREARM_END", pre_start)
    arm_start = output.find("CACHE_BOOTSTRAP_ARMED_BEGIN", pre_end)
    arm_end = output.find("CACHE_BOOTSTRAP_ARMED_END", arm_start)
    if min(pre_start, pre_end, arm_start, arm_end) < 0:
        return fail("missing bootstrap phase markers", output)
    if not (pre_start < pre_end < arm_start < arm_end):
        return fail("bootstrap markers are out of order", output)

    prearm = output[pre_start:pre_end]
    if "resident admission armed after complete cold transition" in prearm:
        return fail("resident admission armed during partial-cold bootstrap", output)
    prearm_restores = re.findall(r"snapshot state event=restore .*?cache-stored=(\d+)", prearm)
    if not prearm_restores or any(int(value) != 0 for value in prearm_restores):
        return fail("pre-arm restore retained a redundant clean cache", output)

    armed_at = output.find("resident admission armed after complete cold transition", arm_start, arm_end)
    if armed_at < 0:
        return fail("admission did not arm after full cold transition", output)
    armed = output[armed_at:arm_end]
    postarm_restores = re.findall(r"snapshot state event=restore .*?cache-stored=(\d+)", armed)
    if not postarm_restores or not any(int(value) > 0 for value in postarm_restores):
        return fail("post-arm restore did not retain clean snapshot bytes", output)
    reuses = re.findall(r"snapshot state event=clean-freeze .*?clean-reuses=(\d+)", armed)
    if not reuses or not any(int(value) > 0 for value in reuses):
        return fail("post-arm clean snapshot was not reused", output)
    evidence = [line for line in output.splitlines()
                if ("CACHE_BOOTSTRAP_" in line or
                    "resident admission armed after complete cold transition" in line or
                    ("snapshot state event=" in line and
                     ("event=restore" in line or "event=clean-freeze" in line)))]
    print("\n".join(evidence))
    print("PASS: bootstrap restores dropped clean copies; armed restores retained and reused them")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
