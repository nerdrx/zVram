# BP16 research format

BP16 is a lossless reference codec for arbitrary 16-bit words. It makes no
floating-point or tensor-layout assumptions. A frame accepts 1 to 32 MiB of
raw bytes, with the size divisible by 256. Each independent block contains 128
little-endian 16-bit words.

All integer fields and bit streams are little-endian. The 16-byte frame header
contains four `u32` fields:

| Offset | Field | Value |
| ---: | --- | --- |
| 0 | magic | `0x36315042` (`BP16` bytes) |
| 4 | version | `1` |
| 8 | raw byte count | positive, at most 32 MiB, divisible by 256 |
| 12 | block count | exactly `rawBytes / 256` |

The header is followed by one 8-byte descriptor per block. A descriptor holds
an absolute frame offset (`u32`) to that block's payload and a packed `u32`:
the low 16 bits are `base`, and the high 16 bits are `varyingMask`. For all 128
words in a block, the encoder computes `base = AND(words)` and
`varyingMask = OR(words) XOR base`. Thus `base & varyingMask` must be zero.

The payload begins immediately after the descriptor table. Blocks appear in
order and their descriptors point to the exact next payload byte. There are no
gaps, overlap, or trailing bytes. A block with `k = popcount(varyingMask)` has
exactly `16*k` payload bytes. `k=0` has no payload; `k=16` carries all input
bits and is raw-sized before descriptor overhead.

For each word, the encoder visits set positions of `varyingMask` from bit 0
through bit 15 and gathers those source bits into a compact `k`-bit value. It
appends that value least-significant bit first to a continuous block bitstream.
Decoding scatters the gathered bits back to the same mask positions and ORs
them with `base`. Bits within each byte are numbered least-significant first.
The 128-word block size makes every payload byte-aligned without per-block
padding.

The reference implementation validates the complete frame, including every
canonical offset and final payload end, before decoding. A production format
would use raw fallback when bit packing saves too little, and would separately
define handling for lengths that are not multiples of 256. This prototype
does neither; those inputs are rejected.

The production `encode`/`encodeFast` entry point selects a whole-block
accumulator and an x86 BMI2 `_pext_u32` gather when the compiler and CPU support
it, otherwise it uses the portable gather. The BMI2 routine is function
targeted, so the rest of the program needs no architecture flags. The CPU
checker keeps the original bit-at-a-time reference encoder locally and compares
its exact frames with both the portable accumulator fallback and runtime path.

These are research implementations, not production codecs. The checker
compares all three paths for every `k` from 0 through 16, constant/random/sparse
patterns, malformed frames, and a 32 MiB sample beginning at byte 67,108,864 in
the existing InternLM2.5 GGUF. It writes raw and encoded sample files plus
CPU-only timing data under `build/bp16-research/` by default:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Werror research/bp16/codec_check.cpp -o /tmp/bp16-codec-check
/tmp/bp16-codec-check
```
