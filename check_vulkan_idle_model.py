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
VulkanBufferPattern = re.compile(
    r"\bVulkan\d+\s+model buffer size\s*=\s*([\d,]+(?:\.\d+)?)\s*MiB\b", re.I)
OffloadPattern = re.compile(r"offloaded\s+(\d+)\s*/\s*(\d+)\s+layers?\s+to GPU", re.I)


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


def run_interactive(label, command, env, output_dir, timeout, automatic):
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
    try:
        while time.monotonic() < deadline:
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
            tolerance = len(model_mib) * 0.01 * MiB
            cold_ready = (cold_state and cold_state[0] == 0 and cold_state[1] + tolerance >=
                          sum(model_mib) * MiB and cold_state[3] > 0 and cold_state[5] == 0)
            if automatic and ready and model_mib and cold and cold_ready:
                if last_cold_time is not None and time.monotonic() - last_cold_time >= 0.2:
                    if "hot" not in backing:
                        raise RuntimeError(f"{label}: no hot DRM snapshot was captured before eviction")
                    backing["cold"] = capture_backing(proc.pid, output_dir / f"{label}-cold.fdinfo.txt")
                    pre_prompt_cold_state = cold_state
                    break
        else:
            raise RuntimeError(f"{label} timed out waiting for ready model/snapshot")
        prompt_time = time.monotonic()
        proc.stdin.write(Prompt)
        proc.stdin.close()
        prompt_sent = True
        while proc.poll() is None and time.monotonic() < deadline:
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
    offloads = [(int(a), int(b)) for a, b in OffloadPattern.findall(text)]
    return {"stdout": bytes(stdout), "stderr": text, "model_buffers_mib": model_mib,
            "cold": cold, "cold_state": cold_state, "pre_prompt_cold_state": pre_prompt_cold_state,
            "backing": backing,
            "offload": list(offloads[-1]) if offloads else None,
            "performance": performance(text),
            "first_stdout_after_input_ms": ((first_output_time - prompt_time) * 1000
                                            if first_output_time and prompt_time else None),
            "returncode": proc.returncode}


def main():
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--tokens", type=int, default=128)
    parser.add_argument("--idle-ms", type=int, default=1000)
    parser.add_argument("--cold-mib", type=int, default=512)
    parser.add_argument("--selective-restore", action="store_true",
                        help="test opt-in per-submission Vulkan restoration")
    parser.add_argument("--active-eviction", action="store_true",
                        help="test tracked eviction while unrelated submissions remain active; enables selective restore")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--output-dir", type=Path, default=Path("build/vulkan-idle-model-check"))
    parser.add_argument("--app-arg", action="append", default=[],
                        help="extra llama-completion option; repeat as --app-arg=VALUE")
    args = parser.parse_args()
    args.selective_restore = args.selective_restore or args.active_eviction
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
    if not 64 <= args.tokens <= 128 or not 1 <= args.idle_ms <= 60000 or not 1 <= args.cold_mib <= 40960 or not 1 <= args.timeout <= 120:
        parser.error("tokens must be 64..128, idle-ms 1..60000, cold-mib 1..40960, timeout 1..120")
    output.mkdir(parents=True, exist_ok=True)
    app = common_app_args(binary, model, args.tokens)[:-2]
    app += ["--conversation", "--interactive-first", "--single-turn", *args.app_arg]
    env = clean_environment()
    env.pop("ROCPROFILER_REGISTER_LIBRARY", None)
    env.pop("ROCPROFILER_REGISTER_SECURE", None)
    env["GGML_CUDA_DISABLE_GRAPHS"] = "1"
    env["GGML_VK_VISIBLE_DEVICES"] = "0"
    env["GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM"] = "1"
    native = run_interactive("native", app, env, output, args.timeout, False)
    command = [str(launcher), "--vulkan-virtual-gib", "96", "--vulkan-auto-idle-ms",
               str(args.idle_ms), "--vulkan-cold-mib", str(args.cold_mib)]
    if args.selective_restore:
        command.append("--vulkan-selective-restore")
    if args.active_eviction:
        command.append("--vulkan-active-eviction")
    command += ["--", *app]
    auto = run_interactive("automatic", command, env, output, args.timeout, True)
    auto_text = auto["stderr"]
    auto_cold = auto["cold"]
    cleanup_fields = re.findall(r"\[zvram\].*(?:summary|automatic).*", auto_text, re.I)
    cold_state = auto["pre_prompt_cold_state"]
    checks = {
        "same_nonempty_stdout": bool(native["stdout"].strip()) and native["stdout"] == auto["stdout"],
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
    selective_events = [tuple(map(int, values)) for values in re.findall(
        r"selective restore selected-pools=(\d+) restored-pools=(\d+) cold-pools-left=(\d+)", auto_text)]
    if args.selective_restore:
        checks["selective_enabled"] = "selective Vulkan restore enabled" in auto_text
        checks["tracked_restore_observed"] = any(restored > 0 for _, restored, _ in selective_events)
    if args.active_eviction:
        checks["active_eviction_enabled"] = "active Vulkan eviction enabled" in auto_text
    result = {"passed": all(checks.values()), "checks": checks, "model": str(model),
              "binary": str(binary), "command": command,
              "native": {k: v for k, v in native.items() if k not in ("stdout", "stderr")},
              "automatic": {k: v for k, v in auto.items() if k not in ("stdout", "stderr")},
              "automatic_cold_state_before_prompt": cold_state,
              "automatic_cold_logical_bytes": cold_state[1] if cold_state else None,
              "automatic_cold_stored_bytes": cold_state[2] if cold_state else None,
              "automatic_snapshot_events": auto_cold,
              "selective_restore_events": selective_events,
              "selective_restore_fallbacks": auto_text.count("selective restore fallback:"),
              "backing_bytes": auto["backing"], "cleanup_log_lines": cleanup_fields,
              "resident_vram_freed_bytes": (auto["backing"].get("hot", {}).get("resident-vram", 0) -
                                             auto["backing"].get("cold", {}).get("resident-vram", 0)),
              "stdout_sha256": hashlib.sha256(auto["stdout"]).hexdigest()}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
