# Restore submit/wait profiling split

Added two counters under the existing profiling switch: `queue-submit-ns` measures
the downstream restore submission call (including its checked result), and
`fence-wait-ns` measures the subsequent fence wait. Existing `submit-wait-ns`
remains the combined wall time. The split sum can be slightly below the total
because timestamp and counter bookkeeping happens between the measured phases.
A failed submit records submission time without adding a fence wait; failed waits
are timed too. The encoder path and synchronization remain unchanged.

The optional BP16 build compiled and its CPU suite passed **12/12** in 4.62 s.
Read-only review checked success and failure paths. No GPU work was run after
the authorized 08:00 Europe/Berlin cutoff. Hardware counter validation and a
full-model timing run remain pending; this is instrumentation, not a speed gain.
The default installed runtime was not rebuilt for this change. Captured hashes
distinguish the optional diagnostic build from that previously tested default.

A later full-model run recorded 54,558 restores, 0.266484886 s downstream queue
submission and 74.542501048 s fence wait within 74.811437618 s combined host
time. Device counters were 0.26544932 s transfer, 52.6819636 s decode, and
0.51993216 s finish. Fence waiting dominates the measured host interval, but
this does not explain its cause or show a performance gain. See the
[full-model profile](../internlm-bp16-gpu-host-split-cold26-owner26-resident19-lfu-retry/README.md).
