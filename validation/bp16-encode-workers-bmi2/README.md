# BP16 CPU encoder worker benchmark

A 32 MiB F16 model slice (33,554,432 bytes) was encoded and decoded exactly. The serial baseline and BMI2-inline candidate both emitted 29,202,816 bytes. Each row is the median of three CPU-only runs:

| Workers | Baseline median | BMI2-inline median |
|---:|---:|---:|
| 1 | 18.358 ms | 9.611 ms |
| 2 | 10.515 ms | 6.058 ms |
| 4 | 6.259 ms | 3.857 ms |
| 8 | 4.218 ms | 3.131 ms |
| 16 | 3.699 ms | 2.535 ms |
| 32 | 3.582 ms | 2.526 ms |

All 18 worker-count outputs were byte-identical to the serial reference and decoded back to the raw slice. The specialization removes a per-word function call in the BMI2 packer; the portable implementation remains available. This is a CPU component benchmark only. It does not measure GPU decode, layer restore, tokens/s, or end-to-end model speed, and no application defaults changed.

Current focused checks passed **11/11 CPU tests** in 4.60 s and **8/8 GPU tests** in 6.91 s. The archived `LastTest.log.gz` is the CTest last-run record for the 8 GPU cases; the dedicated logs preserve the focused CPU and GPU commands/results. This is not a claim that the full suite passed in this run.

Source commits and binary hashes are in [provenance](provenance.json). The optimization landed in `d453be4`; the benchmark harness/results were recorded with `a7044cc`.

[Baseline measurements](encode-workers-baseline.json), [BMI2-inline measurements](encode-workers-bmi2.json), [CPU CTest log](cpu-ctest.log), [focused GPU CTest log](gpu-ctest.log), [CTest last-run log](LastTest.log.gz).
