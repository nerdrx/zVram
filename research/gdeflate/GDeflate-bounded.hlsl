/*
 * SPDX-FileCopyrightText: Copyright (c) 2020, 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define USE_WAVE_INTRINSICS // Enable on machines with WaveOps support (SM 6.0 and above)
//#define USE_WAVE_MATCH      // Enable use of the WaveMatch() intrinsics (requires shader  model 6.5)
//#define SIMD_WIDTH <width>  // SIMD width of the machine (required when USE_WAVE_INTRINSICS)

#define NUM_BITSTREAMS 32          // GDeflate interleaves 32 compressed bitstreams
#define NUM_THREADS NUM_BITSTREAMS // Thread blocks are sized to match that
#define MAX_DECODE_ROUNDS (64 * 1024)
#define MAX_BLOCK_ROUNDS 4096
#define MAX_CODE_LENGTH_ROUNDS 318
#define ERR_HEADER         (1u << 0)
#define ERR_TABLE          (1u << 1)
#define ERR_INPUT_BITS     (1u << 2)
#define ERR_OUTPUT_BOUNDS  (1u << 3)
#define ERR_SYMBOL         (1u << 4)
#define ERR_CODE_LENGTH    (1u << 5)
#define ERR_COPY           (1u << 6)
#define ERR_LOOP           (1u << 7)
#define ERR_FINAL_SIZE     (1u << 8)

#if defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH >= NUM_THREADS)
#define IN_REGISTER_DECODER
#define SINGLE_WAVE
#endif

// Raw input and output buffers
ByteAddressBuffer input : register(t0);
RWByteAddressBuffer control : register(u0);
globallycoherent RWByteAddressBuffer output : register(u1);
RWByteAddressBuffer scratch : register(u2);
groupshared uint groupError;
// Research ABI: control[0]=one stream, control[1]=input offset,
// control[2]=output offset. scratch[0] is a sticky shader error bit.

#include "tilestream.hlsl"

void RecordError(uint bits)
{
    InterlockedOr(groupError, bits);
    scratch.InterlockedOr(0, bits);
}

inline uint32_t mask(uint32_t n)
{
    return (1u << n) - 1u;
}

inline uint32_t ltMask(uint tid)
{
    return mask(tid);
}

inline uint32_t extract(uint32_t data, uint32_t pos, uint32_t n, uint32_t base = 0)
{
    return ((data >> pos) & mask(n)) + base;
}

groupshared uint32_t g_tmp[NUM_THREADS];

#if defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH >= NUM_THREADS)

inline uint32_t vote(bool p, uint tid)
{
    return (uint32_t)WaveActiveBallot(p);
}

inline uint32_t shuffle(uint32_t value, uint idx, uint tid)
{
    return WaveReadLaneAt(value, idx);
}

inline uint32_t broadcast(uint32_t value, uint idx, uint tid)
{
    return WaveReadLaneAt(value, idx);
}

inline bool all(bool p, uint tid)
{
    return (uint32_t)WaveActiveAllTrue(p);
}

uint32_t scan(uint32_t value, uint tid)
{
    return WavePrefixSum(value);
}

#else

groupshared uint32_t g_tmp1[NUM_THREADS];
groupshared uint32_t g_tmp2[NUM_THREADS];
groupshared uint32_t g_tmp3[NUM_THREADS];

inline uint32_t vote(bool p, uint tid)
{
#ifdef USE_WAVE_INTRINSICS
    g_tmp1[tid / SIMD_WIDTH] = (uint32_t)WaveActiveBallot(p);
    GroupMemoryBarrierWithGroupSync();
    uint32_t ballot = g_tmp1[0];
    [unroll] for (uint i = 1; i < NUM_THREADS / SIMD_WIDTH; i++) ballot |= g_tmp1[i] << (SIMD_WIDTH * i);
    GroupMemoryBarrierWithGroupSync();
    return ballot;
#else
    g_tmp1[tid] = p ? (1u << tid) : 0;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint i = NUM_THREADS / 2; i > 0; i >>= 1)
    {
        if (tid < i)
            g_tmp1[tid] |= g_tmp1[tid + i];
        GroupMemoryBarrierWithGroupSync();
    }
    uint ballot = g_tmp1[0];
    GroupMemoryBarrierWithGroupSync();
    return ballot;
#endif
}

inline uint32_t shuffle(uint32_t value, uint idx, uint tid)
{
    g_tmp1[tid] = value;
    GroupMemoryBarrierWithGroupSync();
    uint32_t res = g_tmp1[idx];
    GroupMemoryBarrierWithGroupSync();
    return res;
}

inline uint32_t broadcast(uint32_t value, uint idx, uint tid)
{
    GroupMemoryBarrierWithGroupSync();
    if (tid == idx)
        g_tmp1[0] = value;
    GroupMemoryBarrierWithGroupSync();
    return g_tmp1[0];
}

bool all(bool p, uint tid)
{
    return vote(p, tid) == 0xffffffffu;
}

// Prefix sum
inline uint32_t scan(uint32_t value, uint tid)
{
#if defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH == 16)
    uint32_t sum = WavePrefixSum(value);
    if (tid == SIMD_WIDTH - 1)
        g_tmp1[0] = sum + value;
    GroupMemoryBarrierWithGroupSync();
    if (tid >= SIMD_WIDTH)
        sum += g_tmp1[0];
    return sum;
#else
    uint32_t sum = value;

    [unroll] for (uint i = 1; i < NUM_THREADS; i *= 2)
    {
        uint source = tid >= i ? tid - i : tid;
        uint previous = shuffle(sum, source, tid);
        sum += tid >= i ? previous : 0;
    }

    return sum - value;
#endif
}

#endif

// Segmented prefix sum
uint32_t scan16(uint32_t value, uint tid)
{
#if defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH == 16)
    return WavePrefixSum(value) + value;
#else
    [unroll] for (uint i = 1; i < NUM_THREADS / 2; i *= 2)
    {
        uint lane = tid & 15;
        uint source = lane >= i ? tid - i : tid;
        uint previous = shuffle(value, source, tid);
        value += lane >= i ? previous : 0;
    }
#endif
    return value;
}

uint32_t match(uint32_t value, uint tid)
{
#if defined(USE_WAVE_MATCH) && defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH >= NUM_THREADS)
    return (uint32_t)WaveMatch(value);
#else
    uint32_t mask = 0;

#if defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH >= NUM_THREADS)
    [unroll] for (uint i = 0; i < NUM_THREADS; i++)
    {
        mask |= (WaveReadLaneAt(value, i) == value ? 1u : 0) << i;
    }
#else
    g_tmp1[tid] = value;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint i = 0; i < NUM_THREADS; i++)
    {
        GroupMemoryBarrierWithGroupSync();
        mask |= g_tmp1[i] == value ? (1u << i) : 0;
    }
    GroupMemoryBarrierWithGroupSync();
#endif

    return mask;
#endif
}

inline uint32_t ReadOutputByte(uint32_t offset, uint32_t tileBegin, uint32_t tileEnd, uint32_t tilePaddedEnd)
{
    if (offset < tileBegin || offset >= tileEnd) {
        RecordError(ERR_COPY);
        return 0;
    }
    uint32_t offsetMod4 = offset & 3;
    offset -= offsetMod4;
    if (offset < tileBegin || tilePaddedEnd < 4 || offset > tilePaddedEnd - 4) {
        RecordError(ERR_COPY);
        return 0;
    }
    uint32_t shift = offsetMod4 << 3;
    return (output.Load(offset) >> shift) & 0xff;
}

inline void StoreByte(uint32_t offset, uint32_t data, uint32_t tileBegin, uint32_t tileEnd, uint32_t tilePaddedEnd)
{
    if (offset < tileBegin || offset >= tileEnd) {
        RecordError(ERR_OUTPUT_BOUNDS);
        return;
    }
    uint32_t offsetMod4 = offset & 3;
    offset -= offsetMod4;
    if (offset < tileBegin || tilePaddedEnd < 4 || offset > tilePaddedEnd - 4) {
        RecordError(ERR_OUTPUT_BOUNDS);
        return;
    }
    uint32_t shift = offsetMod4 << 3;
    output.InterlockedOr(offset, (data & 0xff) << shift);
}

struct BitReader
{
    static const uint kWidth = NUM_BITSTREAMS;

    uint base, end, cnt, validCnt, bufferEnd;
    uint64_t buf;

    // Reset bit reader - assume base pointer is word-aligned
    void init(uint i, uint tid, uint inputEnd, uint totalInputBytes)
    {
        cnt = kWidth;
        validCnt = kWidth;
        end = inputEnd;
        bufferEnd = totalInputBytes;
        buf = (uint64_t)input.Load(i + tid * 4);
        base = i + kWidth * 4;
    }

    // Refill bit buffer if needed and advance shared base pointer
    void refill(bool p, uint tid)
    {
        p &= cnt < kWidth;
        uint32_t ballot = vote(p, tid);
        uint offset = countbits(ballot & ltMask(tid)) * 4;
        if (p)
        {
            uint addr = base + offset;
            if (addr < end) {
                uint bytes = min(4u, end - addr);
                uint word = 0;
                if (addr <= bufferEnd && bufferEnd - addr >= 4) {
                    word = input.Load(addr);
                    if (bytes < 4)
                        word &= (1u << (bytes * 8)) - 1u;
                    validCnt += bytes * 8;
                }
                buf |= (uint64_t)word << cnt;
            }
            cnt += kWidth;
        }
        base += countbits(ballot) * 4;
    }

    // Remove n bits from the bit buffer
    void eat(uint n, uint tid, bool p)
    {
        if (p)
        {
            if (n > validCnt)
                RecordError(ERR_INPUT_BITS);
            validCnt = n >= validCnt ? 0 : validCnt - n;
            if (n > cnt) {
                RecordError(ERR_INPUT_BITS);
                buf = 0;
                cnt = 0;
            } else {
                buf >>= n;
                cnt -= n;
            }
        }
        refill(p, tid);
    }

    // Return n bits from the bit buffer without changing reader state (up to 32 bits at a time)
    uint32_t peek(uint n)
    {
        return (uint32_t)buf & mask(n);
    }

    uint32_t peek()
    {
        return (uint32_t)buf;
    }

    // Return n bits from the bit buffer and remove them
    uint32_t read(uint n, uint tid, bool p)
    {
        uint32_t bits = p ? (uint32_t)buf & mask(n) : 0;

        eat(n, tid, p);
        return bits;
    }
};

// Scratch storage for code length array
groupshared struct Scratch
{
    uint32_t data[64];
    void clear(uint tid)
    {
        data[tid] = data[tid + NUM_THREADS] = 0;
    } // Clear first 64 words

    // Returns a nibble of data
    uint32_t get4b(uint i)
    {
        return (data[i / 8] >> (4 * (i % 8))) & 15;
    }
} g_buf;

void set4b(uint32_t nibbles, uint32_t n, uint32_t i, uint32_t count)
{
    if (i >= count) {
        RecordError(ERR_CODE_LENGTH);
        return;
    }
    uint32_t written = min(n, count - i);
    if (written != n)
        RecordError(ERR_CODE_LENGTH);
    [loop] for (uint32_t j = 0; j < written; ++j) {
        uint32_t at = i + j;
        InterlockedOr(g_buf.data[at / 8], (nibbles & 15) << (4 * (at % 8)));
    }
}

// Symbol table
groupshared struct SymbolTable
{
    static const uint32_t kMaxSymbols = 288 + 32;
    static const uint32_t kDistanceCodesBase = 288;

    uint symbols[kMaxSymbols]; // Can be stored in uint16_t

    // Scatter symbols according to in-register lengths and their corresponding offsets
    uint32_t scatter(uint sym, uint len, uint offset, uint tid)
    {
        uint32_t mask = match(len, tid);
        if (len != 0)
        {
            uint index = offset + countbits(mask & ltMask(tid));
            if (index < kMaxSymbols)
                symbols[index] = sym;
            else
                RecordError(ERR_SYMBOL);
        }
        return mask;
    }

    // Init symbol table from an array of code lengths in shared memory
    // hlit is at least 257
    // Assumes offsets contain literal/length offsets in lower numbered threads and distance code offsets in
    // higher-numbered threads
    void init(uint hlit, uint offsets, uint tid)
    {
        GroupMemoryBarrierWithGroupSync(); // code lengths / fixed table must be visible
        if (tid != 15 && tid != 31)
            g_tmp[tid + 1] = offsets;

        GroupMemoryBarrierWithGroupSync();
        // 8 unconditional iterations, fully unroll
        [unroll] for (uint32_t i = 0; i < 256 / NUM_THREADS; i++)
        {
            uint32_t sym = i * NUM_THREADS + tid;
            uint32_t len = g_buf.get4b(sym);
            uint32_t match = scatter(sym, len, g_tmp[len], tid);
            if (tid == firstbitlow(match))
                g_tmp[len] += countbits(match);
            GroupMemoryBarrierWithGroupSync();
        }

        // Bounds check on the last iteration for literals
        uint32_t sym = 8 * NUM_THREADS + tid;
        uint32_t len = sym < hlit ? g_buf.get4b(sym) : 0;
        scatter(sym, len, g_tmp[len], tid);

        // Scatter distance codes (assumes source array is padded with 0)
        len = g_buf.get4b(tid + hlit);
        scatter(tid, len, kDistanceCodesBase + g_tmp[16 + len], tid);
        GroupMemoryBarrierWithGroupSync(); // symbol table is consumed by all lanes
    }

} g_lut;

#ifdef IN_REGISTER_DECODER
#define LVAL(name, index) name
#define RVAL(name, index) WaveReadLaneAt(name, (index))
#else
#define LVAL(name, index) name[index]
#define RVAL(name, index) name[index]
#endif

#define DECLARE(type, name, size) type LVAL(name, size)

// Maintains state of a pair of decoders (in higher and lower numbered threads)
struct DecoderPair
{
    static const uint kMaxCodeLen = 15;

    // Aligned so that both can be indexed with (len-1)
    DECLARE(uint32_t, baseCodes, NUM_THREADS); // Base codes for each code length + sentinel code
    DECLARE(uint, offsets, NUM_THREADS);       // Offsets into the symbol table

    uint offset(uint i)
    {
        return RVAL(offsets, i);
    }

    // Build two decoders in parallel
    void init(uint counts, uint maxlen, uint tid)
    { // counts contain a histogram of code lengths

        // Calculate offsets into the symbol table
        LVAL(offsets, tid) = scan16(counts, tid);

        // Calculate base codes
#ifndef IN_REGISTER_DECODER
        g_tmp1[tid] = counts;
        GroupMemoryBarrierWithGroupSync();
#endif

        uint32_t baseCode = 0;
        [unroll] for (uint32_t i = 1; i < maxlen; i++)
        {
            uint lane = tid & 15;
#ifndef IN_REGISTER_DECODER
            uint count = g_tmp1[(tid & 16) + i];
#else
            uint count = shuffle(counts, (tid & 16) + i, tid);
#endif
            if (lane >= i)
                baseCode += count << (lane - i);
        }

        // Left-align and fill in sentinel values
        uint lane = tid & 15;
        uint tmp = baseCode << (32 - lane);
        LVAL(baseCodes, tid) = tmp < baseCode || (lane >= maxlen) ? 0xffffffff : tmp;
    }

    // Maps a code to its length (base selects decoder)
    uint len4code(uint32_t code, uint base = 0)
    {
        uint len = 1;
        if (code >= RVAL(baseCodes, 7 + base))
            len = 8;
        if (code >= RVAL(baseCodes, len + 3 + base))
            len += 4;
        if (code >= RVAL(baseCodes, len + 1 + base))
            len += 2;
        if (code >= RVAL(baseCodes, len + base))
            len += 1;
        return len;
    }

    // Maps a code and its length to a symbol id (base selects decoder)
    uint id4code(uint32_t code, uint len, uint base = 0)
    {
        uint i = len + base - 1;
        return RVAL(offsets, i) + ((code - RVAL(baseCodes, i)) >> (32 - len));
    }

    // Decode a huffman-coded symbol
    uint decode(uint32_t bits, out uint len, bool isdist = false)
    {
        uint32_t code = reversebits(bits);
        len = len4code(code, isdist ? 16 : 0);
        uint idx = id4code(code, len, isdist ? 16 : 0) + (isdist ? 288 : 0);
        if (idx >= SymbolTable::kMaxSymbols) {
            RecordError(ERR_SYMBOL);
            len = 1;
            return isdist ? 0 : 256;
        }
        return g_lut.symbols[idx];
    }
};

// Declare global decoder if not using in-register decoders
#ifndef IN_REGISTER_DECODER
groupshared DecoderPair dec;
#endif

// Calculate a histogram from in-register code lengths (each thread maps to a length)
uint32_t GetHistogram(uint32_t cnt, uint32_t len, uint32_t maxlen, uint tid)
{
    g_tmp[tid] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (len != 0 && tid < cnt)
        InterlockedAdd(g_tmp[len], 1);
    GroupMemoryBarrierWithGroupSync();
    return g_tmp[tid & 15];
}

// Read and sort code length code lengths
uint ReadLenCodes(inout BitReader br, uint hclen, uint tid)
{
    static const uint lane4id[32] = {3, 17, 15, 13, 11, 9, 7, 5, 4, 6, 8, 10, 12, 14, 16, 18,
                                     0,  1,  2,  0,  0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0};

    uint len = br.read(3, tid, tid < hclen); // Read reordered code length code lengths in
                                             // the first hclen threads (up to 19)
    len = shuffle(len, lane4id[tid], tid);   // Restore original order
    len &= tid < 19 ? 0xf : 0;               // Zero-out the garbage
    return len;
}

// Update histograms
// (distance codes are histogrammed in the higher numbered threads, literal/length codes - in lower numbered threads)
void UpdateHistograms(uint32_t len, int i, int n, int hlit)
{
    uint32_t cnt = max(min(hlit - i, n), 0);
    if (cnt != 0)
        InterlockedAdd(g_tmp[len], cnt);

    cnt = max(min(i + n - hlit, n), 0);
    if (cnt != 0)
        InterlockedAdd(g_tmp[16 + len], cnt);
}

// Unpack code lengths and create a histogram of lengths.
// Returns a histogram of literal/length code lengths in lower numbered threads,
// and a histogram of distance code lengths in higher numbered threads.
uint UnpackCodeLengths(inout BitReader br, uint hlit, uint hdist, uint hclen, uint tid, uint dst)
{
    uint len = ReadLenCodes(br, hclen, tid);

#ifdef IN_REGISTER_DECODER
    DecoderPair dec;
#endif

    // Init decoder
    uint cnts = GetHistogram(19, len, 7, tid);
    dec.init(cnts, 7, tid);
    g_lut.scatter(tid, len, dec.offset(len - 1), tid);

    uint32_t count = hlit + hdist;
    uint32_t baseOffset = 0;
    uint32_t lastlen = ~0;

    // Clear codelens array (4 bit lengths)
    g_buf.clear(tid);
    g_tmp[tid] = 0;

    GroupMemoryBarrierWithGroupSync();

    // Decode code length codes and expand into a shared memory array
    uint iterations = 0;
    do
    {
        ++iterations;
        uint len;
        uint32_t bits = br.peek(7 + 7);
        uint sym = dec.decode(bits, len);
        uint idx = sym <= 15 ? 0 : (sym - 15);

        if (sym > 18) {
            RecordError(ERR_CODE_LENGTH);
            idx = 0;
        }

        static const uint base[4] = {1, 3, 3, 11};
        static const uint xlen[4] = {0, 2, 3, 7};

        uint n = base[idx] + ((bits >> len) & mask(xlen[idx]));

        // Scan back to find the nearest lane which contains a valid symbol
        uint lane = firstbithigh(vote(sym != 16, tid) & ltMask(tid));

        uint codelen = sym;
        if (sym > 16)
            codelen = 0;
        uint prevlen = shuffle(codelen, lane, tid);

        if (sym == 16)
        {
            codelen = lane == ~0 ? lastlen : prevlen;
            if (codelen == ~0) {
                RecordError(ERR_CODE_LENGTH);
                codelen = 0;
            }
        }
        if (codelen > 15) {
            RecordError(ERR_CODE_LENGTH);
            codelen = 0;
        }

        lastlen = broadcast(codelen, NUM_THREADS - 1, tid);
        GroupMemoryBarrierWithGroupSync();
        baseOffset = scan(n, tid) + baseOffset;

        if (baseOffset < count && codelen != 0)
        {
            UpdateHistograms(codelen, baseOffset, n, hlit);
            set4b(codelen, n, baseOffset, count);
        }

        br.eat(len + xlen[idx], tid, baseOffset < count);

        baseOffset = broadcast(baseOffset + n, NUM_THREADS - 1, tid);
        GroupMemoryBarrierWithGroupSync();

        if ((iterations >= MAX_CODE_LENGTH_ROUNDS || iterations >= count) && baseOffset < count) {
            RecordError(ERR_LOOP);
            break;
        }
        GroupMemoryBarrierWithGroupSync();
        if (groupError != 0)
            break;
    } while (all(baseOffset < count));

    GroupMemoryBarrierWithGroupSync(); // code-length scratch consumed after this point
    if (groupError != 0)
        return 0;
    return g_tmp[tid];
}

void WriteOutput(uint32_t dst, uint32_t offset, uint32_t dist, uint32_t length, uint32_t byte, bool iscopy, uint tid, uint tileBegin, uint tileEnd, uint tilePaddedEnd)
{
    if (dst > tileEnd || offset > tileEnd - dst) {
        RecordError(ERR_OUTPUT_BOUNDS);
        dst = tileEnd;
    } else {
        dst += offset;
    }
    // Output literals
    if (!iscopy && length != 0)
        StoreByte(dst, byte, tileBegin, tileEnd, tilePaddedEnd);

    // Literals may be the source bytes of a copy token in this same round.
    AllMemoryBarrierWithGroupSync();

    // Fill in copy destinations
    uint32_t mask = vote(iscopy, tid);
    uint32_t msk = mask;

    uint copyOps = 0;
    while (mask != 0 && copyOps < NUM_THREADS)
    {
        ++copyOps;
        uint32_t lane = firstbitlow(mask);

#if !defined(USE_WAVE_INTRINSICS) || (SIMD_WIDTH < NUM_THREADS)
        g_tmp1[tid] = dist;
        g_tmp2[tid] = length;
        g_tmp3[tid] = dst;

        GroupMemoryBarrierWithGroupSync();

        uint32_t off = g_tmp1[lane];
        uint32_t len = g_tmp2[lane];
        uint32_t output = g_tmp3[lane];
#else
        uint32_t off = broadcast(dist, lane, tid);
        uint32_t len = broadcast(length, lane, tid);
        uint32_t output = broadcast(dst, lane, tid);
#endif

        if (off == 0) {
            RecordError(ERR_COPY);
            break;
        }

        // The maximum logical copy is bounded by the remaining tile output.
        uint32_t copyLen = len;
        if (output < tileBegin || output > tileEnd) {
            RecordError(ERR_COPY);
            copyLen = 0;
        } else if (copyLen > tileEnd - output) {
            RecordError(ERR_COPY);
            copyLen = tileEnd - output;
        }

        // Copy using all lanes; one token cannot loop beyond the 64 KiB tile.
        [loop] for (uint32_t i = tid; i < copyLen && i < kDefaultTileSize; i += NUM_THREADS)
        {
            uint32_t back = off - (i % off);
            uint32_t src = output >= back ? output - back : tileEnd;
            uint32_t data = ReadOutputByte(src, tileBegin, tileEnd, tilePaddedEnd);
            StoreByte(i + output, data, tileBegin, tileEnd, tilePaddedEnd);
        }

        // Next copy token may depend on bytes written by this token.
        AllMemoryBarrierWithGroupSync();

        mask &= mask - 1;
    }
    if (mask != 0)
        RecordError(ERR_LOOP);
}

// Translate a symbol to its value
uint TranslateSymbol(inout BitReader br, int sym, uint len, uint32_t bits, bool isdist, uint tid, bool p)
{
    // Tables for distance/length decoding DEFLATE64
    static const uint32_t baseDist[] =
    {    1,    2,    3,     4,     5,     7,     9,    13,
        17,   25,   33,    49,    65,    97,   129,   193,
       257,  385,  513,   769,  1025,  1537,  2049,  3073,
      4097, 6145, 8193, 12289, 16385, 24577, 32769, 49153 };

    static const uint32_t baseLength[] =
    {  0,   3,   4,   5,   6,  7,  8,  9,
      10,  11,  13,  15,  17, 19, 23, 27,
      31,  35,  43,  51,  59, 67, 83, 99,
     115, 131, 163, 195, 227,  3,  0 };

    static const uint32_t extraDist[] =
    { 0,  0,  0,  0,  1,  1,  2,  2,
      3,  3,  4,  4,  5,  5,  6,  6,
      7,  7,  8,  8,  9,  9, 10, 10,
     11, 11, 12, 12, 13, 13, 14, 14 };

    static const uint32_t extraLength[] =
    {0, 0, 0, 0, 0,  0, 0, 0,
     0, 1, 1, 1, 1,  2, 2, 2,
     2, 3, 3, 3, 3,  4, 4, 4,
     4, 5, 5, 5, 5, 16, 0 };

    if ((isdist && (sym < 0 || sym >= 32)) ||
        (!isdist && (sym < 0 || sym > 285))) {
        RecordError(ERR_SYMBOL);
        br.eat(len, tid, isdist || p);
        return 0;
    }
    uint32_t base = isdist ? baseDist[sym] : (sym >= 256 ? baseLength[sym - 256] : 1);
    uint32_t n = isdist ? extraDist[sym] : (sym >= 256 ? extraLength[sym - 256] : 0);

    br.eat(len + n, tid, isdist || p);

    return base + ((bits >> len) & mask(n));
}

// Assumes code lengths have been stored in the shared memory array
uint CompressedBlock(inout BitReader br, uint hlit, uint counts, uint dst, uint tid, uint tileBegin, uint tileEnd, uint tilePaddedEnd)
{
    // Init decoders
#ifdef IN_REGISTER_DECODER
    DecoderPair dec;
#endif

    dec.init(counts, 15, tid);
    g_lut.init(hlit, RVAL(dec.offsets, tid), tid);
    if (groupError != 0)
        return dst;

    // Initial round - no copy processing
    uint32_t len;
    uint32_t sym = dec.decode(br.peek(15 + 16), len, false);

    uint32_t eob = vote(sym == 256, tid);
    bool oob = (eob & ltMask(tid)) != 0;

    // Translate current symbol
    uint32_t value = TranslateSymbol(br, sym, len, br.peek(), false, tid, !oob);

    // Compute output pointers for the current round
    uint32_t length = oob ? 0 : value;
    uint32_t offset = scan(length, tid);
    GroupMemoryBarrierWithGroupSync(); // synchronize any lane-local decode error before loop predicate

    // Copy predicate for the next round
    bool iscopy = sym > 256;
    uint32_t byte = sym;

    // Translate all symbols in the block
    uint decodeRounds = 0;
    while (eob == 0 && decodeRounds < MAX_DECODE_ROUNDS && groupError == 0)
    {
        ++decodeRounds;
        sym = dec.decode(br.peek(15 + 16), len, iscopy);

        // Set predicates based on the current symbol
        eob = vote(sym == 256, tid);    // end of block symbol
        oob = (eob & ltMask(tid)) != 0; // true in threads which looked at symbols past the end of the block

        // Translate current symbol
        value = TranslateSymbol(br, sym, len, br.peek(), iscopy, tid, !oob);

        WriteOutput(dst, offset, value, length, byte, iscopy, tid, tileBegin, tileEnd, tilePaddedEnd);

        // Advance output pointers
        uint advance = broadcast(offset + length, NUM_THREADS - 1, tid);
        if (dst > tileEnd || advance > tileEnd - dst) {
            RecordError(ERR_OUTPUT_BOUNDS);
            dst = tileEnd;
        } else {
            dst += advance;
        }
        AllMemoryBarrierWithGroupSync();
        if (groupError != 0) {
            eob = ~0u;
            break;
        }
        // Compute output pointers for the current round
        length = iscopy || oob ? 0 : value;
        offset = scan(length, tid);

        iscopy = sym > 256; // Current symbol is a copy, transition to the new state
        byte = sym;
    }

    if (eob == 0)
        RecordError(ERR_LOOP);
    if (groupError != 0)
        return dst;

    // One last round of copy processing
    sym = dec.decode(br.peek(15 + 16), len, true);
    iscopy &= !oob;
    uint32_t dist = TranslateSymbol(br, sym, len, br.peek(), iscopy, tid, false);
    WriteOutput(dst, offset, dist, length, byte, iscopy, tid, tileBegin, tileEnd, tilePaddedEnd);

    uint advance = broadcast(offset + length, NUM_THREADS - 1, tid);
    uint res = dst;
    if (dst > tileEnd || advance > tileEnd - dst)
        RecordError(ERR_OUTPUT_BOUNDS);
    else
        res += advance;
    GroupMemoryBarrierWithGroupSync(); // THIS BARRIER IS REQUIRED
    return res;
}

// Uncompressed block (raw copy)
uint32_t UncompressedBlock(inout BitReader br, uint32_t dst, uint32_t size, uint tid, uint tileBegin, uint tileEnd, uint tilePaddedEnd)
{
    uint32_t nrounds = size / NUM_THREADS;

    // Full rounds with no bounds checking
    uint rounds = min(nrounds, MAX_DECODE_ROUNDS);
    if (rounds != nrounds)
        RecordError(ERR_LOOP);
    [loop] for (uint r = 0; r < rounds; ++r)
    {
        StoreByte(dst + tid, br.read(8, tid, true), tileBegin, tileEnd, tilePaddedEnd);
        dst += NUM_THREADS;
    }

    uint32_t rem = size % NUM_THREADS;

    // Last partial round with bounds check
    if (rem != 0)
    {
        uint32_t byte = br.read(8, tid, tid < rem);
        if (tid < rem)
            StoreByte(dst + tid, byte, tileBegin, tileEnd, tilePaddedEnd);
        dst += rem;
    }

    AllMemoryBarrierWithGroupSync();

    return dst;
}

// Initialize fixed code lengths, return a histogram
uint FixedCodeLengths(uint tid)
{
    g_buf.data[tid] = tid < 18 ? 0x88888888 : 0x99999999;
    g_buf.data[tid + 32] = tid < 3 ? 0x77777777 : (tid < 4 ? 0x88888888 : 0x55555555);

    // Threads can be synchronized later..
    return tid == 7 ? 24 : (tid == 8 ? 152 : (tid == 9 ? 112 : tid == 16 + 5 ? 32 : 0));
}

// This is main entry point for tile decompressor
void DecompressTile(in TileParams params, uint tid, uint totalInputBytes)
{
    if (params.inSize < NUM_THREADS * 4) {
        RecordError(ERR_INPUT_BITS);
        return;
    }
    if (params.outSize == 0 || params.outSize > kDefaultTileSize) {
        RecordError(ERR_OUTPUT_BOUNDS);
        return;
    }

    // Init bit reader
    BitReader br;
    br.init(params.inPos, tid, params.inPos + params.inSize, totalInputBytes);

    uint32_t dst = params.outPos;
    uint32_t tileEnd = params.outPos + params.outSize;
    uint32_t tilePaddedEnd = (tileEnd + 3u) & ~3u;

    // Clear destination to 0
    for (uint32_t i = tid; i < (params.outSize + 3) / 4; i += NUM_THREADS)
        output.Store(dst + i * 4, 0);
    AllMemoryBarrierWithGroupSync();

    // A valid tile cannot emit more blocks than output bytes; cap malformed streams.
    bool done = false;
    uint blockRounds = 0;
    while (!done && blockRounds < MAX_BLOCK_ROUNDS && groupError == 0)
    {
        ++blockRounds;
        // Read block header and broadcast to all threads
        uint32_t header = broadcast(br.peek(), 0, tid);
        GroupMemoryBarrierWithGroupSync();
        done = extract(header, 0, 1) != 0;

        // Parse block type
        uint32_t btype = extract(header, 1, 2);

        br.eat(3, tid, tid == 0);

        uint counts, size, hlit, hdist;

        switch (btype)
        {

        case 2: // Dynamic huffman block
            hlit = extract(header, 3, 5, 257);
            hdist = extract(header, 8, 5, 1);
            br.eat(14, tid, tid == 0);
            counts = UnpackCodeLengths(br, hlit, hdist, extract(header, 13, 4, 4), tid, dst);
            if (groupError != 0) {
                done = true;
                break;
            }
            // Falls through to the following case
        case 1: // Fixed huffman block
            if (btype == 1)
                counts = FixedCodeLengths(tid);

            GroupMemoryBarrierWithGroupSync();
            dst = CompressedBlock(br, btype == 1 ? 288 : hlit, counts, dst, tid,
                                  params.outPos, tileEnd, tilePaddedEnd);
            break;

        case 0: // Uncompressed block
            size = broadcast(br.read(16, tid, tid == 0), 0, tid);
            GroupMemoryBarrierWithGroupSync();
            if (size > params.outPos + params.outSize - min(dst, params.outPos + params.outSize)) {
                RecordError(ERR_OUTPUT_BOUNDS);
                size = 0;
            }
            dst = UncompressedBlock(br, dst, size, tid, params.outPos, tileEnd, tilePaddedEnd);
            break;

        default:
            RecordError(ERR_HEADER);
            done = true;
            break;
        }

        GroupMemoryBarrierWithGroupSync();
        if (groupError != 0)
            break;

    }
    if (!done)
        RecordError(ERR_LOOP);
    if (dst != tileEnd)
        RecordError(ERR_FINAL_SIZE);
}

// Single-stream research ABI: control[0]=1, control[1]=stream input byte offset,
// control[2]=stream output byte offset. scratch[0] is a host-zeroed error mask.
// One workgroup handles one tile (GroupID.x); this removes global work stealing.
[numthreads(NUM_THREADS, 1, 1)]
void CSMain(uint tid : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    if (tid == 0)
        groupError = 0;
    GroupMemoryBarrierWithGroupSync();

    uint inputBytes, outputBytes;
    input.GetDimensions(inputBytes);
    output.GetDimensions(outputBytes);
    if (control.Load(0) != 1) {
        RecordError(ERR_HEADER);
        return;
    }
#ifdef USE_WAVE_INTRINSICS
    if (WaveGetLaneCount() != NUM_THREADS) {
        RecordError(ERR_HEADER);
        return;
    }
#endif

    uint streamInPos = control.Load(4);
    uint streamOutPos = control.Load(8);
    if (streamInPos > inputBytes || inputBytes - streamInPos < kStreamHeaderSize) {
        RecordError(ERR_HEADER);
        return;
    }
    TileStream tileStream = TileStream::construct(streamInPos);
    uint numTiles = tileStream.GetNumTiles();
    uint lastTileSize = tileStream.GetLastTileSize();
    if ((tileStream.m_word1 & 0xffffu) != 0xfb04u ||
        (tileStream.m_word2 & 0xfff00003u) != 1u || numTiles == 0 ||
        lastTileSize > kDefaultTileSize) {
        RecordError(ERR_HEADER);
        return;
    }
    if (numTiles > (inputBytes - streamInPos - kStreamHeaderSize) / 4 ||
        groupId.x >= numTiles) {
        RecordError(ERR_TABLE);
        return;
    }

    uint tableBytes = numTiles * 4;
    uint payloadStart = streamInPos + kStreamHeaderSize + tableBytes;
    TileParams params = tileStream.GetTileParams(streamInPos, streamOutPos, groupId.x);
    uint totalOut = (numTiles - 1) * kDefaultTileSize + lastTileSize;
    if (params.inPos < payloadStart || params.inPos > inputBytes ||
        params.inSize > inputBytes - params.inPos) {
        RecordError(ERR_TABLE);
        return;
    }
    if (streamOutPos > outputBytes || totalOut > outputBytes - streamOutPos ||
        params.outPos < streamOutPos || params.outPos > outputBytes ||
        params.outSize > outputBytes - params.outPos ||
        ((params.outPos + params.outSize + 3u) & ~3u) > outputBytes) {
        RecordError(ERR_OUTPUT_BOUNDS);
        return;
    }
    DecompressTile(params, tid, inputBytes);
}
