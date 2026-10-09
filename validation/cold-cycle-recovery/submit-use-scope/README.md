# Targeted accepted-submit timestamps

Known accepted submissions now update timestamps directly from their tracked uses rather than scanning every allocation, child and use. A valid child touches only that group and its memory; a wildcard touches all groups of the specified memory. Missing handles, invalid indices, empty use sets and empty-group memories leave timestamps unchanged. Unknown metadata preserves the all-pools fallback. Backing existence is not a filter, and the existing locks/lifetime gates remain intact.

CPU production-path gates verify targeted, wildcard, empty, invalid and unknown cases, including an unrelated nonempty pool and a cold child without backing. Normal and compile-hook bootstrap builds pass; all 13 normal CPU checks pass. Six bounded GPU gates pass: pressure-only retention, sync/async live-cap restore, single/two-queue hot recovery, and hidden Gamescope X11 presentation with combined ID/region metadata. Full-buffer/pixel and cleanup checks remain unchanged.

This removes the nested full-map scan for known submissions. No real-game FPS or frame-time gain has been measured. CTest text archives contain captured completion output with the first passed result noted separately; the presentation directory contains the full raw log and source-build hashes.

The separate TUI CI race fix is verified by successful Build workflows 37883520735 and 37883516442 at commit 53681a0. The original failed tag workflow remains recorded.
