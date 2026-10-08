# Interrupted BP16 direct host-input trial

This attempt has no accepted throughput result. The stderr log ends with `vk::Queue::submit: ErrorOutOfDeviceMemory`, after 353 GPU restore calls, zero GPU fallbacks, and 10 snapshot failures. The controller result is `-9`; the parent reports it sent SIGKILL to PID 1756378 while core-dump handling was blocked under disk pressure. That final status does not identify the original crash signal.

The parent also observed a concurrent external Ollama 9B reload (6.1 GB VRAM), with native budget falling to 15.7 GB and effective budget to 13.0 GB from the planned 19 GB. No GPU reset was found in the journal. These observations do not establish that either host-input mode or concurrent loading caused the queue-submit failure. See `summary.json` for the evidence scope and source hashes.
