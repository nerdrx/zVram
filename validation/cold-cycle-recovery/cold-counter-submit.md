# Constant-time cold-state check

`queueCall` uses the maintained `coldLogicalBytes != 0` predicate instead of scanning every tracked allocation. It runs under the existing device/queue locks and retains admission, restore, visibility and error ordering.

The mutation audit covers positive-size lazy initialization, clean-cache reuse, sync/async snapshot commits, successful restores, rollback and both free paths. CPU production-path checks compare the counter against both the exact sum of cold logical sizes and the old any-cold-object predicate, including partial/full restore, allocation refusal and pre/post-commit failure recovery. No valid cold group has zero logical size.

Both normal and compile-hook builds passed the bootstrap test. All 13 normal CPU checks and three bounded GPU pressure-only retention/live-cap sync/live-cap async checks passed. GPU tests preserve all data and clean accounting. This removes a map walk; no game FPS improvement has been measured.

Commands: `ctest --test-dir build -R '^cpu-' --output-on-failure` and `ctest --test-dir build -R '^vulkan-pressure-only-(idle|live-cap-idle|live-cap-async)-regression$' --output-on-failure -j1`.
