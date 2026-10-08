"""CPU integration checks: literal argv, owned stop, stale PID and RAM guard."""
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from zvram_manager import Manager, identity, owned_worker, write_json


class ManagerChecks(unittest.TestCase):
    def test_lifecycle_and_isolation(self):
        with tempfile.TemporaryDirectory() as temp:
            manager = Manager(Path(temp) / "state")
            output = Path(temp) / "literal.txt"
            literal = "$(touch should-never-exist); `false`"
            command = [sys.executable, "-c", "import pathlib,sys,time; pathlib.Path(sys.argv[1]).write_text(sys.argv[2]); time.sleep(60)", str(output), literal]
            manager.save_profile(dict(name="test", priority="low", command=command, min_available_mib=1))
            unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
            try:
                manager.start("test")
                for _ in range(100):
                    if output.exists():
                        break
                    time.sleep(.05)
                self.assertEqual(output.read_text(), literal)
                self.assertTrue(manager.list_profiles()[0]["running"])
                manager.set_priority("test", "high")
                self.assertTrue(manager.list_profiles()[0]["running"])
                self.assertTrue(manager.stop("test"))
                for _ in range(100):
                    if not manager.list_profiles()[0]["running"]:
                        break
                    time.sleep(.05)
                self.assertFalse(manager.list_profiles()[0]["running"])
                self.assertIsNone(unrelated.poll())
                self.assertEqual(manager.home.stat().st_mode & 0o777, 0o700)
                self.assertEqual(manager.profiles_path.stat().st_mode & 0o777, 0o600)
                fake = {"identity": identity(unrelated.pid), "token": "fake"}
                self.assertFalse(owned_worker(fake))
                write_json(manager.job_path("test"), fake)
                self.assertFalse(manager.stop("test"))
                self.assertIsNone(unrelated.poll())
            finally:
                manager.stop("test")
                unrelated.terminate()
                unrelated.wait()

    def test_validation_and_launch_guard(self):
        with tempfile.TemporaryDirectory() as temp:
            manager = Manager(temp)
            for name in ("../bad", "bad/name", ""):
                with self.assertRaises(ValueError):
                    manager.save_profile(dict(name=name, command=["true"]))
            with self.assertRaises(ValueError):
                manager.save_profile(dict(name="bad", command=["true"], resident_mib=0))
            manager.save_profile(dict(name="guard", command=["true"], min_available_mib=1024))
            with patch("zvram_manager.memory_status", return_value={"mem_available_mib": 100, "swap_used_mib": 0}):
                with self.assertRaisesRegex(ValueError, "RAM"):
                    manager.start("guard")
            profile = dict(name="cap", command=["app", "--literal"], priority="low", mode="vulkan")
            command, cap = manager.launch_command(profile)
            self.assertIn("--vulkan-resident-mib", command)
            self.assertNotIn("--vulkan-resident-after-cold", command)
            self.assertEqual(command[-2:], ["app", "--literal"])
            high, highcap = manager.launch_command(dict(profile, priority="high"))
            self.assertGreater(highcap, cap)

    def test_worker_pressure_cleanup(self):
        with tempfile.TemporaryDirectory() as temp:
            manager = Manager(temp)
            profile = {"name": "pressure", "command": [sys.executable, "-c", "import time; time.sleep(60)"],
                       "mode": "native", "priority": "normal", "min_available_mib": 1}
            manager.save_profile(profile)
            job = {"token": "test-token", "identity": identity(os.getpid()), "profile": profile,
                   "command": profile["command"], "resident_mib": None, "min_available_mib": 1}
            write_json(manager.job_path("pressure"), job)
            previous = {s: signal.getsignal(s) for s in (signal.SIGTERM, signal.SIGINT)}
            try:
                with patch("zvram_manager.memory_status", return_value={"mem_available_mib": 0, "swap_used_mib": 0}):
                    manager.worker("pressure", "test-token")
            finally:
                for sig, handler in previous.items():
                    signal.signal(sig, handler)
            result = json.loads(manager.job_path("pressure").read_text())
            self.assertEqual(result["reason"], "Stopped: available RAM guard")
            self.assertEqual(result["returncode"], -signal.SIGTERM)
            self.assertIsNone(identity(result["child_pid"]))


if __name__ == "__main__":
    unittest.main()
