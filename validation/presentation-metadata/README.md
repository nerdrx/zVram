# Allowlisted presentation metadata

On RX 7900 XTX/RADV, hidden Gamescope X11 tests passed present IDs, incremental regions, their combined chain, and the unchanged base-present regression. Each used three presented frames, synchronization validation, exact pixels and all 32 MiB of buffer bytes; the buffer returned to cold state after each frame. Result JSON includes source-build layer and fixture hashes. These are correctness gates, not game compatibility or FPS evidence. The initial source build has compile-only testing hooks; release builds exclude them. A second run of all three metadata variants using the normal production build also passed; its hashes and logs are in `production-*`. All 13 normal CPU checks passed.

The opt-in buffer-presentation path accepts at most one `VkPresentIdKHR` and one `VkPresentRegionsKHR`, with swapchain counts matching the base present. It forwards the original chain unchanged. Unknown, duplicate, cyclic and mismatched chains keep the conservative restore-and-disable fallback. CPU production-path cases cover these decisions, including nullable metadata payloads.

Initial present-ID/combined hardware attempts failed device creation because the fixture feature2 chain did not explicitly enable supported sparse base features. Only fixture feature negotiation changed; production eligibility was retained. The corrected three metadata tests all passed in 2.99 seconds; the base regression passed separately. Unsupported extensions/features are reported as skips only with the exact clean marker and no timeout, PASS, FAIL or validation diagnostics.

Reproduce: `ctest --test-dir build/async-race -R '^vulkan-graphics-present-metadata-' --output-on-failure -j1`.

## Published package

v0.4.12 release workflow 37882431062 succeeded. Hub checksum verification/install completed; installed launcher and manager match the tag, codec metadata has `test_hooks: false`, and both test setters are absent. All three metadata variants passed against the installed layer, using the source graphics fixture and launcher with its build directory set to the installed package. See `installed-*/result.json` and `installed-0.4.12-payload.json`. The only source launcher difference at testing was help text; runtime arguments were unchanged.
