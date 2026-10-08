# Final lean clean-first runtime checks

After removing the unsuccessful fence-polling prototype, rebuilt both the default installed command runtime and optional BP16 build. Runtime hashes and source commit are recorded in `runtime-sha256.json`.

- Full optional-build suite: **121/121**, 85.17 seconds, with clean-first eviction requested.
- Default-build CPU suite: **12/12**, 4.65 seconds.
- Focused BP16 GPU suite: **8/8**, GPU encoder, allocated-host input, profiling, and clean-first requested together; full helper output retained.

Graphics checks render offscreen; no presentation window was requested. These checks validate fixtures, not game performance or universal application compatibility. Both full-model clean-first runs remain the performance evidence. Discarded polling and 20 GiB profile attempts are archived separately.
