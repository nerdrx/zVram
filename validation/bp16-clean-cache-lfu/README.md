# Opt-in LFU clean-cache correctness

Commit `5fcd4f3` adds LFU as an opt-in clean-snapshot replacement policy.
The default remains `first`; `ZVRAM_VULKAN_CLEAN_CACHE_POLICY=lfu` explicitly
selects LFU. The recorded layer binary hash matches the rebuilt 5fcd4f3
library; the BP16 shader is unchanged.

The CPU suite passed **12/12** in **3.03 seconds**. The focused BP16 GPU suite
passed **8/8** in **6.86 seconds**. In the separate 64 MiB quota fixture, the
resident and cold quotas were each 64 MiB. Three clean-cache readback passes
preserved exact bytes; the complete fixture log records **14** cache-trim
events of **33,554,432 bytes** each. GPU restore fallback count was zero, and
the test reported no validation diagnostics. These are correctness checks,
not a performance measurement.

Earlier cold-40 MiB fixture attempts had invalid pressure configuration and
ended with failed submission checks. They are preserved as diagnostics, not
counted as successful evidence; no code workaround is inferred. See the
[quota](diagnostics/cold40-quota-invalid-pressure.log),
[tight](diagnostics/cold40-tight-invalid-pressure.log),
[bootstrap](diagnostics/cold40-bootstrap-invalid-pressure.log), and
[33 MiB](diagnostics/cold40-quota33-invalid-pressure.log) diagnostic logs.

- [CPU CTest](cpu-ctest.log)
- [Normal focused GPU CTest](gpu-normal-ctest.log)
- [64 MiB quota fixture](gpu-quota64.log)
- [All focused GPU tests](gpu-all-ctest.log)
- [CTest detail and accepted LFU setting](gpu-all-lasttest.log)
- [Build logs and hashes](binary-sha256.txt)
- [Source commit](source-commit.txt)
