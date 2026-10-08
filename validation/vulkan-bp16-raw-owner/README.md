# BP16 raw-host-input regression validation

The latest selected full CTest run passed **129/129** in 100.67 seconds, including
the synthetic/native raw-host fixtures and the new owner-budget and cold-budget
negative-fallback cases. The earlier **127/127** result is preserved in
`full-raw-owner-selected-build.log`; the latest run is in
`full-raw-owner-negative-selected-build.log`. The current runtime library hash is `04c6d673e812e7ebee1922e6e4b8fe43580bb663a44ed39b67d14788b2bf87d2`.
The exact dirty-source patch and provenance record from the paired build are
included. The harness and CMake registration are archived with their hashes.

An earlier focused run failed **2/2** because the harness incorrectly required
the *compressed allocated-host owner* counter to be nonzero. Raw BP16 input is
accounted by a separate raw-host-input counter, so valid raw-path runs leave
that compressed-owner counter at zero. After correcting the assertion, the
focused synthetic/native tests passed **2/2** (`raw-owner-fixtures-fixed.log`).
Both full-suite runs include these fixtures; the 129-test suite also covers the
new budget-negative cases.

The earlier `raw-owner-fixture-first.log` passed its byte gate but recorded zero
raw-host-input allocations, so it did not exercise the feature and is not an
accepted feature-validation result. The follow-up
`raw-owner-fixture-pipeline-fixed.log` shows one 32 MiB raw-host-input allocation
and successful fixture bytes; it is diagnostic evidence, not a model result.

This archive records correctness and regression coverage, not model throughput.
A separate four-run same-binary Qwen 27B Q4 comparison is archived at
[validation/qwen27-raw-owner-pair](../qwen27-raw-owner-pair/README.md). Neither
archive establishes general application performance or full-model fit.

`SHA256SUMS` covers every archive file except itself. Verify with
`sha256sum -c SHA256SUMS` from this directory.
