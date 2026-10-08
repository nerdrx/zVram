# BP16 exact-sized snapshot storage transfer

Commit `6b35007` transfers ownership of a BP16 encoded buffer into its cold
snapshot when the encoded vector's size and capacity exactly match the stored
size. Other cases retain the existing copy path so spare vector capacity is
not excluded from quota accounting. This is a storage-copy avoidance change;
these checks do not measure any model-level benefit.

The CPU suite passed 12/12 in 4.67 seconds. The full CTest suite passed
121/121 in 86.12 seconds, and the focused asynchronous range/pressure tests
passed 4/4 in 1.90 seconds. The async `LastTest.log` contains actual
`async-freeze` snapshot events. The recorded layer binary hash corresponds to
commit `6b35007`; the BP16 shader was unchanged.

- [CPU CTest](cpu-ctest.log)
- [Full CTest](full-ctest.log)
- [Async range/pressure CTest](async-ctest.log)
- [Async snapshot evidence](async-lasttest.log)
- [Layer and shader hashes](binary-sha256.txt)
- [Source commit](source-commit.txt)
