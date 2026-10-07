#!/usr/bin/env python3
"""Compare native inference with an unchanged llama-completion app after automatic idle compression."""

import hashlib
import json
import re
import signal
import subprocess
import time
from pathlib import Path

from check_model import (clean_environment, common_app_args, inspect_log,
                         inspect_vmm_summary, parser_for_script, validate_args)


ColdPattern = re.compile(r"auto hibernate: status=0 logical=(\d+) stored=(\d+)")
Prompt = b"Tell me a very short story about a fox.\n"


def performance(text):
    decode = re.findall(r"\beval time\s*=\s*([\d.]+) ms /\s*(\d+) runs\s*\([^\n]*?([\d.]+) tokens per second", text)
    prompt = re.findall(r"prompt eval time\s*=\s*([\d.]+) ms /\s*(\d+) tokens\s*\([^\n]*?([\d.]+) tokens per second", text)
    wakes = re.findall(r"auto resume: status=0 logical=0 stored=0 elapsed_ms=([\d.]+)", text)
    return {"decode_ms": float(decode[-1][0]) if decode else None,
            "decode_runs": int(decode[-1][1]) if decode else None,
            "tokens_per_second": float(decode[-1][2]) if decode else None,
            "prompt_ms": float(prompt[-1][0]) if prompt else None,
            "restore_ms": list(map(float, wakes))}


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
        if client and pdev:
            memory = {key: int(value) * 1024 for key, value in
                      re.findall(r"drm-memory-(vram|gtt):\s*(\d+) KiB", text)}
            clients[(pdev[1], client[1])] = memory
    path.write_text("\n".join(raw))
    return {kind: sum(memory.get(kind, 0) for memory in clients.values()) for kind in ("vram", "gtt")}


def run_interactive(label, command, env, output_dir, timeout, automatic):
    stdout_path = output_dir / f"{label}.stdout.txt"
    stderr_path = output_dir / f"{label}.stderr.txt"
    snapshot = None
    backing = {}
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=stdout,
                                   stderr=stderr, env=env, start_new_session=True)
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                text = stderr_path.read_text(errors="replace")
                if process.poll() is not None:
                    raise RuntimeError(f"{label} exited before receiving input: {process.returncode}")
                if "automatic hibernation disabled:" in text:
                    raise RuntimeError(f"{label} disabled automatic hibernation")
                if "== Running in interactive mode. ==" in text:
                    matches = ColdPattern.findall(text)
                    if "hot" not in backing and not matches:
                        backing["hot"] = capture_backing(process.pid, output_dir / f"{label}-hot.fdinfo.txt")
                    if not automatic or (matches and int(matches[-1][0]) > 0):
                        if automatic:
                            snapshot = list(map(int, matches[-1]))
                            backing["cold"] = capture_backing(process.pid, output_dir / f"{label}-cold.fdinfo.txt")
                        break
                time.sleep(0.05)
            else:
                raise RuntimeError(f"{label} timed out waiting for input-ready cold model")
            process.stdin.write(Prompt)
            process.stdin.close()
            status = process.wait(timeout=max(0.01, deadline - time.monotonic()))
            if status:
                raise RuntimeError(f"{label} exited with status {status}")
        finally:
            if process.poll() is None:
                import os
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            if not process.stdin.closed:
                process.stdin.close()
    text = stderr_path.read_text(errors="replace")
    return stdout_path.read_bytes(), text, snapshot, backing


def main():
    root = Path(__file__).resolve().parent
    parser = parser_for_script()
    parser.description = __doc__
    parser.set_defaults(tokens=128, output_dir=Path("build/idle-model-check"))
    parser.add_argument("--idle-ms", type=int, default=1000)
    parser.add_argument("--cold-mib", type=int, default=2048)
    parser.add_argument("--vmm-baseline", action="store_true", help="compare identical VMM placement with automatic mode off/on")
    args = parser.parse_args()
    try:
        binary, model, launcher, output = validate_args(args, root)
        if args.vmm_only:
            raise ValueError("this comparison requires the native baseline")
        if args.tokens < 64:
            raise ValueError("interactive llama.cpp counts input against --predict; use --tokens 64..128")
        if not 1 <= args.idle_ms <= 60000 or not 1 <= args.cold_mib <= 40960:
            raise ValueError("idle-ms must be 1..60000 and cold-mib 1..40960")
    except ValueError as error:
        parser.error(str(error))
    output.mkdir(parents=True, exist_ok=True)
    # No source changes: disable HIP graph capture using llama.cpp's native setting.
    app = common_app_args(binary, model, args.tokens)[:-2]
    app += ["--conversation", "--interactive-first", "--single-turn"]
    env = clean_environment()
    env.pop("ROCPROFILER_REGISTER_LIBRARY", None)
    env.pop("ROCPROFILER_REGISTER_SECURE", None)
    env["GGML_CUDA_DISABLE_GRAPHS"] = "1"
    placement = [str(launcher), "--hip", "--hip-vmm", "--hip-local-mib", str(args.local_mib),
                 "--hip-host-mib", str(args.host_mib)]
    baseline_label = "vmm-baseline" if args.vmm_baseline else "native"
    baseline_command = [*placement, "--", *app] if args.vmm_baseline else app
    native_output, native_text, _, _ = run_interactive(baseline_label, baseline_command, env, output, args.timeout, False)
    command = [*placement, "--hip-auto-idle-ms", str(args.idle_ms),
               "--hip-cold-mib", str(args.cold_mib), "--", *app]
    auto_output, auto_text, snapshot, backing = run_interactive("automatic", command, env, output, args.timeout, True)
    metrics = inspect_log(auto_text)
    native_metrics = inspect_log(native_text)
    cleanup = inspect_vmm_summary(auto_text)
    baseline_perf, auto_perf = performance(native_text), performance(auto_text)
    offload = metrics["last_offloaded_layers"]
    checks = {
        "same_nonempty_stdout": bool(native_output.strip()) and native_output == auto_output,
        "complete_gpu_offload": bool(offload and offload[0] == offload[1] > 0),
        "baseline_complete_gpu_offload": native_metrics["last_offloaded_layers"] == offload,
        "cold_model_before_input": bool(snapshot and snapshot[0] >=
            metrics["largest_rocm_model_buffer_mib"] * 1024 * 1024),
        "successful_automatic_restore": "auto resume: status=0 logical=0 stored=0" in auto_text,
        "automatic_remained_enabled": "automatic hibernation disabled:" not in auto_text,
        "zero_cleanup_and_failures": bool(cleanup and cleanup["all_required_fields_present"] and
            cleanup["all_required_fields_zero"]),
        "model_buffer_above_minimum": metrics["largest_rocm_model_buffer_mib"] > args.min_model_mib,
    }
    result = {"passed": all(checks.values()), "checks": checks, "model": str(model),
              "baseline": baseline_label,
              "binary": str(binary), "command": command, "graphs_disabled": True,
              "cold_logical_bytes": snapshot[0], "cold_stored_bytes": snapshot[1],
              "metrics": metrics, "cleanup": cleanup,
              "backing_bytes": backing,
              "performance": {"baseline": baseline_perf, "automatic": auto_perf,
                  "decode_speed_change_percent": 100 * (auto_perf["tokens_per_second"] /
                      baseline_perf["tokens_per_second"] - 1) if baseline_perf["tokens_per_second"] and
                      auto_perf["tokens_per_second"] else None},
              "stdout_sha256": hashlib.sha256(auto_output).hexdigest()}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
