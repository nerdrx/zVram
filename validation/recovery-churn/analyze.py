#!/usr/bin/env python3
"""Summarize the bounded synthetic recovery-churn fixture; not game performance."""
import argparse
import json
import re
import statistics
from pathlib import Path


def check(path, quiet):
    text = Path(path).read_text()
    assert not re.search(r"FAIL|VUID-|Validation Error|UNSUPPORTED", text), "fixture/validation failure"
    headers = re.findall(r"recovery-churn-mode [^\n]+", text)
    assert len(headers) == 1 and f"quiet-ms={quiet}" in headers[0], "wrong/missing mode"
    assert "synthetic-forced-nonlocal=1" in headers[0], "missing synthetic provenance"
    rows = [json.loads(m) for m in re.findall(r"recovery-churn (\{[^\n]+\})", text)]
    assert len(rows) == 8, "expected eight full-byte accesses"
    for i, row in enumerate(rows):
        assert row["access"] == i and row["buffer"] == i % 3, "unexpected access order"
        for key in ("queue_submit_us", "fence_wait_us", "freezes_delta", "restores_delta"):
            assert isinstance(row[key], int) and row[key] >= 0, f"invalid {key}"
    summary = [json.loads(m) for m in re.findall(r"recovery-churn-summary (\{[^\n]+\})", text)]
    assert len(summary) == 1 and summary[0]["accesses"] == 8, "missing summary"
    assert "PASS: recovery-churn full bytes and cleanup verified" in text, "missing byte/cleanup proof"
    cleanup = re.findall(r"snapshot state event=destroy-cleanup([^\n]*)", text)
    assert len(cleanup) == 1, "missing final layer cleanup"
    fields = dict((k, int(v)) for k, v in re.findall(r"([a-z-]+)=(\d+)", cleanup[0]))
    for key in ("resident", "cold-logical", "cold-stored", "cache-stored", "failures"):
        assert fields[key] == 0, f"nonzero final {key}"
    assert re.search(r"live-local=0 .*live-nonlocal=0 .*allocation-failures=0", text), "missing driver cleanup"
    warm = re.findall(r"warm-recovery local-copy bytes=(\d+) memory-type=\d+->\d+ duration-us=(\d+)", text)
    def latency(key):
        values = [r[key] for r in rows]
        return {"samples": len(values), "median_us": statistics.median(values), "max_us": max(values)}
    return {"quiet_ms": quiet, "synthetic_forced_nonlocal": True,
            "queue_submit": latency("queue_submit_us"), "fence_wait": latency("fence_wait_us"),
            "actual_warm_copy_events": len(warm), "actual_warm_copy_bytes": sum(int(x[0]) for x in warm),
            "freeze_count": summary[0]["freeze_count"], "restore_count": summary[0]["restore_count"],
            "accesses_with_restore": sum(r["restores_delta"] > 0 for r in rows), "rows": rows,
            "scope": "one synthetic workload per quiet setting; no stable latency or game FPS conclusion"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("quiet0")
    parser.add_argument("quiet1000")
    args = parser.parse_args()
    print(json.dumps({"runs": [check(args.quiet0, 0), check(args.quiet1000, 1000)]}, indent=2))
