# Local-owner metadata validation fixture

A current-runtime tiny GPU fixture passed application byte checks, observed local-owner GPU decoding, zero restore fallback, and zero validation diagnostics. It recorded 13 GPU encode calls over 436,207,616 raw bytes with 44,635,767 ns host encode time. It used only a 32 MiB raw cap and a 64 MiB local-owner cap within a 96 MiB combined cap. This is a component/fixture result, not model throughput.

An older [fixture](../local-owner-fixtures/README.md) took about 2.95 seconds for those encode calls before CPU metadata validation stopped reading the BAR-mapped owner. Hardware clocks and background activity differed, so these observations are not a controlled before/after speed claim.

An isolated before-control library was compiled successfully from current sources with exactly one validation-call change, retained in `before-control.patch`. The copied layer source matches the current source hash. The installed runtime was not replaced. A planned before/current/before/current GPU pair was not launched because the GPU stayed busy through the preflight window. No paired timings are available.

`command.json` and `env.json` reproduce the completed current fixture. `before-command.json` selects the prepared isolated build directory; reconstruct that build from the source commit and patch before use. `provenance.json` records commands and source/library hashes. All source paths are relative to the repository root.
