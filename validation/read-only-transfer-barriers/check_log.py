#!/usr/bin/env python3
"""Verify cache reuse from actual layer telemetry, not freeze counts."""
import argparse
import json
import re
import os
import signal
import subprocess
import sys
import tempfile
from pathlib import Path


def check(path):
    text = Path(path).read_text()
    assert not re.search(r"FAIL|VUID-|Validation Error", text), "failure or validation diagnostic"
    markers = ["READONLY_BARRIER_CACHE_READ_BEGIN", "READONLY_BARRIER_CACHE_READ_END",
               "READONLY_BARRIER_CACHE_WRITE_BEGIN", "READONLY_BARRIER_CACHE_WRITE_END"]
    for marker in markers:
        assert text.count(marker) == 1, f"missing/duplicate marker: {marker}"
    offsets = [text.index(marker) for marker in markers]
    assert offsets == sorted(offsets), "phase order invalid"
    assert ("PASS: finite-barrier read/write bytes and cleanup verified" in text or
            "PASS: read-only finite barriers preserved cache and real writes changed bytes" in text), "byte proof missing"
    states = []
    for match in re.finditer(r"\[zvram\] snapshot state event=(\S+)([^\n]*)", text):
        values = {k: int(v) for k, v in re.findall(r"([a-z-]+)=(\d+)", match[2])}
        states.append((match.start(), match[1], values))
    read_states = [s for s in states if offsets[0] < s[0] < offsets[1]]
    assert read_states, "no read-phase layer telemetry"
    reuses = [s[2]["clean-reuses"] for s in read_states]
    invalidations = [s[2]["cache-invalidations"] for s in read_states]
    assert reuses[-1] - reuses[0] >= 3, "fewer than three read-phase clean-cache reuses"
    assert max(invalidations) == min(invalidations), "read-only phase invalidated cache"
    write_states = [s for s in states if offsets[2] < s[0] < offsets[3]]
    assert write_states, "no actual-write phase layer telemetry"
    write_invalidations = max(s[2]["cache-invalidations"] for s in write_states) - invalidations[-1]
    assert write_invalidations >= 1, "actual write did not invalidate cached data before cleanup"
    final = [s[2] for s in states if s[1] == "destroy-cleanup"]
    assert len(final) == 1, "expected one final cleanup state"
    final = final[0]
    for key in ("resident", "cold-logical", "cold-stored", "cache-stored", "failures"):
        assert final[key] == 0, f"nonzero final {key}"
    assert re.search(r"live-local=0 .*live-nonlocal=0 .*allocation-failures=0", text), "driver cleanup missing"
    return {"read_phase_reuses": reuses[-1] - reuses[0],
            "read_phase_invalidations": max(invalidations) - min(invalidations),
            "write_phase_invalidations": write_invalidations,
            "final": final, "scope": "cache reuse and byte-integrity correctness; not FPS or latency"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    if len(sys.argv) > 1 and sys.argv[1] == "--run":
        assert len(sys.argv) > 2, "missing fixture command"
        process = subprocess.Popen(sys.argv[2:], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            output, _ = process.communicate(timeout=50)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                output, _ = process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                output, _ = process.communicate()
            sys.stdout.buffer.write(output)
            raise SystemExit(124)
        sys.stdout.buffer.write(output)
        sys.stdout.buffer.flush()
        if process.returncode:
            raise SystemExit(process.returncode)
        with tempfile.NamedTemporaryFile(suffix=".log") as capture:
            capture.write(output)
            capture.flush()
            print(json.dumps(check(capture.name), indent=2))
    else:
        parser.add_argument("log")
        print(json.dumps(check(parser.parse_args().log), indent=2))
