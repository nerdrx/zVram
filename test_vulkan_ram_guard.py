#!/usr/bin/env python3
"""CPU-only check for low-memory abort cleanup."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


MODULE = Path(__file__).with_name("check_vulkan_idle_model.py")
spec = importlib.util.spec_from_file_location("idle_model", MODULE)
idle_model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(idle_model)


def check_phase(phase):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        marker = root / "child.pid"
        prompt_marker = root / "prompt.received"
        output = root / "output"
        output.mkdir()
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        def available():
            deadline = time.monotonic() + 3
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            return 2 if phase == "load" or prompt_marker.exists() else 5

        idle_model.available_memory_mib = available
        child = ("import os,time; print('child-start', flush=True); "
                 "print('child-error', file=__import__('sys').stderr, flush=True); "
                 "open(os.environ['PID_FILE'],'w').write(str(os.getpid())); "
                 "print('Vulkan0 model buffer size = 1 MiB\\n== Running in interactive mode. ==', "
                 "file=__import__('sys').stderr, flush=True); "
                 "__import__('sys').stdin.readline(); "
                 "open(os.environ['PROMPT_FILE'],'w').write('received'); time.sleep(30)")
        env = os.environ.copy()
        env["PID_FILE"] = str(marker)
        env["PROMPT_FILE"] = str(prompt_marker)
        try:
            try:
                idle_model.run_interactive("guard", [sys.executable, "-c", child], env,
                                           output, 10, False,
                                           min_available_mib=3)
            except RuntimeError as error:
                assert "below guard floor" in str(error), error
            else:
                raise AssertionError("RAM guard did not abort")

            child_pid = int(marker.read_text())
            for _ in range(50):
                try:
                    os.kill(child_pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.02)
            else:
                raise AssertionError("guard child survived abort")
            assert unrelated.poll() is None, "unrelated process was affected"
            assert "child-start" in (output / "guard.stdout.txt").read_text()
            assert "child-error" in (output / "guard.stderr.txt").read_text()
            assert prompt_marker.exists() == (phase == "generate")
            evidence = json.loads((output / "guard.resources.json").read_text())
            assert evidence["minimum_available_mib"] == 2
            assert evidence["min_available_mib"] == 3
            assert evidence["process_returncode"] != 0
            assert evidence["prompt_sent"] == (phase == "generate")
        finally:
            unrelated.terminate()
            unrelated.wait()


def check_swap_phase(phase):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        marker = root / "child.pid"
        prompt_marker = root / "prompt.received"
        output = root / "output"
        output.mkdir()
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])

        def swap_used():
            if phase == "load" and marker.exists() and marker.read_text().strip():
                return 105
            if phase == "generate" and prompt_marker.exists():
                return 105
            return 100

        idle_model.available_memory_mib = lambda: 128
        idle_model.used_swap_mib = swap_used
        child = ("import os,time,sys; print('child-start', flush=True); "
                 "print('child-error', file=sys.stderr, flush=True); "
                 "open(os.environ['PID_FILE'],'w').write(str(os.getpid())); "
                 "time.sleep(30) if os.environ['GUARD_PHASE']=='load' else None; "
                 "print('Vulkan0 model buffer size = 1 MiB\\n== Running in interactive mode. ==', "
                 "file=sys.stderr, flush=True); "
                 "sys.stdin.readline(); "
                 "open(os.environ['PROMPT_FILE'],'w').write('received'); time.sleep(30)")
        env = os.environ.copy()
        env["PID_FILE"] = str(marker)
        env["PROMPT_FILE"] = str(prompt_marker)
        env["GUARD_PHASE"] = phase
        try:
            try:
                idle_model.run_interactive("swap", [sys.executable, "-c", child], env,
                                           output, 10, False, max_swap_growth_mib=2)
            except RuntimeError as error:
                assert "swap growth 5 MiB above guard limit 2 MiB" in str(error), error
            else:
                raise AssertionError("swap growth guard did not abort")

            child_pid = int(marker.read_text())
            for _ in range(50):
                try:
                    os.kill(child_pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.02)
            else:
                raise AssertionError("swap-guard child survived abort")
            assert unrelated.poll() is None, "unrelated process was affected"
            assert "child-start" in (output / "swap.stdout.txt").read_text()
            assert "child-error" in (output / "swap.stderr.txt").read_text()
            assert prompt_marker.exists() == (phase == "generate")
            assert idle_model.available_memory_mib() == 128, "fixture did not keep MemAvailable healthy"
            evidence = json.loads((output / "swap.resources.json").read_text())
            assert evidence["swap_used_baseline_mib"] == 100
            assert evidence["swap_used_peak_mib"] == 105
            assert evidence["swap_growth_mib"] == 5
            assert evidence["max_swap_growth_mib"] == 2
            assert evidence["process_returncode"] != 0
            assert evidence["prompt_sent"] == (phase == "generate")
        finally:
            unrelated.terminate()
            unrelated.wait()


if __name__ == "__main__":
    assert idle_model.available_memory_mib() > 0
    for phase in ("load", "generate"):
        check_phase(phase)
        check_swap_phase(phase)
    for value in ("0", "18446744073709551616"):
        result = subprocess.run([sys.executable, str(MODULE), "--binary", "/missing", "--model", "/missing",
                                 "--max-swap-growth-mib", value], capture_output=True, text=True)
        assert result.returncode == 2 and "must be positive and fit uint64" in result.stderr, result.stderr
