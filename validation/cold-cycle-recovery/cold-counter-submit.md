# Constant-time cold-state check

`queueCall` uses the maintained `coldLogicalBytes != 0` predicate instead of scanning every tracked allocation. It runs under the existing device/queue locks and retains admission, restore, visibility and error ordering.

The mutation audit covers positive-size lazy initialization, clean-cache reuse, sync/async snapshot commits, successful restores, rollback and both free paths. CPU production-path checks compare the counter against both the exact sum of cold logical sizes and the old any-cold-object predicate, including partial/full restore, allocation refusal and pre/post-commit failure recovery. No valid cold group has zero logical size.

Both normal and compile-hook builds passed the bootstrap test. All 13 normal CPU checks and three bounded GPU pressure-only retention/live-cap sync/live-cap async checks passed. GPU tests preserve all data and clean accounting. This removes a map walk; no game FPS improvement has been measured.

Commands: `ctest --test-dir build -R '^cpu-' --output-on-failure` and `ctest --test-dir build -R '^vulkan-pressure-only-(idle|live-cap-idle|live-cap-async)-regression$' --output-on-failure -j1`.

## Installed v0.4.13

Release workflow 37882853123 succeeded; Hub verified the tarball checksum and installed v0.4.13. Launcher/manager match the tag, both test setters are absent, and codec metadata disables test hooks. The installed async live-cap fixture passed: lowering the cap evicted one 32 MiB chunk, raising it restored every byte, and teardown reached zero resident/cold bytes and errors. See `installed-0.4.13-payload.json` and `installed-0.4.13-live-cap.txt`. A separate tag Build workflow failed the TUI profile fixture (`KeyError: tiny`) after all 19 Python unit tests passed; synchronization investigation is separate from the successful release gate and remains open.

The TUI failure was traced to a fixed 150 ms key-feed schedule. The test now waits up to five seconds for the actual screen/prompt/action output, advancing an output cursor to avoid matching an old occurrence. Existing profile/action assertions are unchanged. Five consecutive agent TUI runs and a root rerun pass; all 19 Python unit tests pass. No production UI changes were needed.
