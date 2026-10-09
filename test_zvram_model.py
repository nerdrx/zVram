#!/usr/bin/env python3
"""CPU-only model runner and endpoint safety checks."""
import json
import os
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch

import zvram_model as m


class ModelIntegrationTest(unittest.TestCase):
    def test_setup_swap_guard_setting(self):
        for ignore in (False, True):
            argv = ['zvram_model.py', 'setup', '--model', '/unused/model.gguf', '--name', 'tiny']
            if ignore:
                argv.append('--ignore-swap-guard')
            with patch.object(sys, 'argv', argv), patch.object(m, 'build_server_command', return_value=(['server'], {})) as build, \
                    patch('zvram_manager.Manager') as manager, patch('builtins.print'):
                m.main()
            self.assertEqual(manager.return_value.save_profile.call_args.args[0]['ignore_swap_guard'], ignore)
            self.assertNotIn('ignore_swap_guard', build.call_args.kwargs)

    def test_command_and_model_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            server = root / 'server with spaces'
            server.write_text('#!/bin/sh\nexit 0\n')
            server.chmod(0o700)
            store = root / 'data/ollama/models'
            digest = 'a' * 64
            blob = store / 'blobs' / ('sha256-' + digest)
            blob.parent.mkdir(parents=True)
            blob.write_bytes(b'GGUFfixture')
            manifest = store / 'manifests/registry.ollama.ai/library/tiny/f16'
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({'layers': [
                {'mediaType': 'application/vnd.ollama.image.model', 'digest': 'sha256:' + digest},
                {'mediaType': 'application/vnd.ollama.image.model', 'digest': '../../outside'}]}))
            models = m.discover_models(root)
            self.assertEqual(models, [{'name': 'tiny:f16', 'path': str(blob), 'size': 11}])
            with patch.dict(os.environ, {'ZVRAM_STALE_SETTING': '1'}):
                command, env = m.build_server_command(blob, 'tiny:f16', server=server,
                                                      compressed=True, resident_mib=1234)
            self.assertNotIn('ZVRAM_STALE_SETTING', env)
            self.assertEqual(command[command.index('--vulkan-resident-mib') + 1], '1234')
            self.assertEqual(command[command.index('--host') + 1], '127.0.0.1')
            self.assertEqual(command[command.index('--') + 1], str(server))
            self.assertEqual(env['ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB'], '26624')
            command, env = m.build_server_command(blob, 'tiny:f16', server=server, mode='native')
            self.assertEqual(command[0], str(server))
            command, _ = m.build_server_command(blob, 'tiny', server=server)
            self.assertIn('--live-control', command)
            self.assertNotIn('--no-live-control', command)
            command, _ = m.build_server_command(blob, 'tiny', server=server, live_control=False)
            self.assertIn('--no-live-control', command)
            command, _ = m.build_server_command(blob, 'tiny', server=server, compressed=True, live_control=False)
            self.assertIn('--live-control', command)
            for options in ({'port': 0}, {'clean_cache_mib': 30000}, {'alias': 'x\ninjected'},
                            {'mode': 'native', 'compressed': True}, {'live_control': 'yes'}):
                kwargs = {'alias': 'tiny', 'server': server, **options}
                with self.assertRaises(ValueError):
                    m.build_server_command(blob, **kwargs)
            config = root / 'config.json'
            config.write_text(json.dumps({'checkout': str(root), 'port': 7000}))
            self.assertEqual(m.discover_xenium(config)['checkout'], str(root))

    def test_webkit_cookie_scope_expiry_and_injection(self):
        with tempfile.TemporaryDirectory() as directory:
            cookie = Path(directory) / 'cookies'
            cookie.write_text('# Netscape HTTP Cookie File\n'
                              'evil.test\tFALSE\t/\tFALSE\t200\todysseus_session\twrong\t0\n'
                              '127.0.0.1\tFALSE\t/\tFALSE\t50\todysseus_session\texpired\n'
                              '#HttpOnly_127.0.0.1\tFALSE\t/\tFALSE\t200\todysseus_session\tvalid\t0\n')
            self.assertEqual(m.session_cookie(cookie, now=100), 'odysseus_session=valid')
            with self.assertRaises(ValueError):
                m.session_cookie(cookie, now=300)
            cookie.write_text('127.0.0.1\tFALSE\t/\tFALSE\t0\todysseus_session\tx;evil\n')
            with self.assertRaises(ValueError):
                m.session_cookie(cookie, now=100)

    def test_endpoint_registration_preserves_unrelated_endpoints(self):
        xenium = {'port': 7000, 'cookie_file': 'unused'}
        inventory = {'data': [{'id': 'tiny'}]}
        unrelated = {'id': 'other', 'name': 'Local Ollama', 'base_url': 'http://127.0.0.1:11434/v1'}
        with patch.object(m, 'session_cookie', return_value='hidden'), patch.object(m, '_request') as request:
            request.side_effect = [inventory, [unrelated], {'id': 'created'}]
            self.assertEqual(m.register_endpoint('tiny', xenium=xenium), {'id': 'created'})
            self.assertEqual(request.call_args.args[2], 'POST')
            self.assertEqual(request.call_args.args[3]['shared'], 'false')
            self.assertNotIn('default', request.call_args.args[3])
        matching = {'id': 'owned', 'name': 'zVram · old', 'base_url': 'http://127.0.0.1:8097/v1'}
        with patch.object(m, 'session_cookie', return_value='hidden'), patch.object(m, '_request') as request:
            request.side_effect = [inventory, [unrelated, matching], {'id': 'owned'}]
            m.register_endpoint('tiny', xenium=xenium)
            self.assertEqual(request.call_args.args[2], 'PATCH')
            self.assertEqual(request.call_args.args[3]['pinned_models'], ['tiny'])
        matching['name'] = 'Someone else'
        with patch.object(m, 'session_cookie', return_value='hidden'), patch.object(m, '_request') as request:
            request.side_effect = [inventory, [matching]]
            with self.assertRaises(ValueError):
                m.register_endpoint('tiny', xenium=xenium)
            self.assertEqual(request.call_count, 2)
        with patch.object(m, '_request') as request:
            with self.assertRaises(ValueError):
                m.register_endpoint('tiny', port=7000, xenium=xenium)
            request.assert_not_called()


if __name__ == '__main__':
    unittest.main()
