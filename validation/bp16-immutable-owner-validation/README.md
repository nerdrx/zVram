# Immutable BP16 frame-owner validation

Commit `dba6a62` caches BP16 frame validation on the owning host allocation.
Reuse requires the exact tuple: input pointer, encoded byte count, and raw byte
count. The CPU checker covers mismatched tuple rejection, ownership cleanup,
alignment, quota, and poisoned-owner cases.

The CPU suite passed **12/12** in **4.68 seconds**. Focused GPU suites passed
**8/8** in allocated-host mode (**6.89 seconds**), direct-host mode
(**6.68 seconds**), and imported-host mode (**6.84 seconds**). Focused direct-
and imported-host integrity tests also passed **1/1** each. GPU logs report
exact byte verification, GPU decoding, zero fallback, and no validation
diagnostics. The build and test library were refreshed after the change; hashes
are recorded in `source-binary-sha256.txt`.

This archive validates owner-cache correctness only; it makes no model
performance claim. [CPU focused check](cpu-focused.log), [CPU suite](cpu-12-ctest.log),
[GPU summaries](allocated-host-gpu-ctest.log), [direct-host checks](direct-host-focused.log),
[imported-host checks](import-host-focused.log), and [binary/source hashes](source-binary-sha256.txt).
