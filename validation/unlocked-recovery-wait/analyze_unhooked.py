#!/usr/bin/env python3
"""Correlate exact-copy submit calls with same-process recovery profile windows."""
import argparse
import json
import re
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    def percentile(percent):
        return values[((len(values) - 1) * percent + 99) // 100]
    return {"count": len(values), "p50_ns": percentile(50),
            "p95_ns": percentile(95), "max_ns": values[-1]}


def analyze(path, unlocked):
    text = Path(path).read_text()
    assert not re.search(r"VUID-|FAIL:|Validation Error", text), "fixture/validation failure"
    phases = [tuple(map(int, m)) for m in re.findall(
        r"warm-recovery copy-phase bytes=(\d+) start-monotonic-ns=(\d+) "
        r"end-monotonic-ns=(\d+) unlocked=(\d+)", text)]
    samples = [tuple(map(int, m)) for m in re.findall(
        r"recovery-submit-sample transaction=(\d+) index=(\d+) "
        r"start-monotonic-ns=(\d+) end-monotonic-ns=(\d+) submit-ns=(\d+)", text)]
    transactions = [tuple(map(int, m)) for m in re.findall(
        r"recovery-submit-transaction=(\d+) last-child0-submit-start-monotonic-ns=(\d+) "
        r"last-child0-submit-end-monotonic-ns=(\d+) window-start-monotonic-ns=(\d+)", text)]
    assert len(phases) == len(transactions) == 5, "need five successful profiled transactions"
    assert len(re.findall(r"PASS: recovery-submit sampling preserved", text)) == 5
    results, all_calls, overlaps = [], [], []
    for phase, transaction in zip(sorted(phases, key=lambda p: p[1]), sorted(transactions)):
        size, phase_start, phase_end, mode = phase
        number, last_start, last_end, window_start = transaction
        assert size == 4 * 1024 * 1024 and mode == unlocked
        assert 0 < last_start <= last_end <= window_start <= phase_start < phase_end
        calls = [s for s in samples if s[0] == number]
        assert calls and all(s[2] <= s[3] and s[4] == s[3] - s[2] for s in calls)
        durations = [s[4] for s in calls]
        intersecting = [s[4] for s in calls if s[2] < phase_end and phase_start < s[3]]
        all_calls.extend(durations)
        overlaps.extend(intersecting)
        results.append({"transaction": number, "copy_phase_ns": phase_end - phase_start,
                        "all_calls": distribution(durations),
                        "intersecting_calls": distribution(intersecting)})
    return {"path": str(path), "unlocked": bool(unlocked), "transactions": results,
            "all_calls": distribution(all_calls), "intersecting_calls": distribution(overlaps)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("unlocked")
    args = parser.parse_args()
    baseline = analyze(args.baseline, 0)
    unlocked = analyze(args.unlocked, 1)
    qualified = bool(baseline["intersecting_calls"] and unlocked["intersecting_calls"])
    print(json.dumps({"status": "window-correlated" if qualified else "inconclusive-no-paired-overlap",
                      "scope": "vkQueueSubmit call only; profile includes copy setup/wait/reacquire, not pure GPU wait or FPS",
                      "baseline": baseline, "unlocked": unlocked}, indent=2))
