#!/usr/bin/env python3
"""CPU-only check for low-memory abort cleanup."""

import importlib.util
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


def main():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        marker = root / "child.pid"
        output = root / "output"
        output.mkdir()
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        def available():
            deadline = time.monotonic() + 3
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            return 2

        idle_model.available_memory_mib = available
        child = ("import os,time; print('child-start', flush=True); "
                 "print('child-error', file=__import__('sys').stderr, flush=True); "
                 "open(os.environ['PID_FILE'],'w').write(str(os.getpid())); time.sleep(30)")
        env = os.environ.copy()
        env["PID_FILE"] = str(marker)
        try:
            try:
                idle_model.run_interactive("guard", [sys.executable, "-c", child], env,
                                           output, 10, False,
                                           min_available_mib=(1 << 64) - 1)
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
        finally:
            unrelated.terminate()
            unrelated.wait()


if __name__ == "__main__":
    main()
