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
Obtain failing CPU regression before changing ranking. Audit recency/frequency and hysteresis before broad policy changes. Do not blindly reduce timeouts or bypass pending-work protection. Recheck fresh host health before small GPU work. Track exact test evidence here; keep periodic heartbeat quiet if unchanged.

## 14:24 UTC — recent-first recovery
- New CPU regression failed against original source with “cold-cycle selection did not prefer the most recently used eligible child”; full output in ranking-before.log.
- Root changed selectColdCycleCandidate to choose greatest lastUse, retaining busy/quiet/budget/local-type exclusions. Pressure eviction remains oldest-first.
- Built normal build/layout bootstrap harness and production layer successfully. Focused CPU/submission suite: 18/18 PASS, 6.48s (ranking-after-cpu.log). New regression checks quiet-age, incompatible pool type, recent-first preference and busy fallback.
- This proves selection policy, not game performance or device residency. No GPU tests executed yet.
- Follow-up read-only audits active: active_queue_markers checks frequency/hysteresis options; auto_safety_review checks safe exchange at full cap and accounting. No production change for either yet.
- Worker currently waits up to 10ms per pass; removing sleep unconditionally could monopolize locks/spin on failure, so not done.
