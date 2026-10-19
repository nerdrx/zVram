# Responsive residency work

Started 2026-10-19 14:18:47 UTC. Deadline 2026-10-20 00:18:47 UTC (02:18:47 Europe/Berlin).
Branch: feat/reactive-residency, based on main 7027446 / v0.4.28.
User requests ten hours improving hot-buffer promotion and cold-buffer eviction responsiveness; standing tested-release authorization applies. Preserve in-flight work, exact accounting, memory headroom and all unrelated user apps/settings/dirty evidence.

## Current work / ownership
- Root: policy implementation, integration, tests and any bounded GPU fixtures.
- active_queue_markers: read-only audit timestamp semantics and recovery ranking.
- auto_safety_review: read-only pressure eviction/cadence/churn audit.
- auto_docs_update: tests/resident_bootstrap_check.cpp ONLY, recent-first recovery regression and guard exclusions.
- No owned GPU jobs or app processes. Root changed recovery candidate ranking; agents completed initial audits.

## Verified starting observations
- Recovery selector in src/layer.cpp selects oldest eligible lastUse, contrary to hot-first goal.
- High recovery priority removes quiet-age requirement but not active-reference or headroom guards.
- Worker scans pressure policy on <=10ms timed cadence; cold pressure eviction also uses oldest lastUse.
- lastUse is conservative: unknown accesses mark all groups, exact known references mark selected children.
- Native images and kernel-transparent GTT migration remain outside managed buffer recovery scope.

## Next
Recent-first ranking committed as 5ee7321. Final cadence review passed (commit/failure semantics verified); next prioritize a bounded end-to-end worker CPU fixture and fresh-health small GPU checks before release. Audit freshness/hysteresis before any hot-for-cold exchange. Do not blindly evict local buffers to manufacture room. No release/install yet.

## 14:24 UTC — recent-first recovery
- New CPU regression failed against original source with “cold-cycle selection did not prefer the most recently used eligible child”; full output in ranking-before.log.
- Root changed selectColdCycleCandidate to choose greatest lastUse, retaining busy/quiet/budget/local-type exclusions. Pressure eviction remains oldest-first.
- Built normal build/layout bootstrap harness and production layer successfully. Focused CPU/submission suite: 18/18 PASS, 6.48s (ranking-after-cpu.log). New regression checks quiet-age, incompatible pool type, recent-first preference and busy fallback.
- This proves selection policy, not game performance or device residency. No GPU tests executed yet.
- Follow-up read-only audits active: active_queue_markers checks frequency/hysteresis options; auto_safety_review checks safe exchange at full cap and accounting. No production change for either yet.
- Worker currently waits up to 10ms per pass; removing sleep unconditionally could monopolize locks/spin on failure, so not done.

## 14:32 UTC — bounded backlog cadence
- Pressure-mode worker now uses 1ms wait after confirmed freeze-count progress or successful recovery commit; otherwise retains up-to-10ms cadence. A 1ms minimum prevents zero-idle busy spinning. Early/error paths reset progress. All existing gates retained.
- Review caught aggregate before/after residency counts could be distorted during unlocked async work; replaced those with committed-freeze count and recovery success result before final rebuild.
- Root rebuilt normal production layer and bootstrap harness from final source. CPU/submission suite 18/18 PASS in 6.50s; cadence-cpu.log. New arithmetic boundary test covers idle0/5/10/UINT64_MAX and progress/no-progress. This is not a full worker scheduling/latency proof; integration check remains next.
- README corrected stale oldest-first and disabled-default recovery wording, documented scheduling intervals without claiming migration latency.
- No per-child completed-use frequency counter exists. writeEpoch/successfulFreezes/submission generation are not frequency metrics. MRU alone may refill with dormant data; consider a carefully defined freshness horizon, no arbitrary gate added yet.
- Full-local-cap exchange needs special caution: residentBytes includes managed nonlocal bytes, liveLocal includes native allocations. A proposed freeze+promote exchange would be two commits with possible failure between them; not implemented. Audit evidence does not yet justify broad new policy.
- No GPU work or owned test/build jobs remain after completion. Agents read-only except completed test edits. Historical dirty files preserved.

## 14:36 UTC — bounded production GPU check
- Fresh host: memory PSI avg10/60/300 all zero, MemAvailable 17,820,416 kB; RX7900XTX busy30%, VRAM23,092,690,944/25,753,026,560 bytes. Existing llama model server active, untouched. No games seen. Ran only existing small headless async live-cap fixture, no presentation/model/stress workload.
- Normal production bee698c: vulkan-pressure-only-live-cap-async-regression PASS1.48s, all restored bytes matched, one32MiB freeze/restore, final resident/cold/cache/failures and driver live-local/nonlocal zero. Full log cadence-gpu.log. External20s timeout, no owned jobs left. Postcheck PSI averages still zero.
- Worker CPU integration fixture in progress with auto_docs_update. Newly confirmed timestamp metadata bug: sync/async freeze replacement defaults lastUse to snapshot construction instead of preserving current application-use time. Tests first, then two explicit metadata copies; no production patch yet.

## Review tracking
- Draft PR14 https://github.com/nerdrx/zVram/pull/14 opened and attached; branch pushed through bee698c. CI may be pending; check once after next commit push.
- Standing ten-hour work continues after user explicitly reiterated to keep going. Current root work: timestamp-preservation bug; auto_docs_update owns tests file (worker integration plus timestamp regressions). Other agents read-only audits complete.
- Do not adopt proposed use-since-promotion latch without concrete sequence proof: cold groups are excluded from warm recovery until restored, so pressure freeze alone cannot immediately re-promote them. Bind/unknown conservative restoration without actual use may be a valid case, but must test first. No latch/frequency fields added.

- PR14 initial CI runs 39270714532 and39270747117 pending at14:39UTC; no repeated polling needed until next change/checkpoint.

## Timestamp regression work
- Successful synchronous freeze reproducibly resets lastUse: timestamp-before.log records expected FAIL after successful snapshot.
- Two source copies preserve group.lastUse immediately before sync/async candidate replacement; async uses current metadata after reacquiring locks, retaining reads accepted during encoding.
- Final normal bootstrap PASS (timestamp-normal.log); normal layer rebuilt.
- Initial build/reactivity-hooks bootstrap PASS (timestamp-hook.log) was NOT hook coverage: inspection of flags.make showed no macro, because CMake option enabled only layer target. Added conditional bootstrap target definition and rebuilding genuine hook-enabled harness. Do not count initial hook-directory PASS as concurrent-read proof.

- Genuine hook-enabled bootstrap (`CXX_DEFINES = -DZVRAM_TEST_ASYNC_HOOK=1`) PASS0.01s, including read timestamp update during unlocked encode; timestamp-hook-enabled.log. Production hooks stay OFF. No GPU needed for metadata copy change.

## 14:45 UTC — end-to-end CPU worker
- auto_docs_update added actual snapshotWorkerLoop mocked-driver fixture, not just delay arithmetic: three resident1MiB children, oldest marked busy, two eligible siblings drained to1MiB cap; asserts busy backing/use time preserved and stop notification joins. No narrow wall-clock assertions. Hook bootstrap PASS0.02s, worker-hook.log; normal build in progress.
- PR14 latest9e5d6ba CI39271420902/39271423811 pending at14:43UTC. Avoid repeated polling until next pushed checkpoint.
- Next review-only frequency design with auto_safety_review: bounded decaying known-submit heat, distinct from recency; unknown accesses must not boost all counters. No fields/policy added yet. Need behavioral tests demonstrating one-off access vs repeatedly used child, aging out idle heat, dedup, and guard preservation before adopting.

- Normal worker bootstrap also PASS0.02s (worker-normal.log). All owned build/test processes complete.

## Frequency prototype in progress
- Root uncommitted source now adds recovery-only known-submit heat (cap16, one-second decay, zeroafter5s) and ranks effective heat then recency. Eviction unchanged LRU. Existing unique submission use-list is reused; no extra per-group generation counter. Unknown/empty submissions do not add heat. All eligibility guards unchanged.
- Fractional decay remainder retained rather than resetting timer on every hit (prevents 900ms hits accumulating without decay). Fields preserved through sync/async snapshot replacements using current post-lock state.
- auto_docs_update owns tests/resident_bootstrap_check.cpp and builds normal/hook CPU checks when ready; auto_safety_review reviews root source read-only. No GPU work for this prototype yet. Do not claim tested/released until their results plus root review. Previous committed8550182 worker integration passed normal/hook; current source differs.

## Frequency policy verified on CPU
- Root authoritative build/layout bootstrap + production layer rebuilt from final source. 18/18 CPU/submission checks PASS6.34s (frequency-final-cpu.log). Agent also ran normal build/ and genuine hook-enabled build/reactivity-hooks; frequency-normal.log/frequency-hook-enabled.log. Do not substitute build/ evidence for root current-build check.
- Behavioral tests prove repeated known uses outrank a newer one-off use, idle heat fades, busy candidates remain excluded, cap16 and 999ms/1000ms/5s decay boundaries, 900ms cadence does not accumulate without decay, command tracker deduplicates repeated ranges, unknown/knownempty do not boost heat, sync/async/current-read metadata survives freeze. Worker still uses oldest-first eviction regardless of heat.
- Initial duplicate-range test omitted allocateCommands; expected tracker rejected unregistered command. Preserved frequency-initial-fixture-failure.log, corrected fixture before passing. No production bug inferred from that failure.
- Source review passed: no arithmetic/lifecycle blocker. Failed-submit frequency increment gating reviewed in queueCall (only downstream success), not separately integration-fault-injected.
- No latest-frequency GPU check yet; earlier cadence GPU pass is not proof of latest policy performance. Next: small fresh-health recovery integrity check, CI once exact pushed head, then review whether additional policy changes are justified. Do not add speculative heat-based eviction or arbitrary full-cap exchange.
- All owned jobs finished; agents holding; unrelated historical files preserved.

## 14:55 UTC — latest-policy GPU proof / release preparation
- b74ac7f hook layer + auto fixture rebuilt from current source in build/reactivity-hooks. Production build/layout hooksOFF and all6 test API strings absent independently verified.
- Fresh health frequency-gpu-health.json: RAM19,611,136kB available; PSI avg10/60=0, avg300=.02; GPU28%, ~2.77GB VRAMfree, two existing llama processes +ollama untouched, no games. Small headless native-recovery-BDA fixture PASS1.14s, fullbytes; 4MiB direct recovery, all final resident/cold/cache/failures and native driver live-local/nonlocal0. frequency-gpu-bda.log. Outer20s limit, no owned jobs remain. PSI avg10/60 still0 after.
- b74ac7f CI39273003597/39273011439 pending at14:53; next sourceproof/version commit supersedes them. Avoid repeated polling.
- Preparing verified checkpoint v0.4.29 under standing release authorization: VERSION bumped, accurate release-notes.md. No merge/tag/release/install yet. After push, require exact final-headCI success, then merge PR14 with match-head; tag actualmainmerge, verify Release+Build/publicarchive SHA/payload/hooksoff and sixAPIabsence, inspect install symlink before versioned publicpayload install. Do not call local artifacts published.
- Ten-hour work continues after this checkpoint; no arbitrary extra patches needed. Existing userapps/settings untouched.

- Release preparation package tests:5PASS0.180s.

## 15:04 UTC — merged/tagged; publication in progress
- Final099f8fb CI39273378286 and39273369361 bothSUCCESS. PR14 merged with exact-head match to main7894955a7ae710db78292eddb6ee9c33cafb837d; v0.4.29 tag pushed at that merge.
- Release39274631502, tagBuild39274631578, mainBuild39274602495 in progress (pages39274601883 too). No publicartifact verification or install yet; current installed symlink still ../share/zvram/0.4.28/zvram, 0.4.29 versioneddir absent.
- Next wait for CI, downloadpublicarchive, verify checksum/tagpayload/hooksOFF6APIsabsence/codecmanifest; update release notes from release-notes.md; install exact publicpayload into newversioneddir atomically, preserveoldversions/userapps. Root-only one small packagedlivecap after freshhealth ifsafe. No local owned jobs currently.

## 15:08 UTC — v0.4.29 published and installed
- Release39274631502, tagBuild39274631578, mainBuild39274602495 andpages39274601883 SUCCESS. Publicarchive SHA279e4d4e8cd383be387bb8597ad899bcf957edc4a1072acce882c40dc5249206; publiclib SHA45a1fe8597df3fb3434d8b8a3d51a0ab63be81a25a8390bd05079a66d5f728e5. published-payload.json verifies allchecksumassets, tagscripts/docs/assets, allcodecstrue/testhooksfalse, relative manifest andsixAPIsabsent. Accurate release notes uploaded.
- Inspected oldsymlink0.4.28 and absent0.4.29dir; copied exact verified publicpayload to ~/.local/share/zvram/0.4.29, bytecompared allfiles, atomically changed launcher. Installed --version0.4.29. Preservedoldversions, apps/profiles/settings.
- Fresh packaged-health.json: ~19GiB available, PSIavg10=0 (avg60=.16/.15,avg300=.35/.32), GPU25%,2.90GBfree; modelserversleftuntouched/no games. One bounded installed-public-artifact async live-cap fixture PASSfullbytes,1freeze/1restore, allfinal resident/cold/cache/failures/driverlive0 (packaged-live-cap.log). Measured maxsubmit10.436ms is not a latencyguarantee. No further unchanged hardware tests warranted.
- Read-only mixed-budget audit found documented intentional conservatism in docs/VALIDATION.md: heap-derived target applies to total tracked backing, may over-evict nonlocal fallback. Do not change only worker predicate; coordinated alternativecontract would be needed. No concretebug found.
- Allownedlocaljobscomplete/agentsholding. Runtimecheckpointshipped; ten-hour window stillactive until00:18:47UTC. Continue concrete CPUaudits/newissues, preserveevidence, do not inventpatches or claim gameimprovement. Current branchfeat/reactive-residency remains at099f8fb (merged PR14); origin/main+tag7894955. Create/rebase appropriate followupbranch only for justified newchanges.

## 15:17 UTC — bounded pressure retry audit
- Root inspecting pressure candidate retries; auto_safety_review owns read-only source audit and auto_docs_update owns read-only deterministic CPU fixture design. No edits/build/GPU authorized to agents in this pass.
- Hypothesis only: global submission generation can re-enable an oldest cold-budget-refused candidate after unrelated submissions, potentially delaying another candidate that fits. Must reproduce before changing retry semantics; no confirmed bug or new runtime patch yet.
- v0.4.29 remains shipped/installed. No owned test processes started; historical dirty evidence preserved.

## 15:26 UTC — pressure retry fix under test
- Branch fix/pressure-budget-retry created from origin/main7894955; all existing dirt retained. Root extracted unchanged pressure selector into selectPressureCandidate so a deterministic regression can exercise production selection.
- Both read-only audits confirmed reachable starvation: unrelated accepted submissions invalidate the oldest child's quota refusal, so it can repeatedly beat a smaller fitting sibling. auto_docs_update owns test source/build-before failure proof; root owns production source.
- Planned fix: invalidate per-child quota refusal on accepted writes (unknown writes invalidate all), retain coldBudgetGeneration for newly freed quota. Avoid using writeEpoch as refusal token: it only advances in async mode and saturates. Existing activity wake remains global. No behavioral patch yet; no GPU work.

## 15:31 UTC — reproduced retry starvation; patch pending validation
- Correctly registered deterministic CPU regression failed on original retry behavior: actual 1MiB snapshot exceeds512KiB quota, unrelated generation increment makes it win over a256KiB candidate. budget-retry-before.log records expected failure. Initial unregistered PASS preserved in budget-retry-initial-unregistered.log; corrected fixture explicitly asserts oldest/younger ordering.
- Root removed global submission generation from cold quota refusal validity. Accepted writes now clear refusal only for touched children; unknown writes clear all. Cold-budget generation still invalidates refusals when quota changes; async token epochs remain conditional on async mode. Selector extraction preserves LRU/busy checks.
- auto_docs_update owns regression guards and normal/hook bootstrap builds; auto_safety_review reviews production diff. Do not ship until final tests/review complete. No GPU jobs started.
- Review found no semantic blocker, but noted extra submit parsing for sync-only devices. Added conservative Device budgetRefusalsSeen latch, set by both actual quota refusal paths, so sync-only devices skip this parsing until their first refusal. No per-submit full-child scan or counter bookkeeping. Agent tests must set latch when manually modeling a refusal; actual-refusal regression establishes it through production. Agent now completing normal/hook CPU checks.

## 15:36 UTC — normal regression passed
- Final normal bootstrap passes budget-retry-normal.log: actual quota refusal, smaller candidate progresses despite unrelated submission generation, quota change permits retry, read/empty/unrelated writes preserve block, own/unknown writes invalidate correctly, sync/async/saturated epochs covered.
- Fixture required exact uint32_t, const VkSubmitInfo*, VkFence types to select typed queueMemoryAccess rather than generic unknown fallback; initial fixture failures are not production failures. Hook bootstrap currently building under auto_docs_update; root waits before production build/full CPU suite. Final source review found no blocker.
- README now states current per-child refusal invalidation; historical VALIDATION measurements left historical. No GPU/release/install in this pass.

## 15:39 UTC — retry fix validated, preparing review
- Normal and hook-enabled bootstrap PASS (budget-retry-normal.log, budget-retry-hook-enabled.log). Root rebuilt authoritative build/layout production layer; full18 CPU/submission tests PASS7.82s (budget-retry-build.log, budget-retry-cpu.log). Final readonly source review: no blocker.
- VERSION prepared0.4.30 with release-notes-0.4.30.md; no release/install yet. Next commit/push PR and exact-head CI, then versioned publication/package verification. Installed remains0.4.29. No owned local jobs left, no new GPU tests run.
