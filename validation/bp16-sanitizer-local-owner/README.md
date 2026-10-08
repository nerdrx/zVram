# CPU BP16 ownership and metadata sanitizer check

The current CPU codec check passed with AddressSanitizer, UndefinedBehaviorSanitizer, and leak detection enabled. It covers canonical and malformed metadata, parallel encoding and upload copies, local-owner quota arithmetic, and imported/allocated-owner lifetime checks. No sanitizer diagnostic was emitted.

This is a CPU validation result, not GPU correctness or a throughput result. The existing GPU/full-suite evidence remains separate. `provenance.json` records the compiler, exact commands, options, source hashes, and executable hash.
