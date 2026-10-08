# Lean clean-first runtime regression

Unhelpful restore/remap batching and input staging prototypes removed; reproducible research archives retained. Added only an encoded-input byte telemetry counter to the retained serial restore path.

Current BP16 build with clean-first enabled:121/121 tests passed84.48s. Installed/default build refreshed; CPU12/12 passed4.59s. Production decoder SPIR-V unchanged. Defaults unchanged; clean-first and GPU encoder remain opt-in. Two clean-first full-model runs measured 1.16969830 and 1.17068766 tokens/s, exact output, 49/49 layers, zero restore fallback, and zero swap growth. Compared with 1.10252711 from the prior repeated encoder run, these are about 6.1–6.2% higher by rate, but sequential clocks/background activity were uncontrolled. [First run](../internlm-bp16-gpu-clean-first-cold26-owner26-resident19-lfu/README.md) · [repeat and limits](../internlm-bp16-gpu-clean-first-cold26-owner26-resident19-lfu-repeat/README.md).
