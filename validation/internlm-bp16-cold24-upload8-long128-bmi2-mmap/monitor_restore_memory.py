#!/usr/bin/env python3
"""Sample one owned run's process tree and system memory to JSONL."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import time
import urllib.error
import urllib.request


KIB = 1024


def read_kib_fields(path, names):
    wanted = set(names)
    values = {}
    try:
        for line in Path(path).read_text(errors="replace").splitlines():
            key, sep, rest = line.partition(":")
            if sep and key in wanted:
                fields = rest.split()
                if fields:
                    values[key] = int(fields[0])
    except (FileNotFoundError, PermissionError, ProcessLookupError, ValueError):
        pass
    return values


def meminfo():
    return read_kib_fields("/proc/meminfo", ("MemTotal", "MemAvailable", "SwapTotal", "SwapFree"))


def process_tree(root_pid):
    found = set()
    pending = [root_pid]
    while pending:
        pid = pending.pop()
        if pid in found or not Path(f"/proc/{pid}").exists():
            continue
        found.add(pid)
        try:
            children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
            pending.extend(int(child) for child in children)
        except (FileNotFoundError, PermissionError, ProcessLookupError, ValueError):
            pass
    return sorted(found)


def process_name(pid):
    try:
        return Path(f"/proc/{pid}/comm").read_text().strip()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        return "?"


def process_cmdline(pid):
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
        return raw.replace(b"\0", b" ").decode(errors="replace").strip()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        return ""


def process_env(pid):
    keys = {"MALLOC_ARENA_MAX", "MALLOC_MMAP_THRESHOLD_", "MALLOC_TRIM_THRESHOLD_"}
    out = {}
    try:
        entries = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
        for entry in entries:
            key, sep, value = entry.partition(b"=")
            if sep and key.decode(errors="ignore") in keys:
                out[key.decode()] = value.decode(errors="replace")
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        pass
    return out


def smaps_rollup(pid):
    names = ("Rss", "Pss", "Pss_Anon", "Pss_File", "Anonymous", "Private_Clean",
             "Private_Dirty", "Shared_Clean", "Shared_Dirty", "Swap")
    return read_kib_fields(f"/proc/{pid}/smaps_rollup", names)


def drm_fdinfo(pid):
    """Return max counters across this process's DRM fds (fdinfo repeats client totals)."""
    wanted = ("drm-total-vram", "drm-resident-vram", "amd-requested-vram",
              "drm-total-gtt", "drm-resident-gtt", "amd-requested-gtt")
    maxima = {}
    directory = Path(f"/proc/{pid}/fdinfo")
    try:
        entries = list(directory.iterdir())
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        return maxima
    for entry in entries:
        try:
            text = entry.read_text(errors="replace")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if "drm-driver:\tamdgpu" not in text and "drm-driver: amdgpu" not in text:
            continue
        for line in text.splitlines():
            key, sep, rest = line.partition(":")
            if sep and key in wanted:
                fields = rest.split()
                if fields:
                    try:
                        maxima[key] = max(maxima.get(key, 0), int(fields[0]))
                    except ValueError:
                        pass
    return maxima


def ollama_models():
    request = urllib.request.Request("http://127.0.0.1:11434/api/ps", method="GET")
    try:
        with urllib.request.urlopen(request, timeout=0.2) as response:
            data = json.loads(response.read())
        return [{key: model.get(key) for key in ("name", "size", "size_vram", "expires_at")}
                for model in data.get("models", [])]
    except (OSError, urllib.error.URLError, json.JSONDecodeError, TimeoutError):
        return None


def sample(root_pid):
    pids = process_tree(root_pid)
    processes = []
    tree_rss = 0
    tree_pss = 0
    drm_peak = {}
    for pid in pids:
        status = read_kib_fields(f"/proc/{pid}/status",
                                 ("VmRSS", "RssAnon", "RssFile", "VmSwap"))
        smaps = smaps_rollup(pid)
        rss = status.get("VmRSS", 0)
        tree_rss += rss
        tree_pss += smaps.get("Pss", 0)
        drm = drm_fdinfo(pid)
        for key, value in drm.items():
            drm_peak[key] = max(drm_peak.get(key, 0), value)
        processes.append({
            "pid": pid,
            "comm": process_name(pid),
            "cmdline": process_cmdline(pid),
            "status_kib": status,
            "smaps_rollup_kib": smaps,
            "drm_fdinfo_kib": drm,
            "allocator_env": process_env(pid),
        })
    return {
        "utc": datetime.now(timezone.utc).isoformat(),
        "root_pid": root_pid,
        "process_tree_pids": pids,
        "system_kib": meminfo(),
        "tree_rss_kib_sum": tree_rss,
        "tree_pss_kib_sum": tree_pss,
        "drm_fdinfo_tree_peak_kib": drm_peak,
        "ollama_models": ollama_models(),
        "processes": processes,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("controller_pid", type=int, help="PID of the run controller; descendants are followed")
    parser.add_argument("--output", type=Path, required=True, help="new JSONL output path")
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--max-seconds", type=float, default=1800.0)
    parser.add_argument("--deadline-utc", default="2026-10-08T06:00:00+00:00")
    args = parser.parse_args()
    deadline = datetime.fromisoformat(args.deadline_utc.replace("Z", "+00:00"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    stop_at = min(started + args.max_seconds,
                  started + max(0.0, (deadline - datetime.now(timezone.utc)).total_seconds()))
    peaks = {"tree_rss_kib_sum": 0, "tree_pss_kib_sum": 0,
             "swap_used_kib": 0, "drm_fdinfo_tree_peak_kib": {}}
    with args.output.open("w", buffering=1) as output:
        while time.monotonic() < stop_at:
            record = sample(args.controller_pid)
            system = record["system_kib"]
            swap_used = max(0, system.get("SwapTotal", 0) - system.get("SwapFree", 0))
            peaks["tree_rss_kib_sum"] = max(peaks["tree_rss_kib_sum"], record["tree_rss_kib_sum"])
            peaks["tree_pss_kib_sum"] = max(peaks["tree_pss_kib_sum"], record["tree_pss_kib_sum"])
            peaks["swap_used_kib"] = max(peaks["swap_used_kib"], swap_used)
            for key, value in record["drm_fdinfo_tree_peak_kib"].items():
                peaks["drm_fdinfo_tree_peak_kib"][key] = max(
                    peaks["drm_fdinfo_tree_peak_kib"].get(key, 0), value)
            record["peaks"] = peaks
            output.write(json.dumps(record, separators=(",", ":")) + "\n")
            if args.controller_pid not in record["process_tree_pids"]:
                break
            time.sleep(min(args.interval, max(0.0, stop_at - time.monotonic())))


if __name__ == "__main__":
    main()
