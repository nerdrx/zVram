"""CPU protocol checks; fake endpoints never control an unrelated process."""
import os
import json
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from zvram_manager import Manager, identity, gpu_status
import zvram_control as control


class ControlChecks(unittest.TestCase):
    def test_launcher_exports_private_control_directory_for_steam(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / 'build'
            build.mkdir()
            for filename in ('VK_LAYER_NX_zvram.json', 'libzvram_layer.so'):
                (build / filename).touch()
            control_path = Path(temporary) / 'private:control'
            control_path.mkdir(mode=0o700)
            for previous in ('', '/existing/a:/existing/b'):
                env = os.environ | {'ZVRAM_CONTROL_DIR': str(control_path),
                                    'PRESSURE_VESSEL_FILESYSTEMS_RW': previous}
                result = subprocess.run(
                    [sys.executable, str(Path(__file__).with_name('zvram')), '--build-dir', str(build),
                     '--no-live-control', '--', sys.executable, '-c',
                     'import os,json; print(json.dumps([os.environ["ZVRAM_CONTROL_DIR"],os.environ["PRESSURE_VESSEL_FILESYSTEMS_RW"]]))'],
                    env=env, text=True, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                base, exported = json.loads(result.stdout)
                self.assertEqual(base, str(control_path))
                self.assertEqual(exported, previous + (':' if previous else '') + str(control_path).replace(':', '\\:'))

    def test_launcher_layer_paths_include_steam_import_and_native_dirs(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / 'build'
            build.mkdir()
            (build / 'VK_LAYER_NX_zvram.json').touch()
            (build / 'libzvram_layer.so').touch()
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith('VK_LAYER_PATH') and k != 'VK_ADD_LAYER_PATH'}
            env.update(XDG_CONFIG_HOME='/user/config', XDG_CONFIG_DIRS='/etc/xdg:/opt/config',
                       XDG_DATA_HOME='/user/data', XDG_DATA_DIRS='/local/share:/usr/share',
                       VK_ADD_LAYER_PATH='/extra/layers')
            launcher = Path(__file__).with_name('zvram')

            def layer_paths(**extra):
                test_env = env | extra
                result = subprocess.run(
                    [sys.executable, str(launcher), '--build-dir', str(build), '--no-live-control',
                     '--', sys.executable, '-c',
                     'import os,json; print(json.dumps([os.environ.get("VK_LAYER_PATH"), os.environ.get("VK_ADD_LAYER_PATH")]))'],
                    env=test_env, text=True, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                return json.loads(result.stdout)

            search, additional = layer_paths()
            self.assertEqual(search.split(os.pathsep), [
                str(build), '/extra/layers', '/user/config/vulkan/explicit_layer.d',
                '/etc/xdg/vulkan/explicit_layer.d', '/opt/config/vulkan/explicit_layer.d',
                '/user/data/vulkan/explicit_layer.d',
                '/local/share/vulkan/explicit_layer.d', '/usr/share/vulkan/explicit_layer.d',
                '/etc/vulkan/explicit_layer.d', '/usr/local/share/vulkan/explicit_layer.d'])
            self.assertEqual(additional, '/extra/layers')
            search, additional = layer_paths(VK_LAYER_PATH='/custom/one:/custom/two')
            self.assertEqual(search.split(os.pathsep), [str(build), '/custom/one', '/custom/two'])
            self.assertEqual(additional, '/extra/layers')

    def test_launcher_defaults_and_opt_out(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            for filename in ('VK_LAYER_NX_zvram.json', 'libzvram_layer.so', 'libzvram_hip.so'):
                (build / filename).touch()
            env = {k: v for k, v in os.environ.items() if not k.startswith('ZVRAM_VULKAN_')}
            launcher = Path(__file__).with_name('zvram')
            def launch(*flags):
                result = subprocess.run([sys.executable, str(launcher), '--build-dir', str(build), *flags,
                                         '--', sys.executable, '-c',
                                         'import os,json; print(json.dumps({k:v for k,v in os.environ.items() if k.startswith("ZVRAM_VULKAN_")}))'],
                                        env=env, text=True, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                return json.loads(result.stdout)
            default = launch()
            self.assertNotIn('ZVRAM_VULKAN_COLD_CYCLE_RECOVERY', default)
            self.assertEqual(default['ZVRAM_VULKAN_ACTIVE_EVICTION'], '1')
            self.assertEqual(default['ZVRAM_VULKAN_RANGE_MIB'], '32')
            self.assertEqual(default['ZVRAM_VULKAN_PRESSURE_ONLY'], '1')
            self.assertEqual(launch('--vulkan-eviction-trigger', 'idle')['ZVRAM_VULKAN_PRESSURE_ONLY'], '0')
            self.assertEqual(launch('--vulkan-auto-idle-ms', '60000')['ZVRAM_VULKAN_PRESSURE_ONLY'], '0')
            detected = max((c['vram_total_mib'] for c in gpu_status()), default=8192)
            self.assertEqual(int(default['ZVRAM_VULKAN_RESIDENT_MIB']), max(256, int(detected)))
            self.assertEqual(default['ZVRAM_VULKAN_HEADROOM_MIB'], '1536')
            self.assertEqual(launch('--vulkan-resident-mib', '96')['ZVRAM_VULKAN_RESIDENT_MIB'], '96')
            recovered = launch('--vulkan-recover-local')
            self.assertEqual(recovered['ZVRAM_VULKAN_COLD_CYCLE_RECOVERY'], '1')
            self.assertEqual(recovered['ZVRAM_VULKAN_PRESSURE_ONLY'], '1')
            self.assertNotIn('ZVRAM_VULKAN_RECOVER_LOCAL_QUIET_MS', recovered)
            for quiet in (0, 1000, (1 << 32) - 1):
                self.assertEqual(launch('--vulkan-recover-local', '--vulkan-recover-local-quiet-ms', str(quiet))[
                    'ZVRAM_VULKAN_RECOVER_LOCAL_QUIET_MS'], str(quiet))
            for flags in (('--vulkan-recover-local-quiet-ms', '0'),
                          ('--vulkan-recover-local', '--vulkan-recover-local-quiet-ms', '-1'),
                          ('--vulkan-recover-local', '--vulkan-recover-local-quiet-ms', str(1 << 32))):
                rejected = subprocess.run([sys.executable, str(launcher), '--build-dir', str(build), *flags,
                                           '--', sys.executable, '-c', 'pass'],
                                          env=env, text=True, capture_output=True, timeout=10)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn('--vulkan-recover-local-quiet-ms requires', rejected.stderr)
            manual = launch('--no-live-control', '--vulkan-recover-local', '--vulkan-virtual-mib', '32768',
                            '--vulkan-auto-idle-ms', '1000', '--vulkan-cold-mib', '4096',
                            '--vulkan-eviction-trigger', 'pressure', '--vulkan-selective-restore',
                            '--vulkan-active-eviction', '--vulkan-range-mib', '32',
                            '--vulkan-resident-mib', '256', '--vulkan-lazy-backing',
                            '--vulkan-headroom-mib', '1536')
            self.assertEqual(manual['ZVRAM_VULKAN_COLD_CYCLE_RECOVERY'], '1')
            invalid = subprocess.run(
                [sys.executable, str(launcher), '--build-dir', str(build), '--no-live-control',
                 '--vulkan-recover-local', '--', sys.executable, '-c', 'pass'],
                env=env, text=True, capture_output=True, timeout=10)
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn('--vulkan-recover-local requires', invalid.stderr)
            self.assertEqual(launch('--live-control'), default)
            self.assertEqual(launch('--no-live-control'), {'ZVRAM_VULKAN_CODEC': 'zstd'})
            self.assertEqual(launch('--no-live-control', '--vulkan-virtual-gib', '96'),
                             {'ZVRAM_VULKAN_VIRTUAL_MIB': '98304', 'ZVRAM_VULKAN_CODEC': 'zstd'})
            self.assertEqual(launch('--hip'), {})
            self.assertEqual(launch('--vulkan-resident-mib', '256')['ZVRAM_VULKAN_RESIDENT_MIB'], '256')

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
