# BP16 27 GiB RAM-guard abort

This 27 GiB cold/owner-limit attempt reached the prompt, then was stopped when
`MemAvailable` fell to **16,380 MiB**, below the **16,384 MiB** guard. Swap
increased by **2,259 MiB**. The response was incomplete; there is no accepted
token rate or completed output comparison. Ollama GPU detection was empty.

A first launch attempt failed before app startup because its Python import path
was incomplete; `launch-import-error.log` preserves that separate setup error.
The later guarded attempt is the run represented by `automatic-result.json`.
This archive is diagnostic only.
