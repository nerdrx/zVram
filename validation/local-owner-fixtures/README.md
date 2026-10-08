# Local BP16 owner-tier fixtures and fused-frame research

The local-owner fixture exercises BP16 GPU encoding/restore on the RX 7900 XTX
with a 32 MiB local-owner limit and an 8 MiB shared allocation limit.

- `local-owner-fixture.log` contains successful application byte checks and
  zero GPU fallback, but the helper reported failure because the owners had
  already been freed by the final teardown log. This was a helper false negative,
  fixed in `7826856`.
- `local-owner-native-fixture.log` is the rerun after that fix and ends in
  `PASS`; application byte checks, observed GPU decoding, and validation
  diagnostics passed.

The archive includes the fused-four BP16 design from commit `c3f8850` as
research-only shader source and a compressed patch for that commit. It is not
production code and has not been GPU-tested. No performance claim is made.

`original-bytes-sha256.json` records hashes and sizes for the fixture logs and
shader; the patch entry records the uncompressed patch size/hash.
