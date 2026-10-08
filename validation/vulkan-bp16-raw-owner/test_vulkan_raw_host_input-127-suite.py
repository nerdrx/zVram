#!/usr/bin/env python3
"""Check BP16 raw host input reuses owned allocations without losing bytes."""
import os
import re
import subprocess
import sys


LIMIT_BYTES = 512 * 1024 * 1024


def fail(message, output=""):
    print(f"FAIL: {message}\n{output}", file=sys.stderr)
    return 1


def main():
    if len(sys.argv) != 4 or sys.argv[3] not in ("synthetic", "native"):
        return fail("usage: test_vulkan_raw_host_input.py LAUNCHER CHECK_EXECUTABLE synthetic|native")
    launcher, check, mode = sys.argv[1:]
    command = [
        launcher, "--validate", "--isolate-layers", "--vulkan-codec", "bp16",
        "--vulkan-bp16-gpu", "--vulkan-virtual-mib", "512",
        "--vulkan-auto-idle-ms", "60000", "--vulkan-cold-mib", "512",
        "--vulkan-selective-restore", "--vulkan-active-eviction",
        "--vulkan-range-mib", "32", "--vulkan-resident-mib", "32",
        "--vulkan-clean-cache", "--vulkan-min-savings-percent", "100", "--",
        check, "--range-cache",
    ]
    if mode == "native":
        command.append("--native-allocation")
    env = os.environ.copy()
    env.update(
        VK_LOADER_LAYERS_DISABLE="~implicit~",
        VK_VALIDATION_VALIDATE_SYNC="1",
        ZVRAM_VULKAN_BP16_RAW_HOST_INPUT="1",
        ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT="1",
        ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB="512",
        ZVRAM_VULKAN_GPU_PROFILE="1",
    )
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=60, env=env)
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        return fail("raw host input check timed out", output)
    output = result.stdout
    if result.returncode != 0:
        return fail(f"GPU check exited {result.returncode}", output)
    if "PASS: resident-cap range pressure" not in output:
        return fail("range-cache fixture did not verify every output byte", output)
    if "BP16 raw host input enabled" not in output:
        return fail("raw host input path was not enabled", output)
    if "VUID-" in output or "Validation Error" in output:
        return fail("Vulkan validation reported an error", output)

    raw_rows = re.findall(
        r"GPU BP16 raw host input allocations=(\d+) reuses=(\d+) "
        r"direct-bytes=(\d+) fallbacks=(\d+)", output)
    if not raw_rows:
        return fail("raw host input telemetry was not emitted", output)
    allocations, reuses, direct_bytes, fallbacks = map(int, raw_rows[-1])
    if allocations <= 0 or reuses <= 0 or direct_bytes <= 0 or fallbacks != 0:
        return fail("raw host input did not show reused direct allocations without fallback", output)

    allocated_rows = re.findall(
        r"GPU BP16 allocated input allocations=(\d+) reuses=(\d+) bytes=(\d+) "
        r"live-bytes=(\d+) limit-bytes=(\d+)", output)
    if not allocated_rows:
        return fail("allocated host input telemetry was not emitted", output)
    if any(int(limit) != LIMIT_BYTES or int(live) > int(limit)
           for _, _, _, live, limit in allocated_rows):
        return fail("allocated host input exceeded its configured live-byte budget", output)

    states = re.findall(
        r"snapshot state event=\S+ .*?cold-stored=(\d+).*?cache-stored=(\d+)", output)
    if not states:
        return fail("snapshot cold/cache accounting was not emitted", output)
    if any(int(cold) + int(cache) > LIMIT_BYTES for cold, cache in states):
        return fail("snapshot cold/cache accounting exceeded 512 MiB", output)
    print(f"PASS: BP16 raw host input ({mode}) reused {reuses} allocations with {direct_bytes} direct bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
