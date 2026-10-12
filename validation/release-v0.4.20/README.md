# Published and installed v0.4.20

The user authorized publication and installation. Annotated tag `v0.4.20`
points to `d513c139997df5f395d559efdc6b1541281d3b3a` (runtime `1d4938e`).
Release 37989538627 and tag Build 37989538850 succeeded. PR #1 remains open
and unmerged. `release-notes.md` records the published notes.

The actual CI archive and sidecar/SHA256SUMS verify. `payload.json` records
archive SHA256 `98ccfd5b04ed5f9b6c95e9afb033efd32d0bce7cb9f66ad1931d782f4c88d06e`
and library SHA256 `2ca544526750682bb096d0186d67bdb4eaed026b05e7830f85b47712faec307b`.
GDeflate/BP16 CPU and GPU capabilities are present, test hooks are OFF,
all six compile-only API strings are absent, scripts match the tag and the
layer manifest is relative. This is the CI artifact, not the local candidate.

NX Hub had already updated the versioned launcher during refresh; `nx update
zvram` confirmed up to date. No duplicate installation or manual symlink
change occurred. All installed payload files match the verified CI archive.
The launcher resolves to `~/.local/share/zvram/0.4.20/zvram`.
Profiles, logs and model state were not edited; user applications were not stopped.

Fresh health inspection found about 28.5 GiB available RAM, memory PSI avg10
0.13%, background GPU load 74%, no games/models other than the ollama daemon,
and both GPU performance levels auto. One 15-second-bounded installed-package
live-cap check passed with synchronization validation: exactly one 32 MiB
eviction, cap-raise restoration of every byte, zero final resident/cold/cache
bytes, failures and driver live allocations. Raw output is `installed-live-cap.txt`.
The observed resource-free submit maximum (12.077 ms) is a component observation,
not a latency guarantee or game FPS result. No GPU fault reproduction occurred.

Recovery, asynchronous compression, buffer presentation and unlocked recovery
remain off by default. The supplied Steam recovery command is
`zvram --vulkan-recover-local -- %command%`. Unlocked waits additionally require
`ZVRAM_VULKAN_UNLOCKED_RECOVERY_WAIT=1`; zero recovery quiet delay is optional.
Both real idle waits failing ambiguously still retain uncertain resources;
native child cleanup remains an abnormal teardown limitation.
