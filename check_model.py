#!/usr/bin/env python3
"""Check an existing llama-completion run with zVram HIP VMM."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys


MiB = 1024 * 1024
CapacityProofMiB = 40 * 1024
OffloadPattern = re.compile(r"offloaded\s+(\d+)\s*/\s*(\d+)\s+layers?\s+to GPU", re.IGNORECASE)
RocmBufferPattern = re.compile(
    r"\bROCm\d+\s+model buffer size\s*=\s*([\d,]+(?:\.\d+)?)\s*MiB\b",
    re.IGNORECASE,
)
SummaryPattern = re.compile(r"\[zvram-hip\]\s+summary:\s*(.*)")
FieldPattern = re.compile(r"([a-zA-Z_]+)=(\d+)")


def parser_for_script():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="existing llama-completion executable")
    parser.add_argument("--model", required=True, type=Path, help="existing GGUF model file")
    parser.add_argument("--local-mib", type=int, default=64, help="VMM local allocation cap (0..40960 MiB)")
    parser.add_argument("--host-mib", type=int, default=2048, help="VMM host/GTT allocation cap (1..40960 MiB)")
    parser.add_argument("--output-dir", type=Path, default=Path("build/model-check"), help="directory for captured logs")
    parser.add_argument("--tokens", type=int, default=32, help="greedy generation token count (1..128)")
    parser.add_argument("--timeout", type=int, default=120, help="per-run timeout in seconds (1..120)")
    parser.add_argument("--min-model-mib", type=int, default=0,
                        help="require actual ROCm model buffer to be greater than this MiB value")
    parser.add_argument("--vmm-only", action="store_true",
                        help="run only VMM; omit the native baseline comparison")
    return parser


def validate_args(args, project_dir):
    if not 0 <= args.local_mib <= 40960:
        raise ValueError("--local-mib must be between 0 and 40960")
    if not 1 <= args.host_mib <= 40960:
        raise ValueError("--host-mib must be between 1 and 40960")
    if not 1 <= args.tokens <= 128:
        raise ValueError("--tokens must be between 1 and 128")
    if not 1 <= args.timeout <= 120:
        raise ValueError("--timeout must be between 1 and 120 seconds")
    if not 0 <= args.min_model_mib <= 40960:
        raise ValueError("--min-model-mib must be between 0 and 40960")

    binary = args.binary.expanduser().resolve()
    model = args.model.expanduser().resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f"binary must be an existing executable file: {binary}")
    if not model.is_file() or model.stat().st_size <= 0:
        raise ValueError(f"model must be an existing nonempty file: {model}")
    launcher = project_dir / "zvram"
    if not launcher.is_file():
        raise ValueError(f"zVram launcher is missing: {launcher}")
    hip_layer = project_dir / "build" / "libzvram_hip.so"
    if not hip_layer.is_file():
        raise ValueError(f"zVram HIP layer is missing: {hip_layer}")
    output_dir = args.output_dir.expanduser()
    if not output_dir.is_absolute():
        output_dir = project_dir / output_dir
    return binary, model, launcher, output_dir.resolve()


def clean_environment():
    env = os.environ.copy()
    for name in list(env):
        if (name.startswith("ZVRAM_") or name == "LD_PRELOAD" or
                name in ("GGML_CUDA_ENABLE_UNIFIED_MEMORY", "GGML_CUDA_REGISTER_HOST")):
            env.pop(name, None)
    return env


def common_app_args(binary, model, tokens):
    return [
        str(binary),
        "--model", str(model),
        "--gpu-layers", "999",
        "--ctx-size", "512",
        "--batch-size", "128",
        "--predict", str(tokens),
        "--temp", "0",
        "--seed", "1",
        "--load-mode", "none",
        "--fit", "off",
        "--flash-attn", "off",
        "--verbose",
        "--simple-io",
        "--no-display-prompt",
        "--prompt", "Once upon a time, a deterministic GPU memory test",
    ]


def run_capture(label, command, env, output_dir, timeout):
    stdout_path = output_dir / f"{label}.stdout.txt"
    stderr_path = output_dir / f"{label}.stderr.txt"
    timed_out = False
    launch_error = None
    returncode = None
    with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
        try:
            process = subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL,
                stdout=stdout_file,
                stderr=stderr_file,
                env=env,
                start_new_session=True,
            )
            try:
                returncode = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        except OSError as error:
            launch_error = str(error)
            stderr_file.write((f"launch error: {error}\n").encode("utf-8", errors="replace"))
    return {
        "label": label,
        "command": command,
        "stdout_path": str(stdout_path),
        "stderr_path": str(stderr_path),
        "returncode": returncode,
        "timed_out": timed_out,
        "launch_error": launch_error,
    }


def read_run_output(run):
    stdout = Path(run["stdout_path"]).read_bytes()
    stderr = Path(run["stderr_path"]).read_bytes()
    text = (stderr + b"\n" + stdout).decode("utf-8", errors="replace")
    return stdout, text


def inspect_log(text):
    offloads = [(int(match.group(1)), int(match.group(2))) for match in OffloadPattern.finditer(text)]
    buffers = [float(match.group(1).replace(",", "")) for match in RocmBufferPattern.finditer(text)]
    return {
        "last_offloaded_layers": list(offloads[-1]) if offloads else None,
        "rocm_model_buffers_mib": buffers,
        "largest_rocm_model_buffer_mib": max(buffers, default=0.0),
    }


def inspect_vmm_summary(text):
    matches = SummaryPattern.findall(text)
    if not matches:
        return None
    fields = {key: int(value) for key, value in FieldPattern.findall(matches[-1])}
    required_zero_fields = [
        "tracked",
        "hipMalloc_api_bytes_current",
        "host_pinned_backing_bytes_current",
        "pending",
        "orphaned_vmm_cleanup",
        "failures",
    ]
    return {
        "fields": fields,
        "required_zero_fields": required_zero_fields,
        "all_required_fields_present": all(name in fields for name in required_zero_fields),
        "all_required_fields_zero": all(fields.get(name) == 0 for name in required_zero_fields),
        "hybrid_vmm_allocations": fields.get("hybrid_vmm_allocations", 0),
    }


def main():
    project_dir = Path(__file__).resolve().parent
    parser = parser_for_script()
    args = parser.parse_args()
    try:
        binary, model, launcher, output_dir = validate_args(args, project_dir)
    except ValueError as error:
        parser.error(str(error))

    output_dir.mkdir(parents=True, exist_ok=True)
    app_args = common_app_args(binary, model, args.tokens)
    env = clean_environment()
    native = None
    if not args.vmm_only:
        native = run_capture("native", app_args, env.copy(), output_dir, args.timeout)
    vmm_command = [
        str(launcher), "--hip", "--hip-vmm", "--hip-report-capacity",
        "--hip-local-mib", str(args.local_mib),
        "--hip-host-mib", str(args.host_mib),
        "--", *app_args,
    ]
    vmm = run_capture("vmm", vmm_command, env.copy(), output_dir, args.timeout)

    vmm_stdout, vmm_text = read_run_output(vmm)
    vmm_metrics = inspect_log(vmm_text)
    vmm_summary = inspect_vmm_summary(vmm_text)
    checks = {
        "vmm_exit_zero": vmm["returncode"] == 0 and not vmm["timed_out"] and not vmm["launch_error"],
        "vmm_stdout_nonempty": bool(vmm_stdout.strip()),
        "vmm_offload_complete": bool(vmm_metrics["last_offloaded_layers"] and
            vmm_metrics["last_offloaded_layers"][0] == vmm_metrics["last_offloaded_layers"][1] > 0),
        "vmm_model_buffer_above_minimum": vmm_metrics["largest_rocm_model_buffer_mib"] > args.min_model_mib,
        "vmm_allocations_positive": bool(vmm_summary and vmm_summary["hybrid_vmm_allocations"] > 0),
        "vmm_summary_zero_cleanups": bool(vmm_summary and vmm_summary["all_required_fields_present"] and
            vmm_summary["all_required_fields_zero"]),
    }
    native_metrics = None
    stdout_match = None
    if native is not None:
        native_stdout, native_text = read_run_output(native)
        native_metrics = inspect_log(native_text)
        stdout_match = native_stdout == vmm_stdout
        checks = {
            "native_exit_zero": native["returncode"] == 0 and not native["timed_out"] and not native["launch_error"],
            **checks,
            "stdout_exact_match": stdout_match,
            "native_offload_complete": bool(native_metrics["last_offloaded_layers"] and
                native_metrics["last_offloaded_layers"][0] == native_metrics["last_offloaded_layers"][1] > 0),
            "native_model_buffer_above_minimum": native_metrics["largest_rocm_model_buffer_mib"] > args.min_model_mib,
        }
    largest_vmm_buffer = vmm_metrics["largest_rocm_model_buffer_mib"]
    if args.vmm_only:
        capacity_note = (
            f"VMM-only passed with an actual ROCm model buffer of {largest_vmm_buffer:.2f} MiB; "
            "no native baseline was run, so this does not establish gain over native or a universal capacity guarantee."
            if all(checks.values()) and largest_vmm_buffer >= CapacityProofMiB else
            f"VMM-only run has no native baseline. No 40 GiB model-capacity proof: "
            f"largest actual VMM-run ROCm model buffer was {largest_vmm_buffer:.2f} MiB."
        )
    else:
        capacity_note = (
            "A passed check with a reported ROCm model buffer of at least 40960 MiB; not a universal capacity guarantee."
            if all(checks.values()) and largest_vmm_buffer >= CapacityProofMiB else
            f"No 40 GiB model-capacity proof: largest actual VMM-run ROCm model buffer was {largest_vmm_buffer:.2f} MiB."
        )
    stdout_hash = hashlib.sha256(vmm_stdout).hexdigest()
    report = {
        "binary": str(binary),
        "model": str(model),
        "model_bytes": model.stat().st_size,
        "local_mib": args.local_mib,
        "host_mib": args.host_mib,
        "tokens": args.tokens,
        "timeout_seconds": args.timeout,
        "minimum_model_buffer_mib": args.min_model_mib,
        "mode": "vmm-only" if args.vmm_only else "native-vmm-comparison",
        "native": {**native, **native_metrics} if native is not None else None,
        "vmm": {**vmm, **vmm_metrics, "summary": vmm_summary},
        "stdout_exact_match": stdout_match,
        "stdout_sha256": stdout_hash,
        "capacity_note": capacity_note,
        "checks": checks,
        "passed": all(checks.values()),
    }
    report_path = output_dir / "summary.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    if native is not None:
        print(f"native stdout: {native['stdout_path']}")
        print(f"native stderr: {native['stderr_path']}")
        print(f"native last offload: {native_metrics['last_offloaded_layers']}; largest ROCm model buffer: "
              f"{native_metrics['largest_rocm_model_buffer_mib']:.2f} MiB")
    else:
        print("native baseline: not run")
    print(f"VMM stdout: {vmm['stdout_path']}")
    print(f"VMM stderr: {vmm['stderr_path']}")
    print(f"summary: {report_path}")
    print(f"VMM last offload: {vmm_metrics['last_offloaded_layers']}; largest ROCm model buffer: "
          f"{largest_vmm_buffer:.2f} MiB")
    if vmm_summary:
        print(f"VMM allocations: {vmm_summary['hybrid_vmm_allocations']}; cleanup counters zero: "
              f"{vmm_summary['all_required_fields_zero']}")
    if stdout_match is not None:
        print(f"native/VMM stdout byte-identical: {stdout_match}; SHA-256: {stdout_hash}")
    else:
        print(f"VMM stdout SHA-256: {stdout_hash}")
    print(capacity_note)
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'}: {name}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
