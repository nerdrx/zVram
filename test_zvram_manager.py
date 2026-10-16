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

from zvram_manager import Manager, identity, owned_worker, write_json, discovered_processes


class ManagerChecks(unittest.TestCase):
    def test_failed_json_update_preserves_original_and_removes_temp(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'profiles.json'
            path.write_text('{"keep": true}')
            with patch('zvram_manager.os.replace', side_effect=OSError('injected replace failure')):
                with self.assertRaisesRegex(OSError, 'injected replace failure'):
                    write_json(path, {'new': True})
            self.assertEqual(json.loads(path.read_text()), {'keep': True})
            self.assertEqual(list(Path(temporary).iterdir()), [path])
            with self.assertRaises(TypeError):
                write_json(path, {'invalid': object()})
            self.assertEqual(json.loads(path.read_text()), {'keep': True})
            self.assertEqual(list(Path(temporary).iterdir()), [path])

    def test_run_rejects_empty_command_after_separator(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = subprocess.run(
                [sys.executable, str(Path(__file__).with_name('zvram')), 'run', '--', '--'],
                env=os.environ | {'ZVRAM_MANAGER_HOME': temporary,
                                  'ZVRAM_CONTROL_DIR': str(Path(temporary) / 'control')},
                capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 2)
            self.assertIn('run requires a command', result.stderr)
            self.assertNotIn('Traceback', result.stderr)
            self.assertFalse((Path(temporary) / 'profiles.json').exists())

    def test_external_discovery_filters_and_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            def process(pid, env=b'', maps='', parent=1):
                p = root / str(pid)
                p.mkdir()
                p.joinpath('stat').write_text(str(pid) + ' (test) ' + ' '.join(['S', str(parent)] + ['0'] * 17 + ['123']))
                p.joinpath('environ').write_bytes(env)
                p.joinpath('maps').write_text(maps)
                p.joinpath('comm').write_text('test-app\n')
            process(10, b'VK_INSTANCE_LAYERS=VK_LAYER_NX_zvram\0ZVRAM_VULKAN_RESIDENT_MIB=512\0SECRET=hidden\0')
            process(11, b'VK_INSTANCE_LAYERS=VK_LAYER_NX_zvram_fake\0')
            process(12, maps='0 0 0 0 0 /old/version/libzvram_layer.so (deleted)\n')
            process(13, b'LD_PRELOAD=/path/libzvram_hip.so\0')
            process(14, b'VK_INSTANCE_LAYERS=VK_LAYER_NX_zvram\0', parent=10)
            process(15, b'ZVRAM_VERBOSE=1\0')
            with patch('zvram_manager.process_usage', return_value={}):
                rows = discovered_processes(proc_root=root)
                self.assertEqual([r['pid'] for r in rows], [10, 12, 13, 14])
                self.assertEqual(rows[0]['active_resident_mib'], 512)
                self.assertEqual(rows[1]['state'], 'Layer loaded')
                self.assertNotIn('hidden', json.dumps(rows))
                self.assertEqual([r['pid'] for r in discovered_processes([10], root)], [12, 13])
                with patch('zvram_manager.os.getuid', return_value=os.getuid() + 1):
                    self.assertEqual(discovered_processes(proc_root=root), [])

    def test_external_launcher_visible_without_ownership(self):
        with tempfile.TemporaryDirectory() as temp:
            manager = Manager(temp)
            wrapped = subprocess.Popen([str(Path(__file__).parent / 'zvram'), '--', sys.executable,
                                        '-c', 'import time; time.sleep(60)'])
            unrelated = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
            try:
                for _ in range(50):
                    rows = discovered_processes()
                    if any(r['pid'] == wrapped.pid for r in rows):
                        break
                    time.sleep(.05)
                row = next(r for r in rows if r['pid'] == wrapped.pid)
                self.assertTrue(row['external'])
                self.assertNotIn(unrelated.pid, [r['pid'] for r in rows])
                self.assertIn(wrapped.pid, [r['pid'] for r in manager.list_profiles()])
                for action in (manager.start, manager.delete):
                    with self.assertRaises(ValueError):
                        action(row['name'])
                self.assertIsNone(wrapped.poll())
                self.assertIsNone(unrelated.poll())
                self.assertFalse(manager.profiles_path.exists())
                self.assertTrue(manager.stop(row['name']))
                self.assertEqual(wrapped.wait(timeout=5), -signal.SIGTERM)
                self.assertIsNone(unrelated.poll())
            finally:
                wrapped.terminate()
                unrelated.terminate()
                wrapped.wait()
                unrelated.wait()

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
                # A new package must still recognize and stop a worker from the previous version.
                job = json.loads(manager.job_path("test").read_text())
                with patch("zvram_manager.__file__", "/different-version/zvram_manager.py"):
                    self.assertTrue(owned_worker(job))
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
            for value in ("false", 1, None):
                with self.assertRaisesRegex(ValueError, "boolean"):
                    manager.save_profile(dict(name="bad", command=["true"], ignore_swap_guard=value))
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
                       "mode": "native", "priority": "normal", "min_available_mib": 1, "ignore_swap_guard": True}
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

    def test_worker_setup_failure_is_recorded_without_following_log_symlink(self):
        with tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, {
                "ZVRAM_CONTROL_DIR": str(Path(temp) / "control"), "XDG_RUNTIME_DIR": ""}):
            manager = Manager(Path(temp) / "state")
            target = Path(temp) / "target.log"
            target.write_text("keep this intact\n")
            (manager.home / "setup.log").symlink_to(target)
            profile = {"name": "setup", "command": [sys.executable, "-c", "raise SystemExit(99)"],
                       "min_available_mib": 1}
            write_json(manager.job_path("setup"), {"token": "setup-token", "identity": identity(os.getpid()),
                       "profile": profile, "command": profile["command"], "min_available_mib": 1})

            with patch("zvram_manager.subprocess.Popen") as popen, patch("zvram_manager.signal.signal"):
                result = manager.worker("setup", "setup-token")

            job = json.loads(manager.job_path("setup").read_text())
            self.assertEqual(result, 1)
            self.assertEqual(job["state"], "failed")
            self.assertEqual(job["returncode"], 1)
            self.assertTrue(job["reason"])
            popen.assert_not_called()
            self.assertEqual(target.read_text(), "keep this intact\n")
            self.assertTrue((manager.home / "setup.log").is_symlink())

    def test_worker_log_write_and_close_failures_persist(self):
        original_open, original_fdopen = os.open, os.fdopen

        class ExitedChild:
            pid = 2147483647
            returncode = 0

            def poll(self):
                return self.returncode

        class LogStream:
            def __init__(self, stream, fail_write=None, fail_close=False):
                self.stream, self.fail_write, self.fail_close = stream, fail_write, fail_close
                self.writes = 0

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.stream.close()
                if self.fail_close:
                    raise OSError("injected log close failure")

            def write(self, value):
                self.writes += 1
                if self.writes == self.fail_write:
                    raise OSError("injected log write failure")
                return self.stream.write(value)

            def flush(self):
                return self.stream.flush()

        for case, fail_write, fail_close, launches_child in (
                ("header", 1, False, False), ("final", 2, False, True), ("close", None, True, True)):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, {
                    "ZVRAM_CONTROL_DIR": str(Path(temp) / "control"), "XDG_RUNTIME_DIR": ""}):
                manager = Manager(Path(temp) / "state")
                profile = {"name": "logging", "command": [sys.executable, "-c", "pass"], "env": {},
                           "min_available_mib": 1}
                write_json(manager.job_path("logging"), {
                    "token": "logging-token", "identity": identity(os.getpid()), "profile": profile,
                    "command": profile["command"], "min_available_mib": 1, "state": "starting"})
                log_fds = set()

                def tracked_open(path, *args, **kwargs):
                    fd = original_open(path, *args, **kwargs)
                    if Path(path).name == "logging.log":
                        log_fds.add(fd)
                    return fd

                def wrapped_fdopen(fd, mode):
                    stream = original_fdopen(fd, mode)
                    if fd in log_fds:
                        log_fds.remove(fd)
                        return LogStream(stream, fail_write, fail_close)
                    return stream

                handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGINT)}
                try:
                    with patch("zvram_manager.os.open", side_effect=tracked_open), \
                            patch("zvram_manager.os.fdopen", side_effect=wrapped_fdopen), \
                            patch("zvram_manager.subprocess.Popen", return_value=ExitedChild()) as popen:
                        result = manager.worker("logging", "logging-token")
                finally:
                    for sig, handler in handlers.items():
                        signal.signal(sig, handler)

                job = json.loads(manager.job_path("logging").read_text())
                self.assertEqual(result, 1)
                self.assertEqual(job["state"], "failed")
                self.assertEqual(job["returncode"], 1)
                self.assertIn("injected log", job["reason"])
                self.assertEqual(popen.call_count, int(launches_child))

    def test_worker_swap_guard_opt_out(self):
        for ignore in (False, True):
            with self.subTest(ignore=ignore), tempfile.TemporaryDirectory() as temp:
                manager = Manager(temp)
                profile = manager.save_profile({"name": "swap", "command": [sys.executable, "-c", "import time; time.sleep(0.8)"],
                                                "ignore_swap_guard": ignore})
                self.assertEqual(manager.list_profiles()[0]["ignore_swap_guard"], ignore)
                write_json(manager.job_path("swap"), {"token": "test", "identity": identity(os.getpid()),
                           "profile": profile, "command": profile["command"], "min_available_mib": 1})
                previous = {s: signal.getsignal(s) for s in (signal.SIGTERM, signal.SIGINT)}
                calls = 0
                def usage():
                    nonlocal calls
                    calls += 1
                    return {"mem_available_mib": 20000, "swap_used_mib": 0 if calls == 1 else 5000}
                try:
                    with patch("zvram_manager.memory_status", side_effect=usage):
                        manager.worker("swap", "test")
                finally:
                    for sig, handler in previous.items():
                        signal.signal(sig, handler)
                result = json.loads(manager.job_path("swap").read_text())
                self.assertEqual(result["reason"], "Process exited (0)" if ignore else "Stopped: system swap growth guard")


if __name__ == "__main__":
    unittest.main()
