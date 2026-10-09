# Resident budget scan CPU baseline

Run `python3 validation/budget-scan-profile/profile.py`. It extracts the current
`residentAdmissionLimit` body from `layer.cpp`, compiles it with `-O2`, and
benchmarks 100 calls per sample over one fake pool. No Vulkan device or backing
allocation is used. The stub returns a fixed budget; each result is checked
against the expected hard-cap calculation.

2026-10-09 run: 3,072 children measured 0.875–0.953 us/call; 100,000 children
measured 27.98–29.26 us/call. The 100,000-child fixture uses about 2 MB of
vector payload. This isolates scan cost; it does not measure lock contention,
the real worker cadence, driver budget-query latency, or frame rate.

Independent root rerun in `root-result.txt`: 0.874–1.129 us/call at 3,072
children and 28.666–30.992 us/call at 100,000. All expected cap calculations
passed. No production cache was added: existing generations do not cover every
backing insertion, removal, or heap/type replacement, and this isolated scan
does not establish a gaming bottleneck.
