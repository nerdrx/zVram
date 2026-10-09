# Async pressure recheck regression

Tested 2026-10-09 on RX 7900 XTX / RADV NAVI31 with no games/models running, GPU idle, and zero memory PSI. Fixtures use a 96 MiB pool and 32 MiB staging; layer helpers are additional.

The test-only build enables `ZVRAM_TEST_ASYNC_HOOK` in `build/async-race`. Its callback runs after the worker releases the device and submission locks, before encoding. One case makes an ordinary Vulkan readback of an already cold range, forcing synchronous admission eviction while the async token is alive. The other only raises the cap during encoding. Both verify every byte of all three ranges and clean accounting after recovery.

Both initially failed. A final pressure check used the ordinary rate-limited live-control poll, so it missed the cap increase until after committing an unnecessary snapshot. The invalid-token early return could also leave the new cap unread before another worker pass.

The fix force-polls live control after encoding and checks current native budget/cap before token validation. A cap increase that removes pressure now discards the candidate; a synchronous admission eviction still invalidates the token. Legacy idle snapshots retain their policy. Both hardware cases passed after the fix (0.83 seconds total), with zero validation or snapshot errors.

The test callback/setter is absent from normal builds. Capability metadata records test-hook builds, and the packaging script refuses them. No test-hook build is installable through the normal package path.

This proves the bounded overlap and cap-increase cases, not general game compatibility or FPS improvement. Async compression remains optional. Warm GTT promotion is a separate prototype and is not established by these checks.
