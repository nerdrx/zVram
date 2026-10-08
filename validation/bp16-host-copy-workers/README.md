# BP16 host-copy worker matrix

All **12/12** 64-iteration component runs copied the same 29,202,816-byte
encoded frame and passed exact-byte validation. Direct coherent host-input
measurements were: 1 worker, 22.90 and 24.27 GB/s; 2 workers, 28.96 and 37.56;
4 workers, 33.32; 8 workers, 37.90. Allocated-host-input measurements were:
1 worker, 60.15 and 24.52 GB/s; 2 workers, 91.87 and 100.03; 4 workers,
108.59; 8 workers, 92.71.

The hot-repeat spread and non-monotonic values show substantial noise. These
are bounded copy-component measurements, not end-to-end decoding or model
performance, and do not justify a production speed claim.

The archived helper binary SHA-256 is recorded in `summary.json` and matches
`zvram-gdeflate-research`. `source.cpp.gz` is the captured C++ source; its hash
is recorded in the summary. The CLI guard regression script is from commit
`b890520` (`runner-source-commit.txt`).

See [summary](summary.json), [captured source](source.cpp.gz),
[runner check](check_host_copy_workers.py), and all 12 per-run logs.
