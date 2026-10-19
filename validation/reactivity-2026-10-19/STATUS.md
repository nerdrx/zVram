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
