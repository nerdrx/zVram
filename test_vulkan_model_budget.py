#!/usr/bin/env python3
"""CPU-only checks for token budget and decode evidence validation."""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile


MODULE = Path(__file__).with_name("check_vulkan_idle_model.py")
spec = importlib.util.spec_from_file_location("idle_model", MODULE)
idle_model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(idle_model)


def check_prompt_gate(pressure):
    # A real child waits for input with tracked backing, but never reports full
    # cold startup. Pressure must send input; the idle test must still wait.
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        received = root / "received"
        child = """
import os,sys,time
print('Vulkan0 model buffer size = 1 MiB', file=sys.stderr, flush=True)
print('snapshot state event=bootstrap-bind resident=1048576 cold-logical=0 cold-stored=0 freezes=0 restores=0 failures=0', file=sys.stderr, flush=True)
print('== Running in interactive mode. ==', file=sys.stderr, flush=True)
sys.stdin.readline()
open(os.environ['RECEIVED'], 'w').write('yes')
print('snapshot state event=restore resident=524288 cold-logical=524288 cold-stored=100 freezes=1 restores=1 failures=0', file=sys.stderr, flush=True)
print('eval time = 1.0 ms / 1 runs (1000.0 tokens per second)', file=sys.stderr, flush=True)
print('done', flush=True)
time.sleep(0.05)
"""
        capture = idle_model.capture_backing
        def fake_capture(pid, path):
            path.write_text('CPU fixture, not GPU evidence')
            return {"resident_vram_present": True}
        idle_model.capture_backing = fake_capture
        try:
            try:
                result = idle_model.run_interactive(
                    "fixture", [sys.executable, "-c", child],
                    {**os.environ, "RECEIVED": str(received)}, root, 1, True,
                    pressure_on_first_submit=pressure)
            except RuntimeError as error:
                assert not pressure and 'timed out waiting' in str(error), error
                assert not received.exists()
            else:
                assert pressure and received.exists()
                assert result['pre_prompt_state'][:2] == (1048576, 0)
                assert result['pre_prompt_cold_state'] is None
                assert result['stdout'] == b'done\n'
                assert 'pressure' in result['backing'] and 'cold' not in result['backing']
                assert idle_model.has_decode_tokens(result['performance'])
        finally:
            idle_model.capture_backing = capture


def check_launch_forwarding():
    class Captured(Exception):
        pass
    captured = []
    calls = []
    original_run, original_argv = idle_model.run_interactive, sys.argv
    def fake_run(label, command, *args, **kwargs):
        calls.append(label)
        if label == "native":
            assert "--vulkan-lazy-backing" not in command
            assert "--vulkan-gdeflate-workers" not in command
            assert "--vulkan-headroom-mib" not in command
            return {}
        captured.extend(command)
        raise Captured
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        model = root / "fixture.gguf"
        model.write_bytes(b"CPU fixture only")
        sys.argv = [str(MODULE), "--binary", sys.executable, "--model", str(model),
                    "--output-dir", str(root / "output"), "--pressure-on-first-submit",
                    "--range-mib", "128", "--resident-mib", "20480", "--lazy-backing",
                    "--codec", "gdeflate", "--gdeflate-workers", "4", "--headroom-mib", "2048"]
        idle_model.run_interactive = fake_run
        try:
            try:
                idle_model.main()
            except Captured:
                pass
            else:
                raise AssertionError("automatic launch was not reached")
        finally:
            idle_model.run_interactive, sys.argv = original_run, original_argv
    assert calls == ["native", "automatic"], calls
    options = captured[:captured.index("--")]
    assert "--vulkan-lazy-backing" in options, captured
    assert options[options.index("--vulkan-gdeflate-workers") + 1] == "4", captured
    assert options[options.index("--vulkan-headroom-mib") + 1] == "2048", captured
    assert "--no-warmup" in captured, captured


def main():
    check_launch_forwarding()
    assert idle_model.has_decode_tokens({"decode_runs": 1, "tokens_per_second": 0.1})
    for metrics in ({"decode_runs": 0, "tokens_per_second": 1},
                    {"decode_runs": 1, "tokens_per_second": 0},
                    {"decode_runs": None, "tokens_per_second": None}, None):
        assert not idle_model.has_decode_tokens(metrics), metrics

    for pressure_mode in (False, True):
        check_prompt_gate(pressure_mode)

    # A late bind is reported even if the next admission successfully reduces
    # backing. An over-budget completed restore must still fail the budget gate.
    def state(event, size):
        return f'snapshot state event={event} resident={size} cold-logical=0 cold-stored=0 freezes=0 restores=1 failures=0\n'
    limit = 192 * idle_model.MiB
    log = state('bootstrap-bind', 256 * idle_model.MiB)
    log += 'resident admission selected-chunks=1\n'
    log += state('bootstrap-bind', limit + idle_model.MiB) + state('restore', limit)
    restores, observed = idle_model.pressure_resident_states(log)
    assert max(restores) <= limit < max(observed)
    restores, _ = idle_model.pressure_resident_states(log + state('restore', limit + 1))
    assert max(restores) > limit

    for tokens in ("31", "129"):
        result = subprocess.run(
            [sys.executable, str(MODULE), "--binary", "/missing", "--model", "/missing",
             "--tokens", tokens], capture_output=True, text=True)
        assert result.returncode == 2, result
        assert "tokens must be 32..128" in result.stderr, result.stderr
        assert "existing executable" not in result.stderr, result.stderr

    pressure = [sys.executable, str(MODULE), "--binary", "/missing", "--model", "/missing",
                "--pressure-on-first-submit", "--range-mib", "128", "--resident-mib", "20480"]
    for extra, expected in ((["--resident-after-cold"], "cannot be combined"),
                            (["--app-arg=--warmup"], "requires --no-warmup")):
        result = subprocess.run(pressure + extra, capture_output=True, text=True)
        assert result.returncode == 2, result
        assert expected in result.stderr, result.stderr
    result = subprocess.run(pressure[:7], capture_output=True, text=True)
    assert result.returncode == 2, result
    assert "requires --range-mib and --resident-mib" in result.stderr, result.stderr
    result = subprocess.run(pressure + ["--byte-shuffle", "2", "--min-savings-percent", "100"],
                            capture_output=True, text=True)
    assert result.returncode == 2 and "cannot be verified" in result.stderr, result.stderr
    result = subprocess.run(pressure[:6] + ["--lazy-backing"], capture_output=True, text=True)
    assert result.returncode == 2 and "requires immediate" in result.stderr, result.stderr
    for extra in (("--headroom-mib", "2048"), ("--lazy-backing", "--headroom-mib", "0"),
                  ("--lazy-backing", "--headroom-mib", "17592186044416")):
        result = subprocess.run(pressure + list(extra), capture_output=True, text=True)
        assert result.returncode == 2 and "requires lazy backing" in result.stderr, result.stderr
    for stride in ("2", "4"):
        result = subprocess.run(pressure + ["--byte-shuffle", stride], capture_output=True, text=True)
        assert result.returncode == 2 and "existing executable" in result.stderr, result.stderr
    for extra, expected in ((["--gdeflate-workers", "4"], "requires --codec gdeflate"),
                            (["--codec", "gdeflate", "--gdeflate-workers", "0"], "invalid choice"),
                            (["--codec", "gdeflate", "--gdeflate-workers", "33"], "invalid choice"),
                            (["--lazy-backing", "--resident-after-cold"], "requires immediate"),
                            (["--gdeflate-gpu"], "requires --codec gdeflate"),
                            (["--codec", "gdeflate", "--byte-shuffle", "2"], "requires the zstd codec")):
        result = subprocess.run(pressure + extra, capture_output=True, text=True)
        assert result.returncode == 2 and expected in result.stderr, result.stderr


if __name__ == "__main__":
    main()
