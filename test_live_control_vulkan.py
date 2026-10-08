#!/usr/bin/env python3
"""Bounded hardware gate: live cap acknowledgment then 64 MiB range integrity."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from zvram_manager import Manager

ROOT = Path(__file__).resolve().parent


def main():
    with tempfile.TemporaryDirectory(prefix='zvram-live-vulkan-') as temporary:
        root = Path(temporary)
        os.environ['ZVRAM_CONTROL_DIR'] = str(root / 'control')
        manager = Manager(root / 'state')
        log = root / 'fixture.log'
        command = [str(ROOT / 'zvram'), '--validate', '--isolate-layers', '--live-control',
                   '--vulkan-virtual-mib', '128', '--vulkan-auto-idle-ms', '100',
                   '--vulkan-cold-mib', '128', '--vulkan-resident-mib', '64',
                   '--vulkan-headroom-mib', '64', '--', str(ROOT / 'build/zvram-vulkan-auto-check'),
                   '--range-submit', '--control-hold-ms', '8000']
        with log.open('w') as stream:
            child = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        events = []
        try:
            deadline = time.monotonic() + 7
            row = None
            while time.monotonic() < deadline:
                row = next((r for r in manager.list_profiles() if r.get('pid') == child.pid and r.get('live_capable')), None)
                if row and 'CONTROL_READY' in log.read_text():
                    break
                if child.poll() is not None:
                    raise RuntimeError(log.read_text())
                time.sleep(.05)
            if not row or not row.get('live_capable'):
                raise RuntimeError('No live endpoint: ' + log.read_text())
            for cap in (96, 64):
                request = manager.set_live_limit(row['name'], cap)[0]
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    current = next(r for r in manager.list_profiles() if r.get('pid') == child.pid)
                    device = next(d for d in current['control_devices'] if d['device'] == request['device'])
                    if device['ack'] == request['seq'] and device['result'] == 0 and device['current_limit_mib'] == cap:
                        events.append(dict(cap_mib=cap, acknowledged=True))
                        break
                    time.sleep(.05)
                else:
                    raise RuntimeError('Cap was not applied: ' + json.dumps(device))
            child.wait(timeout=15)
            text = log.read_text()
            if child.returncode or 'VUID-' in text or 'Validation Error' in text:
                raise RuntimeError(text)
            print(json.dumps(dict(ok=True, live_requests=events, fixture_exit=child.returncode)))
            print(text)
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=5)


if __name__ == '__main__':
    main()
