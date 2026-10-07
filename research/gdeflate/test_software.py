#!/usr/bin/env python3
"""Run bounded GDeflate correctness fixtures on an explicitly selected lavapipe ICD."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
RESEARCH = ROOT / "research" / "gdeflate"
FIXTURES = RESEARCH / "fixtures"
MAX_RAW = 32 * 1024 * 1024
MAX_ENCODED = 64 * 1024 * 1024
MANIFEST = RESEARCH / "modules.json"
HASHED_INPUTS = (
    "research/gdeflate/GDeflate-bounded.hlsl",
    "research/gdeflate/tilestream.hlsl",
    "research/gdeflate/GDeflate-bounded-subgroup8.spv",
    "research/gdeflate/fixtures/synthetic-64k.gdeflate",
    "research/gdeflate/fixtures/synthetic-multi-tail.gdeflate",
)
FP16_ENCODED = ROOT / "build/third-party/gdeflate-research/internlm-f16-32m-offset64m.gdeflate"
FP16_RAW = ROOT / "build/third-party/gdeflate-research/internlm-f16-32m-offset64m.raw"
FP16_HASHES = (
    "build/third-party/gdeflate-research/internlm-f16-32m-offset64m.gdeflate",
    "build/third-party/gdeflate-research/internlm-f16-32m-offset64m.raw",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def repo_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path.resolve())


def manifest_hashes(value):
    found = {}

    def visit(item):
        if isinstance(item, dict):
            path = item.get("path", item.get("file"))
            digest = item.get("sha256", item.get("sha256sum"))
            if isinstance(path, str) and isinstance(digest, str):
                found[path] = digest.lower()
            for key, child in item.items():
                if isinstance(key, str) and isinstance(child, str) and len(child) == 64:
                    if all(c in "0123456789abcdefABCDEF" for c in child):
                        found[key] = child.lower()
                elif isinstance(key, str) and isinstance(child, dict):
                    digest = child.get("sha256")
                    if isinstance(digest, str) and len(digest) == 64:
                        found[key] = digest.lower()
                visit(child)
        elif isinstance(item, list):
            for child in item:
                visit(child)

    visit(value)
    return found


def verify_manifest(extra: tuple[str, ...] = ()) -> dict[str, str]:
    try:
        hashes = manifest_hashes(json.loads(MANIFEST.read_text()))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read hash manifest {MANIFEST}: {error}") from error
    required = (*HASHED_INPUTS, *extra)
    for relative in required:
        expected = hashes.get(relative)
        path = ROOT / relative
        if expected is None:
            raise RuntimeError(f"{relative} is missing from {MANIFEST}")
        actual = sha256(path)
        if actual != expected:
            raise RuntimeError(f"hash mismatch for {relative}: expected {expected}, got {actual}")
    return {name: hashes[name] for name in required}


def expected_pattern(path: Path, size: int) -> None:
    if not 0 < size <= MAX_RAW:
        raise ValueError(f"raw fixture size must be 1..{MAX_RAW}")
    with path.open("wb") as out:
        block_size = 1024 * 1024
        for base in range(0, size, block_size):
            n = min(block_size, size - base)
            block = bytes(((i % 251) * 13 + (i // 65536) * 29) & 0xff
                          for i in range(base, base + n))
            out.write(block)


def pick_icd(requested: str | None) -> Path:
    if requested:
        path = Path(requested).expanduser().resolve(strict=True)
    else:
        choices = sorted(Path("/usr/share/vulkan/icd.d").glob("lvp_icd*.json"))
        if len(choices) != 1:
            raise RuntimeError(f"expected exactly one /usr/share/vulkan/icd.d/lvp_icd*.json, found {len(choices)}")
        path = choices[0].resolve(strict=True)
    try:
        manifest = json.loads(path.read_text())
        library = str(manifest["ICD"]["library_path"])
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise RuntimeError(f"invalid ICD manifest {path}: {error}") from error
    if "libvulkan_lvp" not in library:
        raise RuntimeError(f"refusing non-lavapipe ICD {path} (library={library})")
    return path


def run_case(binary: Path, shader: Path, encoded: Path, raw: Path,
             output_dir: Path, env: dict[str, str], name: str, expected_bytes: int) -> dict:
    if encoded.stat().st_size > MAX_ENCODED or raw.stat().st_size != expected_bytes or expected_bytes > MAX_RAW:
        raise RuntimeError(f"fixture bounds invalid for {name}")
    command = [str(binary), "--software-smoke", str(shader), str(encoded), str(raw)]
    log_path = output_dir / f"{name}.log"
    try:
        result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=45, check=False)
        log = result.stdout
    except subprocess.TimeoutExpired as error:
        log = error.stdout or ""
        if isinstance(log, bytes):
            log = log.decode("utf-8", "replace")
        log += "\nFAIL: software Vulkan test timed out after 45 seconds\n"
        log_path.write_text(log)
        raise RuntimeError(f"{name} timed out; see {log_path}") from error
    log_path.write_text(log)
    required = (
        "execution=CPU-software", "type=4", "validation=on",
        f"PASS: decoded {expected_bytes} exact bytes", "validation-errors=0",
        "validation-vuids=0",
    )
    missing = [marker for marker in required if marker not in log]
    if result.returncode != 0 or missing or "VUID-" in log or "Vulkan validation error:" in log:
        raise RuntimeError(f"{name} failed (exit={result.returncode}, missing={missing}); see {log_path}")
    return {"name": name, "encoded_bytes": encoded.stat().st_size,
            "decoded_bytes": expected_bytes, "log": repo_path(log_path),
            "returncode": result.returncode}


def extract_first_tile(encoded: Path, output: Path) -> None:
    if not 0 < encoded.stat().st_size <= MAX_ENCODED:
        raise RuntimeError("FP16 GDeflate stream exceeds the 64 MiB read bound")
    data = encoded.read_bytes()
    if len(data) < 12 or data[0:2] != b"\x04\xfb":
        raise RuntimeError("FP16 GDeflate stream has an invalid fixed header")
    count = struct.unpack_from("<H", data, 2)[0]
    if not count or 8 + 4 * count > len(data):
        raise RuntimeError("FP16 GDeflate stream has a truncated tile table")
    table = struct.unpack_from(f"<{count}I", data, 8)
    payload = 8 + 4 * count
    first_end = table[1] if count > 1 else table[0]
    if first_end < 4 or (first_end & 3) or first_end > len(data) - payload:
        raise RuntimeError("FP16 GDeflate first tile has an invalid payload range")
    output.write_bytes(struct.pack("<BBHI", 4, 0xfb, 1, 1) +
                       struct.pack("<I", first_end) + data[payload:payload + first_end])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="compiled research Vulkan host binary")
    parser.add_argument("--icd", help="lavapipe ICD JSON; default requires exactly one system lvp_icd*.json")
    parser.add_argument("--output-dir", default="build/gdeflate-software-tests")
    args = parser.parse_args()

    binary = Path(args.binary).expanduser().resolve(strict=True)
    shader = RESEARCH / "GDeflate-bounded-subgroup8.spv"
    output_dir = (ROOT / args.output_dir).resolve()
    payload_dir = ROOT / "build" / "gdeflate-software-tests" / "payloads"
    output_dir.mkdir(parents=True, exist_ok=True)
    payload_dir.mkdir(parents=True, exist_ok=True)
    has_model = FP16_ENCODED.is_file() and FP16_RAW.is_file()
    if FP16_ENCODED.exists() != FP16_RAW.exists():
        raise RuntimeError("FP16 stream and raw fixture must both be present")
    hashes = verify_manifest(FP16_HASHES if has_model else ())
    icd = pick_icd(args.icd)

    env = os.environ.copy()
    for key in ("VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES", "VK_INSTANCE_LAYERS",
                "VK_LAYER_PATH", "VK_ADD_LAYER_PATH"):
        env.pop(key, None)
    env["VK_DRIVER_FILES"] = str(icd)
    env["VK_LOADER_LAYERS_DISABLE"] = "~implicit~"
    env["LP_NUM_THREADS"] = "2"

    cases = []
    fixtures = (
        ("synthetic-64k", FIXTURES / "synthetic-64k.gdeflate", 65536),
        ("synthetic-multi-tail", FIXTURES / "synthetic-multi-tail.gdeflate", 2 * 65536 + 123),
    )
    for name, encoded, raw_bytes in fixtures:
        raw = payload_dir / f"{name}.raw"
        expected_pattern(raw, raw_bytes)
        cases.append(run_case(binary, shader, encoded, raw, output_dir, env, name, raw_bytes))

    model_status = "unavailable"
    if has_model:
        first_stream = payload_dir / "fp16-first-tile.gdeflate"
        first_raw = payload_dir / "fp16-first-tile.raw"
        extract_first_tile(FP16_ENCODED, first_stream)
        with FP16_RAW.open("rb") as source, first_raw.open("wb") as target:
            target.write(source.read(65536))
        cases.append(run_case(binary, shader, first_stream, first_raw, output_dir,
                              env, "fp16-first-tile", 65536))
        cases.append(run_case(binary, shader, FP16_ENCODED, FP16_RAW, output_dir,
                              env, "fp16-full-32m", MAX_RAW))
        model_status = "verified and tested"

    report = {"icd": repo_path(icd), "binary": repo_path(binary),
              "module": repo_path(shader),
              "verified_sha256": hashes, "fp16_fixture_status": model_status,
              "cases": cases, "status": "PASS"}
    report_path = output_dir / "results.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
