#!/usr/bin/env python3
"""Compare before/after final transitions with unlocked wait enabled in both."""
import argparse
import importlib.util
import json
import re
import statistics
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "unhooked", Path(__file__).resolve().parents[1] / "analyze_unhooked.py")
unhooked = importlib.util.module_from_spec(spec)
spec.loader.exec_module(unhooked)


def analyze(path):
    result = unhooked.analyze(path, 1, True)
    phases = [tuple(map(int, m)) for m in re.findall(
        r"warm-recovery phases bytes=(\d+) view-us=\d+ allocate-us=\d+ "
        r"alias-unbind-us=\d+ private-bind-us=\d+ copy-wait-us=\d+ "
        r"private-unbind-us=(\d+) app-rebind-us=(\d+) release-us=\d+",
        Path(path).read_text())]
    assert len(phases) == 5 and all(p[0] == 4 * 1024 * 1024 for p in phases)
    totals = [unbind + rebind for _, unbind, rebind in phases]
    result["final_transition_us"] = totals
    result["final_transition_median_us"] = statistics.median(totals)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before")
    parser.add_argument("after")
    args = parser.parse_args()
    print(json.dumps({
        "scope": "final sparse transition wall time, including planning/wait/commit; one before/after pair, not FPS",
        "before": analyze(args.before), "after": analyze(args.after)}, indent=2))
