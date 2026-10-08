// Research-only fused BP16 decode: one dispatch handles four independent
// canonical frames. Bindings 0..3 are inputs, 4..7 outputs, 8 shared error mask.
[[vk::binding(0, 0)]] ByteAddressBuffer input0;
[[vk::binding(1, 0)]] ByteAddressBuffer input1;
[[vk::binding(2, 0)]] ByteAddressBuffer input2;
[[vk::binding(3, 0)]] ByteAddressBuffer input3;
[[vk::binding(4, 0)]] RWByteAddressBuffer output0;
[[vk::binding(5, 0)]] RWByteAddressBuffer output1;
[[vk::binding(6, 0)]] RWByteAddressBuffer output2;
[[vk::binding(7, 0)]] RWByteAddressBuffer output3;
[[vk::binding(8, 0)]] RWByteAddressBuffer scratch;

static const uint HeaderBytes = 16;
static const uint DescriptorBytes = 8;
static const uint MaxRawBytes = 32u * 1024u * 1024u;

void RecordError(uint bits)
{
    uint ignored;
    scratch.InterlockedOr(0, bits, ignored);
}

uint InputLoad(uint frame, uint offset)
{
    switch (frame)
    {
    case 0: return input0.Load(offset);
    case 1: return input1.Load(offset);
    case 2: return input2.Load(offset);
    default: return input3.Load(offset);
    }
}

void InputDimensions(uint frame, out uint bytes)
{
    switch (frame)
    {
    case 0: input0.GetDimensions(bytes); break;
    case 1: input1.GetDimensions(bytes); break;
    case 2: input2.GetDimensions(bytes); break;
    default: input3.GetDimensions(bytes); break;
    }
}

void OutputDimensions(uint frame, out uint bytes)
{
    switch (frame)
    {
    case 0: output0.GetDimensions(bytes); break;
    case 1: output1.GetDimensions(bytes); break;
    case 2: output2.GetDimensions(bytes); break;
    default: output3.GetDimensions(bytes); break;
    }
}

void OutputStore(uint frame, uint offset, uint value)
{
    switch (frame)
    {
    case 0: output0.Store(offset, value); break;
    case 1: output1.Store(offset, value); break;
    case 2: output2.Store(offset, value); break;
    default: output3.Store(offset, value); break;
    }
}

uint ExtractValue(uint frame, uint payloadOffset, uint payloadBytes,
                  uint bitPosition, uint bits)
{
    if (bits == 0) return 0;
    const uint byteOffset = (bitPosition >> 5) * 4;
    const uint shift = bitPosition & 31;
    if (byteOffset > payloadBytes || payloadBytes - byteOffset < 4) return 0;
    uint value = InputLoad(frame, payloadOffset + byteOffset) >> shift;
    if (shift + bits > 32)
    {
        if (payloadBytes - byteOffset < 8) return 0;
        value |= InputLoad(frame, payloadOffset + byteOffset + 4) << (32 - shift);
    }
    return value & ((1u << bits) - 1u);
}

uint DepositValue(uint gathered, uint varyingMask)
{
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
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID,
            uint3 groupId : SV_GroupID)
{
    const uint frame = groupId.y;
    const uint outputWord = dispatchThreadId.x;
    if (frame >= 4) return;

    uint scratchBytes;
    scratch.GetDimensions(scratchBytes);
    if (scratchBytes < 4) return;
    uint inputBytes, outputBytes;
    InputDimensions(frame, inputBytes);
    OutputDimensions(frame, outputBytes);
    if (inputBytes < HeaderBytes || outputBytes < 4)
    {
        RecordError(1u);
        return;
    }

    const uint magic = InputLoad(frame, 0);
    const uint version = InputLoad(frame, 4);
    const uint rawBytes = InputLoad(frame, 8);
    const uint blockCount = InputLoad(frame, 12);
    if (magic != 0x36315042u || version != 1u || rawBytes == 0 ||
        rawBytes > MaxRawBytes || (rawBytes & 255u) != 0 ||
        blockCount != rawBytes / 256u || rawBytes > outputBytes)
    {
        if (outputWord == 0) RecordError(1u);
        return;
    }
    if (outputWord >= rawBytes / 4u) return;

    const uint block = outputWord / 64u;
    const uint local16 = (outputWord % 64u) * 2u;
    const uint tableEnd = HeaderBytes + blockCount * DescriptorBytes;
    const uint descriptorOffset = HeaderBytes + block * DescriptorBytes;
    if (descriptorOffset > inputBytes || inputBytes - descriptorOffset < DescriptorBytes ||
        tableEnd > inputBytes)
    {
        RecordError(2u);
        return;
    }

    const uint payloadOffset = InputLoad(frame, descriptorOffset);
    const uint packedDescriptor = InputLoad(frame, descriptorOffset + 4u);
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
    const uint first = ExtractValue(frame, payloadOffset, payloadBytes, firstBits, bitsPerValue);
    const uint second = ExtractValue(frame, payloadOffset, payloadBytes,
                                     firstBits + bitsPerValue, bitsPerValue);
    const uint value0 = base | DepositValue(first, varyingMask);
    const uint value1 = base | DepositValue(second, varyingMask);
    OutputStore(frame, outputWord * 4u, value0 | (value1 << 16));
}
