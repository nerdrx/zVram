"""CPU protocol checks; fake endpoints never control an unrelated process."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from zvram_manager import Manager, identity
import zvram_control as control


class ControlChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.env = patch.dict(os.environ, ZVRAM_CONTROL_DIR=str(base / 'control'))
        self.env.start()
        self.addCleanup(self.env.stop)
        self.manager = Manager(base / 'state')
        self.saved = identity(os.getpid())
        self.proc = self.manager.control_home / f"{os.getpid()}-{self.saved['start']}"
        self.proc.mkdir(mode=0o700)
        self.device = dict(version=1, pid=os.getpid(), start=int(self.saved['start']), device=42,
                           capable=1, current_limit_mib=512, resident_mib=256, min_limit_mib=32,
                           max_limit_mib=2048, requested_mib=0, seq=0, ack=0, result=0, reason=0)
        self.status = self.proc / '42.status'
        self.write_status()

    def write_status(self):
        self.status.write_text(''.join(f'{k}={v}\n' for k, v in self.device.items()))
        self.status.chmod(0o600)

    def row(self):
        return dict(name='@test', running=True, external=True, pid=os.getpid(), process_start=self.saved['start'],
                    control_devices=control.status(self.manager.control_home, os.getpid(), self.saved['start']))

    def test_requests_and_identity(self):
        with patch.object(self.manager, 'list_profiles', side_effect=lambda: [self.row()]):
            first = self.manager.set_live_priority('@test', 'low')
            self.assertEqual(first, [dict(device='42', seq=1, resident_mib=512)])
            self.assertEqual(self.manager.set_live_limit('@test', 1024)[0]['seq'], 2)
            self.assertEqual((self.proc / '42.request').stat().st_mode & 0o777, 0o600)
            with self.assertRaises(ValueError):
                self.manager.set_live_limit('@test', 4096)
            with patch('zvram_manager.identity', return_value=None):
                with self.assertRaises(ValueError):
                    self.manager.set_live_limit('@test', 256)
            self.assertFalse(self.manager.profiles_path.exists())

    def test_reject_unsafe_and_unsupported(self):
        self.status.chmod(0o644)
        self.assertEqual(self.row()['control_devices'], [])
        self.status.unlink()
        outside = Path(self.temp.name) / 'outside'
        outside.write_text('private sentinel')
        self.status.symlink_to(outside)
        self.assertEqual(self.row()['control_devices'], [])
        self.status.unlink()
        self.device['capable'] = 0
        self.write_status()
        with patch.object(self.manager, 'list_profiles', side_effect=lambda: [self.row()]):
            with self.assertRaisesRegex(ValueError, 'Restart'):
                self.manager.set_live_limit('@test', 256)
        self.device['capable'] = 1
        self.write_status()
        (self.proc / '42.request').symlink_to(outside)
        with patch.object(self.manager, 'list_profiles', side_effect=lambda: [self.row()]):
            with self.assertRaises(OSError):
                self.manager.set_live_limit('@test', 256)
        self.assertEqual(outside.read_text(), 'private sentinel')

    def test_bounded_numeric_parser(self):
        for contents in ('version=2\n', 'version=1\nseq=1\nseq=2\n', 'version=1\nseq=-1\n', 'x' * 1025):
            self.status.write_text(contents)
            self.status.chmod(0o600)
            self.assertEqual(self.row()['control_devices'], [])


if __name__ == '__main__':
    unittest.main()
