// BP16 lossless decode research shader.
// ABI reuses the bounded GDeflate smoke host: input=binding 0, output=2,
// scratch=3. The host must CPU-validate the complete canonical frame first.
ByteAddressBuffer input : register(t0);
RWByteAddressBuffer output : register(u1);
RWByteAddressBuffer scratch : register(u2);

static const uint HeaderBytes = 16;
static const uint DescriptorBytes = 8;
static const uint BlockWords = 128;
static const uint MaxRawBytes = 32u * 1024u * 1024u;

void RecordError(uint bits)
{
    uint ignored;
    scratch.InterlockedOr(0, bits, ignored);
}

uint ExtractValue(uint payloadOffset, uint payloadBytes, uint bitPosition, uint bits)
{
    if (bits == 0) return 0;
    const uint wordIndex = bitPosition >> 5;
    const uint shift = bitPosition & 31;
    const uint byteOffset = wordIndex * 4;
    if (byteOffset > payloadBytes || payloadBytes - byteOffset < 4) return 0;
    uint value = input.Load(payloadOffset + byteOffset) >> shift;
    if (shift + bits > 32)
    {
        if (payloadBytes - byteOffset < 8) return 0;
        value |= input.Load(payloadOffset + byteOffset + 4) << (32 - shift);
    }
    return value & ((1u << bits) - 1u);
}

uint DepositValue(uint gathered, uint varyingMask)
{
    const uint bits = countbits(varyingMask);
    if (varyingMask != 0)
    {
        const uint shift = firstbitlow(varyingMask);
        const uint compactMask = (1u << bits) - 1u;
        if (varyingMask == (compactMask << shift))
            return gathered << shift;

        const uint lowMask = varyingMask & 0x7fffu;
        if ((varyingMask & 0x8000u) != 0 && lowMask != 0)
        {
            const uint lowBits = countbits(lowMask);
            const uint lowShift = firstbitlow(lowMask);
            const uint lowCompactMask = (1u << lowBits) - 1u;
            if (lowMask == (lowCompactMask << lowShift))
                return ((gathered & lowCompactMask) << lowShift) |
                       (((gathered >> lowBits) & 1u) << 15u);
        }
    }

    uint value = 0;
    uint sourceBit = 0;
    [unroll]
    for (uint bit = 0; bit < 16; ++bit)
    {
        const uint bitMask = 1u << bit;
        if (varyingMask & bitMask)
        {
            value |= ((gathered >> sourceBit) & 1u) << bit;
            ++sourceBit;
        }
    }
    return value;
}

[numthreads(256, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint outputWord = dispatchThreadId.x;
    uint inputBytes, outputBytes, scratchBytes;
    input.GetDimensions(inputBytes);
    output.GetDimensions(outputBytes);
    scratch.GetDimensions(scratchBytes);
    if (scratchBytes < 4)
        return;
    if (inputBytes < HeaderBytes || outputBytes < 4)
    {
        RecordError(1u);
        return;
    }

    const uint magic = input.Load(0);
    const uint version = input.Load(4);
    const uint rawBytes = input.Load(8);
    const uint blockCount = input.Load(12);
    if (magic != 0x36315042u || version != 1u || rawBytes == 0 ||
        rawBytes > MaxRawBytes || (rawBytes & 255u) != 0 ||
        blockCount != rawBytes / 256u || rawBytes > outputBytes)
    {
        if (outputWord == 0) RecordError(1u);
        return;
    }
    if (outputWord >= rawBytes / 4u) return;

    const uint block = outputWord / 64u;
    const uint wordInBlock = outputWord % 64u;
    const uint local16 = wordInBlock * 2u;
    const uint tableBytes = blockCount * DescriptorBytes;
    const uint tableEnd = HeaderBytes + tableBytes;
    const uint descriptorOffset = HeaderBytes + block * DescriptorBytes;
    if (descriptorOffset > inputBytes || inputBytes - descriptorOffset < DescriptorBytes ||
        tableEnd > inputBytes)
    {
        RecordError(2u);
        return;
    }

    const uint payloadOffset = input.Load(descriptorOffset);
    const uint packedDescriptor = input.Load(descriptorOffset + 4u);
    const uint base = packedDescriptor & 0xffffu;
    const uint varyingMask = packedDescriptor >> 16;
    const uint bitsPerValue = countbits(varyingMask);
    const uint payloadBytes = 16u * bitsPerValue;
    if ((base & varyingMask) != 0 || payloadOffset < tableEnd ||
        payloadOffset > inputBytes || payloadBytes > inputBytes - payloadOffset)
    {
        RecordError(2u);
        return;
    }

    const uint firstBits = local16 * bitsPerValue;
    const uint first = ExtractValue(payloadOffset, payloadBytes, firstBits, bitsPerValue);
    const uint second = ExtractValue(payloadOffset, payloadBytes,
                                     firstBits + bitsPerValue, bitsPerValue);
    const uint value0 = base | DepositValue(first, varyingMask);
    const uint value1 = base | DepositValue(second, varyingMask);
    output.Store(outputWord * 4u, value0 | (value1 << 16));
}
