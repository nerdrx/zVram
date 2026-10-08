#!/usr/bin/env python3
"""CPU-only checks for token budget and decode evidence validation."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest import mock


MODULE = Path(__file__).with_name("check_vulkan_idle_model.py")
spec = importlib.util.spec_from_file_location("idle_model", MODULE)
idle_model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(idle_model)


def check_ollama_guard_and_child_core_limit():
    child = """
import resource,sys
print('Vulkan0 model buffer size = 1 MiB', file=sys.stderr, flush=True)
print('== Running in interactive mode. ==', file=sys.stderr, flush=True)
print('core-limit=' + str(resource.getrlimit(resource.RLIMIT_CORE)[0]), file=sys.stderr, flush=True)
sys.stdin.readline()
print('done', flush=True)
"""

    def fake_capture(pid, path):
        path.write_text("CPU fixture")
        return {"resident_vram_present": True}

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        with mock.patch.object(idle_model, "capture_backing", fake_capture), \
                mock.patch.object(idle_model, "urlopen", side_effect=AssertionError("default guard ran")):
            result = idle_model.run_interactive(
                "default", [sys.executable, "-c", child], os.environ.copy(), root, 2, False)
        assert "core-limit=0" in result["stderr"]
        assert json.loads((root / "default.resources.json").read_text())["reject_ollama_gpu"] is False

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        with mock.patch.object(idle_model, "capture_backing", fake_capture), \
                mock.patch.object(idle_model, "urlopen", side_effect=OSError("Ollama unavailable")) as check:
            result = idle_model.run_interactive(
                "offline", [sys.executable, "-c", child], os.environ.copy(), root, 2,
                False, reject_ollama_gpu=True)
        assert check.call_count == 1
        assert result["stdout"] == b"done\n"
        resources = json.loads((root / "offline.resources.json").read_text())
        assert resources["reject_ollama_gpu"] is True
        assert resources["ollama_gpu_detected"] == []

    class FakeResponse:
        def __enter__(self):
            return self

        def __exit__(self, *args):
            return False

        def read(self, size):
            assert size == idle_model.OllamaPsMaxBytes + 1
            return b'{"models":[{"name":"qwen3.5:9b-local","size_vram":1234}]}'

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        with mock.patch.object(idle_model, "urlopen", return_value=FakeResponse()):
            try:
                idle_model.run_interactive(
                    "busy", [sys.executable, "-c", child], os.environ.copy(), root, 2,
                    False, reject_ollama_gpu=True)
            except RuntimeError as error:
                assert "qwen3.5:9b-local" in str(error) and "1234 bytes VRAM" in str(error), error
            else:
                raise AssertionError("active Ollama GPU model was not rejected")
        resources = json.loads((root / "busy.resources.json").read_text())
        assert resources["ollama_gpu_detected"] == [
            {"name": "qwen3.5:9b-local", "size_vram": 1234}]


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


def check_incremental_stderr_chunk_boundaries():
    child = """
import sys
def emit(text):
    sys.stderr.write(text)
    sys.stderr.flush()
emit('Vulkan0 model buffer size = 1,024 MiB\\n')
emit('snapshot state event=bootstrap-bind resident=1048576 cold-logical=0 cold-stored=0 freezes=0 restores=0 failures=0\\n')
emit('== Running in interactive mode. ==\\n')
sys.stdin.readline()
emit('snapshot state event=restore resident=524288 cold-logical=524288 cold-stored=100 freezes=1 restores=1 failures=0\\n')
emit('eval time = 1.0 ms / 1 runs (1000.0 tokens per second)\\n')
print('done', flush=True)
"""
    capture = idle_model.capture_backing
    original_read = idle_model.os.read
    def fake_capture(pid, path):
        path.write_text("CPU fixture")
        return {"resident_vram_present": True}
    def small_read(fd, size):
        return original_read(fd, min(size, 5))
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        idle_model.capture_backing = fake_capture
        try:
            with mock.patch.object(idle_model.os, "read", side_effect=small_read):
                result = idle_model.run_interactive(
                    "split", [sys.executable, "-c", child], os.environ.copy(), root, 3,
                    True, pressure_on_first_submit=True)
        finally:
            idle_model.capture_backing = capture
    assert result["model_buffers_mib"] == [1024.0], result["model_buffers_mib"]
    assert result["pre_prompt_state"][:2] == (1048576, 0), result["pre_prompt_state"]
    assert result["pre_prompt_cold_state"] is None
    assert "pressure" in result["backing"], result["backing"]
    assert result["state_events"][-1][0] == "restore", result["state_events"]
    assert idle_model.has_decode_tokens(result["performance"]), result["performance"]
    assert result["stdout"] == b"done\n", result["stdout"]


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
            assert "--vulkan-bp16-workers" not in command
            assert "--vulkan-headroom-mib" not in command
            assert "--vulkan-async-compression" not in command
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
                    "--async-compression",
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
    assert "--vulkan-async-compression" in options, captured
    assert "--vulkan-active-eviction" in options, captured
    assert options[options.index("--vulkan-range-mib") + 1] == "128", captured
    assert options[options.index("--vulkan-gdeflate-workers") + 1] == "4", captured
    assert options[options.index("--vulkan-headroom-mib") + 1] == "2048", captured
    assert "--no-warmup" in captured, captured


def check_bp16_launch_forwarding():
    class Captured(Exception):
        pass
    captured = []
    original_run, original_argv = idle_model.run_interactive, sys.argv
    def fake_run(label, command, *args, **kwargs):
        if label == "native":
            return {}
        captured.extend(command)
        raise Captured
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        model = root / "fixture.gguf"
        model.write_bytes(b"CPU fixture only")
        sys.argv = [str(MODULE), "--binary", sys.executable, "--model", str(model),
                    "--output-dir", str(root / "output"), "--codec", "bp16",
                    "--bp16-workers", "16"]
        idle_model.run_interactive = fake_run
        try:
            try:
                idle_model.main()
            except Captured:
                pass
            else:
                raise AssertionError("BP16 automatic launch was not reached")
        finally:
            idle_model.run_interactive, sys.argv = original_run, original_argv
    options = captured[:captured.index("--")]
    assert options[options.index("--vulkan-codec") + 1] == "bp16", captured
    assert options[options.index("--vulkan-bp16-workers") + 1] == "16", captured


def check_gpu_restore_evidence():
    for codec in ("BP16", "GDeflate"):
        enabled = f"GPU {codec} restore enabled"
        state = ("snapshot state event=restore resident=0 cold-logical=0 cold-stored=0 "
                 "freezes=1 restores=1 failures=0 gpu-decode-calls=3 gpu-decode-bytes=4096 "
                 "gpu-decode-ns=120 gpu-decode-fallbacks=0")
        profile, used, no_fallback = idle_model.gpu_restore_evidence(enabled + "\n" + state, codec)
        assert profile == (3, 4096, 120, 0) and used and no_fallback, (codec, profile, used, no_fallback)
    enabled = "GPU BP16 restore enabled\n"
    state = "snapshot state event=restore gpu-decode-calls=0 gpu-decode-bytes=0 gpu-decode-ns=0 gpu-decode-fallbacks=0"
    profile, used, no_fallback = idle_model.gpu_restore_evidence(enabled + state, "BP16")
    assert profile == (0, 0, 0, 0) and not used and no_fallback
    fallback = "snapshot state event=restore gpu-decode-calls=3 gpu-decode-bytes=4096 gpu-decode-ns=120 gpu-decode-fallbacks=1"
    profile, used, no_fallback = idle_model.gpu_restore_evidence(enabled + fallback, "BP16")
    assert profile == (3, 4096, 120, 1) and used and not no_fallback
    malformed = "snapshot state event=restore gpu-decode-calls=3 gpu-decode-bytes=4096 gpu-decode-ns=bad gpu-decode-fallbacks=0"
    profile, used, no_fallback = idle_model.gpu_restore_evidence(enabled + malformed, "BP16")
    assert profile is None and not used and not no_fallback
    profile, used, no_fallback = idle_model.gpu_restore_evidence(fallback, "BP16")
    assert profile is None and not used and not no_fallback
    summary = "GPU BP16 restore calls=2 bytes=2048 host-ns=70 fallbacks=0\n" + fallback
    profile, used, no_fallback = idle_model.gpu_restore_evidence(enabled + summary, "BP16")
    assert profile == (2, 2048, 70, 0) and used and no_fallback


def main():
    check_ollama_guard_and_child_core_limit()
    check_launch_forwarding()
    check_bp16_launch_forwarding()
    check_gpu_restore_evidence()
    assert idle_model.has_decode_tokens({"decode_runs": 1, "tokens_per_second": 0.1})
    for metrics in ({"decode_runs": 0, "tokens_per_second": 1},
                    {"decode_runs": 1, "tokens_per_second": 0},
                    {"decode_runs": None, "tokens_per_second": None}, None):
        assert not idle_model.has_decode_tokens(metrics), metrics

    for pressure_mode in (False, True):
        check_prompt_gate(pressure_mode)
    check_incremental_stderr_chunk_boundaries()

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
    result = subprocess.run([sys.executable, str(MODULE), "--binary", "/missing", "--model", "/missing",
                             "--async-compression"], capture_output=True, text=True)
    assert result.returncode == 2 and "requires --range-mib and active eviction" in result.stderr, result.stderr
    for extra, expected in ((["--gdeflate-workers", "4"], "requires --codec gdeflate"),
                            (["--codec", "gdeflate", "--gdeflate-workers", "0"], "invalid choice"),
                            (["--codec", "gdeflate", "--gdeflate-workers", "33"], "invalid choice"),
                            (["--lazy-backing", "--resident-after-cold"], "requires immediate"),
                            (["--gdeflate-gpu"], "requires --codec gdeflate"),
                            (["--bp16-workers", "4"], "requires --codec bp16"),
                            (["--codec", "bp16", "--bp16-workers", "0"], "invalid choice"),
                            (["--codec", "bp16", "--bp16-workers", "33"], "invalid choice"),
                            (["--codec", "gdeflate", "--byte-shuffle", "2"], "requires the zstd codec")):
        result = subprocess.run(pressure + extra, capture_output=True, text=True)
        assert result.returncode == 2 and expected in result.stderr, result.stderr


if __name__ == "__main__":
    main()
