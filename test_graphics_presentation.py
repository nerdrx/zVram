#!/usr/bin/env python3
"""Bounded hidden Gamescope presentation correctness gate, not a game benchmark."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--icd", required=True, type=Path)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--cpu", action="store_true")
    parser.add_argument("--native-allocation", action="store_true")
    parser.add_argument("--async-compression", action="store_true")
    parser.add_argument("--lazy-backing", action="store_true")
    parser.add_argument("--headroom-mib", type=int)
    parser.add_argument("--expect-headroom-refusal", action="store_true")
    codec = parser.add_mutually_exclusive_group()
    codec.add_argument("--gdeflate-gpu", action="store_true")
    codec.add_argument("--bp16-gpu", action="store_true")
    host_input = parser.add_mutually_exclusive_group()
    host_input.add_argument("--bp16-host-input", action="store_true")
    host_input.add_argument("--bp16-import-host-input", action="store_true")
    host_input.add_argument("--bp16-allocated-host-input", action="store_true")
    parser.add_argument("--bp16-upload-workers", type=int, choices=range(1, 9))
    parser.add_argument("--video-driver", choices=("x11", "wayland"), default="x11")
    parser.add_argument("--present-metadata", choices=("id", "regions", "both"))
    parser.add_argument("--prefer-device")
    parser.add_argument("--output-dir", type=Path, default=Path("build/presentation-check"))
    args = parser.parse_args()
    if args.native and (args.async_compression or args.gdeflate_gpu or args.bp16_gpu or
                        args.bp16_host_input or args.bp16_import_host_input or args.bp16_allocated_host_input or args.lazy_backing or args.headroom_mib is not None):
        parser.error("paging options require a wrapped run")
    if args.headroom_mib is not None and (not args.lazy_backing or args.headroom_mib <= 0):
        parser.error("headroom requires lazy backing and a positive MiB size")
    if args.expect_headroom_refusal and (args.headroom_mib is None or args.async_compression or
                                         args.gdeflate_gpu or args.bp16_gpu or args.bp16_host_input or args.bp16_import_host_input or args.bp16_allocated_host_input):
        parser.error("refusal check requires headroom without encoder/decode options")
    if (args.bp16_host_input or args.bp16_import_host_input or args.bp16_allocated_host_input) and not args.bp16_gpu:
        parser.error("BP16 host input requires --bp16-gpu")
    if args.bp16_upload_workers is not None and not args.bp16_gpu:
        parser.error("BP16 upload workers require --bp16-gpu")
    if args.cpu and not args.native:
        parser.error("CPU control requires --native; this does not validate zVram paging")
    binary, icd = args.binary.resolve(strict=True), args.icd.resolve(strict=True)
    root = Path(__file__).resolve().parent
    env = os.environ.copy()
    for key in ("LD_PRELOAD", "VK_INSTANCE_LAYERS", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES",
                "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_LOADER_LAYERS_ENABLE", "DRI_PRIME"):
        env.pop(key, None)
    for key in tuple(env):
        if key.startswith("ZVRAM_"):
            env.pop(key)
    env.update(VK_DRIVER_FILES=str(icd), VK_LOADER_LAYERS_DISABLE="~implicit~",
               VK_VALIDATION_VALIDATE_SYNC="1", DISABLE_GAMESCOPE_WSI="1", DISABLE_LSFGVK="1",
               LP_NUM_THREADS="2", MALLOC_ARENA_MAX="2")
    if args.bp16_host_input:
        env["ZVRAM_VULKAN_BP16_HOST_INPUT"] = "1"
    if args.bp16_import_host_input:
        env["ZVRAM_VULKAN_BP16_IMPORT_HOST_INPUT"] = "1"
    if args.bp16_allocated_host_input:
        env["ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT"] = "1"
    if args.bp16_upload_workers is not None:
        env["ZVRAM_VULKAN_BP16_UPLOAD_WORKERS"] = str(args.bp16_upload_workers)
    command = ["gamescope", "--backend", "headless", "--expose-wayland",
               "-W", "16", "-H", "16", "-w", "16", "-h", "16", "-r", "60"]
    if args.prefer_device:
        command += ["--prefer-vk-device", args.prefer_device]
    command += ["--", "env", "SDL_VIDEODRIVER=" + args.video_driver]
    if not args.native:
        command += [str(root / "zvram"), "--no-live-control", "--build-dir", str(args.build_dir.resolve()),
                    "--validate", "--isolate-layers", "--vulkan-virtual-mib", "128",
                    "--vulkan-auto-idle-ms", "100", "--vulkan-cold-mib", "64",
                    "--vulkan-selective-restore", "--vulkan-active-eviction",
                    "--vulkan-buffer-presentation"]
        if args.async_compression or args.lazy_backing or args.bp16_gpu:
            command += ["--vulkan-range-mib", "32", "--vulkan-resident-mib", "32"]
        if args.async_compression:
            command.append("--vulkan-async-compression")
        if args.lazy_backing:
            command.append("--vulkan-lazy-backing")
        if args.headroom_mib is not None:
            command += ["--vulkan-headroom-mib", str(args.headroom_mib)]
        if args.gdeflate_gpu:
            command += ["--vulkan-codec", "gdeflate", "--vulkan-gdeflate-workers", "32",
                        "--vulkan-gdeflate-gpu"]
        if args.bp16_gpu:
            command += ["--vulkan-codec", "bp16", "--vulkan-bp16-workers", "8",
                        "--vulkan-bp16-gpu"]
        command.append("--")
    command += [str(binary), "--present", "--frames", "3"]
    if args.present_metadata:
        command += ["--present-metadata", args.present_metadata]
    if args.native:
        command.append("--native")
    if args.native_allocation:
        command.append("--native-allocation")
    if args.lazy_backing:
        command.append("--expect-lazy-backing")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    log_path = args.output_dir / "run.log"
    started, timed_out = time.monotonic(), False
    with log_path.open("w") as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    text = log_path.read_text(errors="replace")
    expected = "PASS: 3 presented draw/readback frames " + ("(native mode)" if args.native else "with cold restore")
    if args.present_metadata:
        expected += " metadata=" + args.present_metadata
    unsupported_lines = [line for line in text.splitlines()
                         if line.startswith("UNSUPPORTED: PRESENT_METADATA: ")]
    unsupported_metadata = bool(
        args.present_metadata and unsupported_lines and not timed_out and
        "PASS:" not in text and
        not any(marker in text for marker in (
            "FAIL:", "VUID-", "Vulkan validation error:", "Validation Error")))
    passed = (not timed_out and process.returncode == 0 and expected in text and
              "validation=on" in text and ("type=4" if args.cpu else "type=2") in text and
              not unsupported_lines and
              not any(marker in text for marker in ("FAIL:", "VUID-", "Vulkan validation error:", "Validation Error")))
    if args.async_compression:
        passed = passed and text.count("async snapshot committed raw=33554432") >= 4
    if args.lazy_backing:
        passed = passed and "PASS: lazy bootstrap resident=0 cold-logical=33554432 cold-stored=0" in text
    if args.headroom_mib is not None:
        passed = passed and "resident budget native-heap=" in text
    if args.gdeflate_gpu:
        import re
        profiles = re.findall(r"GPU GDeflate restore calls=(\d+) bytes=(\d+) host-ns=(\d+) fallbacks=(\d+)", text)
        passed = passed and bool(profiles) and int(profiles[-1][0]) >= 3 and int(profiles[-1][3]) == 0
    if args.bp16_gpu:
        import re
        profiles = re.findall(r"GPU BP16 restore calls=(\d+) bytes=(\d+) host-ns=(\d+) fallbacks=(\d+)", text)
        passed = passed and bool(profiles) and int(profiles[-1][0]) >= 3 and int(profiles[-1][3]) == 0
    if args.bp16_upload_workers is not None:
        import re
        workers = re.findall(r"GPU BP16 upload workers=(\d+)", text)
        passed = passed and bool(workers) and int(workers[-1]) == args.bp16_upload_workers
    if args.bp16_import_host_input:
        imports = re.findall(r"GPU BP16 imported input imports=(\d+) reuses=(\d+) bytes=(\d+)", text)
        passed = passed and bool(imports) and int(imports[-1][0]) > 0 and int(imports[-1][2]) > 0
    if args.bp16_allocated_host_input:
        allocations = re.findall(r"GPU BP16 allocated input allocations=(\d+) reuses=(\d+) bytes=(\d+)", text)
        passed = passed and bool(allocations) and int(allocations[-1][0]) > 0 and int(allocations[-1][2]) > 0
    if args.expect_headroom_refusal:
        passed = (not timed_out and process.returncode == 0 and "graphics-validation=on" in text and
                  "type=2" in text and "PASS: lazy bootstrap resident=0 cold-logical=33554432 cold-stored=0" in text and
                  "effective-limit=0 hard-limit=33554432" in text and
                  "resident admission refused: submission working set exceeds limit-bytes=0 known=1" in text and
                  "FAIL: submit graphics fixture: -2" in text and
                  "event=destroy-cleanup resident=0 cold-logical=0 cold-stored=0 freezes=0 restores=0 failures=0" in text and
                  not any(marker in text for marker in ("VUID-", "Vulkan validation error:", "Validation Error")))
    report = dict(passed=passed, skipped=unsupported_metadata,
                  presentation_metadata=args.present_metadata, command=command, environment={key: env[key] for key in
                  ("VK_DRIVER_FILES", "VK_VALIDATION_VALIDATE_SYNC", "DISABLE_GAMESCOPE_WSI", "DISABLE_LSFGVK")},
                  timeout=timed_out, exit=process.returncode, seconds=time.monotonic()-started,
                  scope=("expected zero-budget refusal before GPU buffer use; no presented frames" if args.expect_headroom_refusal else
                         "three hidden presented frames with pixel/full-buffer checks; no game or speed claim"),
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    if args.bp16_host_input:
        report["environment"]["ZVRAM_VULKAN_BP16_HOST_INPUT"] = env["ZVRAM_VULKAN_BP16_HOST_INPUT"]
    if args.bp16_allocated_host_input:
        report["environment"]["ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT"] = env["ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT"]
    if args.bp16_import_host_input:
        report["environment"]["ZVRAM_VULKAN_BP16_IMPORT_HOST_INPUT"] = env["ZVRAM_VULKAN_BP16_IMPORT_HOST_INPUT"]
    if args.bp16_upload_workers is not None:
        report["environment"]["ZVRAM_VULKAN_BP16_UPLOAD_WORKERS"] = env["ZVRAM_VULKAN_BP16_UPLOAD_WORKERS"]
    if not args.native:
        report["layer_binary_sha256"] = hashlib.sha256((args.build_dir.resolve() / "libzvram_layer.so").read_bytes()).hexdigest()
    (args.output_dir / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    print("See " + str(log_path))
    return 77 if unsupported_metadata else (0 if passed else 1)


if __name__ == "__main__":
    sys.exit(main())
