#!/usr/bin/env python3
"""Bounded CPU-only Zstd restore probe for three slices of the local F16 GGUF."""
import concurrent.futures
import ctypes
import ctypes.util
import gc
import hashlib
import json
import os
import resource
import statistics
import time
from pathlib import Path

MODEL = Path(os.environ.get("ZVRAM_PROBE_MODEL", str(Path(__file__).resolve().parent.parent / "build/third-party/models/internlm2_5-20b-chat-fp16.gguf")))
CHUNK = 32 * 1024 * 1024
REPEATS = 5
LIB = ctypes.CDLL(ctypes.util.find_library("zstd"))
LIB.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
LIB.ZSTD_compressBound.restype = ctypes.c_size_t
LIB.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
LIB.ZSTD_compress.restype = ctypes.c_size_t
LIB.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
LIB.ZSTD_decompress.restype = ctypes.c_size_t
LIB.ZSTD_isError.argtypes = [ctypes.c_size_t]
LIB.ZSTD_isError.restype = ctypes.c_uint


def rss_bytes():
    with open("/proc/self/status", encoding="ascii") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    return 0


def buffer_ptr(buf):
    return ctypes.addressof((ctypes.c_ubyte * len(buf)).from_buffer(buf))


def digest(buf):
    return hashlib.sha256(memoryview(buf)).hexdigest()


def check_zstd(n, expected, operation):
    if LIB.ZSTD_isError(n) or n != expected:
        raise RuntimeError(f"{operation}: Zstd result {n}, expected {expected}")


def decode_ptr(frame, destination):
    return LIB.ZSTD_decompress(destination, CHUNK, frame, len(frame))


def main():
    if MODEL.stat().st_size != 39_725_643_136:
        raise RuntimeError(f"unexpected model size: {MODEL.stat().st_size}")
    total = MODEL.stat().st_size
    offsets = [((total * q // 4) // CHUNK) * CHUNK for q in (1, 2, 3)]
    source_hashes, frames, sizes = [], [], []
    peak = rss_bytes()
    with MODEL.open("rb", buffering=0) as source:
        for offset in offsets:
            source.seek(offset)
            raw = bytearray(source.read(CHUNK))
            if len(raw) != CHUNK:
                raise RuntimeError(f"short read at {offset}")
            source_hashes.append(digest(raw))
            bound = LIB.ZSTD_compressBound(CHUNK)
            encoded = bytearray(bound)
            n = LIB.ZSTD_compress(buffer_ptr(encoded), bound, buffer_ptr(raw), CHUNK, 1)
            if LIB.ZSTD_isError(n):
                raise RuntimeError("ZSTD_compress failed")
            frames.append(bytes(memoryview(encoded)[:n]))
            sizes.append(n)
            del raw, encoded
            peak = max(peak, rss_bytes())

    serial_ms, parallel_copy_ms, parallel_direct_ms = [], [], []
    serial_out = bytearray(CHUNK)
    serial_ptr = buffer_ptr(serial_out)
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        for _ in range(REPEATS):
            serial_elapsed = 0.0
            for i, frame in enumerate(frames):
                t0 = time.perf_counter()
                n = decode_ptr(frame, serial_ptr)
                serial_elapsed += time.perf_counter() - t0
                check_zstd(n, CHUNK, "serial decode")
                if digest(serial_out) != source_hashes[i]:
                    raise RuntimeError("serial decoded bytes differ from source")
            serial_ms.append(serial_elapsed * 1000)

        del serial_out, serial_ptr
        gc.collect()
        parallel_out = [bytearray(CHUNK) for _ in frames]
        copy_staging = bytearray(CHUNK)
        parallel_ptrs = [buffer_ptr(out) for out in parallel_out]
        for _ in range(REPEATS):
            t0 = time.perf_counter()
            futures = [pool.submit(decode_ptr, frame, ptr)
                       for frame, ptr in zip(frames, parallel_ptrs)]
            parallel_sizes = [future.result() for future in futures]
            for out in parallel_out:
                copy_staging[:] = out
            parallel_copy_ms.append((time.perf_counter() - t0) * 1000)
            for i, (n, out) in enumerate(zip(parallel_sizes, parallel_out)):
                check_zstd(n, CHUNK, "parallel decode + copy")
                if digest(out) != source_hashes[i]:
                    raise RuntimeError("parallel decoded bytes differ from source")
                copy_staging[:] = out
                if digest(copy_staging) != source_hashes[i]:
                    raise RuntimeError("parallel staging copy differs from source")

        del parallel_out, copy_staging, parallel_ptrs
        gc.collect()
        direct_staging = bytearray(len(frames) * CHUNK)
        direct_base_ptr = buffer_ptr(direct_staging)
        for _ in range(REPEATS):
            t0 = time.perf_counter()
            futures = [pool.submit(decode_ptr, frame, direct_base_ptr + i * CHUNK)
                       for i, frame in enumerate(frames)]
            direct_sizes = [future.result() for future in futures]
            parallel_direct_ms.append((time.perf_counter() - t0) * 1000)
            for i, n in enumerate(direct_sizes):
                check_zstd(n, CHUNK, "parallel direct staging")
                view = memoryview(direct_staging)[i * CHUNK:(i + 1) * CHUNK]
                if digest(view) != source_hashes[i]:
                    raise RuntimeError("parallel direct-staging bytes differ from source")
            peak = max(peak, rss_bytes())
            del view
        del direct_staging, direct_base_ptr
        gc.collect()

    peak = max(peak, resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024)

    result = {
        "model": str(MODEL), "model_bytes": total, "slice_offsets": offsets,
        "slice_bytes_each": CHUNK, "codec": "Zstd level 1", "workers": 3,
        "repeats": REPEATS, "compressed_bytes_each": sizes,
        "source_sha256": source_hashes,
        "serial_decode_only_ms": serial_ms,
        "serial_decode_only_median_ms": statistics.median(serial_ms),
        "parallel_decode_plus_copy_ms": parallel_copy_ms,
        "parallel_decode_plus_copy_median_ms": statistics.median(parallel_copy_ms),
        "parallel_direct_staging_decode_only_ms": parallel_direct_ms,
        "parallel_direct_staging_median_ms": statistics.median(parallel_direct_ms),
        "serial_to_parallel_copy_speedup": statistics.median(serial_ms) / statistics.median(parallel_copy_ms),
        "serial_to_parallel_direct_speedup": statistics.median(serial_ms) / statistics.median(parallel_direct_ms),
        "peak_resident_bytes": peak, "temporary_ram_bound_bytes": 256 * 1024 * 1024,
        "checks": "SHA-256 per decoded chunk and copy; exact Zstd output size; checks run outside timed intervals",
        "scope": "CPU only; three 32 MiB ranges; no full-model scan or GPU",
    }
    del frames
    out = Path(os.environ.get("ZVRAM_PROBE_OUTPUT", "build/cold_decode_cpu_probe.json"))
    out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
