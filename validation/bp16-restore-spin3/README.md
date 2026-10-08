# BP16 fence-spin focused checks

The focused CTest selection passed **20/20** in **10.86 s**: 12 CPU checks and
eight GPU integrity cases, with each GPU case passing under the opt-in spin
mode. The full log and CTest `LastTest.log` are retained here. This verifies the
prototype's tested correctness cases only; the fence-spin option has been
removed from the runtime after showing no meaningful full-model gain.
