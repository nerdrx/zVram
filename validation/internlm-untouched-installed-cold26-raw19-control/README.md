# Untouched installed-binary same-headroom control: failed load

The previously validated installed binary (`6d310fb`) was run with the same
26 GiB cold/owner, 19 GiB resident, and 2.5 GiB reserve settings as the nearby
budget-snapshot run. It exited before input with status 1 and
`vk::Queue::submit: ErrorOutOfDeviceMemory`; it has no model output or accepted
rate. The Ollama GPU guard found no model. This failure on the old binary while
machine budget conditions had changed means the adjacent budget-snapshot model
rate cannot be attributed to the option or runtime change.

The captured installed layer binary SHA-256 is
`49c5119a8bd87a153f17c9f4a4dad37b4fc551079f41c8ff2473a5afba28b19a`. Original
files and hashes are retained in `original-bytes-sha256.json`.
