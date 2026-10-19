#!/usr/bin/env python3
"""Check BP16 raw host input reuses owned allocations without losing bytes."""
import os
import re
import subprocess
import sys


LIMIT_BYTES = 512 * 1024 * 1024
SMALL_COLD_BYTES = 64 * 1024 * 1024


def fail(message, output=""):
    print(f"FAIL: {message}\n{output}", file=sys.stderr)
    return 1


def main():
    modes = ("synthetic", "native", "owner-budget", "cold-budget")
    if len(sys.argv) != 4 or sys.argv[3] not in modes:
        return fail("usage: test_vulkan_raw_host_input.py LAUNCHER CHECK_EXECUTABLE synthetic|native|owner-budget|cold-budget")
    launcher, check, mode = sys.argv[1:]
    owner_budget = mode == "owner-budget"
    cold_budget = mode == "cold-budget"
    no_allocation = owner_budget or cold_budget
    cold_limit = SMALL_COLD_BYTES if cold_budget else LIMIT_BYTES
    allocated_limit = 8 if owner_budget else 512
    command = [
        launcher, "--validate", "--isolate-layers", "--vulkan-codec", "bp16",
        "--vulkan-bp16-gpu", "--vulkan-virtual-mib", "512",
        "--vulkan-auto-idle-ms", "60000", "--vulkan-cold-mib",
        "64" if cold_budget else "512",
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
        ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB=str(allocated_limit),
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
    if no_allocation:
        if allocations != 0 or reuses != 0 or direct_bytes != 0 or fallbacks <= 0:
            return fail("raw-owner admission refusal did not fall back to lossless staging", output)
    elif allocations <= 0 or reuses <= 0 or direct_bytes <= 0 or fallbacks != 0:
        return fail("raw host input did not show reused direct allocations without fallback", output)

    allocated_rows = re.findall(
        r"GPU BP16 allocated input allocations=(\d+) reuses=(\d+) bytes=(\d+) "
        r"live-bytes=(\d+) limit-bytes=(\d+)", output)
    if not allocated_rows:
        return fail("allocated host input telemetry was not emitted", output)
    last_owner = tuple(map(int, allocated_rows[-1]))
    owner_allocations, _, owner_bytes, live_bytes, owner_limit = last_owner
    if no_allocation:
        if (owner_allocations != 0 or owner_bytes != 0 or live_bytes != 0 or
                owner_limit != allocated_limit * 1024 * 1024):
            return fail("raw-owner refusal did not keep allocated-host usage at zero", output)
    elif any(int(limit) != LIMIT_BYTES or int(live) > int(limit)
             for _, _, _, live, limit in allocated_rows):
        return fail("allocated host input exceeded its configured live-byte budget", output)

    states = re.findall(
        r"snapshot state event=\S+ .*?cold-stored=(\d+).*?cache-stored=(\d+)", output)
    if not states:
        return fail("snapshot cold/cache accounting was not emitted", output)
    if any(int(cold) + int(cache) > cold_limit for cold, cache in states):
        return fail(f"snapshot cold/cache accounting exceeded {cold_limit // (1024 * 1024)} MiB", output)
    decoder_fallbacks = re.findall(r"snapshot state event=\S+ .*?gpu-decode-fallbacks=(\d+)", output)
    if not decoder_fallbacks or any(int(count) != 0 for count in decoder_fallbacks):
        return fail("raw-owner fallback was incorrectly counted as a GPU decoder fallback", output)
    if owner_budget:
        print("PASS: BP16 raw owner budget refusal preserved lossless staging and zero owner usage")
    elif cold_budget:
        print("PASS: BP16 raw cold-quota headroom refusal preserved lossless staging and zero owner usage")
    else:
        print(f"PASS: BP16 raw host input ({mode}) reused {reuses} allocations with {direct_bytes} direct bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
