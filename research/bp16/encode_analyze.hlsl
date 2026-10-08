// Research-only BP16 encoder pass 1. One invocation analyzes one 256-byte block.
ByteAddressBuffer raw : register(t0);
RWByteAddressBuffer metadata : register(u0);

[numthreads(256, 1, 1)]
void AnalyzeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint block = dispatchThreadId.x;
    uint rawBytes, metadataBytes;
    raw.GetDimensions(rawBytes);
    metadata.GetDimensions(metadataBytes);
    const uint blockCount = rawBytes / 256u;
    if (block >= blockCount || block * 4u > metadataBytes || metadataBytes - block * 4u < 4u)
        return;

    const uint baseOffset = block * 256u;
    uint first = raw.Load(baseOffset);
    uint andValue = first & 0xffffu;
    uint orValue = andValue;
    uint high = first >> 16;
    andValue &= high;
    orValue |= high;
    [loop]
    for (uint i = 1; i < 64u; ++i)
    {
        const uint pair = raw.Load(baseOffset + i * 4u);
        const uint low = pair & 0xffffu;
        const uint upper = pair >> 16;
        andValue &= low;
        andValue &= upper;
        orValue |= low;
        orValue |= upper;
    }
    const uint varying = (orValue ^ andValue) & 0xffffu;
    metadata.Store(block * 4u, (varying << 16) | (andValue & 0xffffu));
}
