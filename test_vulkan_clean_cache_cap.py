#!/usr/bin/env python3
"""Exercise clean-cache trimming without dropping lossless cold snapshots."""
import os
import re
import subprocess
import sys


CAP_BYTES = 1024 * 1024
PASS = ("PASS: resident-cap range pressure evicted completed chunks, refused "
        "broad/unknown submissions, and preserved every byte")


def fail(message, output):
    print(f"FAIL: {message}\n{output}", file=sys.stderr)
    return 1


def main():
    if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] not in ("synthetic", "native")):
        return fail("usage: test_vulkan_clean_cache_cap.py LAUNCHER CHECK_EXECUTABLE [synthetic|native]", "")
    launcher, check = sys.argv[1:3]
    native = len(sys.argv) == 4 and sys.argv[3] == "native"
    command = [
        launcher, "--validate", "--isolate-layers", "--vulkan-virtual-mib", "128",
        "--vulkan-auto-idle-ms", "60000", "--vulkan-cold-mib", "64",
        "--vulkan-selective-restore", "--vulkan-active-eviction", "--vulkan-range-mib", "32",
        "--vulkan-resident-mib", "64", "--vulkan-clean-cache", "--vulkan-clean-cache-mib", "1",
    ]
    command.extend(("--", check, "--range-cache-quota"))
    if native:
        command.append("--native-allocation")
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=60,
                                env={**os.environ, "VK_LOADER_LAYERS_DISABLE": "~implicit~"})
    except subprocess.TimeoutExpired as exc:
        return fail("clean-cache cap check timed out", exc.stdout or "")
    output = result.stdout
    if result.returncode != 0:
        return fail(f"GPU check exited {result.returncode}", output)
    errors = [line for line in output.splitlines()
              if "VUID-" in line or "Validation Error" in line]
    if errors:
        return fail("Vulkan validation reported an error", "\n".join(errors))
    if PASS not in output:
        return fail("range quota fixture did not report full-byte success", output)
    if f"Vulkan clean snapshot cache cap enabled limit-bytes={CAP_BYTES}" not in output:
        return fail("selected clean-cache cap was not logged", output)
    if not re.search(r"clean snapshot cache trimmed bytes=\d+", output):
        return fail("fixture did not exercise clean-cache trimming", output)

    states = re.findall(
        r"snapshot state event=\S+ .*?cold-logical=(\d+) cold-stored=(\d+).*?cache-stored=(\d+)",
        output,
    )
    if not states:
        return fail("fixture emitted no snapshot-state accounting", output)
    if any(int(cache) > CAP_BYTES for _, _, cache in states):
        return fail("clean-cache accounting exceeded the configured cap", output)
    if not any(int(logical) > 0 and int(stored) > 0 for logical, stored, _ in states):
        return fail("no lossless cold snapshot remained while the clean cache was capped", output)
    print(f"PASS: 1 MiB clean-cache cap ({'native' if native else 'synthetic'}) trimmed expendable copies; cold snapshots and all fixture bytes survived")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
