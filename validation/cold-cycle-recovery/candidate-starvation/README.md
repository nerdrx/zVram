# Recovery candidate starvation

The worker selected the oldest nonlocal child before checking two permanent refusals enforced by the promotion helper: adopted allocation callbacks and children above 32 MiB. Such a child was selected every pass and immediately refused without the transaction-failure backoff. Younger eligible children could therefore never recover.

The worker now excludes these candidates before comparing ages. The small selection helper preserves the existing completion, quiet-time, type compatibility and headroom checks and clears output handles when no candidate exists. Recovery transactions and opt-in defaults are unchanged.

A CPU regression places an older refused pool beside a younger eligible pool, for both refusal cases, and requires selection of the younger child. Removing the younger pool must leave no candidate, including after an earlier successful selection. Existing GPU checks cover byte integrity and pending-queue deferral of the unchanged promotion transaction. See the archived CPU and GPU results in this directory.

This fixes opt-in recovery starvation; it does not establish game FPS gains or control kernel-transparent GTT placement.

Validation: all 13 normal CPU checks passed in 5.33 seconds. Hook bootstrap plus five bounded native recovery checks passed 6/6 in 6.56 seconds, including single/two-queue recovery, BDA shader integrity, hot completed use and pending-queue deferral. Full 32 MiB checks and clean teardown remained enabled. The normal shared library contained neither test setter.
