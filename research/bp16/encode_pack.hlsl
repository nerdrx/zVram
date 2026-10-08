// Research-only BP16 encoder pass 2. Host writes canonical header/descriptors
// after the analysis fence; invocations independently fill packed payload dwords.
ByteAddressBuffer raw : register(t0);
RWByteAddressBuffer frame : register(u1);

uint GatherBits(uint value, uint mask)
{
    uint gathered = 0u;
    uint destinationBit = 0u;
    [loop]
    while (mask != 0u)
    {
        const uint bit = firstbitlow(mask);
        gathered |= ((value >> bit) & 1u) << destinationBit;
        mask &= mask - 1u;
        ++destinationBit;
    }
    return gathered;
}

[numthreads(256, 1, 1)]
void PackMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint packedWord = dispatchThreadId.x;
    uint rawBytes, frameBytes;
    raw.GetDimensions(rawBytes);
    frame.GetDimensions(frameBytes);
    const uint blockCount = rawBytes / 256u;
    if (packedWord >= blockCount * 64u || frameBytes < 16u)
        return;

    const uint block = packedWord / 64u;
    const uint wordInBlock = packedWord % 64u;
    const uint descriptorOffset = 16u + block * 8u;
    if (descriptorOffset > frameBytes || frameBytes - descriptorOffset < 8u)
        return;
    const uint payloadOffset = frame.Load(descriptorOffset);
    const uint mask = frame.Load(descriptorOffset + 4u) >> 16;
    const uint bitsPerValue = countbits(mask);
    const uint payloadDwords = bitsPerValue * 4u;
    if (wordInBlock >= payloadDwords)
        return;
    if (payloadOffset > frameBytes || payloadDwords * 4u > frameBytes - payloadOffset)
        return;

    const uint inputOffset = block * 256u + wordInBlock * 4u;
    if (bitsPerValue == 16u)
    {
        frame.Store(payloadOffset + wordInBlock * 4u, raw.Load(inputOffset));
        return;
    }
    if (bitsPerValue == 0u)
        return;

    const uint firstBit = wordInBlock * 32u;
    uint sourceValue = firstBit / bitsPerValue;
    uint sourceBit = firstBit % bitsPerValue;
    uint written = 0u;
    uint packed = 0u;
    [loop]
    while (written < 32u)
    {
        const uint sourceByte = block * 256u + sourceValue * 2u;
        const uint pair = raw.Load(sourceByte & ~3u);
        const uint sourceWord = (sourceByte & 2u) != 0u ? pair >> 16 : pair & 0xffffu;
        const uint gathered = GatherBits(sourceWord, mask);
        const uint available = bitsPerValue - sourceBit;
        const uint take = min(available, 32u - written);
        const uint part = (gathered >> sourceBit) & ((1u << take) - 1u);
        packed |= part << written;
        written += take;
        ++sourceValue;
        sourceBit = 0u;
    }
    frame.Store(payloadOffset + wordInBlock * 4u, packed);
}
