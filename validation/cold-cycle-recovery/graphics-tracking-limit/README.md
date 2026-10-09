# Narrow graphics tracking limit

A validation-only 32 MiB graphics probe used a 4 MiB storage descriptor and pixel-only measured readbacks. Full-buffer integrity checks ran before and after measured frames. The strict 4 MiB resident / 28 MiB cold check failed: `vkCmdBeginRenderPass` is unknown to the command tracker, so conservative restore materializes all eight chunks. Initial buffer verification passed and teardown reported zero resident/cold bytes and failures. [Original strict failure](narrow-before-tracking.txt).

Static review found further conservative gates: direct draw commands are unknown and graphics pipelines are not registered in logical shader tracking. Allowing native-image render-pass begin/end alone would not fix draws. Draws must not be blindly whitelisted: shader resources, vertex/index accesses, device addresses and extension state need coverage. No production graphics guard was relaxed.

The registered `vulkan-graphics-narrow-descriptor-conservative-fallback` regression explicitly tests this limitation. Its first draw must restore all eight 4 MiB chunks; the next five draws must retain all 32 MiB with no additional restores. Each frame verifies exact pixels, and initial/final integrity checks verify every buffer byte. The strict narrow mode remains unregistered and retains its failing selective-residency assertion for future implementation work.

## Verified results

- [Hook-build fallback](conservative-fallback-ctest.txt) passed.
- [Production fallback and unchanged full-buffer regression](production-fallback-and-full-check-ctest.txt) passed 2/2 with validation enabled and clean teardown.
- [Installed v0.4.14 fallback](installed-0.4.14-fallback.txt) passed six frames, exact eight restores on the first draw and zero additional restores on the next five.

In the installed-layer observation, the first `vkQueueSubmit` call took 8,516 us; five warm calls had median 13 us and maximum 22 us. Production-build observation: first 9,315 us, warm median 13 us / maximum 21 us. These are tiny offscreen fixture component timings, not game FPS or a general performance gain. Full-buffer startup/final readbacks are excluded from submission timing.

Existing full-buffer graphics correctness checks remain valid, but they do not establish selective graphics residency. General selective draw tracking remains unfinished. No narrow working-set success is claimed.
