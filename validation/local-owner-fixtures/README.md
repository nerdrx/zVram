# Local BP16 owner-tier fixtures and fused-frame research

The local-owner fixture exercises BP16 GPU encoding/restore on the RX 7900 XTX
with a 32 MiB local-owner limit and an 8 MiB shared allocation limit.

- `local-owner-fixture.log` contains successful application byte checks and
  zero GPU fallback, but the helper reported failure because the owners had
  already been freed by the final teardown log. This was a helper false negative,
  fixed in `7826856`.
- `local-owner-native-fixture.log` is a native-backed rerun after that fix and
  ends in `PASS`; application byte checks, observed GPU decoding, and validation
  diagnostics passed.
- `local-owner-fixture-liveheadroom.log` records a later full-helper PASS for
  the local-owner path after `7826856`.
- `local-owner-cpu12-gpu2.log` records the focused 14/14 CPU/GPU CTest suite
  after metadata fix `427d93a` / `e394ea8` (12 CPU checks and two owner GPU
  fixtures; 5.41 s).

The fused-four BP16 shader design from `c3f8850` remains research-only. A later
GPU component run decoded four distinct canonical 32 MiB BP16 frames in one
dispatch for 16 iterations, with exact bytes and zero validation errors. Median
decode time was **4.16712 ms for all four frames** (~1.04 ms/frame). A prior
single-frame component result was ~1.06 ms, but those runs were not a matched
comparison, so no speedup claim follows. The component proves correctness for
that smoke fixture only; it is not production code or model-performance proof.

`original-bytes-sha256.json` records hashes and sizes for the fixture logs and
shader; the patch entry records the uncompressed patch size/hash.
