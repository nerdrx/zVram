#pragma once

#include <hip/hip_runtime_api.h>
#include <cstddef>

// Experimental explicit lifecycle for zVram-owned HIP VMM allocations.
// The caller must serialize all application HIP/HSA activity across hibernate
// and resume. No application may use a cold pointer until resume succeeds.
// GPU virtual addresses remain reserved. Released physical backing stops
// consuming the resident caps; resume can fail if another allocation uses it.
// Native and mapped-host allocations are outside this compression path.
//
// Budget refusal before eviction keeps allocations hot. A later copy, mapping,
// or cleanup failure may leave a mixture of snapshots and physical backing;
// the complete data remains owned, but the caller must resume or free it.
// No automatic kernel-launch interception or fault-driven paging is provided.
extern "C" {
// Limit the aggregate allocated cold payload; metadata and bounded codec/copy
// scratch are separate. Compression is lossless Zstd with raw fallback.
hipError_t zvramHipHibernate(std::size_t coldBudgetBytes);
hipError_t zvramHipResume();
// Logical bytes count whole allocations until every segment is restored.
// Stored bytes count the currently retained snapshot payload.
hipError_t zvramHipColdBytes(std::size_t* logicalColdBytes, std::size_t* storedColdBytes);
}
