#!/usr/bin/env python3
"""Run both known Odysseus GGUFs concurrently through zVram HIP VMM."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import check_model  # noqa: E402


MiB = 1024 * 1024
PHYSICAL_VRAM_BYTES = 25753026560
OUTPUT = ROOT / "build" / "odysseus-pair"
ROCM_LIBS = str(ROOT / "build/third-party/rocm-prefix/root/opt/rocm/lib") + ":/opt/rocm/lib"
RUNS = [
    ("coder30b", Path("/run/media/nerdrx/Lex/Odysseus/data/ollama/models/blobs/sha256-9fddd9b57b678ca9f9f7b07b02c6f7107bc0f8b307e2383a68cf7a513e6ae0f5"), 64),
    ("dense27b", Path("/run/media/nerdrx/Lex/Odysseus/data/ollama/models/blobs/sha256-6c2c13cef89238c3604d756b07b3ef5fafebbd61095feb8553ff449c95e4c1c6"), 32),
]


def gpu_memory():
    device = Path("/sys/class/drm/card1/device")
    result = {}
    for name in ("mem_info_vram_used", "mem_info_gtt_used"):
        try:
            result[name] = int((device / name).read_text().strip())
        except (OSError, ValueError):
            result[name] = None
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                result["mem_available_kib"] = int(line.split()[1])
                break
    except (OSError, ValueError):
        result["mem_available_kib"] = None
    return result


def launch(label, model, tokens):
    model_bytes = model.stat().st_size
    binary = ROOT / "build/third-party/llama-build/bin/llama-completion"
    launcher = ROOT / "zvram"
    command = [str(launcher), "--hip", "--hip-vmm", "--hip-report-capacity",
               "--hip-local-mib", "4096", "--hip-host-mib", "20000", "--",
               *check_model.common_app_args(binary, model, tokens)]
    env = check_model.clean_environment()
    env.update(HIP_VISIBLE_DEVICES="0", LD_LIBRARY_PATH=ROCM_LIBS)
    out = OUTPUT / f"{label}.stdout.log"
    err = OUTPUT / f"{label}.stderr.log"
    stdout_file, stderr_file = out.open("wb"), err.open("wb")
    try:
        proc = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                stdout=stdout_file, stderr=stderr_file, env=env,
                                start_new_session=True)
    except BaseException:
        stdout_file.close()
        stderr_file.close()
        raise
    return {"label": label, "model": str(model), "model_bytes": model_bytes,
            "tokens": tokens, "command": command, "stdout_path": str(out),
            "stderr_path": str(err), "proc": proc,
            "files": (stdout_file, stderr_file), "deadline": time.monotonic() + 120}


def stop_owned(runs):
    for run in runs:
        proc = run["proc"]
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    for run in runs:
        run["proc"].wait()


def main():
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f"interrupted by signal {signum}")
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    runs = []
    samples = []
    memory_before = gpu_memory()
    try:
        for label, model, tokens in RUNS:
            if not model.is_file() or model.stat().st_size <= 0:
                raise FileNotFoundError(f"missing/nonempty model required: {model}")
        for label, model, tokens in RUNS:
            runs.append(launch(label, model, tokens))

        while any(run["proc"].poll() is None for run in runs):
            now = time.monotonic()
            for run in runs:
                if run["proc"].poll() is None and now >= run["deadline"]:
                    raise TimeoutError(f"{run['label']} exceeded its 120-second deadline")
            states = {}
            for run in runs:
                states[run["label"]] = {
                    "alive": run["proc"].poll() is None,
                    "stdout_bytes": Path(run["stdout_path"]).stat().st_size,
                }
            samples.append({"elapsed_seconds": round(now - runs[0]["deadline"] + 120, 3),
                            "runs": states, "gpu": gpu_memory()})
            time.sleep(0.1)
    except BaseException:
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(signum, signal.SIG_IGN)
        stop_owned(runs)
        raise
    finally:
        for run in runs:
            for file in run["files"]:
                file.close()
    memory_after = gpu_memory()

    reports = []
    for run in runs:
        proc = run["proc"]
        _, text = check_model.read_run_output(run)
        metrics = check_model.inspect_log(text)
        summary = check_model.inspect_vmm_summary(text)
        stdout_size = Path(run["stdout_path"]).stat().st_size
        reports.append({key: value for key, value in run.items() if key not in ("proc", "files", "deadline")}
                       | {"returncode": proc.returncode, "stdout_bytes": stdout_size,
                          "last_offloaded_layers": metrics["last_offloaded_layers"],
                          "largest_model_buffer_mib": metrics["largest_rocm_model_buffer_mib"],
                          "vmm_summary": summary})

    overlap = sum(1 for sample in samples if all(
        state["alive"] and state["stdout_bytes"] > 0
        for state in sample["runs"].values()))
    checks = {
        "both_exit_zero": all(run["returncode"] == 0 for run in reports),
        "both_stdout_nonempty": all(run["stdout_bytes"] > 0 for run in reports),
        "both_offload_complete": all(run["last_offloaded_layers"] and
            run["last_offloaded_layers"][0] == run["last_offloaded_layers"][1] > 0 for run in reports),
        "both_model_buffers_over_12000_mib": all(run["largest_model_buffer_mib"] > 12000 for run in reports),
        "both_positive_vmm_allocations": all(run["vmm_summary"] and
            run["vmm_summary"]["hybrid_vmm_allocations"] > 0 for run in reports),
        "both_cleanup_zero": all(run["vmm_summary"] and run["vmm_summary"]["all_required_fields_present"] and
            run["vmm_summary"]["all_required_fields_zero"] for run in reports),
        "observed_live_output_overlap": overlap > 0,
    }
    total_weights = sum(run["model_bytes"] for run in reports)
    total_model_buffer_mib = sum(run["largest_model_buffer_mib"] for run in reports)
    checks["combined_actual_model_buffers_exceed_physical_vram"] = total_model_buffer_mib * MiB > PHYSICAL_VRAM_BYTES
    report = {"runs": reports, "checks": checks, "passed": all(checks.values()),
              "sample_interval_ms": 100, "sample_count": len(samples),
              "live_output_overlap_samples": overlap, "gpu_memory_samples": samples,
              "gpu_memory_before": memory_before, "gpu_memory_after": memory_after,
              "physical_vram_bytes": PHYSICAL_VRAM_BYTES,
              "combined_actual_model_buffer_mib": total_model_buffer_mib,
              "combined_model_file_bytes": total_weights,
              "combined_model_files_exceed_physical_vram": total_weights > PHYSICAL_VRAM_BYTES,
              "claim_limit": "Concurrent VMM run only; does not establish compression or native-baseline benefit."}
    path = OUTPUT / "summary.json"
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"summary: {path}")
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'}: {name}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
