#!/usr/bin/env python3
"""Compare an unchanged llama-completion run with zVram Vulkan idle snapshots."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import time

from check_idle_model import performance
from check_model import clean_environment, common_app_args


MiB = 1024 * 1024
Prompt = b"Tell me a very short story about a fox.\n"
ColdPattern = re.compile(r"snapshot cold bytes=(\d+) stored=(\d+)")
StatePattern = re.compile(
    r"snapshot state (?:event=\S+ )?resident=(\d+) cold-logical=(\d+) cold-stored=(\d+) "
    r"freezes=(\d+) restores=(\d+) failures=(\d+)")
StateEventPattern = re.compile(
    r"snapshot state event=(\S+) resident=(\d+) cold-logical=(\d+) cold-stored=(\d+) "
    r"freezes=(\d+) restores=(\d+) failures=(\d+)")
VulkanBufferPattern = re.compile(
    r"\bVulkan\d+\s+model buffer size\s*=\s*([\d,]+(?:\.\d+)?)\s*MiB\b", re.I)
OffloadPattern = re.compile(r"offloaded\s+(\d+)\s*/\s*(\d+)\s+layers?\s+to GPU", re.I)


def has_decode_tokens(metrics):
    return bool(metrics and metrics.get("decode_runs", 0) and
                metrics.get("tokens_per_second", 0) and
                metrics["decode_runs"] > 0 and metrics["tokens_per_second"] > 0)


def available_memory_mib():
    match = re.search(r"^MemAvailable:\s+(\d+)\s+kB$",
                      Path("/proc/meminfo").read_text(), re.M)
    if not match:
        raise RuntimeError("/proc/meminfo has no MemAvailable value")
    return int(match[1]) // 1024


def pressure_prompt_ready(ready, model_mib):
    """Pressure mode prompts at readiness; first inference submit performs admission."""
    return bool(ready and model_mib)


def cold_prompt_ready(ready, model_mib, cold_seen, state):
    if not ready or not model_mib or not cold_seen or not state:
        return False
    tolerance = len(model_mib) * 0.01 * MiB
    return (state[0] == 0 and state[1] + tolerance >= sum(model_mib) * MiB and
            state[3] > 0 and state[5] == 0)


def pressure_resident_states(text):
    """Keep allocation peaks visible separately from completed restore states.

    Allocation/bind can temporarily exceed admission's budget before the next
    submit evicts backing. Every completed restore must respect that budget.
    """
    first = text.find("resident admission selected-chunks=")
    states = [(match[1], int(match[2])) for match in StateEventPattern.finditer(text)
              if first >= 0 and match.start() >= first]
    return [size for event, size in states if event == "restore"], [size for _, size in states]


def validate_pressure_options(args, parser):
    if not args.pressure_on_first_submit:
        return
    if args.range_mib is None or args.resident_mib is None:
        parser.error("--pressure-on-first-submit requires --range-mib and --resident-mib")
    if args.resident_after_cold:
        parser.error("--pressure-on-first-submit cannot be combined with --resident-after-cold")
    if "--warmup" in args.app_arg:
        parser.error("--pressure-on-first-submit requires --no-warmup; remove app --warmup")


def capture_backing(pid, path):
    clients = {}
    raw = []
    for fd in sorted(Path(f"/proc/{pid}/fdinfo").iterdir()):
        try:
            text = fd.read_text()
        except (FileNotFoundError, PermissionError):
            continue
        if "drm-driver:\tamdgpu" not in text:
            continue
        raw.append(f"fdinfo {fd.name}\n{text}")
        client = re.search(r"drm-client-id:\s*(\d+)", text)
        pdev = re.search(r"drm-pdev:\s*(\S+)", text)
        if not (client and pdev):
            continue
        values = {key: int(value) * 1024 for key, value in re.findall(
            r"drm-(resident-vram|memory-vram|resident-gtt|memory-gtt):\s*(\d+) KiB", text)}
        clients[(pdev[1], client[1])] = values
    path.write_text("\n".join(raw))
    totals = {key: sum(client.get(key, 0) for client in clients.values()) for key in
              ("resident-vram", "memory-vram", "resident-gtt", "memory-gtt")}
    totals["unique_drm_clients"] = len(clients)
    totals["resident_vram_present"] = bool(clients) and all("resident-vram" in c for c in clients.values())
    return totals


def run_interactive(label, command, env, output_dir, timeout, automatic,
                    min_available_mib=None, pressure_on_first_submit=False):
    out_path = output_dir / f"{label}.stdout.txt"
    err_path = output_dir / f"{label}.stderr.txt"
    stdout = bytearray()
    stderr = bytearray()
    backing = {}
    cold = []
    cold_state = None
    model_mib = []
    prompt_time = first_output_time = None
    prompt_sent = False
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env, start_new_session=True,
                            bufsize=0)
    deadline = time.monotonic() + timeout
    ready = False
    last_cold_time = None
    pre_prompt_cold_state = None
    pre_prompt_state = None
    minimum_available_mib = None
    try:
        while time.monotonic() < deadline:
            if min_available_mib is not None:
                available = available_memory_mib()
                minimum_available_mib = (available if minimum_available_mib is None
                                          else min(minimum_available_mib, available))
                if available < min_available_mib:
                    raise RuntimeError(f"{label}: MemAvailable {available} MiB below guard "
                                       f"floor {min_available_mib} MiB")
            readable, _, _ = select.select([proc.stdout, proc.stderr], [], [], 0.05)
            for stream in readable:
                chunk = os.read(stream.fileno(), 65536)
                if not chunk:
                    continue
                if stream is proc.stdout:
                    stdout.extend(chunk)
                    if prompt_sent and first_output_time is None:
                        first_output_time = time.monotonic()
                else:
                    stderr.extend(chunk)
                    text = stderr.decode("utf-8", errors="replace")
                    model_mib = [float(x.replace(",", "")) for x in VulkanBufferPattern.findall(text)]
                    new_cold = [(int(a), int(b)) for a, b in ColdPattern.findall(text)]
                    if len(new_cold) > len(cold):
                        cold = new_cold
                        last_cold_time = time.monotonic()
                    states = [tuple(map(int, x)) for x in StatePattern.findall(text)]
                    if states:
                        cold_state = states[-1]
                        if not prompt_sent:
                            pre_prompt_state = cold_state
                    if automatic and "automatic Vulkan snapshots disabled:" in text:
                        raise RuntimeError(f"{label}: automatic Vulkan snapshots were disabled")
                    if automatic and any(word in text.lower() for word in
                                         ("snapshot failed", "snapshot error", "snapshot failure")):
                        raise RuntimeError(f"{label}: Vulkan snapshot error reported")
                    if model_mib and not cold and "hot" not in backing:
                        backing["hot"] = capture_backing(proc.pid, output_dir / f"{label}-hot.fdinfo.txt")
                    if "== Running in interactive mode. ==" in text and not ready:
                        ready = True
                        if model_mib and not cold:
                            backing["hot"] = capture_backing(proc.pid, output_dir / f"{label}-hot.fdinfo.txt")
            if proc.poll() is not None:
                raise RuntimeError(f"{label} exited before input (status {proc.returncode})")
            if not automatic and ready and model_mib:
                break
            if (automatic and pressure_on_first_submit and
                    pressure_prompt_ready(ready, model_mib)):
                if "hot" not in backing:
                    raise RuntimeError(f"{label}: no hot DRM snapshot was captured before pressure")
                break
            if automatic and cold_prompt_ready(ready, model_mib, cold, cold_state):
                if last_cold_time is not None and time.monotonic() - last_cold_time >= 0.2:
                    if "hot" not in backing:
                        raise RuntimeError(f"{label}: no hot DRM snapshot was captured before eviction")
                    backing["cold"] = capture_backing(proc.pid, output_dir / f"{label}-cold.fdinfo.txt")
                    pre_prompt_cold_state = cold_state
                    break
        else:
            raise RuntimeError(f"{label} timed out waiting for ready model/snapshot")
        prompt_time = time.monotonic()
        pressure_stderr_start = len(stderr)
        proc.stdin.write(Prompt)
        proc.stdin.close()
        prompt_sent = True
        while proc.poll() is None and time.monotonic() < deadline:
            if min_available_mib is not None:
                available = available_memory_mib()
                minimum_available_mib = min(minimum_available_mib, available)
                if available < min_available_mib:
                    raise RuntimeError(f"{label}: MemAvailable {available} MiB below guard "
                                       f"floor {min_available_mib} MiB")
            readable, _, _ = select.select([proc.stdout, proc.stderr], [], [], 0.05)
            for stream in readable:
                chunk = os.read(stream.fileno(), 65536)
                if not chunk:
                    continue
                if stream is proc.stdout:
                    stdout.extend(chunk)
                    if first_output_time is None:
                        first_output_time = time.monotonic()
                else:
                    stderr.extend(chunk)
                    if (automatic and pressure_on_first_submit and prompt_sent and
                            b"snapshot state event=restore" in stderr[pressure_stderr_start:] and
                            "pressure" not in backing):
                        backing["pressure"] = capture_backing(
                            proc.pid, output_dir / f"{label}-pressure.fdinfo.txt")
            if proc.poll() is not None:
                break
        if proc.poll() is None:
            raise RuntimeError(f"{label} timed out generating response")
        if proc.returncode != 0:
            raise RuntimeError(f"{label} exited with status {proc.returncode}")
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        # Pipes can still contain the child's final buffered output after poll sees exit.
        for stream in (proc.stdout, proc.stderr):
            while select.select([stream], [], [], 0)[0]:
                chunk = os.read(stream.fileno(), 65536)
                if not chunk:
                    break
                (stdout if stream is proc.stdout else stderr).extend(chunk)
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            if stream and not stream.closed:
                stream.close()
        out_path.write_bytes(stdout)
        err_path.write_bytes(stderr)
    text = stderr.decode("utf-8", errors="replace")
    states = [tuple(map(int, x)) for x in StatePattern.findall(text)]
    if states:
        cold_state = states[-1]
    state_events = [(event, *map(int, values)) for event, *values in
                    StateEventPattern.findall(text)]
    offloads = [(int(a), int(b)) for a, b in OffloadPattern.findall(text)]
    return {"stdout": bytes(stdout), "stderr": text, "model_buffers_mib": model_mib,
            "cold": cold, "cold_state": cold_state, "pre_prompt_cold_state": pre_prompt_cold_state,
            "pre_prompt_state": pre_prompt_state, "state_events": state_events,
            "backing": backing,
            "offload": list(offloads[-1]) if offloads else None,
            "performance": performance(text),
            "first_stdout_after_input_ms": ((first_output_time - prompt_time) * 1000
                                            if first_output_time and prompt_time else None),
            "minimum_available_mib": minimum_available_mib,
            "returncode": proc.returncode}


def main():
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--build-dir", type=Path, help="select a zVram backend CMake build directory")
    parser.add_argument("--codec", choices=("zstd", "gdeflate"), help="select the snapshot codec explicitly")
    parser.add_argument("--gdeflate-workers", type=int, choices=range(1,33), help="bounded CPU GDeflate encoding workers; requires --codec gdeflate")
    parser.add_argument("--gdeflate-gpu", action="store_true", help="require observable direct GPU GDeflate restoration")
    parser.add_argument("--tokens", type=int, default=128)
    parser.add_argument("--idle-ms", type=int, default=1000)
    parser.add_argument("--cold-mib", type=int, default=512)
    parser.add_argument("--min-savings-percent", type=int,
                        help="minimum snapshot savings 0..100; 0 keeps any saving, 100 uses raw snapshots")
    parser.add_argument("--byte-shuffle", type=int, choices=(2, 4),
                        help="test opt-in lossless byte-plane filtering before Zstd")
    parser.add_argument("--selective-restore", action="store_true",
                        help="test opt-in per-submission Vulkan restoration")
    parser.add_argument("--active-eviction", action="store_true",
                        help="test tracked eviction while unrelated submissions remain active; enables selective restore")
    parser.add_argument("--range-mib", type=int,
                        help="test sparse range residency at this MiB chunk size; enables active eviction")
    parser.add_argument("--validate", action="store_true", help="enable Vulkan core/synchronization validation on both runs")
    parser.add_argument("--resident-mib", type=int, help="test pressure admission limit; requires --range-mib")
    parser.add_argument("--min-available-mib", type=int,
                        help="abort when /proc/meminfo MemAvailable falls below this positive MiB floor")
    parser.add_argument("--eviction-policy", choices=("lru", "mru"),
                        help="choose resident-range eviction order; requires --resident-mib")
    parser.add_argument("--strict-robustness", action="store_true", help="enable supported robustness2 for narrow descriptor ranges")
    parser.add_argument("--clean-cache", action="store_true", help="retain and reuse snapshots after proven read-only GPU work")
    parser.add_argument("--lazy-backing", action="store_true", help="test pristine backing on demand; requires immediate first-submit pressure mode")
    parser.add_argument("--headroom-mib", type=int, help="reserve native VRAM budget headroom; requires lazy backing")
    parser.add_argument("--resident-after-cold", action="store_true", help="allow bootstrap then arm pressure admission after all eligible backing is cold")
    parser.add_argument("--pressure-on-first-submit", action="store_true",
                        help="keep the resident cap active and prompt after full backing is tracked, before requiring full cold")
    parser.add_argument("--max-nodes-per-submit", type=int, help="set llama.cpp graph batching identically for both runs")
    parser.add_argument("--serialize-submissions", action="store_true", help="use llama.cpp synchronous submissions identically for both runs")
    parser.add_argument("--timeout", type=int, default=60, help="seconds allowed per run, including model loading")
    parser.add_argument("--output-dir", type=Path, default=Path("build/vulkan-idle-model-check"))
    parser.add_argument("--app-arg", action="append", default=[],
                        help="extra llama-completion option; repeat as --app-arg=VALUE")
    args = parser.parse_args()
    if args.gdeflate_workers is not None and args.codec != "gdeflate":
        parser.error("--gdeflate-workers requires --codec gdeflate")
    if args.gdeflate_gpu and args.codec != "gdeflate":
        parser.error("--gdeflate-gpu requires --codec gdeflate")
    if args.byte_shuffle and args.codec == "gdeflate":
        parser.error("--byte-shuffle requires the zstd codec")
    if not 32 <= args.tokens <= 128:
        parser.error("tokens must be 32..128")
    args.active_eviction = args.active_eviction or args.range_mib is not None
    args.selective_restore = args.selective_restore or args.active_eviction
    if args.range_mib is not None and not 0 < args.range_mib <= ((1 << 64) - 1) // (1024 * 1024):
        parser.error("--range-mib must be positive and fit 64-bit bytes")
    if args.resident_mib is not None and (args.range_mib is None or not 0 < args.resident_mib <= ((1 << 64) - 1) // (1024 * 1024)):
        parser.error("--resident-mib requires --range-mib and positive size fitting 64-bit bytes")
    if args.min_available_mib is not None and not 0 < args.min_available_mib <= (1 << 64) - 1:
        parser.error("--min-available-mib must be positive and fit uint64")
    if args.min_savings_percent is not None and not 0 <= args.min_savings_percent <= 100:
        parser.error("--min-savings-percent must be 0..100")
    if args.byte_shuffle is not None and args.min_savings_percent == 100:
        parser.error("--byte-shuffle cannot be verified when 100% savings forces raw snapshots")
    if args.eviction_policy is not None and args.resident_mib is None:
        parser.error("--eviction-policy requires --resident-mib")
    if args.strict_robustness and args.range_mib is None:
        parser.error("--strict-robustness requires --range-mib")
    if args.clean_cache and args.range_mib is None:
        parser.error("--clean-cache requires --range-mib")
    if args.resident_after_cold and args.resident_mib is None:
        parser.error("--resident-after-cold requires --resident-mib")
    if args.lazy_backing and (not args.pressure_on_first_submit or args.resident_after_cold):
        parser.error("--lazy-backing requires immediate --pressure-on-first-submit mode")
    if args.headroom_mib is not None and (not args.lazy_backing or not 0 < args.headroom_mib <= ((1 << 64)-1)//MiB):
        parser.error("--headroom-mib requires lazy backing and positive 64-bit size")
    validate_pressure_options(args, parser)
    if args.max_nodes_per_submit is not None and not 1 <= args.max_nodes_per_submit <= (1 << 32) - 1:
        parser.error("--max-nodes-per-submit must fit a positive uint32")
    binary = args.binary.expanduser().resolve()
    model = args.model.expanduser().resolve()
    launcher = root / "zvram"
    output = args.output_dir.expanduser()
    if not output.is_absolute():
        output = root / output
    output = output.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"--binary must be an existing executable: {binary}")
    if not model.is_file() or model.stat().st_size <= 0:
        parser.error(f"--model must be an existing nonempty file: {model}")
    if not launcher.is_file():
        parser.error(f"zVram launcher missing: {launcher}")
    if not 1 <= args.idle_ms <= (1 << 32) - 1 or not 1 <= args.cold_mib <= 40960 or not 1 <= args.timeout <= (1 << 32) - 1:
        parser.error("idle-ms and timeout a positive uint32, cold-mib 1..40960")
    output.mkdir(parents=True, exist_ok=True)
    app = common_app_args(binary, model, args.tokens)[:-2]
    app += ["--conversation", "--interactive-first", "--single-turn", *args.app_arg]
    if args.pressure_on_first_submit and "--no-warmup" not in app:
        app.append("--no-warmup")
    env = clean_environment()
    env.pop("ROCPROFILER_REGISTER_LIBRARY", None)
    env.pop("ROCPROFILER_REGISTER_SECURE", None)
    env["GGML_CUDA_DISABLE_GRAPHS"] = "1"
    env["GGML_VK_VISIBLE_DEVICES"] = "0"
    env["GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM"] = "1"
    if args.max_nodes_per_submit is not None:
        env["GGML_VK_MAX_NODES_PER_SUBMIT"] = str(args.max_nodes_per_submit)
    if args.serialize_submissions:
        env["GGML_VK_SERIALIZE_SUBMISSIONS"] = "1"
    if args.validate:
        env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
        env["VK_VALIDATION_VALIDATE_SYNC"] = "1"
        env["VK_LOADER_LAYERS_DISABLE"] = "~implicit~"
    native = run_interactive("native", app, env, output, args.timeout, False,
                             args.min_available_mib)
    command = [str(launcher), "--vulkan-virtual-gib", "96", "--vulkan-auto-idle-ms",
               str(args.idle_ms), "--vulkan-cold-mib", str(args.cold_mib)]
    if args.build_dir:
        command += ["--build-dir", str(args.build_dir.expanduser().resolve())]
    if args.codec:
        command += ["--vulkan-codec", args.codec]
    if args.gdeflate_workers is not None:
        command += ["--vulkan-gdeflate-workers", str(args.gdeflate_workers)]
    if args.gdeflate_gpu:
        command.append("--vulkan-gdeflate-gpu")
    if args.min_savings_percent is not None:
        command += ["--vulkan-min-savings-percent", str(args.min_savings_percent)]
    if args.byte_shuffle is not None:
        command += ["--vulkan-byte-shuffle", str(args.byte_shuffle)]
    if args.selective_restore:
        command.append("--vulkan-selective-restore")
    if args.active_eviction:
        command.append("--vulkan-active-eviction")
    if args.range_mib is not None:
        command += ["--vulkan-range-mib", str(args.range_mib)]
    if args.resident_mib is not None:
        command += ["--vulkan-resident-mib", str(args.resident_mib)]
    if args.eviction_policy is not None:
        command += ["--vulkan-eviction-policy", args.eviction_policy]
    if args.lazy_backing:
        command.append("--vulkan-lazy-backing")
    if args.headroom_mib is not None:
        command += ["--vulkan-headroom-mib", str(args.headroom_mib)]
    if args.resident_after_cold:
        command.append("--vulkan-resident-after-cold")
    if args.strict_robustness:
        command.append("--vulkan-strict-robustness")
    if args.clean_cache:
        command.append("--vulkan-clean-cache")
    if args.validate:
        command += ["--validate", "--isolate-layers"]
    command += ["--", *app]
    auto = run_interactive("automatic", command, env, output, args.timeout, True,
                           args.min_available_mib, args.pressure_on_first_submit)
    auto_text = auto["stderr"]
    auto_cold = auto["cold"]
    cleanup_fields = re.findall(r"\[zvram\].*(?:summary|automatic).*", auto_text, re.I)
    cold_state = auto["pre_prompt_cold_state"]
    pre_prompt_state = auto["pre_prompt_state"]
    model_bytes = sum(auto["model_buffers_mib"]) * MiB
    coverage_tolerance = len(auto["model_buffers_mib"]) * 0.01 * MiB
    checks = {
        "same_nonempty_stdout": bool(native["stdout"].strip()) and native["stdout"] == auto["stdout"],
        "both_runs_decode_tokens": has_decode_tokens(native["performance"]) and has_decode_tokens(auto["performance"]),
        "full_gpu_offload_matches": bool(native["offload"] and native["offload"][0] == native["offload"][1] > 0 and auto["offload"] == native["offload"]),
        "automatic_enabled": "automatic Vulkan snapshots enabled" in auto_text and "automatic Vulkan snapshots disabled:" not in auto_text,
        "model_buffer_cold_before_prompt": bool(auto["model_buffers_mib"] and cold_state and
            cold_state[0] == 0 and cold_state[1] + len(auto["model_buffers_mib"]) * 0.01 * MiB >= sum(auto["model_buffers_mib"]) * MiB and
            cold_state[3] > 0 and cold_state[5] == 0),
        "hot_and_cold_fdinfo_captured": "hot" in auto["backing"] and "cold" in auto["backing"],
        "resident_vram_captured": all(auto["backing"].get(where, {}).get("resident_vram_present")
                                      for where in ("hot", "cold")),
        "no_snapshot_errors": not any(s in auto_text.lower() for s in ("snapshot failed", "snapshot error", "snapshot failure")),
        "nonempty_cold_data": bool(cold_state and cold_state[1] > 0),
        "successful_restore": bool(cold_state and auto["cold_state"] and
            (args.selective_restore or auto["cold_state"][1] == 0) and auto["cold_state"][4] > cold_state[4] and
            auto["cold_state"][5] == 0),
    }
    if args.min_savings_percent is not None:
        checks["minimum_savings_percent_configured"] = (
            f"Vulkan snapshot minimum savings percent={args.min_savings_percent}" in auto_text)
    if args.codec:
        checks["snapshot_codec_selected"] = args.codec == "zstd" or "Vulkan snapshot codec=gdeflate" in auto_text
    if args.gdeflate_gpu:
        gpu_profiles = [tuple(map(int, values)) for values in re.findall(
            r"gpu-decode-calls=(\d+) gpu-decode-bytes=(\d+) gpu-decode-ns=(\d+) gpu-decode-fallbacks=(\d+)", auto_text)]
        checks["gpu_decoder_used"] = bool(gpu_profiles and gpu_profiles[-1][0] > 0 and gpu_profiles[-1][1] > 0 and
            "GPU GDeflate restore enabled" in auto_text and "decode=GPU" in auto_text)
        checks["no_gpu_decoder_fallback"] = bool(gpu_profiles and gpu_profiles[-1][3] == 0)
    encodings = [tuple(map(int, values)) for values in re.findall(
        r"snapshot cold bytes=(\d+) stored=(\d+) compressed-chunks=(\d+) raw-chunks=(\d+)", auto_text)]
    shuffled_counts = [int(value) for value in re.findall(
        r"snapshot cold bytes=[^\n]* shuffled-chunks=(\d+)", auto_text)]
    if args.byte_shuffle is not None:
        checks["byte_shuffle_configured"] = (
            f"Vulkan snapshot byte shuffle stride={args.byte_shuffle}" in auto_text)
        checks["byte_shuffle_encoding_observed"] = any(count > 0 for count in shuffled_counts)
    if args.min_savings_percent == 100:
        checks["raw_only_snapshots"] = bool(encodings) and all(
            logical == stored and compressed == 0 and raw > 0
            for logical, stored, compressed, raw in encodings)
    selective_events = [tuple(map(int, values)) for values in re.findall(
        r"selective restore selected-pools=(\d+) restored-pools=(\d+) cold-pools-left=(\d+)", auto_text)]
    range_events = [tuple(map(int, values)) for values in re.findall(
        r"selective range restore selected-chunks=(\d+) restored-chunks=(\d+) cold-pools-left=(\d+)", auto_text)]
    if args.selective_restore:
        checks["selective_enabled"] = "selective Vulkan restore enabled" in auto_text
        checks["tracked_restore_observed"] = any(restored > 0 for _, restored, _ in selective_events + range_events)
    if args.active_eviction:
        checks["active_eviction_enabled"] = "active Vulkan eviction enabled" in auto_text
    if args.range_mib is not None:
        checks["range_residency_enabled"] = "Vulkan range residency enabled" in auto_text
    pressure_events = [tuple(map(int, values)) for values in re.findall(
        r"resident admission selected-chunks=(\d+) evicted-chunks=(\d+) resident-before-restore=(\d+) incoming-bytes=(\d+) limit-bytes=(\d+)", auto_text)]
    if args.validate:
        checks["no_validation_diagnostics"] = not any(
            "Validation Error" in text or "VUID-" in text
            for text in (native["stderr"], native["stdout"].decode(errors="replace"), auto_text, auto["stdout"].decode(errors="replace")))
    admission_text = auto_text.split("resident admission armed after complete cold transition", 1)[-1]
    resident_states = [int(value) for value in re.findall(r"snapshot state event=[^ ]+ resident=(\d+)", admission_text)]
    if args.pressure_on_first_submit:
        checks.pop("model_buffer_cold_before_prompt")
        checks.pop("hot_and_cold_fdinfo_captured")
        checks["model_buffer_coverage_before_prompt"] = bool(
            pre_prompt_state and model_bytes and
            pre_prompt_state[0] + pre_prompt_state[1] + coverage_tolerance >= model_bytes and
            pre_prompt_state[0] + pre_prompt_state[1] > 0 and pre_prompt_state[5] == 0)
        checks["hot_and_pressure_fdinfo_captured"] = (
            "hot" in auto["backing"] and "pressure" in auto["backing"])
        checks["resident_vram_captured"] = all(
            auto["backing"].get(where, {}).get("resident_vram_present")
            for where in ("hot", "pressure"))
        checks["nonempty_cold_data"] = bool(
            (pre_prompt_state and pre_prompt_state[1] > 0) or
            any(state[2] > 0 for state in auto["state_events"]) or auto_cold)
        checks["successful_restore"] = bool(
            pre_prompt_state and auto["cold_state"] and
            auto["cold_state"][4] > pre_prompt_state[4] and auto["cold_state"][5] == 0)
        resident_states, all_pressure_states = pressure_resident_states(auto_text)
    if args.strict_robustness:
        checks["bounded_robustness_enabled"] = "bounded Vulkan robustness enabled" in auto_text
    cache_states = [tuple(map(int, values)) for values in re.findall(
        r"snapshot state [^\n]*cold-stored=(\d+)[^\n]*cache-stored=(\d+) clean-reuses=(\d+) cache-invalidations=(\d+)", auto_text)]
    if args.clean_cache:
        checks["clean_cache_enabled"] = "Vulkan clean snapshot cache enabled" in auto_text
        checks["clean_cache_reused"] = any(reuses > 0 for _, _, reuses, _ in cache_states)
        checks["clean_cache_invalidated"] = any(invalidations > 0 for _, _, _, invalidations in cache_states)
        checks["shared_cold_cache_budget"] = bool(cache_states) and all(cold + cached <= args.cold_mib * MiB for cold, cached, _, _ in cache_states)
    if args.resident_mib is not None:
        if args.resident_after_cold:
            checks["resident_admission_armed"] = "resident admission armed after complete cold transition" in auto_text
        checks["resident_admission_enabled"] = "Vulkan resident admission enabled" in auto_text
        checks["pressure_eviction_observed"] = any(evicted > 0 for _, evicted, _, _, _ in pressure_events)
        checks["pressure_admissions_within_limit"] = bool(pressure_events) and all(
            resident + incoming <= limit == args.resident_mib * MiB for _, _, resident, incoming, limit in pressure_events)
        checks["tracked_resident_peak_within_limit"] = bool(resident_states) and max(resident_states) <= args.resident_mib * MiB
        checks["no_admission_refusals"] = "resident admission refused:" not in auto_text
        if args.pressure_on_first_submit:
            checks["completed_restores_within_limit"] = checks.pop("tracked_resident_peak_within_limit")
    tracked_peak = max(resident_states) if resident_states else None
    if args.eviction_policy is not None:
        checks["eviction_policy_enabled"] = f"Vulkan eviction policy={args.eviction_policy}" in auto_text
    profiles = [dict(zip(("copy_calls", "copy_bytes", "copy_ns", "decode_bytes", "decode_ns"), map(int, values)))
                for values in re.findall(r"copy-calls=(\d+) copy-bytes=(\d+) copy-ns=(\d+) decode-bytes=(\d+) decode-ns=(\d+)", auto_text)]
    result = {"passed": all(checks.values()), "checks": checks, "model": str(model),
              "transfer_profile": profiles[-1] if profiles else None,
              "gpu_restore_profile": (dict(zip(("calls", "bytes", "host_ns", "fallbacks"), gpu_profiles[-1]))
                                      if args.gdeflate_gpu and gpu_profiles else None),
              "snapshot_codec": args.codec or "zstd", "gdeflate_gpu_requested": args.gdeflate_gpu,
              "gdeflate_encoding_workers": args.gdeflate_workers or 1, "lazy_backing_requested": args.lazy_backing, "headroom_mib": args.headroom_mib,
              "validation_enabled": args.validate,
              "binary": str(binary), "command": command,
              "batching": {"max_nodes_per_submit": env.get("GGML_VK_MAX_NODES_PER_SUBMIT"), "serialize_submissions": env.get("GGML_VK_SERIALIZE_SUBMISSIONS")},
              "eviction_policy": args.eviction_policy,
              "min_savings_percent": args.min_savings_percent,
              "byte_shuffle": args.byte_shuffle,
              "snapshot_encoding_counts": {"compressed_chunks": sum(x[2] for x in encodings),
                                            "raw_chunks": sum(x[3] for x in encodings),
                                            "shuffled_chunks": sum(shuffled_counts)},
              "resident_admission_events": pressure_events,
              "tracked_resident_peak_after_arming": None if args.pressure_on_first_submit else tracked_peak,
              "tracked_resident_peak_at_completed_restore": tracked_peak if args.pressure_on_first_submit else None,
              "tracked_resident_peak_observed_since_first_admission": (
                  max(all_pressure_states) if args.pressure_on_first_submit and all_pressure_states else None),
              "clean_cache_state": cache_states[-1] if cache_states else None,
              "native": {k: v for k, v in native.items() if k not in ("stdout", "stderr")},
              "automatic": {k: v for k, v in auto.items() if k not in ("stdout", "stderr")},
              "automatic_cold_state_before_prompt": cold_state,
              "automatic_pre_prompt_state": pre_prompt_state,
              "automatic_pre_prompt_state_kind": (
                  "first-submit-pressure" if args.pressure_on_first_submit else "fully-cold"),
              "automatic_cold_logical_bytes": cold_state[1] if cold_state else None,
              "automatic_cold_stored_bytes": cold_state[2] if cold_state else None,
              "automatic_snapshot_events": auto_cold,
              "selective_restore_events": selective_events,
              "selective_range_restore_events": range_events,
              "selective_restore_fallbacks": auto_text.count("selective restore fallback:"),
              "backing_bytes": auto["backing"], "cleanup_log_lines": cleanup_fields,
              "resident_vram_freed_bytes": (None if args.pressure_on_first_submit else
                  auto["backing"].get("hot", {}).get("resident-vram", 0) -
                  auto["backing"].get("cold", {}).get("resident-vram", 0)),
              "resident_vram_pressure_delta_bytes": (
                  auto["backing"].get("hot", {}).get("resident-vram", 0) -
                  auto["backing"].get("pressure", {}).get("resident-vram", 0)
                  if args.pressure_on_first_submit else None),
              "stdout_sha256": hashlib.sha256(auto["stdout"]).hexdigest()}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
