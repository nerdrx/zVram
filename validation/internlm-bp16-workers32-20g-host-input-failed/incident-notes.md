# Failed 20 GiB BP16 host-input attempt

The prompt was sent, but after 65.22 seconds the process aborted on `vk::Queue::submit: ErrorOutOfDeviceMemory` (return status -6). The last recorded point showed 235 GPU restores, zero GPU fallback, and zero snapshot failures. It produced no accepted rate or final output hash.

This run requested a 20 GiB tracked cap with a 2 GiB reserve. A prior 19 GiB / 2.5 GiB reserve host-input run succeeded; because both cap and reserve changed, this attempt does not prove a host-input defect or a native GPU fault. RAM remained above the 16 GiB guard, swap growth was zero, and the Ollama GPU guard detected no process. The owned child had `RLIMIT_CORE=0`.
