#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <memory>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
#include <tuple>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <unistd.h>
#include <zstd.h>
#include "auto_queues.hpp"
#include "buffer_barriers.hpp"
#include "submission_tracking.hpp"
#include "active_refs.hpp"
#include "compression_policy.hpp"
#include "clean_cache_policy.hpp"
#include "snapshot_decode.hpp"
#include "resident_budget.hpp"
#include "snapshot_pipeline.hpp"
#include "gdeflate_gpu.hpp"

namespace {
constexpr char kLayer[] = "VK_LAYER_NX_zvram";
#ifdef VK_KHR_internally_synchronized_queues
constexpr VkDeviceQueueCreateFlags kAllowedQueueCreateFlags =
    VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
#else
constexpr VkDeviceQueueCreateFlags kAllowedQueueCreateFlags = 0;
#endif
struct Device;
template<class H> void* key(H h) { return h ? *reinterpret_cast<void**>(h) : nullptr; }
template<class H> H tokenHandle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<H>) return reinterpret_cast<H>(value);
    else return static_cast<H>(value);
}
template<class H> std::uintptr_t handleToken(H value) {
    if constexpr (std::is_pointer_v<H>) return reinterpret_cast<std::uintptr_t>(value);
    else return static_cast<std::uintptr_t>(value);
}
bool verbose() { const char* p=std::getenv("ZVRAM_VERBOSE"); return p && std::strcmp(p,"1")==0; }
void logf(const char* fmt,...) { std::fputs("[zvram] ",stderr); va_list ap; va_start(ap,fmt); std::vfprintf(stderr,fmt,ap); va_end(ap); std::fputc('\n',stderr); }
void logSnapshotState(const char* event,Device& d);

struct PhysicalMemoryView {
    VkPhysicalDeviceMemoryProperties native{};
    std::uint32_t virtualType{UINT32_MAX};
    std::uint32_t virtualHeap{UINT32_MAX};
    VkDeviceSize virtualBytes{};
    bool enabled{};
    bool radvDiscrete{};
};
struct Instance {
    VkInstance handle{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_GetPhysicalDeviceProcAddr physProc{};
    PFN_vkDestroyInstance destroy{};
    PFN_vkEnumerateDeviceExtensionProperties enumerateExtensions{};
    PFN_vkGetPhysicalDeviceProperties properties{};
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties{};
    PFN_vkGetPhysicalDeviceMemoryProperties2 memoryProperties2{};
    bool properties2Enabled{};
    std::uint32_t apiVersion{VK_API_VERSION_1_0};
    PFN_vkGetPhysicalDeviceFeatures features{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queueFamilies{};
    std::mutex physicalMutex;
    std::unordered_map<VkPhysicalDevice,PhysicalMemoryView> physicalViews;
};
struct Allocation {
    VkDeviceSize size{};
    bool local{};
    std::uint32_t type{};
    bool bound{};
    VkDeviceMemory nativeHandle{};
    void* token{};
    bool wrapped{};
    VkAllocationCallbacks callbacks{};
    bool hasCallbacks{};
    VkMemoryAllocateFlags flags{};
    float priority{0.5f};
    bool hasPriority{};
    bool adoptable{};
};
struct VirtualMemory {
    VkDeviceSize size{};
    std::uint64_t identityGeneration{};
    std::uint64_t bindingGeneration{};
    VkMemoryAllocateFlags allocationFlags{};
    float priority{0.5f};
    bool hasPriority{};
    VkDeviceSize residentBytes{};
    std::uint32_t backingMemoryTypeBits{};
    std::uint32_t nativeTypeBits{};
    std::vector<VkDeviceMemory> children;
    std::vector<VkDeviceSize> childSizes;
    std::vector<std::uint32_t> childTypes;
    std::vector<std::uint64_t> childGenerations;
    struct ColdChunk {
        using Imported = zvram::gdeflate::gpu::Decoder::ImportedHostInputPtr;
        std::vector<std::uint8_t> bytes;
        Imported imported;
        VkDeviceSize rawSize{};
        bool compressed{};
        bool hostInputUsed{};
        unsigned byteShuffle{};
        zvram::snapshot::Codec codec{zvram::snapshot::Codec::Zstd};
        const std::uint8_t* data() const noexcept { return imported ? imported->data() : bytes.data(); }
        std::size_t size() const noexcept { return imported ? imported->encodedBytes : bytes.size(); }
    };
    struct ColdGroup {
        std::vector<ColdChunk> chunks;
        VkDeviceSize logicalBytes{};
        VkDeviceSize storedBytes{};
        VkDeviceSize importedPaddingBytes{};
        std::uint64_t failedBudgetGeneration{};
        std::uint64_t failedBudgetSubmissionGeneration{};
        bool cold{};
        bool pristine{}; // Never submitted: undefined contents need no snapshot.
        bool restoreBound{};
        bool budgetBlocked{};
        std::uint64_t writeEpoch{};
        std::chrono::steady_clock::time_point lastUse{std::chrono::steady_clock::now()};
        VkDeviceSize accountedBytes() const noexcept { return storedBytes + importedPaddingBytes; }
    };
    struct Binding { VkBuffer buffer{}; VkDeviceSize memoryOffset{},size{},alignment{}; };
    std::vector<Binding> bindings;
    std::vector<VkBuffer> poolViews;
    std::vector<VkDeviceSize> poolViewAlignments;
    std::vector<std::uint32_t> poolViewMemoryTypeBits;
    std::vector<ColdGroup> coldGroups;
    VkDeviceSize coldStoredBytes{};
    VkDeviceSize cacheStoredBytes{};
    VkDeviceSize coldLogicalSize{};
    bool cold{};
    bool capacityAccounted{};
    bool trackPhysicalStats{};
    VkBuffer buffer{};
    void* token{};
    bool bound{};
    bool everBound{};
    bool deferredFree{};
    std::chrono::steady_clock::time_point lastUse{std::chrono::steady_clock::now()};
    VkAllocationCallbacks adoptedCallbacks{};
    bool hasAdoptedCallbacks{};
};
struct ZvramSnapshotStatsNX {
    std::uint32_t structSize{};
    std::uint32_t version{};
    std::uint64_t coldLogicalBytes{};
    std::uint64_t coldStoredBytes{};
    std::uint64_t coldBudgetBytes{};
    std::uint64_t residentBytes{};
    std::uint64_t freezes{};
    std::uint64_t restores{};
    std::uint64_t failures{};
    std::int32_t lastError{};
};
struct SnapshotResources {
    PFN_vkDeviceWaitIdle deviceWaitIdle{};
    PFN_vkCreateCommandPool createCommandPool{};
    PFN_vkDestroyCommandPool destroyCommandPool{};
    PFN_vkResetCommandPool resetCommandPool{};
    PFN_vkAllocateCommandBuffers allocateCommandBuffers{};
    PFN_vkBeginCommandBuffer beginCommandBuffer{};
    PFN_vkEndCommandBuffer endCommandBuffer{};
    PFN_vkCmdCopyBuffer cmdCopyBuffer{};
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier{};
    PFN_vkQueueSubmit queueSubmit{};
    PFN_vkCreateBuffer createBuffer{};
    PFN_vkDestroyBuffer destroyBuffer{};
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements{};
    PFN_vkAllocateMemory allocateMemory{};
    PFN_vkFreeMemory freeMemory{};
    PFN_vkBindBufferMemory bindBufferMemory{};
    PFN_vkMapMemory mapMemory{};
    PFN_vkUnmapMemory unmapMemory{};
    VkCommandPool commandPool{};
    VkCommandBuffer commandBuffer{};
    VkBuffer stagingBuffer{};
    VkDeviceMemory stagingMemory{};
    VkDeviceSize chunkSize{32u*1024u*1024u};
    VkDeviceSize stagingSize{32u*1024u*1024u};
    std::uint64_t copyCalls{}, copyBytes{}, copyNanoseconds{}, decodeBytes{}, decodeNanoseconds{};
    void* mapped{};
    VkBuffer lookaheadBuffer{};
    VkDeviceMemory lookaheadMemory{};
    void* lookaheadMapped{};
    std::uint64_t prefetchLaunches{}, preparedRestores{}, prefetchWaitNanoseconds{};
};
struct PromotedBuffer {
    VkDeviceSize size{};
    VkDeviceSize alignment{};
    VkMemoryRequirements requirements{};
    VkDeviceMemory memory{};
    bool synthetic{};
    bool deviceAddress{};
    bool concurrentForced{};
};
struct Device {
    VkDevice handle{};
    VkPhysicalDevice physical{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkDestroyDevice destroy{};
    PFN_vkAllocateMemory allocate{};
    PFN_vkFreeMemory free{};
    PFN_vkCreateBuffer createBuffer{};
    PFN_vkDestroyBuffer destroyBuffer{};
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements{};
    PFN_vkGetBufferMemoryRequirements2 getBufferMemoryRequirements2{};
    PFN_vkBindBufferMemory bindBufferMemory{};
    PFN_vkBindBufferMemory2 bindBufferMemory2{};
    PFN_vkQueueBindSparse queueBindSparse{};
    PFN_vkQueueWaitIdle queueWaitIdle{};
    PFN_vkGetDeviceQueue getDeviceQueue{};
    PFN_vkGetDeviceQueue2 getDeviceQueue2{};
    PFN_vkSetDeviceLoaderData setDeviceLoaderData{};
    std::unordered_map<VkDeviceMemory,VirtualMemory> virtualMemory;
    std::unordered_map<VkBuffer,PromotedBuffer> promotedBuffers;
    std::unordered_map<VkDeviceMemory,Allocation> allocations;
    VkQueue sparseQueue{};
    VkQueue copyQueue{};
    std::vector<VkQueue> appQueues;
    std::vector<std::uint32_t> queueFamilies;
    ZvramAutoQueues autoQueues;
    std::uint32_t virtualType{UINT32_MAX};
    std::uint32_t virtualHeap{UINT32_MAX};
    VkDeviceSize virtualBytes{};
    bool virtualEnabled{};
    bool bufferDeviceAddressEnabled{};
    bool nativeWrappingAllowed{true};
    std::uint64_t virtualUsage{};
    bool autoEnabled{};
    bool autoInitialized{};
    bool bufferPresentation{};
    bool asyncCompression{};
    bool asyncFreezePending{};
    std::uint64_t nextVirtualIdentity{1};
    std::atomic<bool> selectiveRestore{false};
    bool reportedAccessFallback{};
    VkSubmissionTracker submission;
    ActiveRefs activeRefs;
    bool activeEviction{};
    VkDeviceSize rangeChunkBytes{};
    VkDeviceSize residentLimitBytes{};
    PFN_vkGetPhysicalDeviceMemoryProperties2 budgetProperties{};
    std::uint32_t budgetHeap{UINT32_MAX};
    VkDeviceSize budgetReserveBytes{};
    VkDeviceSize lastBudgetLimit{std::numeric_limits<VkDeviceSize>::max()};
    bool lazyBacking{};
    bool residentAdmissionArmed{true};
    bool restoreBudgetRefused{};
    bool cleanCache{};
    zvram::clean_cache::Policy cleanCachePolicy{zvram::clean_cache::Policy::First};
    bool mruEviction{};
    unsigned minSavingsPercent{};
    unsigned byteShuffle{};
    zvram::snapshot::Codec snapshotCodec{zvram::snapshot::Codec::Zstd};
    unsigned gdeflateWorkers{1}, bp16Workers{1};
    bool gpuProfileEnabled{};
    std::atomic<std::uint64_t> gpuProfileAllocCalls{}, gpuProfileAllocSuccess{}, gpuProfileAllocFailures{}, gpuProfileAllocNs{};
    std::atomic<std::uint64_t> gpuProfileFreeCalls{}, gpuProfileFreeNs{};
    std::atomic<std::uint64_t> gpuProfileSparseCalls{}, gpuProfileSparseSuccess{}, gpuProfileSparseFailures{}, gpuProfileSparseNs{};
    bool gpuRestoreEnabled{}, gpuRestoreUnsafe{};
    bool gpuImportHostInput{};
    bool gpuAllocatedHostInput{};
    std::uint64_t gpuAllocatedHostBudgetBytes{
        zvram::gdeflate::gpu::Decoder::DefaultAllocatedHostBudgetBytes};
    VkDeviceSize gpuImportHostAlignment{};
    std::uint64_t gpuImportedFrames{}, gpuImportedReuses{}, gpuImportedBytes{};
    std::uint64_t gpuAllocatedHostAllocations{}, gpuAllocatedHostReuses{}, gpuAllocatedHostBytes{};
    std::uint64_t gpuProfileLogRestoreCount{};
    std::uint64_t lastGpuProfileLoggedCalls{};
    VkDeviceSize gpuStorageAlignment{1};
    VkDeviceSize gpuStorageRange{};
    std::uint64_t gpuDecodeCalls{}, gpuDecodeBytes{}, gpuDecodeNanoseconds{}, gpuDecodeFallbacks{};
    VkPhysicalDeviceProperties gpuProperties{};
    std::uint32_t gpuTimestampBits{};
    std::unique_ptr<zvram::gdeflate::gpu::Decoder> gpuDecoder;
    bool narrowDescriptorRanges{};
    VkDeviceSize robustRangeAlignment{1};
    std::uint64_t restoreGeneration{};
    std::vector<std::pair<VkQueue,std::uint64_t>> restoreQueueGenerations;
    std::atomic<VkResult> gpuGateError{VK_SUCCESS};
    std::uint64_t idleMilliseconds{};
    std::uint64_t coldBudget{};
    std::uint64_t coldBytes{};
    std::uint64_t cacheBytes{};
    std::uint64_t cleanReuseCount{}, cacheInvalidations{};
    std::uint64_t coldLogicalBytes{};
    std::uint64_t residentBytes{};
    std::uint64_t freezeCount{}, restoreCount{}, snapshotFailures{};
    std::uint64_t gpuSubmissionGeneration{};
    std::uint64_t coldBudgetGeneration{};
    std::uint64_t restoreFailureAfterGroups{};
    bool restoreFailureInjected{};
    VkResult lastSnapshotError{VK_SUCCESS};
    std::atomic<bool> stopWorker{false};
    std::condition_variable activity;
    std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
    std::thread snapshotWorker;
    SnapshotResources snapshot;
    VkPhysicalDeviceMemoryProperties memory{};
    std::string gpu;
    bool autoPolicy{};
    std::mutex mutex;
    // ponytail: one host queue lock per virtual device; use per-queue locks if
    // simultaneous queue host calls become a measured bottleneck.
    std::mutex queueMutex;
    uint64_t liveLocal{}, peakLocal{}, liveOther{}, peakOther{}, failures{};
};
struct AsyncFreezeToken {
    VkDeviceMemory memory{};
    std::uint64_t allocationGeneration{};
    std::size_t child{};
    VkDeviceMemory backing{};
    std::uint64_t childGeneration{};
    std::uint64_t bindingGeneration{};
    std::uint64_t writeEpoch{};
};
void bumpAsyncVersion(Device& d,std::uint64_t& version) {
    if(version==std::numeric_limits<std::uint64_t>::max()) d.asyncCompression=false;
    else ++version;
}
bool assignVirtualIdentity(Device& d,VirtualMemory& memory) {
    if(!d.nextVirtualIdentity) { d.asyncCompression=false; return false; }
    memory.identityGeneration=d.nextVirtualIdentity;
    if(d.nextVirtualIdentity==std::numeric_limits<std::uint64_t>::max()) {
        d.nextVirtualIdentity=0; d.asyncCompression=false;
    } else ++d.nextVirtualIdentity;
    return true;
}
bool asyncFreezeTokenValid(const Device& d,const AsyncFreezeToken& token) {
    if(!d.asyncCompression || !d.autoEnabled || !d.activeEviction || d.stopWorker.load() ||
       d.gpuGateError!=VK_SUCCESS) return false;
    const auto found=d.virtualMemory.find(token.memory);
    if(found==d.virtualMemory.end()) return false;
    const auto& memory=found->second;
    if(memory.identityGeneration!=token.allocationGeneration ||
       memory.bindingGeneration!=token.bindingGeneration ||
       token.child>=memory.children.size() || token.child>=memory.childGenerations.size() ||
       token.child>=memory.coldGroups.size()) return false;
    const auto& group=memory.coldGroups[token.child];
    return memory.children[token.child]==token.backing && token.backing &&
           memory.childGenerations[token.child]==token.childGeneration && !group.cold &&
           !group.pristine && group.writeEpoch==token.writeEpoch &&
           !d.activeRefs.busy(token.memory,token.child);
}
bool asyncColdBudgetFits(const Device& d,VkDeviceSize bytes) {
    if(bytes>d.coldBudget || d.coldBytes>d.coldBudget || d.cacheBytes>d.coldBudget-d.coldBytes) return false;
    return bytes<=d.coldBudget-d.coldBytes-d.cacheBytes;
}
template<class Callback>
void runAsyncEncoderUnlocked(std::unique_lock<std::mutex>& deviceLock,
                             std::unique_lock<std::mutex>& queueLock,Callback&& callback) {
    queueLock.unlock(); deviceLock.unlock();
    try { callback(); }
    catch(...) { deviceLock.lock(); queueLock.lock(); throw; }
    deviceLock.lock(); queueLock.lock();
}
void logGpuProfileSummary(Device& d,const char* suffix,bool force=false) {
    bool newProfile=false;
    if(d.gpuDecoder && d.gpuDecoder->profilingEnabled()) {
        const auto profile=d.gpuDecoder->profile();
        newProfile=profile.calls>d.lastGpuProfileLoggedCalls;
        if(newProfile || force) {
            logf("GPU restore host profile calls=%llu validation-ns=%llu input-prepare-ns=%llu submit-wait-ns=%llu%s",
                 static_cast<unsigned long long>(profile.calls),static_cast<unsigned long long>(profile.validationNs),
                 static_cast<unsigned long long>(profile.inputPrepareNs),static_cast<unsigned long long>(profile.submitWaitNs),suffix);
            if(d.snapshotCodec==zvram::snapshot::Codec::BP16)
                logf("GPU BP16 upload profile buffer-prepare-ns=%llu direct-copy-ns=%llu%s",
                     static_cast<unsigned long long>(profile.bp16BufferPrepareNs),
                     static_cast<unsigned long long>(profile.bp16DirectCopyNs),suffix);
            logf("GPU restore device profile samples=%llu transfer-ns=%llu decode-ns=%llu finish-ns=%llu%s",
                 static_cast<unsigned long long>(profile.gpuSamples),static_cast<unsigned long long>(profile.gpuTransferNs),
                 static_cast<unsigned long long>(profile.gpuDecodeNs),static_cast<unsigned long long>(profile.gpuFinishNs),suffix);
            d.lastGpuProfileLoggedCalls=profile.calls;
        }
    }
    if(d.gpuProfileEnabled && (newProfile || force))
        logf("GPU restore backing profile alloc-calls=%llu alloc-ok=%llu alloc-fail=%llu alloc-ns=%llu free-calls=%llu free-ns=%llu sparse-calls=%llu sparse-ok=%llu sparse-fail=%llu sparse-ns=%llu%s",
             static_cast<unsigned long long>(d.gpuProfileAllocCalls.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(d.gpuProfileAllocSuccess.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(d.gpuProfileAllocFailures.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(d.gpuProfileAllocNs.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(d.gpuProfileFreeCalls.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(d.gpuProfileFreeNs.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(d.gpuProfileSparseCalls.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(d.gpuProfileSparseSuccess.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(d.gpuProfileSparseFailures.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(d.gpuProfileSparseNs.load(std::memory_order_relaxed)),suffix);
}
void logSnapshotState(const char* event,Device& d) {
    logf("snapshot state event=%s resident=%llu cold-logical=%llu cold-stored=%llu freezes=%llu restores=%llu failures=%llu cache-stored=%llu clean-reuses=%llu cache-invalidations=%llu copy-calls=%llu copy-bytes=%llu copy-ns=%llu decode-bytes=%llu decode-ns=%llu pipeline-prefetches=%llu pipeline-restores=%llu pipeline-wait-ns=%llu gpu-decode-calls=%llu gpu-decode-bytes=%llu gpu-decode-ns=%llu gpu-decode-fallbacks=%llu",
         event,static_cast<unsigned long long>(d.residentBytes),static_cast<unsigned long long>(d.coldLogicalBytes),
         static_cast<unsigned long long>(d.coldBytes),static_cast<unsigned long long>(d.freezeCount),
         static_cast<unsigned long long>(d.restoreCount),static_cast<unsigned long long>(d.snapshotFailures),
         static_cast<unsigned long long>(d.cacheBytes),static_cast<unsigned long long>(d.cleanReuseCount),
         static_cast<unsigned long long>(d.cacheInvalidations),static_cast<unsigned long long>(d.snapshot.copyCalls),
         static_cast<unsigned long long>(d.snapshot.copyBytes),static_cast<unsigned long long>(d.snapshot.copyNanoseconds),
         static_cast<unsigned long long>(d.snapshot.decodeBytes),static_cast<unsigned long long>(d.snapshot.decodeNanoseconds),
         static_cast<unsigned long long>(d.snapshot.prefetchLaunches),static_cast<unsigned long long>(d.snapshot.preparedRestores),
         static_cast<unsigned long long>(d.snapshot.prefetchWaitNanoseconds),
         static_cast<unsigned long long>(d.gpuDecodeCalls),static_cast<unsigned long long>(d.gpuDecodeBytes),
         static_cast<unsigned long long>(d.gpuDecodeNanoseconds),static_cast<unsigned long long>(d.gpuDecodeFallbacks));
    bool logGpuDiagnostics=false;
    if(std::strcmp(event,"restore")==0) {
        const auto sample=++d.gpuProfileLogRestoreCount;
        logGpuDiagnostics=sample==1 || sample%256==0;
    }
    if(logGpuDiagnostics) logGpuProfileSummary(d,"");
    if(d.gpuImportHostInput && logGpuDiagnostics)
        logf("GPU BP16 imported input imports=%llu reuses=%llu bytes=%llu",
             static_cast<unsigned long long>(d.gpuImportedFrames),
             static_cast<unsigned long long>(d.gpuImportedReuses),
             static_cast<unsigned long long>(d.gpuImportedBytes));
    if(d.gpuAllocatedHostInput && logGpuDiagnostics)
        logf("GPU BP16 allocated input allocations=%llu reuses=%llu bytes=%llu live-bytes=%llu limit-bytes=%llu",
             static_cast<unsigned long long>(d.gpuAllocatedHostAllocations),
             static_cast<unsigned long long>(d.gpuAllocatedHostReuses),
             static_cast<unsigned long long>(d.gpuAllocatedHostBytes),
             static_cast<unsigned long long>(d.gpuDecoder?d.gpuDecoder->allocatedHostInputUsedBytes():0),
             static_cast<unsigned long long>(d.gpuDecoder?d.gpuDecoder->allocatedHostInputLimitBytes():0));
}
std::mutex mapsMutex;
// Future command entry points absent from our build cannot be tracked safely.
std::atomic<bool> unknownCommandProc{false};
PFN_vkGetInstanceProcAddr globalGipa{};
std::unordered_map<void*,std::shared_ptr<Instance>> instances;
std::unordered_map<void*,std::shared_ptr<Device>> devices;

std::shared_ptr<Instance> findInstance(void* k) {
    std::lock_guard<std::mutex> lock(mapsMutex); auto i=instances.find(k); return i==instances.end()?nullptr:i->second;
}
std::shared_ptr<Device> findDevice(VkDevice d) {
    std::lock_guard<std::mutex> lock(mapsMutex); auto i=devices.find(key(d)); return i==devices.end()?nullptr:i->second;
}
VkDeviceMemory nativeMemoryLocked(const Device& d,VkDeviceMemory memory) {
    const auto found=d.allocations.find(memory);
    return found!=d.allocations.end() && found->second.wrapped ? found->second.nativeHandle : memory;
}
VkLayerInstanceCreateInfo* instanceLink(const VkInstanceCreateInfo* ci) {
    for(auto* p=static_cast<const VkBaseInStructure*>(ci->pNext);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            auto* l=reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<VkBaseInStructure*>(p));
            if(l->function==VK_LAYER_LINK_INFO) return l;
        }
    return nullptr;
}
VkLayerDeviceCreateInfo* deviceLink(const VkDeviceCreateInfo* ci) {
    for(auto* p=static_cast<const VkBaseInStructure*>(ci->pNext);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* l=reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<VkBaseInStructure*>(p));
            if(l->function==VK_LAYER_LINK_INFO) return l;
        }
    return nullptr;
}
bool hasPolicy(const void* chain) {
    for(auto* p=static_cast<const VkBaseInStructure*>(chain);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_DEVICE_MEMORY_OVERALLOCATION_CREATE_INFO_AMD) return true;
    return false;
}
bool appEnabled(const VkDeviceCreateInfo* ci,const char* name) {
    for(uint32_t i=0;i<ci->enabledExtensionCount;i++) if(std::strcmp(ci->ppEnabledExtensionNames[i],name)==0) return true;
    return false;
}
bool enabledBufferDeviceAddress(const VkDeviceCreateInfo* ci) {
    for(auto* p=static_cast<const VkBaseInStructure*>(ci->pNext);p;p=p->pNext) {
#ifdef VK_VERSION_1_2
        if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES &&
           reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(p)->bufferDeviceAddress) return true;
#endif
        if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES &&
           reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(p)->bufferDeviceAddress) return true;
    }
    return false;
}

VKAPI_ATTR VkResult VKAPI_CALL layerCreateInstance(const VkInstanceCreateInfo*,const VkAllocationCallbacks*,VkInstance*);
VKAPI_ATTR void VKAPI_CALL layerDestroyInstance(VkInstance,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL layerCreateDevice(VkPhysicalDevice,const VkDeviceCreateInfo*,const VkAllocationCallbacks*,VkDevice*);
VKAPI_ATTR void VKAPI_CALL layerDestroyDevice(VkDevice,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL layerAllocateMemory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory*);
VKAPI_ATTR void VKAPI_CALL layerFreeMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*);
bool initSnapshotResources(Device&,std::uint32_t);
void releaseSnapshotResources(Device&);
void snapshotWorkerLoop(const std::shared_ptr<Device>&);
VkResult restoreColdLocked(VkDevice,Device&,VkDeviceMemory only=VK_NULL_HANDLE,std::size_t childOnly=SIZE_MAX,
                          VkBuffer preparedBuffer=VK_NULL_HANDLE,VkDeviceSize preparedBytes=0);
VkResult freezeChildLocked(Device&,VirtualMemory&,std::size_t);
void discardCleanCacheLocked(Device&,VirtualMemory&,std::size_t);
std::vector<std::uint32_t> backingMemoryTypes(const Device&,const VkMemoryRequirements&);
VkResult allocateBackingChild(Device&,VkDevice,VkDeviceSize,std::uint32_t,VkMemoryAllocateFlags,bool,float,VkDeviceMemory*);
void releaseChildren(Device&,VirtualMemory&);
void trackBackingAllocation(Device&,std::uint32_t,VkDeviceSize);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetInstanceProcAddr(VkInstance,const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(VkDevice,const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetPhysicalDeviceProcAddr(VkInstance,const char*);
VKAPI_ATTR void VKAPI_CALL layerGetPhysicalDeviceMemoryProperties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties*);
VKAPI_ATTR void VKAPI_CALL layerGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties2*);
VKAPI_ATTR VkResult VKAPI_CALL layerCreateBuffer(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer*);
VKAPI_ATTR void VKAPI_CALL layerDestroyBuffer(VkDevice,VkBuffer,const VkAllocationCallbacks*);
VKAPI_ATTR void VKAPI_CALL layerGetBufferMemoryRequirements(VkDevice,VkBuffer,VkMemoryRequirements*);
VKAPI_ATTR void VKAPI_CALL layerGetBufferMemoryRequirements2(VkDevice,const VkBufferMemoryRequirementsInfo2*,VkMemoryRequirements2*);
VKAPI_ATTR VkResult VKAPI_CALL layerBindBufferMemory(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize);
VKAPI_ATTR VkResult VKAPI_CALL layerBindBufferMemory2(VkDevice,std::uint32_t,const VkBindBufferMemoryInfo*);
VKAPI_ATTR VkResult VKAPI_CALL layerMapMemory(VkDevice,VkDeviceMemory,VkDeviceSize,VkDeviceSize,VkMemoryMapFlags,void**);
VKAPI_ATTR VkResult VKAPI_CALL layerBindImageMemory(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize);
VKAPI_ATTR VkResult VKAPI_CALL layerBindImageMemory2(VkDevice,std::uint32_t,const VkBindImageMemoryInfo*);
VKAPI_ATTR void VKAPI_CALL layerGetDeviceMemoryCommitment(VkDevice,VkDeviceMemory,VkDeviceSize*);
VKAPI_ATTR VkResult VKAPI_CALL layerGetSnapshotStats(VkDevice,ZvramSnapshotStatsNX*);
VKAPI_ATTR VkResult VKAPI_CALL layerArmRestoreFailure(VkDevice,std::uint32_t);
VKAPI_ATTR VkResult VKAPI_CALL layerAllocateMemory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory*);
VKAPI_ATTR void VKAPI_CALL layerFreeMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*);

std::uint64_t configuredVirtualBytes() {
    const char* value=std::getenv("ZVRAM_VULKAN_VIRTUAL_MIB");
    if(!value || !*value) return 0;
    char* end=nullptr; errno=0;
    const unsigned long long mib=std::strtoull(value,&end,10);
    if(errno || end==value || *end || mib==0 || mib>std::numeric_limits<VkDeviceSize>::max()/(1024ull*1024ull)) return 0;
    return mib*1024ull*1024ull;
}
std::uint64_t positiveEnv(const char* name,std::uint64_t max=std::numeric_limits<std::uint64_t>::max()) {
    const char* value=std::getenv(name); if(!value || !*value) return 0;
    char* end=nullptr; errno=0; const auto parsed=std::strtoull(value,&end,10);
    if(errno || end==value || *end || parsed==0 || parsed>max) return 0;
    return parsed;
}
bool snapshotConfig(const Device& d,std::uint64_t& idleMs,std::uint64_t& coldBudget) {
    idleMs=positiveEnv("ZVRAM_VULKAN_AUTO_IDLE_MS",UINT32_MAX);
    const auto coldMiB=positiveEnv("ZVRAM_VULKAN_COLD_MIB");
    if(!idleMs || !coldMiB || coldMiB>std::numeric_limits<std::uint64_t>::max()/(1024ull*1024ull)) return false;
    coldBudget=coldMiB*1024ull*1024ull;
    return coldBudget<=d.virtualBytes && coldBudget<=std::numeric_limits<std::size_t>::max();
}
PhysicalMemoryView physicalView(const std::shared_ptr<Instance>& instance,VkPhysicalDevice physical) {
    std::lock_guard<std::mutex> lock(instance->physicalMutex);
    try {
    auto found=instance->physicalViews.find(physical);
    if(found!=instance->physicalViews.end()) return found->second;
    PhysicalMemoryView view{};
    if(!instance->memoryProperties || !instance->properties || !instance->features || !instance->queueFamilies) return view;
    instance->memoryProperties(physical,&view.native);
    VkPhysicalDeviceProperties props{}; instance->properties(physical,&props);
    const auto configured=configuredVirtualBytes();
    if(!configured || props.deviceType!=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
       std::strstr(props.deviceName,"RADV")==nullptr ||
       view.native.memoryTypeCount>=VK_MAX_MEMORY_TYPES || view.native.memoryHeapCount>=VK_MAX_MEMORY_HEAPS)
        { instance->physicalViews.emplace(physical,view); return view; }
    VkPhysicalDeviceFeatures features{}; instance->features(physical,&features);
    if(!features.sparseBinding) { instance->physicalViews.emplace(physical,view); return view; }
    std::uint32_t count=0; instance->queueFamilies(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    instance->queueFamilies(physical,&count,queues.data());
    if(std::none_of(queues.begin(),queues.end(),[](const auto& q){return (q.queueFlags&VK_QUEUE_SPARSE_BINDING_BIT)!=0;})) { instance->physicalViews.emplace(physical,view); return view; }
    view.virtualType=view.native.memoryTypeCount;
    view.virtualHeap=view.native.memoryHeapCount;
    view.virtualBytes=configured;
    view.enabled=true; view.radvDiscrete=true;
    instance->physicalViews.emplace(physical,view);
    return view;
    } catch(const std::bad_alloc&) { return {}; }
}
void exposeVirtualMemory(const PhysicalMemoryView& view,VkPhysicalDeviceMemoryProperties* props) {
    if(!view.enabled || !props) return;
    props->memoryHeaps[view.virtualHeap]={view.virtualBytes,VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    props->memoryTypes[view.virtualType]={VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,view.virtualHeap};
    props->memoryHeapCount=view.virtualHeap+1;
    props->memoryTypeCount=view.virtualType+1;
}

VKAPI_ATTR VkResult VKAPI_CALL layerCreateInstance(const VkInstanceCreateInfo* ci,const VkAllocationCallbacks* allocator,VkInstance* out) {
    if(!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    auto* link=instanceLink(ci); if(!link || !link->u.pLayerInfo) { logf("missing loader instance link information"); return VK_ERROR_INITIALIZATION_FAILED; }
    auto* info=link->u.pLayerInfo; auto next=info->pfnNextGetInstanceProcAddr; auto nextPhys=info->pfnNextGetPhysicalDeviceProcAddr;
    link->u.pLayerInfo=info->pNext;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE,"vkCreateInstance"));
    if(!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=create(ci,allocator,out); if(r!=VK_SUCCESS) return r;
    try {
        auto s=std::make_shared<Instance>(); s->handle=*out; s->gipa=next; s->physProc=nextPhys;
        if(ci->pApplicationInfo && ci->pApplicationInfo->apiVersion)
            s->apiVersion=ci->pApplicationInfo->apiVersion;
        s->properties2Enabled=ci->pApplicationInfo && ci->pApplicationInfo->apiVersion>=VK_API_VERSION_1_1;
        for(std::uint32_t i=0;i<ci->enabledExtensionCount;++i)
            if(std::strcmp(ci->ppEnabledExtensionNames[i],VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)==0)
                s->properties2Enabled=true;
        s->destroy=reinterpret_cast<PFN_vkDestroyInstance>(next(*out,"vkDestroyInstance"));
        s->enumerateExtensions=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(next(*out,"vkEnumerateDeviceExtensionProperties"));
        s->properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(next(*out,"vkGetPhysicalDeviceProperties"));
        s->memoryProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(next(*out,"vkGetPhysicalDeviceMemoryProperties"));
        const bool coreProperties2=ci->pApplicationInfo && ci->pApplicationInfo->apiVersion>=VK_API_VERSION_1_1;
        s->memoryProperties2=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(next(*out,coreProperties2 ?
            "vkGetPhysicalDeviceMemoryProperties2" : "vkGetPhysicalDeviceMemoryProperties2KHR"));
        s->features=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(next(*out,"vkGetPhysicalDeviceFeatures"));
        s->queueFamilies=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(next(*out,"vkGetPhysicalDeviceQueueFamilyProperties"));
        std::lock_guard<std::mutex> lock(mapsMutex); instances[key(*out)]=std::move(s); globalGipa=next;
    } catch(const std::bad_alloc&) {
        auto destroy=reinterpret_cast<PFN_vkDestroyInstance>(next(*out,"vkDestroyInstance")); if(destroy) destroy(*out,allocator);
        *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL layerDestroyInstance(VkInstance instance,const VkAllocationCallbacks* allocator) {
    auto s=findInstance(key(instance)); if(!s) return;
    { std::lock_guard<std::mutex> lock(mapsMutex); instances.erase(key(instance)); }
    if(s->destroy) s->destroy(instance,allocator);
}
VKAPI_ATTR void VKAPI_CALL layerGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical,VkPhysicalDeviceMemoryProperties* out) {
    auto in=findInstance(key(physical));
    if(!in || !in->memoryProperties) return;
    in->memoryProperties(physical,out);
    exposeVirtualMemory(physicalView(in,physical),out);
}
VKAPI_ATTR void VKAPI_CALL layerGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physical,VkPhysicalDeviceMemoryProperties2* out) {
    auto in=findInstance(key(physical));
    if(!in || !in->memoryProperties2) return;
    in->memoryProperties2(physical,out);
    auto view=physicalView(in,physical);
    exposeVirtualMemory(view,out?&out->memoryProperties:nullptr);
#ifdef VK_EXT_memory_budget
    if(view.enabled && out) for(auto* p=static_cast<VkBaseOutStructure*>(out->pNext);p;p=p->pNext) {
        if(p->sType!=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) continue;
        auto* budget=reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(p);
        budget->heapBudget[view.virtualHeap]=view.virtualBytes;
        std::uint64_t usage=0;
        { std::lock_guard<std::mutex> lock(mapsMutex);
          for(const auto& pair:devices) if(pair.second->physical==physical) {
              std::lock_guard<std::mutex> deviceLock(pair.second->mutex); usage+=pair.second->virtualUsage;
          } }
        budget->heapUsage[view.virtualHeap]=usage;
        break;
    }
#endif
}
VKAPI_ATTR VkResult VKAPI_CALL layerCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* ci,const VkAllocationCallbacks* allocator,VkDevice* out) {
    if(!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    auto in=findInstance(key(physical)); if(!in) return VK_ERROR_INITIALIZATION_FAILED;
    auto view=physicalView(in,physical);
    auto* link=deviceLink(ci); if(!link || !link->u.pLayerInfo) { logf("missing loader device link information"); return VK_ERROR_INITIALIZATION_FAILED; }
    PFN_vkSetDeviceLoaderData setDeviceLoaderData=nullptr;
    for(auto* p=static_cast<const VkBaseInStructure*>(ci->pNext);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* loader=reinterpret_cast<const VkLayerDeviceCreateInfo*>(p);
            if(loader->function==VK_LOADER_DATA_CALLBACK) setDeviceLoaderData=loader->u.pfnSetDeviceLoaderData;
        }
    auto* info=link->u.pLayerInfo; auto nextGdpa=info->pfnNextGetDeviceProcAddr; auto nextGipa=info->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo=info->pNext;
    bool policy=hasPolicy(ci->pNext), supported=false;
    bool robustness2Supported=false, subgroupControlSupported=false, budgetSupported=false;
    bool externalMemoryHostSupported=false;
    if(in->enumerateExtensions) {
        uint32_t count=0; VkResult er=in->enumerateExtensions(physical,nullptr,&count,nullptr);
        if(er==VK_SUCCESS || er==VK_INCOMPLETE) {
            try { std::vector<VkExtensionProperties> exts(count); er=in->enumerateExtensions(physical,nullptr,&count,exts.data());
                if(er==VK_SUCCESS || er==VK_INCOMPLETE) for(const auto& e:exts) {
                    if(std::strcmp(e.extensionName,VK_AMD_MEMORY_OVERALLOCATION_BEHAVIOR_EXTENSION_NAME)==0) supported=true;
                    if(std::strcmp(e.extensionName,VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)==0) subgroupControlSupported=true;
                    if(std::strcmp(e.extensionName,VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)==0) budgetSupported=true;
#ifdef VK_EXT_robustness2
                    if(std::strcmp(e.extensionName,VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)==0) robustness2Supported=true;
#endif
                    if(std::strcmp(e.extensionName,VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)==0) externalMemoryHostSupported=true;
                }
            } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        }
    }
    const bool budgetRequested=std::getenv("ZVRAM_VULKAN_HEADROOM_MIB")!=nullptr;
    const auto reserveMiB=positiveEnv("ZVRAM_VULKAN_HEADROOM_MIB",std::numeric_limits<VkDeviceSize>::max()/(1024ull*1024ull));
    std::uint32_t budgetHeap=UINT32_MAX;
    if(budgetRequested) {
        const char* lazy=std::getenv("ZVRAM_VULKAN_LAZY_BACKING");
        const char* afterCold=std::getenv("ZVRAM_VULKAN_RESIDENT_AFTER_COLD");
        if(!reserveMiB || !lazy || std::strcmp(lazy,"1") ||
           (afterCold && std::strcmp(afterCold,"1")==0) ||
           !positiveEnv("ZVRAM_VULKAN_RESIDENT_MIB") || !in->properties2Enabled ||
           !in->memoryProperties2 || !budgetSupported) {
            logf("VRAM headroom requires lazy immediate admission, valid reserve, memory_budget and enabled properties2");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        for(std::uint32_t i=0;i<view.native.memoryHeapCount;++i)
            if(view.native.memoryHeaps[i].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                if(budgetHeap!=UINT32_MAX) { logf("VRAM headroom requires a single native local heap"); return VK_ERROR_FEATURE_NOT_PRESENT; }
                budgetHeap=i;
            }
        if(budgetHeap==UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    bool inject=supported && !policy;
    if(!supported) logf("AMD overallocation extension unavailable; passing device creation through unchanged");
    const bool snapshotRequested=std::getenv("ZVRAM_VULKAN_AUTO_IDLE_MS") || std::getenv("ZVRAM_VULKAN_COLD_MIB");
    bool deviceGroupRequested=false;
    for(auto* p=static_cast<const VkBaseInStructure*>(ci->pNext);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO) deviceGroupRequested=true;
    std::uint32_t sparseFamily=UINT32_MAX, familyCount=0, sparseQueueFlags=0;
    std::uint32_t appSparseFamily=UINT32_MAX, appSparseQueueFlags=0;
    std::uint32_t privateFamily=UINT32_MAX, privateIndex=UINT32_MAX;
    VkDeviceQueueCreateFlags privateQueueFlags{};
    bool unsupportedQueueFlags=false;
    bool queuePlanOutOfHostMemory=false;
    std::vector<VkQueueFamilyProperties> queueProps;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    std::vector<std::vector<float>> queuePriorities;
    bool privateQueuePlanned=false;
    if(view.enabled && in->queueFamilies) {
        in->queueFamilies(physical,&familyCount,nullptr);
        try { queueProps.resize(familyCount); }
        catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        in->queueFamilies(physical,&familyCount,queueProps.data());
        for(std::uint32_t i=0;i<familyCount;i++) {
            if((queueProps[i].queueFlags&VK_QUEUE_SPARSE_BINDING_BIT)==0) continue;
            for(std::uint32_t q=0;q<ci->queueCreateInfoCount;q++)
                if(ci->pQueueCreateInfos[q].queueFamilyIndex==i && ci->pQueueCreateInfos[q].queueCount &&
                   (ci->pQueueCreateInfos[q].flags&~kAllowedQueueCreateFlags)==0) {
                    sparseFamily=i; sparseQueueFlags=queueProps[i].queueFlags; break;
                }
            if(sparseFamily!=UINT32_MAX) break;
        }
        appSparseFamily=sparseFamily; appSparseQueueFlags=sparseQueueFlags;
        if(!deviceGroupRequested) {
            bool appQueuesSupported=true;
            for(std::uint32_t q=0;q<ci->queueCreateInfoCount;q++)
                if(ci->pQueueCreateInfos[q].flags&~kAllowedQueueCreateFlags) {
                    appQueuesSupported=false; unsupportedQueueFlags=true;
                }
            for(std::uint32_t i=0;i<familyCount;i++) {
                if(!appQueuesSupported) break;
                const auto flags=queueProps[i].queueFlags;
                if((flags&VK_QUEUE_SPARSE_BINDING_BIT)==0) continue;
                if(snapshotRequested &&
                   ((flags&(VK_QUEUE_TRANSFER_BIT|VK_QUEUE_SPARSE_BINDING_BIT))!=
                        (VK_QUEUE_TRANSFER_BIT|VK_QUEUE_SPARSE_BINDING_BIT) ||
                    (flags&(VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT))==0)) continue;
                std::uint32_t used=0; bool supportedFlags=true; std::uint32_t existing=UINT32_MAX;
                for(std::uint32_t q=0;q<ci->queueCreateInfoCount;q++) if(ci->pQueueCreateInfos[q].queueFamilyIndex==i) {
                    existing=q; used=ci->pQueueCreateInfos[q].queueCount;
                    if(ci->pQueueCreateInfos[q].flags&~kAllowedQueueCreateFlags) supportedFlags=false;
                    break;
                }
                if(!supportedFlags || used>=queueProps[i].queueCount) continue;
                privateFamily=i; privateIndex=used; privateQueueFlags=existing==UINT32_MAX?0:ci->pQueueCreateInfos[existing].flags;
                privateQueuePlanned=true;
                try {
                    if(ci->queueCreateInfoCount) queueInfos.assign(ci->pQueueCreateInfos,ci->pQueueCreateInfos+ci->queueCreateInfoCount);
                    queuePriorities.resize(queueInfos.size()+(existing==UINT32_MAX?1u:0u));
                    for(std::size_t q=0;q<queueInfos.size();q++) {
                        const auto& original=ci->pQueueCreateInfos[q];
                        queuePriorities[q].assign(original.pQueuePriorities,original.pQueuePriorities+original.queueCount);
                    }
                    if(existing==UINT32_MAX) {
                        queueInfos.push_back(VkDeviceQueueCreateInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO});
                        auto& qci=queueInfos.back(); qci.queueFamilyIndex=i; qci.queueCount=1;
                        qci.flags=privateQueueFlags;
                        queuePriorities.back().push_back(1.0f);
                    } else {
                        queueInfos[existing].queueCount++;
                        queuePriorities[existing].push_back(1.0f);
                    }
                    for(std::size_t q=0;q<queueInfos.size();q++) queueInfos[q].pQueuePriorities=queuePriorities[q].data();
                } catch(const std::bad_alloc&) { privateQueuePlanned=false; queuePlanOutOfHostMemory=true; queueInfos.clear(); queuePriorities.clear(); }
                if(privateQueuePlanned) { sparseFamily=i; sparseQueueFlags=flags; break; }
                privateFamily=privateIndex=UINT32_MAX;
            }
        }
    }
    bool virtualEnabled=view.enabled && sparseFamily!=UINT32_MAX;
    VkPhysicalDeviceFeatures injectedFeatures{};
    VkPhysicalDeviceFeatures2 injectedFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    const auto* featureChain=static_cast<const VkBaseInStructure*>(ci->pNext);
    const VkPhysicalDeviceFeatures2* requestedFeatures2=nullptr;
    for(auto* p=featureChain;p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) { requestedFeatures2=reinterpret_cast<const VkPhysicalDeviceFeatures2*>(p); break; }
    const bool hasFeatures2=requestedFeatures2!=nullptr;
    const bool features2AtHead=ci->pNext && static_cast<const VkBaseInStructure*>(ci->pNext)->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    const bool sparseBindingEnabled=hasFeatures2?requestedFeatures2->features.sparseBinding!=VK_FALSE:
        (ci->pEnabledFeatures && ci->pEnabledFeatures->sparseBinding!=VK_FALSE);
    const bool coreRobustEnabled=hasFeatures2?requestedFeatures2->features.robustBufferAccess!=VK_FALSE:
        (ci->pEnabledFeatures && ci->pEnabledFeatures->robustBufferAccess!=VK_FALSE);
    const bool bdaEnabled=enabledBufferDeviceAddress(ci);
    const auto rangeMiB=positiveEnv("ZVRAM_VULKAN_RANGE_MIB",std::numeric_limits<std::uint64_t>::max()/(1024ull*1024ull));
    VkPhysicalDeviceFeatures physicalFeatures{};
    if(rangeMiB && in->features) in->features(physical,&physicalFeatures);
    const bool residencyEnabled=hasFeatures2?requestedFeatures2->features.sparseResidencyBuffer!=VK_FALSE:
        (ci->pEnabledFeatures && ci->pEnabledFeatures->sparseResidencyBuffer!=VK_FALSE);
    const bool rangeEnabled=rangeMiB && snapshotRequested && physicalFeatures.sparseResidencyBuffer &&
        (!hasFeatures2 || features2AtHead || residencyEnabled);
    bool gpuRestorePlanned=false, injectGpuSubgroup=false, gpuRequiresInt64=false;
    bool gpuImportHostPlanned=false;
    VkDeviceSize gpuImportHostAlignment=0;
    VkPhysicalDeviceSubgroupSizeControlFeatures gpuSubgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkPhysicalDeviceProperties gpuProperties{};
    const char* codecEnv=std::getenv("ZVRAM_VULKAN_CODEC");
#ifdef ZVRAM_HAVE_GDEFLATE
    const char* gpuEnv=std::getenv("ZVRAM_VULKAN_GDEFLATE_GPU");
    if(gpuEnv && std::strcmp(gpuEnv,"1")==0 && codecEnv && std::strcmp(codecEnv,"gdeflate")==0 &&
       snapshotRequested && virtualEnabled && privateQueuePlanned && subgroupControlSupported &&
       (queueProps[privateFamily].queueFlags&VK_QUEUE_COMPUTE_BIT)) {
        const VkPhysicalDeviceSubgroupSizeControlFeatures* appSubgroup=nullptr;
        const VkPhysicalDeviceVulkan13Features* app13=nullptr;
        for(auto* p=featureChain;p;p=p->pNext) {
            if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES)
                appSubgroup=reinterpret_cast<const VkPhysicalDeviceSubgroupSizeControlFeatures*>(p);
            if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
                app13=reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(p);
        }
        const bool int64Enabled=hasFeatures2?requestedFeatures2->features.shaderInt64:
            (ci->pEnabledFeatures && ci->pEnabledFeatures->shaderInt64);
        const bool appAllowsSubgroup=(!appSubgroup || (appSubgroup->subgroupSizeControl && appSubgroup->computeFullSubgroups)) &&
            (!app13 || (app13->subgroupSizeControl && app13->computeFullSubgroups));
        auto getFeatures=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(in->gipa(in->handle,"vkGetPhysicalDeviceFeatures2"));
        auto getProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(in->gipa(in->handle,"vkGetPhysicalDeviceProperties2"));
        if(getFeatures && getProperties && appAllowsSubgroup && (!hasFeatures2 || features2AtHead || int64Enabled)) {
            VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            available.pNext=&gpuSubgroup; getFeatures(physical,&available);
            VkPhysicalDeviceSubgroupProperties subgroups{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
            VkPhysicalDeviceSubgroupSizeControlProperties sizes{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
            subgroups.pNext=&sizes;
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext=&subgroups; getProperties(physical,&properties); gpuProperties=properties.properties;
            constexpr VkSubgroupFeatureFlags ops=VK_SUBGROUP_FEATURE_BALLOT_BIT|VK_SUBGROUP_FEATURE_ARITHMETIC_BIT|VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
            gpuRestorePlanned=available.features.shaderInt64 && gpuSubgroup.subgroupSizeControl && gpuSubgroup.computeFullSubgroups &&
                (subgroups.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT) && (subgroups.supportedOperations&ops)==ops &&
                (sizes.requiredSubgroupSizeStages&VK_SHADER_STAGE_COMPUTE_BIT) && sizes.minSubgroupSize<=32 && sizes.maxSubgroupSize>=32 &&
                gpuProperties.limits.maxComputeWorkGroupInvocations>=32 && gpuProperties.limits.maxComputeWorkGroupSize[0]>=32 &&
                gpuProperties.limits.maxComputeWorkGroupCount[0]>=512 && gpuProperties.limits.maxStorageBufferRange>=32u*1024u*1024u;
            gpuRequiresInt64=gpuRestorePlanned;
            injectGpuSubgroup=gpuRestorePlanned && !appSubgroup && !app13;
        }
    }
#endif
    const char* bp16GpuEnv=std::getenv("ZVRAM_VULKAN_BP16_GPU");
    if(bp16GpuEnv && std::strcmp(bp16GpuEnv,"1")==0 && codecEnv && std::strcmp(codecEnv,"bp16")==0 &&
       snapshotRequested && virtualEnabled && privateQueuePlanned && in->properties &&
       (queueProps[privateFamily].queueFlags&VK_QUEUE_COMPUTE_BIT)) {
        in->properties(physical,&gpuProperties);
        gpuRestorePlanned=gpuProperties.limits.maxComputeWorkGroupInvocations>=256 &&
            gpuProperties.limits.maxComputeWorkGroupSize[0]>=256 &&
            gpuProperties.limits.maxComputeWorkGroupCount[0]>=32768 &&
            gpuProperties.limits.maxStorageBufferRange>=zvram::bp16::MaxRawBytes;
    }
    const char* importHostEnv=std::getenv("ZVRAM_VULKAN_BP16_IMPORT_HOST_INPUT");
    bool importHostRequested=importHostEnv && std::strcmp(importHostEnv,"1")==0;
    const char* allocatedHostEnv=std::getenv("ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT");
    bool allocatedHostRequested=allocatedHostEnv && std::strcmp(allocatedHostEnv,"1")==0;
    if(importHostRequested && allocatedHostRequested) {
        logf("BP16 imported and allocated host input conflict; disabling both modes");
        importHostRequested=false; allocatedHostRequested=false;
    }
    std::uint64_t gpuAllocatedHostBudgetBytes=
        zvram::gdeflate::gpu::Decoder::DefaultAllocatedHostBudgetBytes;
    const char* allocatedHostBudgetEnv=std::getenv("ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB");
    if(allocatedHostRequested && allocatedHostBudgetEnv &&
       !zvram::gdeflate::gpu::Decoder::parseAllocatedHostBudgetMiB(
           allocatedHostBudgetEnv,gpuAllocatedHostBudgetBytes)) {
        logf("invalid ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB; using ordinary compressed upload");
        allocatedHostRequested=false;
    }
    bool gpuAllocatedHostPlanned=false;
    if(allocatedHostRequested && gpuRestorePlanned && codecEnv && std::strcmp(codecEnv,"bp16")==0 &&
       in->apiVersion>=VK_API_VERSION_1_1 && gpuProperties.apiVersion>=VK_API_VERSION_1_1) {
        VkPhysicalDeviceMemoryProperties hostMemory{};
        in->memoryProperties(physical,&hostMemory);
        gpuAllocatedHostPlanned=std::any_of(hostMemory.memoryTypes,
            hostMemory.memoryTypes+hostMemory.memoryTypeCount,[](const auto& type) {
                const auto flags=type.propertyFlags;
                constexpr VkMemoryPropertyFlags required=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
                return (flags&required)==required && !(flags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            });
        if(!gpuAllocatedHostPlanned)
            logf("BP16 allocated cached host input unavailable; using ordinary compressed upload");
    } else if(allocatedHostRequested && gpuRestorePlanned && codecEnv && std::strcmp(codecEnv,"bp16")==0)
        logf("BP16 allocated cached host input requires Vulkan 1.1; using ordinary compressed upload");
    if(importHostRequested && gpuRestorePlanned && codecEnv && std::strcmp(codecEnv,"bp16")==0) {
        auto getProperties2=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            in->gipa(in->handle,"vkGetPhysicalDeviceProperties2"));
        auto getExternalBufferProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceExternalBufferProperties>(
            in->gipa(in->handle,"vkGetPhysicalDeviceExternalBufferProperties"));
        if(in->apiVersion>=VK_API_VERSION_1_1 && gpuProperties.apiVersion>=VK_API_VERSION_1_1 &&
           externalMemoryHostSupported && getProperties2 && getExternalBufferProperties) {
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProperties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext=&hostProperties; getProperties2(physical,&properties);
            VkPhysicalDeviceExternalBufferInfo bufferInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
            bufferInfo.flags=0; bufferInfo.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bufferInfo.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            VkExternalBufferProperties bufferProperties{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
            getExternalBufferProperties(physical,&bufferInfo,&bufferProperties);
            const auto external=bufferProperties.externalMemoryProperties;
            const auto pageSize=sysconf(_SC_PAGESIZE);
            const auto importAlignment=pageSize>0
                ?std::max<VkDeviceSize>(hostProperties.minImportedHostPointerAlignment,static_cast<VkDeviceSize>(pageSize)):0;
            VkPhysicalDeviceMemoryProperties hostMemory{}; in->memoryProperties(physical,&hostMemory);
            const bool hasCoherentNonlocal=std::any_of(hostMemory.memoryTypes,
                hostMemory.memoryTypes+hostMemory.memoryTypeCount,[](const auto& type) {
                    const auto flags=type.propertyFlags;
                    return (flags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
                           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
                           !(flags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                });
            if(hostProperties.minImportedHostPointerAlignment &&
               !(hostProperties.minImportedHostPointerAlignment&(hostProperties.minImportedHostPointerAlignment-1)) &&
               pageSize>0 && !(pageSize&(pageSize-1)) &&
               importAlignment>=sizeof(void*) && importAlignment<=65536 &&
               !(importAlignment&(importAlignment-1)) &&
               (external.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
               !(external.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) &&
               (external.compatibleHandleTypes&VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) &&
               hasCoherentNonlocal) {
                gpuImportHostPlanned=true;
                gpuImportHostAlignment=importAlignment;
            }
        }
        if(!gpuImportHostPlanned)
            logf("BP16 imported host input unavailable; using ordinary compressed upload");
    }
    bool strictRobustnessEnabled=false;
    bool appRobustnessPresent=false;
    VkDeviceSize robustAlignment=1;
    const char* strictEnv=std::getenv("ZVRAM_VULKAN_STRICT_ROBUSTNESS");
    const bool strictRequested=strictEnv && std::strcmp(strictEnv,"1")==0;
#ifdef VK_EXT_robustness2
    VkPhysicalDeviceRobustness2FeaturesEXT injectedRobustness{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    const VkPhysicalDeviceRobustness2FeaturesEXT* appRobustness=nullptr;
    for(auto* p=featureChain;p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT)
            appRobustness=reinterpret_cast<const VkPhysicalDeviceRobustness2FeaturesEXT*>(p);
    appRobustnessPresent=appRobustness!=nullptr;
    if(rangeEnabled && (strictRequested || (appRobustness && appRobustness->robustBufferAccess2)) && robustness2Supported) {
        auto features2=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(in->gipa(in->handle,"vkGetPhysicalDeviceFeatures2"));
        if(!features2) features2=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(in->gipa(in->handle,"vkGetPhysicalDeviceFeatures2KHR"));
        auto properties2=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(in->gipa(in->handle,"vkGetPhysicalDeviceProperties2"));
        if(!properties2) properties2=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(in->gipa(in->handle,"vkGetPhysicalDeviceProperties2KHR"));
        if(features2 && properties2 && (!appRobustness || appRobustness->robustBufferAccess2)) {
            VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            available.pNext=&injectedRobustness; features2(physical,&available);
            VkPhysicalDeviceRobustness2PropertiesEXT robustProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext=&robustProperties; properties2(physical,&properties);
            const auto alignment=robustProperties.robustStorageBufferAccessSizeAlignment;
            if(injectedRobustness.robustBufferAccess2 && alignment && !(alignment&(alignment-1)) &&
               (!hasFeatures2 || features2AtHead || coreRobustEnabled)) {
                strictRobustnessEnabled=true; robustAlignment=alignment;
            }
        }
    }
#endif
    if(hasFeatures2 && !features2AtHead && !sparseBindingEnabled) {
        virtualEnabled=false;
        if(privateQueuePlanned) {
            privateQueuePlanned=false; privateFamily=privateIndex=UINT32_MAX;
            sparseFamily=UINT32_MAX;
            if(view.enabled && in->queueFamilies) {
                // Only the pre-existing app queue is safe when the feature chain
                // cannot be amended without changing an earlier loader node.
                sparseFamily=appSparseFamily; sparseQueueFlags=appSparseQueueFlags;
            }
            queueInfos.clear(); queuePriorities.clear();
        }
    }
    gpuRestorePlanned = gpuRestorePlanned && virtualEnabled && privateQueuePlanned;
    injectGpuSubgroup = injectGpuSubgroup && gpuRestorePlanned;
    gpuRequiresInt64 = gpuRequiresInt64 && gpuRestorePlanned;
    gpuImportHostPlanned = gpuImportHostPlanned && gpuRestorePlanned;
    gpuAllocatedHostPlanned = gpuAllocatedHostPlanned && gpuRestorePlanned;
    VkDeviceCreateInfo copy=*ci; VkDeviceMemoryOverallocationCreateInfoAMD behavior{VK_STRUCTURE_TYPE_DEVICE_MEMORY_OVERALLOCATION_CREATE_INFO_AMD};
    if(privateQueuePlanned) { copy.queueCreateInfoCount=static_cast<std::uint32_t>(queueInfos.size()); copy.pQueueCreateInfos=queueInfos.data(); }
    std::vector<const char*> extensions;
    if(inject) {
        try {
            if(!appEnabled(ci,VK_AMD_MEMORY_OVERALLOCATION_BEHAVIOR_EXTENSION_NAME)) {
                if(ci->enabledExtensionCount) extensions.assign(ci->ppEnabledExtensionNames,ci->ppEnabledExtensionNames+ci->enabledExtensionCount);
                extensions.push_back(VK_AMD_MEMORY_OVERALLOCATION_BEHAVIOR_EXTENSION_NAME);
                copy.enabledExtensionCount=static_cast<uint32_t>(extensions.size()); copy.ppEnabledExtensionNames=extensions.data();
            }
        } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        behavior.overallocationBehavior=VK_MEMORY_OVERALLOCATION_BEHAVIOR_ALLOWED_AMD; behavior.pNext=ci->pNext; copy.pNext=&behavior;
    }
    if(virtualEnabled) {
        if(hasFeatures2) {
            if(!sparseBindingEnabled || (rangeEnabled && !residencyEnabled) || (strictRobustnessEnabled && !appRobustnessPresent && !coreRobustEnabled) ||
               (gpuRequiresInt64 && features2AtHead && !requestedFeatures2->features.shaderInt64)) {
                injectedFeatures2=*requestedFeatures2; injectedFeatures2.features.sparseBinding=VK_TRUE;
                if(rangeEnabled) injectedFeatures2.features.sparseResidencyBuffer=VK_TRUE;
                if(strictRobustnessEnabled) injectedFeatures2.features.robustBufferAccess=VK_TRUE;
                if(gpuRequiresInt64) injectedFeatures2.features.shaderInt64=VK_TRUE;
                if(copy.pNext==ci->pNext) copy.pNext=&injectedFeatures2;
                else behavior.pNext=&injectedFeatures2;
            }
        } else if(!sparseBindingEnabled || (rangeEnabled && !residencyEnabled) || strictRobustnessEnabled || gpuRequiresInt64) {
            if(ci->pEnabledFeatures) injectedFeatures=*ci->pEnabledFeatures;
            injectedFeatures.sparseBinding=VK_TRUE;
            if(rangeEnabled) injectedFeatures.sparseResidencyBuffer=VK_TRUE;
            if(strictRobustnessEnabled) injectedFeatures.robustBufferAccess=VK_TRUE;
            if(gpuRequiresInt64) injectedFeatures.shaderInt64=VK_TRUE;
            copy.pEnabledFeatures=&injectedFeatures;
        }
    }
#ifdef VK_EXT_robustness2
    if(virtualEnabled && strictRobustnessEnabled && !appRobustness) {
        try {
            if(!appEnabled(ci,VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
                if(extensions.empty() && ci->enabledExtensionCount)
                    extensions.assign(ci->ppEnabledExtensionNames,ci->ppEnabledExtensionNames+ci->enabledExtensionCount);
                extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
                copy.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size()); copy.ppEnabledExtensionNames=extensions.data();
            }
        } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        injectedRobustness.robustBufferAccess2=VK_TRUE;
        injectedRobustness.robustImageAccess2=VK_FALSE; injectedRobustness.nullDescriptor=VK_FALSE;
        injectedRobustness.pNext=const_cast<void*>(copy.pNext); copy.pNext=&injectedRobustness;
    }
#endif
    if(gpuRequiresInt64) {
        try {
            if(!appEnabled(ci,VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)) {
                if(extensions.empty() && ci->enabledExtensionCount)
                    extensions.assign(ci->ppEnabledExtensionNames,ci->ppEnabledExtensionNames+ci->enabledExtensionCount);
                extensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
                copy.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size()); copy.ppEnabledExtensionNames=extensions.data();
            }
        } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        if(injectGpuSubgroup) {
            gpuSubgroup.subgroupSizeControl=VK_TRUE; gpuSubgroup.computeFullSubgroups=VK_TRUE;
            gpuSubgroup.pNext=const_cast<void*>(copy.pNext); copy.pNext=&gpuSubgroup;
        }
    }
    if(budgetRequested && !appEnabled(ci,VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
        try {
            if(extensions.empty() && ci->enabledExtensionCount)
                extensions.assign(ci->ppEnabledExtensionNames,ci->ppEnabledExtensionNames+ci->enabledExtensionCount);
            extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            copy.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());
            copy.ppEnabledExtensionNames=extensions.data();
        } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    }
    if(gpuImportHostPlanned && !appEnabled(ci,VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        try {
            if(extensions.empty() && ci->enabledExtensionCount)
                extensions.assign(ci->ppEnabledExtensionNames,ci->ppEnabledExtensionNames+ci->enabledExtensionCount);
            extensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
            copy.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());
            copy.ppEnabledExtensionNames=extensions.data();
        } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    }
    VkResult r=VK_ERROR_INITIALIZATION_FAILED;
    auto nextCreate=reinterpret_cast<PFN_vkCreateDevice>(nextGipa(in->handle,"vkCreateDevice"));
    if(!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;
    r=nextCreate(physical,&copy,allocator,out); if(r!=VK_SUCCESS) return r;
    try {
        auto d=std::make_shared<Device>(); d->handle=*out; d->physical=physical; d->gdpa=nextGdpa; d->setDeviceLoaderData=setDeviceLoaderData;
        const char* gpuProfile=std::getenv("ZVRAM_VULKAN_GPU_PROFILE");
        d->gpuProfileEnabled=gpuProfile && std::strcmp(gpuProfile,"1")==0;
        const char* bufferPresentation=std::getenv("ZVRAM_VULKAN_BUFFER_PRESENTATION");
        d->bufferPresentation=bufferPresentation && std::strcmp(bufferPresentation,"1")==0;
        const char* asyncCompression=std::getenv("ZVRAM_VULKAN_ASYNC_COMPRESSION");
        d->asyncCompression=asyncCompression && std::strcmp(asyncCompression,"1")==0;
        d->gpuRestoreEnabled=gpuRestorePlanned;
        d->gpuImportHostInput=gpuImportHostPlanned;
        d->gpuAllocatedHostInput=gpuAllocatedHostPlanned;
        d->gpuAllocatedHostBudgetBytes=gpuAllocatedHostBudgetBytes;
        d->gpuImportHostAlignment=gpuImportHostAlignment;
        if(budgetRequested) {
            d->budgetProperties=in->memoryProperties2; d->budgetHeap=budgetHeap;
            d->budgetReserveBytes=reserveMiB*1024ull*1024ull;
        }
        if(gpuRestorePlanned) {
            d->gpuProperties=gpuProperties;
            d->gpuTimestampBits=queueProps[privateFamily].timestampValidBits;
            d->gpuStorageAlignment=std::max<VkDeviceSize>(1,gpuProperties.limits.minStorageBufferOffsetAlignment);
            d->gpuStorageRange=gpuProperties.limits.maxStorageBufferRange;
        }
        d->narrowDescriptorRanges=strictRobustnessEnabled || (hasFeatures2?!requestedFeatures2->features.robustBufferAccess:
            (!ci->pEnabledFeatures || !ci->pEnabledFeatures->robustBufferAccess));
        d->robustRangeAlignment=strictRobustnessEnabled?robustAlignment:1;
        d->submission.boundedRobustness(strictRobustnessEnabled);
        d->destroy=reinterpret_cast<PFN_vkDestroyDevice>(nextGdpa(*out,"vkDestroyDevice"));
        d->allocate=reinterpret_cast<PFN_vkAllocateMemory>(nextGdpa(*out,"vkAllocateMemory"));
        d->free=reinterpret_cast<PFN_vkFreeMemory>(nextGdpa(*out,"vkFreeMemory"));
        d->createBuffer=reinterpret_cast<PFN_vkCreateBuffer>(nextGdpa(*out,"vkCreateBuffer"));
        d->destroyBuffer=reinterpret_cast<PFN_vkDestroyBuffer>(nextGdpa(*out,"vkDestroyBuffer"));
        d->getBufferMemoryRequirements=reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(nextGdpa(*out,"vkGetBufferMemoryRequirements"));
        d->getBufferMemoryRequirements2=reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(nextGdpa(*out,"vkGetBufferMemoryRequirements2"));
        if(!d->getBufferMemoryRequirements2) d->getBufferMemoryRequirements2=reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(nextGdpa(*out,"vkGetBufferMemoryRequirements2KHR"));
        d->bindBufferMemory=reinterpret_cast<PFN_vkBindBufferMemory>(nextGdpa(*out,"vkBindBufferMemory"));
        d->bindBufferMemory2=reinterpret_cast<PFN_vkBindBufferMemory2>(nextGdpa(*out,"vkBindBufferMemory2"));
        if(!d->bindBufferMemory2) d->bindBufferMemory2=reinterpret_cast<PFN_vkBindBufferMemory2>(nextGdpa(*out,"vkBindBufferMemory2KHR"));
        d->queueBindSparse=reinterpret_cast<PFN_vkQueueBindSparse>(nextGdpa(*out,"vkQueueBindSparse"));
        d->queueWaitIdle=reinterpret_cast<PFN_vkQueueWaitIdle>(nextGdpa(*out,"vkQueueWaitIdle"));
        d->getDeviceQueue=reinterpret_cast<PFN_vkGetDeviceQueue>(nextGdpa(*out,"vkGetDeviceQueue"));
        d->getDeviceQueue2=reinterpret_cast<PFN_vkGetDeviceQueue2>(nextGdpa(*out,"vkGetDeviceQueue2"));
        d->virtualEnabled=virtualEnabled; d->bufferDeviceAddressEnabled=bdaEnabled;
        d->virtualType=view.virtualType; d->virtualHeap=view.virtualHeap; d->virtualBytes=view.virtualBytes;
        if(deviceGroupRequested) d->nativeWrappingAllowed=false;
        constexpr const char* rawMemoryHandleExtensions[]={"VK_KHR_video_queue","VK_NV_ray_tracing","VK_ARM_tensors",
            "VK_ARM_data_graph","VK_QCOM_tile_memory_heap","VK_EXT_device_memory_report"};
        for(const auto* extension:rawMemoryHandleExtensions)
            if(appEnabled(ci,extension)) d->nativeWrappingAllowed=false;
        if(!d->nativeWrappingAllowed) logf("native memory-handle wrapping/adoption disabled by device-group or raw-handle extension");
        if(d->getDeviceQueue || d->getDeviceQueue2) {
            for(std::uint32_t q=0;q<ci->queueCreateInfoCount;q++) {
                const auto& requested=ci->pQueueCreateInfos[q];
                if(std::find(d->queueFamilies.begin(),d->queueFamilies.end(),requested.queueFamilyIndex)==d->queueFamilies.end())
                    d->queueFamilies.push_back(requested.queueFamilyIndex);
                for(std::uint32_t n=0;n<requested.queueCount;n++) {
                    VkQueue queue{};
                    if(requested.flags==0 && d->getDeviceQueue) d->getDeviceQueue(*out,requested.queueFamilyIndex,n,&queue);
                    else if(d->getDeviceQueue2) {
                        VkDeviceQueueInfo2 queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
                        queueInfo.flags=requested.flags; queueInfo.queueFamilyIndex=requested.queueFamilyIndex; queueInfo.queueIndex=n;
                        d->getDeviceQueue2(*out,&queueInfo,&queue);
                    }
                    if(queue) d->appQueues.push_back(queue);
                }
            }
            if(privateQueuePlanned) {
                if(privateQueueFlags==0 && d->getDeviceQueue) d->getDeviceQueue(*out,privateFamily,privateIndex,&d->copyQueue);
                else if(d->getDeviceQueue2) {
                    VkDeviceQueueInfo2 queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
                    queueInfo.flags=privateQueueFlags; queueInfo.queueFamilyIndex=privateFamily; queueInfo.queueIndex=privateIndex;
                    d->getDeviceQueue2(*out,&queueInfo,&d->copyQueue);
                }
                if(d->copyQueue && d->setDeviceLoaderData && d->setDeviceLoaderData(*out,d->copyQueue)!=VK_SUCCESS)
                    d->copyQueue=VK_NULL_HANDLE;
                if(std::find(d->queueFamilies.begin(),d->queueFamilies.end(),privateFamily)==d->queueFamilies.end())
                    d->queueFamilies.push_back(privateFamily);
            }
            if(privateQueuePlanned && d->copyQueue) d->sparseQueue=d->copyQueue;
            else if(virtualEnabled) {
                if(d->getDeviceQueue) d->getDeviceQueue(*out,sparseFamily,0,&d->sparseQueue);
                else if(d->getDeviceQueue2) {
                    VkDeviceQueueInfo2 queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
                    queueInfo.queueFamilyIndex=sparseFamily; queueInfo.queueIndex=0;
                    d->getDeviceQueue2(*out,&queueInfo,&d->sparseQueue);
                }
            }
        }
        in->memoryProperties(physical,&d->memory); VkPhysicalDeviceProperties props{}; in->properties(physical,&props); d->gpu=props.deviceName; d->autoPolicy=inject;
        { std::lock_guard<std::mutex> lock(mapsMutex); devices[key(*out)]=d; }
        if(snapshotRequested) {
            if(deviceGroupRequested)
                logf("automatic Vulkan snapshots disabled: device-group device queues are unsupported");
            else if(unsupportedQueueFlags)
                logf("automatic Vulkan snapshots disabled: unsupported application queue creation flags");
            else if(queuePlanOutOfHostMemory)
                logf("automatic Vulkan snapshots disabled: host memory exhausted while planning private queue");
            else if(!virtualEnabled)
                logf("automatic Vulkan snapshots disabled: sparse-binding queue or safe sparse feature chain unavailable");
            else if(!privateQueuePlanned || !virtualEnabled || !d->copyQueue)
                logf("automatic Vulkan snapshots disabled: no unused sparse+transfer+compute/graphics queue and virtual-memory opt-in");
            else if(!snapshotConfig(*d,d->idleMilliseconds,d->coldBudget))
                logf("automatic Vulkan snapshots disabled: invalid idle interval or cold budget");
            else {
                std::vector<VkQueue> handles;
                VkResult autoResult=VK_SUCCESS;
                const char* selective=std::getenv("ZVRAM_VULKAN_SELECTIVE_RESTORE");
                d->selectiveRestore=selective && std::strcmp(selective,"1")==0;
                const char* active=std::getenv("ZVRAM_VULKAN_ACTIVE_EVICTION");
                d->activeEviction=d->selectiveRestore && active && std::strcmp(active,"1")==0;
                if(d->activeEviction && rangeEnabled) d->rangeChunkBytes=rangeMiB*1024ull*1024ull;
                const char* clean=std::getenv("ZVRAM_VULKAN_CLEAN_CACHE");
                d->cleanCache=d->rangeChunkBytes && clean && std::strcmp(clean,"1")==0;
                if(const char* policy=std::getenv("ZVRAM_VULKAN_CLEAN_CACHE_POLICY")) {
                    bool valid=false;
                    d->cleanCachePolicy=zvram::clean_cache::parsePolicy(policy,valid);
                    logf("Vulkan clean snapshot cache policy requested=%s effective=%s status=%s",
                         policy,d->cleanCachePolicy==zvram::clean_cache::Policy::Lru?"lru":
                               d->cleanCachePolicy==zvram::clean_cache::Policy::Mru?"mru":"first",
                         valid?"accepted":"rejected");
                }
                d->minSavingsPercent=static_cast<unsigned>(positiveEnv("ZVRAM_VULKAN_MIN_SAVINGS_PERCENT",100));
                if(const char* shuffle=std::getenv("ZVRAM_VULKAN_BYTE_SHUFFLE")) {
                    if(std::strcmp(shuffle,"2")==0) d->byteShuffle=2;
                    else if(std::strcmp(shuffle,"4")==0) d->byteShuffle=4;
                    else { autoResult=VK_ERROR_FEATURE_NOT_PRESENT; logf("invalid Vulkan byte shuffle stride: expected 2 or 4"); }
                }
                if(const char* codec=std::getenv("ZVRAM_VULKAN_CODEC")) {
                    if(std::strcmp(codec,"gdeflate")==0) {
#ifdef ZVRAM_HAVE_GDEFLATE
                        d->snapshotCodec=zvram::snapshot::Codec::GDeflate;
                        if(d->byteShuffle) { autoResult=VK_ERROR_FEATURE_NOT_PRESENT; logf("GDeflate snapshots do not support byte shuffle"); }
#else
                        autoResult=VK_ERROR_FEATURE_NOT_PRESENT; logf("GDeflate codec was not built");
#endif
                    } else if(std::strcmp(codec,"bp16")==0) {
                        d->snapshotCodec=zvram::snapshot::Codec::BP16;
                        if(d->byteShuffle) { autoResult=VK_ERROR_FEATURE_NOT_PRESENT; logf("BP16 snapshots do not support byte shuffle"); }
                    } else if(std::strcmp(codec,"zstd")!=0) {
                        autoResult=VK_ERROR_FEATURE_NOT_PRESENT; logf("invalid Vulkan snapshot codec");
                    }
                }
                if(const char* workers=std::getenv("ZVRAM_VULKAN_GDEFLATE_WORKERS")) {
                    const auto count=positiveEnv("ZVRAM_VULKAN_GDEFLATE_WORKERS",32);
                    if(!count || d->snapshotCodec!=zvram::snapshot::Codec::GDeflate) {
                        autoResult=VK_ERROR_FEATURE_NOT_PRESENT;
                        logf("GDeflate workers require the GDeflate codec and a count from 1 to 32");
                    } else d->gdeflateWorkers=static_cast<unsigned>(count);
                }
                if(const char* workers=std::getenv("ZVRAM_VULKAN_BP16_WORKERS")) {
                    const auto count=positiveEnv("ZVRAM_VULKAN_BP16_WORKERS",32);
                    if(!count || d->snapshotCodec!=zvram::snapshot::Codec::BP16) {
                        autoResult=VK_ERROR_FEATURE_NOT_PRESENT;
                        logf("BP16 workers require the BP16 codec and a count from 1 to 32");
                    } else d->bp16Workers=static_cast<unsigned>(count);
                }
                if(d->activeEviction && d->rangeChunkBytes) {
                    const auto residentMiB=positiveEnv("ZVRAM_VULKAN_RESIDENT_MIB",std::numeric_limits<std::uint64_t>::max()/(1024ull*1024ull));
                    d->residentLimitBytes=residentMiB*1024ull*1024ull;
                    const char* eviction=std::getenv("ZVRAM_VULKAN_EVICTION_POLICY");
                    d->mruEviction=d->residentLimitBytes && eviction && std::strcmp(eviction,"mru")==0;
                    const char* afterCold=std::getenv("ZVRAM_VULKAN_RESIDENT_AFTER_COLD");
                    d->residentAdmissionArmed=!(afterCold && std::strcmp(afterCold,"1")==0);
                    const char* lazy=std::getenv("ZVRAM_VULKAN_LAZY_BACKING");
                    d->lazyBacking=lazy && std::strcmp(lazy,"1")==0;
                    if(d->lazyBacking && (!d->residentLimitBytes || !d->residentAdmissionArmed)) {
                        autoResult=VK_ERROR_FEATURE_NOT_PRESENT;
                        logf("lazy backing requires immediate resident admission");
                    }
                }
                try {
                    handles=d->appQueues; handles.push_back(d->copyQueue);
                    if(d->activeEviction) for(auto queue:d->appQueues)
                        d->restoreQueueGenerations.emplace_back(queue,0);
                }
                catch(const std::bad_alloc&) { autoResult=VK_ERROR_OUT_OF_HOST_MEMORY; }
                if(autoResult==VK_SUCCESS) autoResult=d->autoQueues.init(*out,nextGdpa,d->copyQueue,handles,d->activeEviction);
                if(autoResult==VK_SUCCESS) {
                    d->autoInitialized=true;
                    if(!initSnapshotResources(*d,privateFamily)) autoResult=VK_ERROR_FEATURE_NOT_PRESENT;
                }
                if(autoResult!=VK_SUCCESS) {
                    releaseSnapshotResources(*d); d->autoQueues.destroy(); d->autoInitialized=false;
                    logf("automatic Vulkan snapshots disabled: private queue synchronization resources unavailable result=%d",static_cast<int>(autoResult));
                } else {
                    d->autoEnabled=true;
                    try { d->snapshotWorker=std::thread(snapshotWorkerLoop,d); }
                    catch(...) { d->autoEnabled=false; d->autoInitialized=false; releaseSnapshotResources(*d); d->autoQueues.destroy(); }
                    if(d->autoEnabled) logf("automatic Vulkan snapshots enabled idle-ms=%llu cold-budget=%llu",static_cast<unsigned long long>(d->idleMilliseconds),static_cast<unsigned long long>(d->coldBudget));
                    else logf("automatic Vulkan snapshots disabled: worker creation failed");
                    if(d->autoEnabled && d->selectiveRestore) logf("selective Vulkan restore enabled: tracked whole allocations; unknown commands and address shaders restore all");
                    if(d->autoEnabled && d->activeEviction) logf("active Vulkan eviction enabled: completed resource epochs; whole allocations; unknown access blocks eviction");
                    if(d->autoEnabled && d->bufferPresentation) logf("base Vulkan buffer presentation passthrough enabled");
                    if(d->autoEnabled && d->asyncCompression) logf("background range snapshot compression uses off-lock transactions max-raw-bytes=33554432");
                    if(d->autoEnabled && d->rangeChunkBytes) logf("Vulkan range residency enabled chunk-bytes=%llu",static_cast<unsigned long long>(d->rangeChunkBytes));
                    else if(rangeMiB) logf("Vulkan range residency disabled: feature chain, sparse residency support, or active mode unavailable");
                    if(d->autoEnabled && strictRobustnessEnabled) logf("bounded Vulkan robustness enabled alignment-bytes=%llu",static_cast<unsigned long long>(robustAlignment));
                    else if(strictRequested) logf("bounded Vulkan robustness unavailable: feature chain or device support");
                    if(d->autoEnabled && d->residentLimitBytes) logf("Vulkan resident admission enabled limit-bytes=%llu",static_cast<unsigned long long>(d->residentLimitBytes));
                    if(d->autoEnabled && d->residentLimitBytes) logf("Vulkan eviction policy=%s",d->mruEviction?"mru":"lru");
                    if(d->autoEnabled && d->cleanCache)
                        logf("Vulkan clean snapshot cache enabled: retained read-only backing shares cold budget");
                    if(d->autoEnabled) logf("Vulkan snapshot minimum savings percent=%u",d->minSavingsPercent);
                    if(d->autoEnabled && d->byteShuffle) logf("Vulkan snapshot byte shuffle stride=%u",d->byteShuffle);
                    if(d->autoEnabled && d->snapshotCodec==zvram::snapshot::Codec::GDeflate)
                        logf("Vulkan snapshot codec=gdeflate decode=%s experimental=1",d->gpuRestoreEnabled?"GPU":"CPU");
                    if(d->autoEnabled && d->snapshotCodec==zvram::snapshot::Codec::BP16)
                        logf("Vulkan snapshot codec=bp16 decode=%s workers=%u experimental=1",d->gpuRestoreEnabled?"GPU":"CPU",d->bp16Workers);
                }
            }
        }
        if(budgetRequested && (!d->autoEnabled || !d->lazyBacking || !d->rangeChunkBytes ||
                               !d->residentLimitBytes || !d->residentAdmissionArmed)) {
            logf("VRAM headroom refused: lazy immediate paging initialization unavailable");
            layerDestroyDevice(*out,allocator); *out=VK_NULL_HANDLE;
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if(d->bufferPresentation && (!d->autoEnabled || !d->autoInitialized || !d->activeEviction)) {
            logf("buffer presentation refused: automatic snapshots and active eviction are required");
            layerDestroyDevice(*out,allocator); *out=VK_NULL_HANDLE;
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if(d->asyncCompression && (!d->autoEnabled || !d->autoInitialized || !d->activeEviction || !d->rangeChunkBytes)) {
            logf("async compression refused: automatic range snapshots and active eviction are required");
            layerDestroyDevice(*out,allocator); *out=VK_NULL_HANDLE;
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if(virtualEnabled && !privateQueuePlanned)
            logf("virtual sparse binds use an application queue and wait synchronously: no unused sparse queue is available");
        logf("device=%s policy=%s",d->gpu.c_str(),policy?"application-specified":inject?"allowed":"unchanged");
    } catch(const std::bad_alloc&) {
        auto destroy=reinterpret_cast<PFN_vkDestroyDevice>(nextGdpa(*out,"vkDestroyDevice")); if(destroy) destroy(*out,allocator);
        *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return r;
}
VkResult bindSparseBatchLocked(VkDevice d,Device& state,
                               const VkSparseBufferMemoryBindInfo* buffers,std::uint32_t count) {
    if(state.gpuGateError!=VK_SUCCESS) return state.gpuGateError;
    if(!count) return VK_SUCCESS;
    if(!buffers) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(state.autoInitialized) {
        const auto r=state.autoQueues.sparseBind(buffers,count);
        if(r!=VK_SUCCESS) state.gpuGateError=r;
        return r;
    }
    if(!state.sparseQueue || !state.queueBindSparse || !state.queueWaitIdle) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO}; info.bufferBindCount=count; info.pBufferBinds=buffers;
    VkResult r=state.queueBindSparse(state.sparseQueue,1,&info,VK_NULL_HANDLE);
    if(r==VK_SUCCESS) r=state.queueWaitIdle(state.sparseQueue);
    if(r!=VK_SUCCESS) state.gpuGateError=r;
    return r;
}
VkResult bindSparseLocked(VkDevice d,Device& state,VkBuffer buffer,const VkSparseMemoryBind* binds,std::uint32_t count) {
    VkSparseBufferMemoryBindInfo bufferInfo{}; bufferInfo.buffer=buffer; bufferInfo.bindCount=count; bufferInfo.pBinds=binds;
    const auto started=state.gpuProfileEnabled?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    const auto result=bindSparseBatchLocked(d,state,&bufferInfo,1);
    if(state.gpuProfileEnabled) {
        state.gpuProfileSparseCalls.fetch_add(1,std::memory_order_relaxed);
        (result==VK_SUCCESS?state.gpuProfileSparseSuccess:state.gpuProfileSparseFailures).fetch_add(1,std::memory_order_relaxed);
        state.gpuProfileSparseNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count()),std::memory_order_relaxed);
    }
    return result;
}
VkResult bindChildAppsLocked(VkDevice device,Device& d,VirtualMemory& memory,std::size_t childIndex,bool unbind,
                             VkBuffer releaseView=VK_NULL_HANDLE) {
    if(childIndex>=memory.children.size() || childIndex>=memory.childSizes.size()) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(releaseView && unbind) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct Plan { VkBuffer buffer; VkSparseMemoryBind bind; };
    std::vector<Plan> plans;
    try { plans.reserve(memory.bindings.size()+(releaseView?1:0)); }
    catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkDeviceSize childBase=0;
    for(std::size_t i=0;i<childIndex;i++) childBase+=memory.childSizes[i];
    const VkDeviceSize childEnd=childBase+memory.childSizes[childIndex];
    // Decode/copy has completed before this transition. Retire the private view
    // and expose app aliases in one operation, followed by one completion wait.
    if(releaseView) {
        VkSparseMemoryBind release{}; release.size=memory.childSizes[childIndex];
        plans.push_back({releaseView,release});
    }
    for(const auto& app:memory.bindings) {
        const VkDeviceSize appEnd=app.memoryOffset+app.size;
        const VkDeviceSize lo=std::max(app.memoryOffset,childBase), hi=std::min(appEnd,childEnd);
        if(lo>=hi) continue;
        if(!app.alignment || (lo-app.memoryOffset)%app.alignment || (lo-childBase)%app.alignment || (hi-lo)%app.alignment)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        VkSparseMemoryBind bind{};
        bind.resourceOffset=lo-app.memoryOffset; bind.size=hi-lo;
        if(!unbind) {
            if(!memory.children[childIndex]) return VK_ERROR_FEATURE_NOT_PRESENT;
            bind.memory=memory.children[childIndex]; bind.memoryOffset=lo-childBase;
        }
        plans.push_back({app.buffer,bind});
    }
    if(plans.empty()) return VK_SUCCESS;
    if(plans.size()>std::numeric_limits<std::uint32_t>::max()) return VK_ERROR_OUT_OF_HOST_MEMORY;
    std::vector<VkSparseBufferMemoryBindInfo> buffers;
    try { buffers.resize(plans.size()); }
    catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    for(std::size_t i=0;i<plans.size();++i) {
        buffers[i].buffer=plans[i].buffer;
        buffers[i].bindCount=1;
        buffers[i].pBinds=&plans[i].bind;
    }
    const auto result=bindSparseBatchLocked(device,d,buffers.data(),static_cast<std::uint32_t>(buffers.size()));
    if(result==VK_SUCCESS && buffers.size()>1)
        logf("snapshot app sparse bind batch count=%zu unbind=%u view-release=%u",
            buffers.size()-(releaseView?1:0),unbind?1u:0u,releaseView?1u:0u);
    return result;
}
VkResult createPoolViews(Device& d,VirtualMemory& memory,const std::vector<VkDeviceSize>& sizes) {
    if(!d.autoEnabled) return VK_SUCCESS;
    if(!d.snapshot.createBuffer || !d.snapshot.destroyBuffer) return VK_ERROR_FEATURE_NOT_PRESENT;
    std::vector<VkBuffer> views;
    std::vector<VkDeviceSize> alignments;
    std::vector<std::uint32_t> typeBits;
    try { views.reserve(sizes.size()); alignments.reserve(sizes.size()); typeBits.reserve(sizes.size()); }
    catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    for(auto size:sizes) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.flags=VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
        ci.size=size; ci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(d.gpuRestoreEnabled) ci.usage|=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        ci.sharingMode=d.queueFamilies.size()>1?VK_SHARING_MODE_CONCURRENT:VK_SHARING_MODE_EXCLUSIVE;
        ci.queueFamilyIndexCount=ci.sharingMode==VK_SHARING_MODE_CONCURRENT?static_cast<std::uint32_t>(d.queueFamilies.size()):0;
        ci.pQueueFamilyIndices=ci.queueFamilyIndexCount?d.queueFamilies.data():nullptr;
        VkBuffer view{};
        VkResult r=d.snapshot.createBuffer(d.handle,&ci,nullptr,&view);
        if(r!=VK_SUCCESS) { for(auto h:views) d.snapshot.destroyBuffer(d.handle,h,nullptr); return r; }
        VkMemoryRequirements req{}; d.snapshot.getBufferMemoryRequirements(d.handle,view,&req);
        if(!req.alignment || req.size>size || !req.memoryTypeBits || size%req.alignment) {
            d.snapshot.destroyBuffer(d.handle,view,nullptr);
            for(auto h:views) d.snapshot.destroyBuffer(d.handle,h,nullptr);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        views.push_back(view); alignments.push_back(req.alignment); typeBits.push_back(req.memoryTypeBits);
    }
    memory.poolViews=std::move(views); memory.poolViewAlignments=std::move(alignments); memory.poolViewMemoryTypeBits=std::move(typeBits);
    return VK_SUCCESS;
}
void destroyPoolViews(Device& d,VirtualMemory& memory) {
    if(!d.gpuRestoreUnsafe && d.snapshot.destroyBuffer) for(auto view:memory.poolViews) if(view) d.snapshot.destroyBuffer(d.handle,view,nullptr);
    memory.poolViews.clear(); memory.poolViewAlignments.clear(); memory.poolViewMemoryTypeBits.clear();
}
VkResult bindPoolBuffer(VkDevice device,Device& d,VkBuffer buffer,VkDeviceMemory handle,
                        VkDeviceSize memoryOffset,const VkMemoryRequirements& req) {
    std::lock_guard<std::mutex> queueLock(d.queueMutex);
    auto it=d.virtualMemory.find(handle);
    if(it==d.virtualMemory.end() || !req.size || !req.alignment || req.memoryTypeBits==0 ||
       memoryOffset%req.alignment || memoryOffset>it->second.size || req.size>it->second.size-memoryOffset)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    auto& m=it->second;
    bool initializedNow=false;
    auto discardFirstBacking=[&] {
        if(!initializedNow) return;
        d.residentBytes-=std::min<std::uint64_t>(d.residentBytes,m.residentBytes);
        d.coldBytes-=m.coldStoredBytes; d.coldLogicalBytes-=m.coldLogicalSize;
        releaseChildren(d,m);
        m.coldStoredBytes=0; m.coldLogicalSize=0; m.cold=false;
        m.backingMemoryTypeBits=0; m.everBound=false; m.bound=false; m.trackPhysicalStats=false;
    };
    const VkDeviceSize end=memoryOffset+req.size;
    for(const auto& b:m.bindings) {
        const auto bEnd=b.memoryOffset+b.size;
        if(memoryOffset<bEnd && b.memoryOffset<end) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    // Range bindings describe aliases even when some children are cold. Their
    // pages are bound only after queue admission selects and restores them.
    if(m.cold && !(d.lazyBacking && d.rangeChunkBytes)) {
        const auto r=restoreColdLocked(device,d,handle); if(r!=VK_SUCCESS) return r;
    }
    if(m.everBound && m.backingMemoryTypeBits==0) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(!m.everBound) {
        m.backingMemoryTypeBits=req.memoryTypeBits&(m.nativeTypeBits?m.nativeTypeBits:UINT32_MAX);
        const VkDeviceSize alignment=req.alignment;
        VkDeviceSize chunk=d.rangeChunkBytes?d.rangeChunkBytes:256u*1024u*1024u;
        chunk-=chunk%alignment;
        if(!chunk) return VK_ERROR_FEATURE_NOT_PRESENT;
        std::vector<VkDeviceSize> sizes;
        try {
            for(VkDeviceSize at=0;at<m.size;) { const auto n=std::min(chunk,m.size-at); sizes.push_back(n); at+=n; }
            m.children.assign(sizes.size(),VK_NULL_HANDLE); m.childSizes=sizes; m.childTypes.assign(sizes.size(),UINT32_MAX);
            m.childGenerations.assign(sizes.size(),0);
            m.coldGroups.resize(sizes.size());
        } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        initializedNow=true;
        if(d.autoEnabled) {
            auto r=createPoolViews(d,m,sizes); if(r!=VK_SUCCESS) { discardFirstBacking(); return r; }
            for(auto bits:m.poolViewMemoryTypeBits) m.backingMemoryTypeBits&=bits;
        }
        if(!m.backingMemoryTypeBits) { discardFirstBacking(); return VK_ERROR_FEATURE_NOT_PRESENT; }
        std::vector<std::uint32_t> types;
        try { VkMemoryRequirements all{}; all.memoryTypeBits=m.backingMemoryTypeBits; types=backingMemoryTypes(d,all); }
        catch(const std::bad_alloc&) { discardFirstBacking(); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        if(types.empty()) { discardFirstBacking(); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
        const bool lazy=d.lazyBacking && d.autoEnabled && d.rangeChunkBytes &&
            d.residentLimitBytes && d.residentAdmissionArmed;
        m.trackPhysicalStats=true;
        for(std::size_t i=0;i<sizes.size();i++) {
            if(lazy) {
                auto& group=m.coldGroups[i];
                group.cold=true; group.pristine=true; group.logicalBytes=sizes[i];
                m.cold=true; m.coldLogicalSize+=sizes[i]; d.coldLogicalBytes+=sizes[i];
                continue;
            }
            VkResult r=VK_ERROR_OUT_OF_DEVICE_MEMORY;
            for(auto type:types) { r=allocateBackingChild(d,device,sizes[i],type,m.allocationFlags,m.hasPriority,m.priority,&m.children[i]); if(r==VK_SUCCESS) { m.childTypes[i]=type; bumpAsyncVersion(d,m.childGenerations[i]); break; } }
            if(r!=VK_SUCCESS) { discardFirstBacking(); return r; }
            trackBackingAllocation(d,m.childTypes[i],sizes[i]); m.trackPhysicalStats=true;
        }
        if(!lazy) for(auto n:sizes) { m.residentBytes+=n; d.residentBytes+=n; }
        m.everBound=true;
    }
    const auto compatibleTypeBits=m.backingMemoryTypeBits&req.memoryTypeBits;
    if(!compatibleTypeBits) { discardFirstBacking(); return VK_ERROR_FEATURE_NOT_PRESENT; }
    for(std::size_t i=0;i<m.children.size();i++) {
        VkDeviceSize base=0; for(std::size_t j=0;j<i;j++) base+=m.childSizes[j];
        const auto lo=std::max(memoryOffset,base), hi=std::min(end,base+m.childSizes[i]);
        if(lo<hi && ((m.children[i] && (req.memoryTypeBits&(1u<<m.childTypes[i]))==0) ||
           (lo-memoryOffset)%req.alignment || (lo-base)%req.alignment || (hi-lo)%req.alignment)) { discardFirstBacking(); return VK_ERROR_FEATURE_NOT_PRESENT; }
    }
    VirtualMemory::Binding binding{buffer,memoryOffset,req.size,req.alignment};
    try { m.bindings.push_back(binding); }
    catch(const std::bad_alloc&) { discardFirstBacking(); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkDeviceSize childBase=0;
    for(std::size_t i=0;i<m.children.size();i++) {
        const auto childEnd=childBase+m.childSizes[i];
        const auto lo=std::max(memoryOffset,childBase), hi=std::min(end,childEnd);
        if(lo<hi && m.children[i] && !m.coldGroups[i].cold) {
            VkSparseMemoryBind bind{}; bind.resourceOffset=lo-memoryOffset; bind.size=hi-lo;
            bind.memory=m.children[i]; bind.memoryOffset=lo-childBase;
            const auto r=bindSparseLocked(device,d,buffer,&bind,1);
            if(r!=VK_SUCCESS) {
                bool rollbackFailed=false;
                for(std::size_t j=0;j<i;j++) {
                    VkDeviceSize base=0; for(std::size_t k=0;k<j;k++) base+=m.childSizes[k];
                    const auto a=std::max(memoryOffset,base), z=std::min(end,base+m.childSizes[j]);
                    if(a<z && m.children[j] && !m.coldGroups[j].cold) { VkSparseMemoryBind undo{}; undo.resourceOffset=a-memoryOffset; undo.size=z-a; if(bindSparseLocked(device,d,buffer,&undo,1)!=VK_SUCCESS) rollbackFailed=true; }
                }
                if(rollbackFailed || d.gpuGateError!=VK_SUCCESS) {
                    d.gpuGateError=r; d.autoEnabled=false; d.stopWorker.store(true); m.bound=true; return r;
                }
                m.bindings.pop_back(); discardFirstBacking(); return r;
            }
        }
        childBase=childEnd;
    }
    m.bound=true;
    bumpAsyncVersion(d,m.bindingGeneration);
    m.backingMemoryTypeBits=compatibleTypeBits;
    if(initializedNow && d.autoInitialized) logSnapshotState("bootstrap-bind",d);
    return VK_SUCCESS;
}
VkResult bindSparse(VkDevice d,Device& state,VkBuffer buffer,const VkSparseMemoryBind* binds,std::uint32_t count) {
    std::lock_guard<std::mutex> queueLock(state.queueMutex);
    return bindSparseLocked(d,state,buffer,binds,count);
}
void releaseChildren(Device& d,VirtualMemory& memory) {
    if(memory.cacheStoredBytes) {
        d.cacheBytes-=memory.cacheStoredBytes; memory.cacheStoredBytes=0;
        ++d.coldBudgetGeneration; d.activity.notify_all();
    }
    destroyPoolViews(d,memory);
    for(std::size_t i=0;i<memory.children.size();i++) {
        const auto child=memory.children[i];
        if(!child) continue;
        if(memory.trackPhysicalStats && i<memory.childSizes.size() && i<memory.childTypes.size()) {
            const auto type=memory.childTypes[i];
            const bool local=type<d.memory.memoryTypeCount &&
                (d.memory.memoryHeaps[d.memory.memoryTypes[type].heapIndex].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
            auto& live=local?d.liveLocal:d.liveOther;
            live=live>=memory.childSizes[i]?live-memory.childSizes[i]:0;
        }
        if(child && !d.gpuRestoreUnsafe) d.free(d.handle,child,memory.hasAdoptedCallbacks && i==0?&memory.adoptedCallbacks:nullptr);
    }
    memory.hasAdoptedCallbacks=false;
    memory.children.clear(); memory.childSizes.clear(); memory.childTypes.clear(); memory.childGenerations.clear(); memory.coldGroups.clear(); memory.residentBytes=0;
}
void releaseBackingChild(Device& d,VirtualMemory& memory,std::size_t i) {
    if(d.gpuRestoreUnsafe) return;
    if(i>=memory.children.size() || !memory.children[i]) return;
    const auto child=memory.children[i];
    if(memory.trackPhysicalStats && i<memory.childSizes.size() && i<memory.childTypes.size()) {
        const auto type=memory.childTypes[i];
        const bool local=type<d.memory.memoryTypeCount &&
            (d.memory.memoryHeaps[d.memory.memoryTypes[type].heapIndex].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        auto& live=local?d.liveLocal:d.liveOther;
        live=live>=memory.childSizes[i]?live-memory.childSizes[i]:0;
    }
    const auto freeStarted=d.gpuProfileEnabled?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    d.free(d.handle,child,memory.hasAdoptedCallbacks && i==0?&memory.adoptedCallbacks:nullptr);
    if(d.gpuProfileEnabled) {
        d.gpuProfileFreeCalls.fetch_add(1,std::memory_order_relaxed);
        d.gpuProfileFreeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-freeStarted).count()),std::memory_order_relaxed);
    }
    if(i==0) memory.hasAdoptedCallbacks=false;
    if(i<memory.childGenerations.size()) bumpAsyncVersion(d,memory.childGenerations[i]);
    memory.children[i]=VK_NULL_HANDLE;
}
void trackBackingAllocation(Device& d,std::uint32_t type,VkDeviceSize size) {
    const auto heap=type<d.memory.memoryTypeCount?d.memory.memoryTypes[type].heapIndex:UINT32_MAX;
    const bool local=heap<d.memory.memoryHeapCount && (d.memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
    auto& live=local?d.liveLocal:d.liveOther; auto& peak=local?d.peakLocal:d.peakOther;
    live+=size; peak=std::max(peak,live);
}
bool initSnapshotResources(Device& d,std::uint32_t family) {
    auto get=[&](const char* name){return d.gdpa(d.handle,name);};
    auto& s=d.snapshot;
    s.deviceWaitIdle=reinterpret_cast<PFN_vkDeviceWaitIdle>(get("vkDeviceWaitIdle"));
    s.createCommandPool=reinterpret_cast<PFN_vkCreateCommandPool>(get("vkCreateCommandPool"));
    s.destroyCommandPool=reinterpret_cast<PFN_vkDestroyCommandPool>(get("vkDestroyCommandPool"));
    s.resetCommandPool=reinterpret_cast<PFN_vkResetCommandPool>(get("vkResetCommandPool"));
    s.allocateCommandBuffers=reinterpret_cast<PFN_vkAllocateCommandBuffers>(get("vkAllocateCommandBuffers"));
    s.beginCommandBuffer=reinterpret_cast<PFN_vkBeginCommandBuffer>(get("vkBeginCommandBuffer"));
    s.endCommandBuffer=reinterpret_cast<PFN_vkEndCommandBuffer>(get("vkEndCommandBuffer"));
    s.cmdCopyBuffer=reinterpret_cast<PFN_vkCmdCopyBuffer>(get("vkCmdCopyBuffer"));
    s.cmdPipelineBarrier=reinterpret_cast<PFN_vkCmdPipelineBarrier>(get("vkCmdPipelineBarrier"));
    s.queueSubmit=reinterpret_cast<PFN_vkQueueSubmit>(get("vkQueueSubmit"));
    s.createBuffer=reinterpret_cast<PFN_vkCreateBuffer>(get("vkCreateBuffer"));
    s.destroyBuffer=reinterpret_cast<PFN_vkDestroyBuffer>(get("vkDestroyBuffer"));
    s.getBufferMemoryRequirements=reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get("vkGetBufferMemoryRequirements"));
    s.allocateMemory=reinterpret_cast<PFN_vkAllocateMemory>(get("vkAllocateMemory"));
    s.freeMemory=reinterpret_cast<PFN_vkFreeMemory>(get("vkFreeMemory"));
    s.bindBufferMemory=reinterpret_cast<PFN_vkBindBufferMemory>(get("vkBindBufferMemory"));
    s.mapMemory=reinterpret_cast<PFN_vkMapMemory>(get("vkMapMemory"));
    s.unmapMemory=reinterpret_cast<PFN_vkUnmapMemory>(get("vkUnmapMemory"));
    if(!s.deviceWaitIdle || !s.createCommandPool || !s.destroyCommandPool || !s.resetCommandPool ||
       !s.allocateCommandBuffers || !s.beginCommandBuffer || !s.endCommandBuffer || !s.cmdCopyBuffer ||
       !s.cmdPipelineBarrier || !s.queueSubmit || !s.createBuffer || !s.destroyBuffer ||
       !s.getBufferMemoryRequirements || !s.allocateMemory || !s.freeMemory || !s.bindBufferMemory ||
       !s.mapMemory || !s.unmapMemory || !d.queueWaitIdle || !d.setDeviceLoaderData) return false;
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex=family;
    if(s.createCommandPool(d.handle,&pool,nullptr,&s.commandPool)!=VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool=s.commandPool; command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount=1;
    if(s.allocateCommandBuffers(d.handle,&command,&s.commandBuffer)!=VK_SUCCESS) return false;
    if(d.setDeviceLoaderData(d.handle,s.commandBuffer)!=VK_SUCCESS) return false;
    // Batch large ranges without growing staging with the application's pool.
    if(d.rangeChunkBytes>s.chunkSize) s.stagingSize=4*s.chunkSize;
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; buffer.size=s.stagingSize;
    buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    auto makeStaging=[&](VkBuffer& handle,VkDeviceMemory& memory,void*& mapped) {
        if(s.createBuffer(d.handle,&buffer,nullptr,&handle)!=VK_SUCCESS) return false;
        VkMemoryRequirements req{}; s.getBufferMemoryRequirements(d.handle,handle,&req);
        std::uint32_t type=UINT32_MAX; int bestScore=-1;
        for(std::uint32_t i=0;i<d.memory.memoryTypeCount;i++) {
            if(!(req.memoryTypeBits&(1u<<i))) continue;
            const auto flags=d.memory.memoryTypes[i].propertyFlags;
            if((flags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))!=
               (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
            const auto heap=d.memory.memoryTypes[i].heapIndex;
            const bool local=heap<d.memory.memoryHeapCount && (d.memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
            const bool cached=(flags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)!=0;
            const int score=!local&&cached?4:cached?3:!local?2:1;
            if(score>bestScore) { type=i; bestScore=score; }
        }
        if(type==UINT32_MAX) return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize=req.size; allocation.memoryTypeIndex=type;
        if(s.allocateMemory(d.handle,&allocation,nullptr,&memory)!=VK_SUCCESS) return false;
        if(s.bindBufferMemory(d.handle,handle,memory,0)!=VK_SUCCESS) return false;
        void* address{};
        if(s.mapMemory(d.handle,memory,0,s.stagingSize,0,&address)!=VK_SUCCESS) return false;
        mapped=address;
        return true;
    };
    if(!makeStaging(s.stagingBuffer,s.stagingMemory,s.mapped)) return false;
    if(d.rangeChunkBytes && d.selectiveRestore &&
       !makeStaging(s.lookaheadBuffer,s.lookaheadMemory,s.lookaheadMapped)) {
        if(s.lookaheadBuffer) s.destroyBuffer(d.handle,s.lookaheadBuffer,nullptr);
        if(s.lookaheadMemory) s.freeMemory(d.handle,s.lookaheadMemory,nullptr);
        s.lookaheadBuffer=VK_NULL_HANDLE; s.lookaheadMemory=VK_NULL_HANDLE;
        logf("Vulkan snapshot lookahead unavailable: using serial restore");
    }
    logf("Vulkan snapshot transfer staging bytes=%llu",static_cast<unsigned long long>(s.stagingSize));
    logf("Vulkan snapshot decode max-workers=%u",s.stagingSize>s.chunkSize?4u:1u);
    if(s.lookaheadMapped) logf("Vulkan snapshot lookahead staging bytes=%llu",static_cast<unsigned long long>(s.stagingSize));
    if(d.gpuRestoreEnabled) {
        const bool bp16=d.snapshotCodec==zvram::snapshot::Codec::BP16;
        const char* codecName=bp16?"BP16":"GDeflate";
        try { d.gpuDecoder=std::make_unique<zvram::gdeflate::gpu::Decoder>(); }
        catch(const std::bad_alloc&) { d.gpuRestoreEnabled=false; }
        const auto* path=std::getenv(bp16?"ZVRAM_BP16_SHADER_PATH":"ZVRAM_GDEFLATE_SHADER_PATH");
        const auto format=bp16?zvram::gdeflate::gpu::Format::BP16:zvram::gdeflate::gpu::Format::GDeflate;
        const auto result=d.gpuDecoder && path?d.gpuDecoder->initialize(d.handle,d.gdpa,d.memory,d.copyQueue,family,
            d.setDeviceLoaderData,path,&d.gpuProperties,format,d.gpuTimestampBits,
            bp16 && d.gpuImportHostInput,d.gpuImportHostAlignment,
            bp16 && d.gpuAllocatedHostInput,d.gpuAllocatedHostBudgetBytes):VK_ERROR_INITIALIZATION_FAILED;
        if(result!=VK_SUCCESS) {
            d.gpuRestoreEnabled=false; d.gpuDecoder.reset();
            logf("GPU %s restore unavailable result=%d; retaining CPU codec",codecName,result);
            if(result==VK_ERROR_DEVICE_LOST) { d.gpuGateError=result; return false; }
        } else {
            logf("GPU %s restore enabled: %s, %s, direct backing output",codecName,bp16?"256-thread":"wave32",
                d.gpuDecoder->importedHostInputEnabled()?"cached imported host input":
                d.gpuDecoder->allocatedHostInputEnabled()?"cached allocated host input":
                d.gpuDecoder->hostInputEnabled()?"direct coherent host input":"compressed upload");
            if(bp16) logf("GPU BP16 upload workers=%u",d.gpuDecoder->uploadWorkers());
        }
    }
    return true;
}
void releaseSnapshotResources(Device& d) {
    const auto allocatedHostLiveBytes=d.gpuDecoder
        ?d.gpuDecoder->allocatedHostInputUsedBytes():0;
    const auto allocatedHostLimitBytes=d.gpuDecoder
        ?d.gpuDecoder->allocatedHostInputLimitBytes():0;
    logGpuProfileSummary(d," final=1",true);
    d.gpuDecoder.reset();
    if(d.gpuImportHostInput)
        logf("GPU BP16 imported input imports=%llu reuses=%llu bytes=%llu",
             static_cast<unsigned long long>(d.gpuImportedFrames),
             static_cast<unsigned long long>(d.gpuImportedReuses),
             static_cast<unsigned long long>(d.gpuImportedBytes));
    if(d.gpuAllocatedHostInput)
        logf("GPU BP16 allocated input allocations=%llu reuses=%llu bytes=%llu live-bytes=%llu limit-bytes=%llu",
             static_cast<unsigned long long>(d.gpuAllocatedHostAllocations),
             static_cast<unsigned long long>(d.gpuAllocatedHostReuses),
             static_cast<unsigned long long>(d.gpuAllocatedHostBytes),
             static_cast<unsigned long long>(allocatedHostLiveBytes),
             static_cast<unsigned long long>(allocatedHostLimitBytes));
    if(d.gpuDecodeCalls || d.gpuDecodeFallbacks)
        logf("GPU %s restore calls=%llu bytes=%llu host-ns=%llu fallbacks=%llu",
             d.snapshotCodec==zvram::snapshot::Codec::BP16?"BP16":"GDeflate",
             static_cast<unsigned long long>(d.gpuDecodeCalls),static_cast<unsigned long long>(d.gpuDecodeBytes),
             static_cast<unsigned long long>(d.gpuDecodeNanoseconds),static_cast<unsigned long long>(d.gpuDecodeFallbacks));
    auto& s=d.snapshot;
    if(d.gpuRestoreUnsafe) { s={}; return; }
    if(s.mapped) logf("snapshot transfer profile copy-calls=%llu copy-bytes=%llu copy-ns=%llu decode-bytes=%llu decode-ns=%llu",
        static_cast<unsigned long long>(s.copyCalls),static_cast<unsigned long long>(s.copyBytes),
        static_cast<unsigned long long>(s.copyNanoseconds),static_cast<unsigned long long>(s.decodeBytes),
        static_cast<unsigned long long>(s.decodeNanoseconds));
    if(s.lookaheadMapped && s.unmapMemory) s.unmapMemory(d.handle,s.lookaheadMemory);
    if(s.lookaheadBuffer && s.destroyBuffer) s.destroyBuffer(d.handle,s.lookaheadBuffer,nullptr);
    if(s.lookaheadMemory && s.freeMemory) s.freeMemory(d.handle,s.lookaheadMemory,nullptr);
    if(s.mapped && s.unmapMemory) s.unmapMemory(d.handle,s.stagingMemory);
    if(s.stagingBuffer && s.destroyBuffer) s.destroyBuffer(d.handle,s.stagingBuffer,nullptr);
    if(s.stagingMemory && s.freeMemory) s.freeMemory(d.handle,s.stagingMemory,nullptr);
    if(s.commandPool && s.destroyCommandPool) s.destroyCommandPool(d.handle,s.commandPool,nullptr);
    s={};
}
VkResult copyChunkLocked(Device& d,VkBuffer source,VkBuffer destination,VkDeviceSize sourceOffset,
                         VkDeviceSize destinationOffset,VkDeviceSize bytes,bool restoring) {
    auto& s=d.snapshot;
    const auto started=std::chrono::steady_clock::now();
    VkResult r=s.resetCommandPool(d.handle,s.commandPool,0); if(r!=VK_SUCCESS) return r;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r=s.beginCommandBuffer(s.commandBuffer,&begin); if(r!=VK_SUCCESS) return r;
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask=restoring?VK_ACCESS_HOST_WRITE_BIT:VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask=restoring?VK_ACCESS_TRANSFER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT;
    s.cmdPipelineBarrier(s.commandBuffer,restoring?VK_PIPELINE_STAGE_HOST_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
    VkBufferCopy region{}; region.srcOffset=sourceOffset; region.dstOffset=destinationOffset; region.size=bytes;
    s.cmdCopyBuffer(s.commandBuffer,source,destination,1,&region);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask=restoring?(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT):VK_ACCESS_HOST_READ_BIT;
    s.cmdPipelineBarrier(s.commandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,
                         restoring?VK_PIPELINE_STAGE_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_HOST_BIT,
                         0,1,&after,0,nullptr,0,nullptr);
    r=s.endCommandBuffer(s.commandBuffer); if(r!=VK_SUCCESS) return r;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&s.commandBuffer;
    r=s.queueSubmit(d.copyQueue?d.copyQueue:d.sparseQueue,1,&submit,VK_NULL_HANDLE);
    const auto queue=d.copyQueue?d.copyQueue:d.sparseQueue;
    if(r==VK_SUCCESS) r=d.queueWaitIdle(queue);
    ++s.copyCalls; s.copyBytes+=bytes;
    s.copyNanoseconds+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
    return r;
}
void discardCleanCacheLocked(Device& d,VirtualMemory& memory,std::size_t i) {
    auto& group=memory.coldGroups[i];
    if(group.cold || !group.storedBytes) return;
    d.cacheBytes-=group.accountedBytes(); memory.cacheStoredBytes-=group.accountedBytes();
    group.chunks.clear(); group.logicalBytes=0; group.storedBytes=0; group.importedPaddingBytes=0;
    ++d.cacheInvalidations; ++d.coldBudgetGeneration; d.activity.notify_all();
}
// Clean copies are expendable; cold copies remain the only lossless backing.
// Both share the existing cold-store quota, so retaining copies cannot grow RAM
// beyond that quota. Drop clean copies only when a new snapshot needs room.
void trimCleanCacheLocked(Device& d,VkDeviceSize required) {
    if(required>d.coldBudget) return;
    if(d.cleanCachePolicy!=zvram::clean_cache::Policy::First) {
        while(zvram::clean_cache::needsTrim(d.coldBytes,d.cacheBytes,d.coldBudget,required)) {
            VirtualMemory* victimMemory=nullptr;
            std::size_t victimIndex=0;
            zvram::clean_cache::Candidate victim{};
            bool found=false;
            for(auto& pair:d.virtualMemory) {
                auto& memory=pair.second;
                for(std::size_t i=0;i<memory.coldGroups.size();++i) {
                    const auto& group=memory.coldGroups[i];
                    if(group.cold || !group.storedBytes) continue;
                    const zvram::clean_cache::Candidate candidate{
                        group.lastUse,memory.identityGeneration,i};
                    const bool preferred=d.cleanCachePolicy==zvram::clean_cache::Policy::Lru
                        ? zvram::clean_cache::older(candidate,victim)
                        : zvram::clean_cache::newer(candidate,victim);
                    if(!found || preferred) {
                        victimMemory=&memory;
                        victimIndex=i;
                        victim=candidate;
                        found=true;
                    }
                }
            }
            if(!found) return;
            auto& group=victimMemory->coldGroups[victimIndex];
            logf("clean snapshot cache trimmed bytes=%llu",static_cast<unsigned long long>(group.storedBytes));
            discardCleanCacheLocked(d,*victimMemory,victimIndex);
        }
        return;
    }
    for(auto& pair:d.virtualMemory) {
        for(std::size_t i=0;i<pair.second.coldGroups.size();++i) {
            if(!zvram::clean_cache::needsTrim(d.coldBytes,d.cacheBytes,d.coldBudget,required)) return;
            const auto& group=pair.second.coldGroups[i];
            if(!group.cold && group.storedBytes)
                logf("clean snapshot cache trimmed bytes=%llu",static_cast<unsigned long long>(group.storedBytes));
            discardCleanCacheLocked(d,pair.second,i);
        }
    }
}
// Sample the raw native heap, never the public synthetic budget slot. Usage
// already includes backing and private helper memory; subtract backing only.
// Estimates can change immediately after a query: retain the hard cap too.
VkResult residentAdmissionLimit(Device& d,VkDeviceSize& limit) {
    limit=d.residentLimitBytes;
    if(!d.budgetReserveBytes) return VK_SUCCESS;
    if(!d.budgetProperties || d.budgetHeap>=d.memory.memoryHeapCount) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkDeviceSize tracked=0;
    for(const auto& pair:d.virtualMemory) {
        const auto& memory=pair.second;
        if(memory.children.size()!=memory.childSizes.size() || memory.children.size()!=memory.childTypes.size())
            return VK_ERROR_UNKNOWN;
        for(std::size_t i=0;i<memory.children.size();++i) {
            if(!memory.children[i]) continue;
            if(memory.childTypes[i]>=d.memory.memoryTypeCount) return VK_ERROR_UNKNOWN;
            if(d.memory.memoryTypes[memory.childTypes[i]].heapIndex!=d.budgetHeap) continue;
            if(memory.childSizes[i]>std::numeric_limits<VkDeviceSize>::max()-tracked) return VK_ERROR_UNKNOWN;
            tracked+=memory.childSizes[i];
        }
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext=&budget;
    d.budgetProperties(d.physical,&properties);
    if(d.budgetHeap>=properties.memoryProperties.memoryHeapCount) return VK_ERROR_FEATURE_NOT_PRESENT;
    const auto total=std::min(budget.heapBudget[d.budgetHeap],d.memory.memoryHeaps[d.budgetHeap].size);
    const auto usage=budget.heapUsage[d.budgetHeap];
    limit=zvram::residentBudgetLimit(limit,total,usage,tracked,d.budgetReserveBytes);
    if(limit!=d.lastBudgetLimit) {
        logf("resident budget native-heap=%u budget=%llu usage=%llu tracked-local=%llu reserve=%llu effective-limit=%llu hard-limit=%llu",
             d.budgetHeap,static_cast<unsigned long long>(total),static_cast<unsigned long long>(usage),
             static_cast<unsigned long long>(tracked),static_cast<unsigned long long>(d.budgetReserveBytes),
             static_cast<unsigned long long>(limit),static_cast<unsigned long long>(d.residentLimitBytes));
        d.lastBudgetLimit=limit;
    }
    return VK_SUCCESS;
}
VkResult restoreColdLocked(VkDevice device,Device& d,VkDeviceMemory only,std::size_t childOnly,
                          VkBuffer preparedBuffer,VkDeviceSize preparedBytes) {
    d.restoreBudgetRefused=false;
    if(d.gpuGateError!=VK_SUCCESS) return d.gpuGateError;
    auto& s=d.snapshot;
    if(preparedBuffer && (!only || childOnly==SIZE_MAX || !preparedBytes || preparedBytes>s.stagingSize))
        return VK_ERROR_UNKNOWN;
    // Guard every restore entry, including conservative fallback and non-submit
    // calls. Admission must evict first; restoration never silently exceeds the
    // configured tracked-backing cap. Preflight all missing children before any
    // allocation so a rejected full restore cannot materialize a partial model.
    if(d.residentLimitBytes && d.residentAdmissionArmed) {
        VkDeviceSize limit{};
        const auto result=residentAdmissionLimit(d,limit); if(result!=VK_SUCCESS) return result;
        VkDeviceSize incoming=0;
        for(const auto& pair:d.virtualMemory) {
            if(only && pair.first!=only) continue;
            const auto& memory=pair.second;
            if(!memory.cold) continue;
            if(memory.children.size()!=memory.childSizes.size() ||
               memory.children.size()!=memory.coldGroups.size()) return VK_ERROR_UNKNOWN;
            for(std::size_t i=0;i<memory.children.size();++i) {
                if(childOnly!=SIZE_MAX && i!=childOnly) continue;
                if(!memory.coldGroups[i].cold || memory.children[i]) continue;
                const auto amount=memory.childSizes[i];
                if(amount>limit-incoming) { d.restoreBudgetRefused=true; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
                incoming+=amount;
            }
        }
        if(d.residentBytes>limit-incoming) { d.restoreBudgetRefused=true; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    }
    bool restoredAny=false;
    std::uint64_t restoredThisCall=0;
    for(auto& pair:d.virtualMemory) {
        if(only && pair.first!=only) continue;
        auto& memory=pair.second;
        if(!memory.cold) continue;
        // Active snapshots never unbind a pool with pending references. Restore
        // visibility is acquired by every app queue before subsequent work.
        VkResult r=VK_SUCCESS;
        if(!d.activeEviction) {
            r=s.deviceWaitIdle(device); if(r!=VK_SUCCESS) return r;
        }
        if(memory.children.size()!=memory.childSizes.size() ||
           memory.children.size()!=memory.childTypes.size() || memory.children.size()!=memory.coldGroups.size() ||
           memory.poolViews.size()!=memory.children.size()) return VK_ERROR_UNKNOWN;
        std::vector<std::uint32_t> types;
        try {
            VkMemoryRequirements req{}; req.memoryTypeBits=memory.backingMemoryTypeBits;
            types=backingMemoryTypes(d,req);
        } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        if(types.empty()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        for(std::size_t i=0;i<memory.coldGroups.size();i++) {
            if(childOnly!=SIZE_MAX && i!=childOnly) continue;
            auto& group=memory.coldGroups[i];
            const auto amount=memory.childSizes[i];
            if(!group.cold) continue;
            if(preparedBuffer && preparedBytes!=group.logicalBytes) return VK_ERROR_UNKNOWN;
            if(group.pristine && (preparedBuffer || group.logicalBytes!=amount ||
               group.storedBytes || !group.chunks.empty())) return VK_ERROR_UNKNOWN;
            bool allocatedNow=false;
            if(!memory.children[i]) {
                VkDeviceMemory child{}; std::uint32_t used=UINT32_MAX; r=VK_ERROR_OUT_OF_DEVICE_MEMORY;
                for(auto type:types) {
                    r=allocateBackingChild(d,device,amount,type,memory.allocationFlags,memory.hasPriority,memory.priority,&child);
                    if(r==VK_SUCCESS) { used=type; break; }
                }
                if(r!=VK_SUCCESS) return r;
                memory.children[i]=child; memory.childTypes[i]=used;
                if(i<memory.childGenerations.size()) bumpAsyncVersion(d,memory.childGenerations[i]);
                if(memory.trackPhysicalStats) trackBackingAllocation(d,used,amount);
                memory.residentBytes+=amount; d.residentBytes+=amount;
                allocatedNow=true;
            }
            if(!group.pristine && !group.restoreBound) {
                VkSparseMemoryBind bind{}; bind.size=amount; bind.memory=memory.children[i];
                r=bindSparseLocked(device,d,memory.poolViews[i],&bind,1);
                if(r!=VK_SUCCESS) return r;
                group.restoreBound=true;
            }
            VkDeviceSize offset=group.pristine?group.logicalBytes:0;
            if(preparedBuffer) {
                r=copyChunkLocked(d,preparedBuffer,memory.poolViews[i],0,0,preparedBytes,true);
                if(r==VK_SUCCESS) offset=preparedBytes;
            }
            for(std::size_t next=0;!preparedBuffer && next<group.chunks.size();) {
                auto& gpuChunk=group.chunks[next];
                if(d.gpuRestoreEnabled && d.gpuDecoder && gpuChunk.compressed &&
                   gpuChunk.codec==d.snapshotCodec && gpuChunk.codec!=zvram::snapshot::Codec::Zstd && !gpuChunk.byteShuffle &&
                   gpuChunk.rawSize && gpuChunk.rawSize<=s.chunkSize && offset<=group.logicalBytes &&
                   gpuChunk.rawSize<=group.logicalBytes-offset && offset%d.gpuStorageAlignment==0 &&
                   ((gpuChunk.rawSize+3)&~VkDeviceSize(3))<=d.gpuStorageRange && offset<=amount &&
                   ((gpuChunk.rawSize+3)&~VkDeviceSize(3))<=amount-offset) {
                    if((d.gpuImportHostInput || d.gpuAllocatedHostInput) && !gpuChunk.imported) {
                        zvram::gdeflate::gpu::Decoder::ImportedHostInputPtr imported;
                        const auto importResult=d.gpuImportHostInput
                            ?d.gpuDecoder->importHostInput(gpuChunk.data(),gpuChunk.size(),imported)
                            :d.gpuDecoder->allocateHostInput(gpuChunk.data(),gpuChunk.size(),imported);
                        if(importResult==VK_SUCCESS && imported) {
                            const auto padding=imported->allocationBytes-gpuChunk.size();
                            const auto charged=gpuChunk.size()<=std::numeric_limits<std::uint64_t>::max()-padding
                                ?gpuChunk.size()+padding:std::numeric_limits<std::uint64_t>::max();
                            if(zvram::gdeflate::gpu::Decoder::importedHostFitsBudget(
                                   d.coldBytes,d.cacheBytes,padding,d.coldBudget) &&
                               padding<=std::numeric_limits<VkDeviceSize>::max()-group.importedPaddingBytes &&
                               retainCompression(gpuChunk.rawSize,charged,d.minSavingsPercent)) {
                                gpuChunk.imported=std::move(imported);
                                gpuChunk.bytes.clear(); std::vector<std::uint8_t>().swap(gpuChunk.bytes);
                                group.importedPaddingBytes+=padding;
                                d.coldBytes+=padding; memory.coldStoredBytes+=padding;
                            }
                        }
                    }
                    const auto gpuStarted=std::chrono::steady_clock::now();
                    const auto gpuResult=d.gpuDecoder->restore(gpuChunk.data(),gpuChunk.size(),
                        memory.poolViews[i],offset,static_cast<std::size_t>(gpuChunk.rawSize),gpuChunk.imported.get());
                    d.gpuDecodeNanoseconds+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-gpuStarted).count();
                    if(gpuResult==VK_SUCCESS) {
                        ++d.gpuDecodeCalls; d.gpuDecodeBytes+=gpuChunk.rawSize;
                        if(gpuChunk.imported) {
                            if(d.gpuAllocatedHostInput) {
                                if(gpuChunk.hostInputUsed) ++d.gpuAllocatedHostReuses;
                                else {
                                    ++d.gpuAllocatedHostAllocations;
                                    d.gpuAllocatedHostBytes+=gpuChunk.imported->allocationBytes;
                                }
                            } else if(gpuChunk.hostInputUsed) ++d.gpuImportedReuses;
                            else {
                                ++d.gpuImportedFrames;
                                d.gpuImportedBytes+=gpuChunk.imported->allocationBytes;
                            }
                            gpuChunk.hostInputUsed=true;
                        }
                        offset+=gpuChunk.rawSize; ++next; continue;
                    }
                    if(d.gpuDecoder->unsafe() || gpuResult==VK_TIMEOUT || gpuResult==VK_ERROR_DEVICE_LOST) {
                        d.gpuRestoreUnsafe=true; d.gpuGateError=VK_ERROR_DEVICE_LOST;
                        d.autoEnabled=false; d.stopWorker.store(true); d.activity.notify_all();
                        // The private view and cold bytes remain live: timeout
                        // cannot cancel dispatched work or permit rebinding.
                        return VK_ERROR_DEVICE_LOST;
                    }
                    ++d.gpuDecodeFallbacks;
                    logf("GPU %s restore fallback result=%d",d.snapshotCodec==zvram::snapshot::Codec::BP16?"BP16":"GDeflate",gpuResult);
                }
                std::array<zvram::snapshot::EncodedChunk,4> batch{};
                std::size_t count=0;
                VkDeviceSize staged=0;
                while(count<batch.size() && next<group.chunks.size()) {
                    const auto& chunk=group.chunks[next];
                    if(count && d.gpuRestoreEnabled && chunk.compressed && chunk.codec!=zvram::snapshot::Codec::Zstd) break;
                    if(chunk.rawSize>s.chunkSize || chunk.rawSize>group.logicalBytes-offset-staged) {
                        r=VK_ERROR_UNKNOWN; break;
                    }
                    if(chunk.rawSize>s.stagingSize-staged) break;
                    batch[count++]={chunk.data(),chunk.size(),static_cast<std::size_t>(chunk.rawSize),chunk.compressed,chunk.byteShuffle,chunk.codec};
                    staged+=chunk.rawSize; ++next;
                }
                if(r!=VK_SUCCESS || !count) { r=VK_ERROR_UNKNOWN; break; }
                const auto decodeStarted=std::chrono::steady_clock::now();
                if(!zvram::snapshot::decodeBatch(batch.data(),count,static_cast<std::uint8_t*>(s.mapped),
                    static_cast<std::size_t>(s.stagingSize),static_cast<std::size_t>(s.chunkSize))) {
                    r=VK_ERROR_UNKNOWN; break;
                }
                s.decodeBytes+=staged;
                s.decodeNanoseconds+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-decodeStarted).count();
                r=copyChunkLocked(d,s.stagingBuffer,memory.poolViews[i],0,offset,staged,true);
                if(r!=VK_SUCCESS) break;
                offset+=staged;
            }
            if(r==VK_SUCCESS && offset!=group.logicalBytes) r=VK_ERROR_UNKNOWN;
            if(r!=VK_SUCCESS) {
                VkSparseMemoryBind unbind{}; unbind.size=amount;
                if(group.restoreBound && bindSparseLocked(device,d,memory.poolViews[i],&unbind,1)!=VK_SUCCESS) d.gpuGateError=VK_ERROR_DEVICE_LOST;
                group.restoreBound=false;
                return r;
            }
            const bool finishingView=group.restoreBound;
            r=bindChildAppsLocked(device,d,memory,i,false,
                finishingView?memory.poolViews[i]:VK_NULL_HANDLE);
            if(r!=VK_SUCCESS) {
                // The failed merged transition can leave either mapping in
                // flight. Retain the view, backing and cold data behind the gate.
                if(finishingView) { d.gpuGateError=r; return r; }
                const auto rollback=bindChildAppsLocked(device,d,memory,i,true);
                if(rollback!=VK_SUCCESS) d.gpuGateError=VK_ERROR_DEVICE_LOST;
                else if(allocatedNow && group.pristine && d.gpuGateError==VK_SUCCESS) {
                    releaseBackingChild(d,memory,i);
                    memory.residentBytes-=amount; d.residentBytes-=amount;
                }
                return r;
            }
            group.restoreBound=false;
            memory.bound=!memory.bindings.empty();
            const auto accounted=group.accountedBytes();
            d.coldBytes-=accounted; d.coldLogicalBytes-=group.logicalBytes;
            // Bootstrap admission permits the whole model to remain resident.
            // Keep no redundant snapshots until the resident cap is armed;
            // the cold chunks remain intact until restore has fully succeeded.
            const bool retainClean=d.cleanCache && d.residentAdmissionArmed && !group.pristine;
            if(retainClean) {
                d.cacheBytes+=accounted; memory.cacheStoredBytes+=accounted;
            } else if(group.storedBytes) {
                ++d.coldBudgetGeneration; d.lastActivity=std::chrono::steady_clock::now(); d.activity.notify_all();
            }
            memory.coldStoredBytes-=accounted; memory.coldLogicalSize-=group.logicalBytes;
            if(!retainClean) { group.chunks.clear(); group.logicalBytes=0; group.storedBytes=0; group.importedPaddingBytes=0; }
            group.cold=false; group.pristine=false; group.restoreBound=false;
            restoredAny=true; ++d.restoreCount; ++restoredThisCall;
            memory.cold=std::any_of(memory.coldGroups.begin(),memory.coldGroups.end(),[](const auto& entry){return entry.cold;});
            const bool coldRemains=std::any_of(d.virtualMemory.begin(),d.virtualMemory.end(),
                [only](const auto& entry){return (!only || entry.first==only) && entry.second.cold;});
            if(coldRemains && d.restoreFailureAfterGroups && !d.restoreFailureInjected &&
               restoredThisCall>=d.restoreFailureAfterGroups) {
                d.restoreFailureInjected=true; ++d.snapshotFailures; d.lastSnapshotError=VK_ERROR_OUT_OF_DEVICE_MEMORY;
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
        }
        memory.cold=std::any_of(memory.coldGroups.begin(),memory.coldGroups.end(),[](const auto& group){return group.cold;});
    }
    if(restoredAny && d.autoInitialized) {
        VkResult visible=VK_SUCCESS;
        if(d.activeEviction) {
            // Each application queue acquires the latest restored writes before
            // its next submission, including queues that did not trigger restore.
            // Defer waits to avoid accumulating semaphores on a stalled queue.
            if(d.restoreGeneration==std::numeric_limits<std::uint64_t>::max()) visible=VK_ERROR_UNKNOWN;
            else ++d.restoreGeneration;
        } else visible=d.autoQueues.fromCopyQueue();
        if(visible!=VK_SUCCESS) {
            d.gpuGateError=visible; d.autoEnabled=false; d.stopWorker.store(true); d.activity.notify_all();
            return visible;
        }
        logSnapshotState("restore",d);
    }
    return VK_SUCCESS;
}
VkResult freezeChildLocked(Device& d,VirtualMemory& memory,std::size_t i) {
    if(i>=memory.children.size() || i>=memory.coldGroups.size() ||
       i>=memory.childSizes.size() || i>=memory.poolViews.size()) return VK_ERROR_UNKNOWN;
    auto& group=memory.coldGroups[i];
    if(!memory.children[i] || group.cold ||
       (group.budgetBlocked && group.failedBudgetGeneration==d.coldBudgetGeneration &&
        group.failedBudgetSubmissionGeneration==d.gpuSubmissionGeneration)) return VK_SUCCESS;
    const auto dataSize=memory.childSizes[i];
    const auto logicalBytes=dataSize;
    auto r=bindChildAppsLocked(d.handle,d,memory,i,true);
    if(r!=VK_SUCCESS) {
        ++d.snapshotFailures; d.lastSnapshotError=r; return r;
    }
    if(d.cleanCache && !group.chunks.empty()) {
        d.residentBytes-=memory.childSizes[i]; memory.residentBytes-=memory.childSizes[i];
        releaseBackingChild(d,memory,i);
        memory.bound=std::any_of(memory.children.begin(),memory.children.end(),[](VkDeviceMemory child){return child!=VK_NULL_HANDLE;});
            d.cacheBytes-=group.accountedBytes(); memory.cacheStoredBytes-=group.accountedBytes();
            group.cold=true; memory.cold=true;
            d.coldBytes+=group.accountedBytes(); d.coldLogicalBytes+=group.logicalBytes;
            memory.coldStoredBytes+=group.accountedBytes(); memory.coldLogicalSize+=group.logicalBytes;
        ++d.freezeCount; ++d.cleanReuseCount;
        logf("snapshot reused clean bytes=%llu stored=%llu",static_cast<unsigned long long>(group.logicalBytes),static_cast<unsigned long long>(group.storedBytes));
        logSnapshotState("clean-freeze",d);
        return VK_SUCCESS;
    }
    VkSparseMemoryBind viewBind{}; viewBind.size=dataSize; viewBind.memory=memory.children[i];
    r=bindSparseLocked(d.handle,d,memory.poolViews[i],&viewBind,1);
    if(r!=VK_SUCCESS) {
        const auto rebound=bindChildAppsLocked(d.handle,d,memory,i,false);
        if(rebound!=VK_SUCCESS) { d.gpuGateError=rebound; d.autoEnabled=false; d.stopWorker.store(true); }
        ++d.snapshotFailures; d.lastSnapshotError=r; return r;
    }
    group.budgetBlocked=false;
    VirtualMemory::ColdGroup candidate;
    VkDeviceSize stored=0;
    bool okay=true;
    bool budgetExceeded=false;
    try {
        candidate.chunks.reserve(static_cast<std::size_t>((logicalBytes+d.snapshot.chunkSize-1)/d.snapshot.chunkSize));
        std::vector<std::uint8_t> encoded;
        std::vector<std::uint8_t> shuffled;
        for(VkDeviceSize offset=0;offset<logicalBytes;offset+=d.snapshot.chunkSize) {
            const auto amount=std::min(d.snapshot.chunkSize,logicalBytes-offset);
            const auto stagedOffset=offset%d.snapshot.stagingSize;
            if(!stagedOffset) {
                r=copyChunkLocked(d,memory.poolViews[i],d.snapshot.stagingBuffer,offset,0,
                    std::min(d.snapshot.stagingSize,logicalBytes-offset),false);
                if(r!=VK_SUCCESS) { okay=false; break; }
            }
            const auto* source=static_cast<std::uint8_t*>(d.snapshot.mapped)+stagedOffset;
            VirtualMemory::ColdChunk chunk; chunk.rawSize=amount;
            std::size_t compressed=0;
            bool keepCompressed=false;
            if(d.minSavingsPercent<100) {
#ifdef ZVRAM_HAVE_GDEFLATE
                if(d.snapshotCodec==zvram::snapshot::Codec::GDeflate) {
                    if(!zvram::gdeflate::encode(source,static_cast<std::size_t>(amount),encoded,d.gdeflateWorkers)) {
                        okay=false; r=VK_ERROR_UNKNOWN; break;
                    }
                    compressed=encoded.size();
                    keepCompressed=retainCompression(amount,compressed,d.minSavingsPercent);
                } else
#endif
                if(d.snapshotCodec==zvram::snapshot::Codec::BP16) {
                    if(amount<=zvram::bp16::MaxRawBytes && amount%zvram::bp16::RawBytesPerBlock==0) {
                        if(!zvram::bp16::encodeFast(source,static_cast<std::size_t>(amount),encoded,d.bp16Workers)) {
                            okay=false; r=VK_ERROR_UNKNOWN; break;
                        }
                        compressed=encoded.size();
                        keepCompressed=retainCompression(amount,compressed,d.minSavingsPercent);
                    }
                } else {
                const auto* compressionSource=source;
                if(d.byteShuffle) {
                    shuffled.resize(static_cast<std::size_t>(amount));
                    if(!zvram::byte_shuffle(source,shuffled.data(),static_cast<std::size_t>(amount),d.byteShuffle)) {
                        okay=false; r=VK_ERROR_UNKNOWN; break;
                    }
                    compressionSource=shuffled.data();
                }
                encoded.resize(ZSTD_compressBound(static_cast<std::size_t>(amount)));
                compressed=ZSTD_compress(encoded.data(),encoded.size(),compressionSource,static_cast<std::size_t>(amount),1);
                keepCompressed=!ZSTD_isError(compressed) && retainCompression(amount,compressed,d.minSavingsPercent);
                }
            }
            if(keepCompressed) {
                // Transfer exact-sized BP16 storage; keep copying when spare capacity would evade the quota.
                if(d.snapshotCodec==zvram::snapshot::Codec::BP16 && encoded.size()==compressed && encoded.capacity()==compressed)
                    chunk.bytes=std::move(encoded);
                else { chunk.bytes.resize(compressed); std::memcpy(chunk.bytes.data(),encoded.data(),compressed); }
                chunk.compressed=true;
                chunk.byteShuffle=d.byteShuffle;
                chunk.codec=d.snapshotCodec;
            } else {
                chunk.bytes.resize(static_cast<std::size_t>(amount)); std::memcpy(chunk.bytes.data(),source,static_cast<std::size_t>(amount));
            }
            if(stored<=d.coldBudget && chunk.bytes.size()<=d.coldBudget-stored)
                trimCleanCacheLocked(d,stored+chunk.bytes.size());
            const auto remaining=d.coldBudget-d.coldBytes-d.cacheBytes;
            if(stored>remaining || chunk.bytes.size()>remaining-stored) { okay=false; budgetExceeded=true; break; }
            stored+=chunk.bytes.size(); candidate.chunks.push_back(std::move(chunk));
        }
    } catch(const std::bad_alloc&) { okay=false; r=VK_ERROR_OUT_OF_HOST_MEMORY; }
    if(!okay || candidate.chunks.size()!=(logicalBytes+d.snapshot.chunkSize-1)/d.snapshot.chunkSize) {
        const auto failure=okay?VK_ERROR_OUT_OF_DEVICE_MEMORY:(r==VK_SUCCESS?VK_ERROR_OUT_OF_DEVICE_MEMORY:r);
        ++d.snapshotFailures; d.lastSnapshotError=failure;
        VkSparseMemoryBind unbindView{}; unbindView.size=dataSize;
        const auto ur=bindSparseLocked(d.handle,d,memory.poolViews[i],&unbindView,1);
        if(ur==VK_SUCCESS) {
            const auto br=bindChildAppsLocked(d.handle,d,memory,i,false);
            if(br!=VK_SUCCESS) { d.gpuGateError=br; d.autoEnabled=false; d.stopWorker.store(true); }
        } else { d.gpuGateError=ur; d.autoEnabled=false; d.stopWorker.store(true); }
        if(budgetExceeded) {
            logf("snapshot cold-budget refusal logical=%llu encoded-prefix=%llu cold=%llu cache=%llu limit=%llu retained-resident=1",
                static_cast<unsigned long long>(logicalBytes),static_cast<unsigned long long>(stored),
                static_cast<unsigned long long>(d.coldBytes),static_cast<unsigned long long>(d.cacheBytes),
                static_cast<unsigned long long>(d.coldBudget));
            group.budgetBlocked=true; group.failedBudgetGeneration=d.coldBudgetGeneration;
            group.failedBudgetSubmissionGeneration=d.gpuSubmissionGeneration;
        }
        return failure;
    }
    VkSparseMemoryBind unbindView{}; unbindView.size=dataSize;
    r=bindSparseLocked(d.handle,d,memory.poolViews[i],&unbindView,1);
    if(r!=VK_SUCCESS) {
        ++d.snapshotFailures; d.lastSnapshotError=r;
        d.gpuGateError=r; d.autoEnabled=false; d.stopWorker.store(true);
        return r;
    }
    d.residentBytes-=dataSize;
    memory.residentBytes-=dataSize;
    releaseBackingChild(d,memory,i);
    memory.bound=std::any_of(memory.children.begin(),memory.children.end(),[](VkDeviceMemory child){return child!=VK_NULL_HANDLE;});
    candidate.logicalBytes=logicalBytes; candidate.storedBytes=stored; candidate.cold=true; group=std::move(candidate);
    memory.cold=true; memory.coldStoredBytes+=stored; memory.coldLogicalSize+=logicalBytes;
    d.coldBytes+=stored; d.coldLogicalBytes+=logicalBytes; ++d.freezeCount;
    const auto compressedChunks=static_cast<std::size_t>(std::count_if(group.chunks.begin(),group.chunks.end(),[](const auto& chunk){return chunk.compressed;}));
    const auto shuffledChunks=static_cast<std::size_t>(std::count_if(group.chunks.begin(),group.chunks.end(),[](const auto& chunk){return chunk.byteShuffle!=0;}));
    logf("snapshot cold bytes=%llu stored=%llu compressed-chunks=%zu raw-chunks=%zu shuffled-chunks=%zu",static_cast<unsigned long long>(logicalBytes),static_cast<unsigned long long>(stored),compressedChunks,group.chunks.size()-compressedChunks,shuffledChunks);
    logSnapshotState("freeze",d);
    return VK_SUCCESS;
}
namespace {
constexpr VkDeviceSize kAsyncSnapshotMaxRaw=32ull*1024ull*1024ull;
bool encodeAsyncSnapshot(const std::uint8_t* raw,std::size_t rawSize,std::size_t chunkLimit,
                         unsigned minSavings,unsigned shuffle,zvram::snapshot::Codec codec,
                         unsigned encodingWorkers,VirtualMemory::ColdGroup& candidate,
                         VkDeviceSize& stored) noexcept {
    try {
        if(!raw || !rawSize || !chunkLimit || rawSize>kAsyncSnapshotMaxRaw) return false;
        candidate.chunks.reserve((rawSize+chunkLimit-1)/chunkLimit);
        std::vector<std::uint8_t> encoded;
        std::vector<std::uint8_t> shuffled;
        for(std::size_t offset=0;offset<rawSize;offset+=chunkLimit) {
            const auto amount=std::min(chunkLimit,rawSize-offset);
            const auto* source=raw+offset;
            VirtualMemory::ColdChunk chunk; chunk.rawSize=amount;
            std::size_t compressed=0; bool keepCompressed=false;
            if(minSavings<100) {
                if(codec==zvram::snapshot::Codec::GDeflate) {
#ifdef ZVRAM_HAVE_GDEFLATE
                    if(shuffle) return false;
                    if(!zvram::gdeflate::encode(source,amount,encoded,encodingWorkers)) return false;
                    compressed=encoded.size(); keepCompressed=retainCompression(amount,compressed,minSavings);
#else
                    return false;
#endif
                } else if(codec==zvram::snapshot::Codec::BP16) {
                    if(shuffle) return false;
                    if(amount<=zvram::bp16::MaxRawBytes && amount%zvram::bp16::RawBytesPerBlock==0) {
                        if(!zvram::bp16::encodeFast(source,amount,encoded,encodingWorkers)) return false;
                        compressed=encoded.size(); keepCompressed=retainCompression(amount,compressed,minSavings);
                    }
                } else {
                    const auto* compressionSource=source;
                    if(shuffle) {
                        shuffled.resize(amount);
                        if(!zvram::byte_shuffle(source,shuffled.data(),amount,shuffle)) return false;
                        compressionSource=shuffled.data();
                    }
                    encoded.resize(ZSTD_compressBound(amount));
                    compressed=ZSTD_compress(encoded.data(),encoded.size(),compressionSource,amount,1);
                    keepCompressed=!ZSTD_isError(compressed) && retainCompression(amount,compressed,minSavings);
                }
            }
            if(keepCompressed) {
                if(codec==zvram::snapshot::Codec::BP16 && encoded.size()==compressed && encoded.capacity()==compressed)
                    chunk.bytes=std::move(encoded);
                else chunk.bytes.assign(encoded.begin(),encoded.begin()+compressed);
                chunk.compressed=true; chunk.codec=codec;
                chunk.byteShuffle=codec==zvram::snapshot::Codec::Zstd?shuffle:0;
            } else chunk.bytes.assign(source,source+amount);
            if(chunk.bytes.size()>std::numeric_limits<VkDeviceSize>::max()-stored) return false;
            stored+=chunk.bytes.size(); candidate.chunks.push_back(std::move(chunk));
        }
        candidate.logicalBytes=rawSize; candidate.storedBytes=stored; candidate.cold=true;
        return true;
    } catch(...) { return false; }
}

// Returns false only when the child is outside this bounded async path; true
// means the background attempt was consumed, whether committed or discarded.
bool freezeChildAsyncLocked(Device& d,VkDeviceMemory handle,std::size_t child,
                            std::unique_lock<std::mutex>& deviceLock,
                            std::unique_lock<std::mutex>& queueLock) {
    std::vector<std::uint8_t> raw;
    VkDeviceSize rawSize{};
    VkResult result=VK_SUCCESS;
    AsyncFreezeToken token{};
    std::size_t chunkLimit{};
    unsigned minSavings{},shuffle{},workers{};
    zvram::snapshot::Codec codec{};
    {
        auto found=d.virtualMemory.find(handle);
        if(found==d.virtualMemory.end()) return false;
        auto& memory=found->second;
        if(child>=memory.children.size() || child>=memory.childSizes.size() ||
           child>=memory.coldGroups.size() || child>=memory.childGenerations.size() ||
           child>=memory.poolViews.size()) return false;
        auto& group=memory.coldGroups[child]; rawSize=memory.childSizes[child];
        if(!d.asyncCompression || !d.autoEnabled || !d.activeEviction || !d.rangeChunkBytes || d.asyncFreezePending ||
           !memory.children[child] || group.cold || group.pristine || !group.chunks.empty() ||
           (group.budgetBlocked && group.failedBudgetGeneration==d.coldBudgetGeneration &&
            group.failedBudgetSubmissionGeneration==d.gpuSubmissionGeneration) ||
           d.minSavingsPercent>=100 || !rawSize || rawSize>kAsyncSnapshotMaxRaw ||
           rawSize>d.snapshot.stagingSize || d.activeRefs.busy(handle,child)) return false;
        try { raw.resize(static_cast<std::size_t>(rawSize)); }
        catch(const std::bad_alloc&) { ++d.snapshotFailures; d.lastSnapshotError=VK_ERROR_OUT_OF_HOST_MEMORY; return true; }
        const auto backing=memory.children[child];
        auto result=bindChildAppsLocked(d.handle,d,memory,child,true);
        if(result!=VK_SUCCESS) { ++d.snapshotFailures; d.lastSnapshotError=result; return true; }
        VkSparseMemoryBind viewBind{}; viewBind.size=rawSize; viewBind.memory=backing;
        result=bindSparseLocked(d.handle,d,memory.poolViews[child],&viewBind,1);
        if(result!=VK_SUCCESS) {
            const auto rebound=bindChildAppsLocked(d.handle,d,memory,child,false);
            if(rebound!=VK_SUCCESS) { d.gpuGateError=rebound; d.autoEnabled=false; d.stopWorker.store(true); }
            ++d.snapshotFailures; d.lastSnapshotError=result; return true;
        }
        result=copyChunkLocked(d,memory.poolViews[child],d.snapshot.stagingBuffer,0,0,rawSize,false);
        if(result==VK_SUCCESS) std::memcpy(raw.data(),d.snapshot.mapped,raw.size());
        VkSparseMemoryBind unbindView{}; unbindView.size=rawSize;
        const auto unbindResult=bindSparseLocked(d.handle,d,memory.poolViews[child],&unbindView,1);
        if(unbindResult!=VK_SUCCESS) {
            d.gpuGateError=unbindResult; d.autoEnabled=false; d.stopWorker.store(true);
            ++d.snapshotFailures; d.lastSnapshotError=unbindResult; return true;
        }
        const auto rebound=bindChildAppsLocked(d.handle,d,memory,child,false);
        if(rebound!=VK_SUCCESS) {
            d.gpuGateError=rebound; d.autoEnabled=false; d.stopWorker.store(true);
            ++d.snapshotFailures; d.lastSnapshotError=rebound; return true;
        }
        if(result!=VK_SUCCESS) { ++d.snapshotFailures; d.lastSnapshotError=result; return true; }
        token={handle,memory.identityGeneration,child,backing,memory.childGenerations[child],
               memory.bindingGeneration,group.writeEpoch};
        chunkLimit=static_cast<std::size_t>(d.snapshot.chunkSize);
        minSavings=d.minSavingsPercent; shuffle=d.byteShuffle; codec=d.snapshotCodec; workers=codec==zvram::snapshot::Codec::BP16?d.bp16Workers:d.gdeflateWorkers;
    } // No map iterators or VirtualMemory/ColdGroup references survive the unlock.
    d.asyncFreezePending=true;
    VirtualMemory::ColdGroup candidate; VkDeviceSize stored=0; bool encoded=false;
    try {
        runAsyncEncoderUnlocked(deviceLock,queueLock,[&] {
            encoded=encodeAsyncSnapshot(raw.data(),raw.size(),chunkLimit,minSavings,shuffle,codec,workers,candidate,stored);
        });
    } catch(...) { encoded=false; }
    d.asyncFreezePending=false;
    if(!encoded || !asyncFreezeTokenValid(d,token)) {
        if(!encoded) { ++d.snapshotFailures; d.lastSnapshotError=VK_ERROR_UNKNOWN; }
        return true;
    }
    auto found=d.virtualMemory.find(token.memory);
    if(found==d.virtualMemory.end()) return true;
    auto& current=found->second; auto& currentGroup=current.coldGroups[token.child];
    candidate.writeEpoch=token.writeEpoch;
    if(!asyncColdBudgetFits(d,stored)) trimCleanCacheLocked(d,stored);
    if(!asyncColdBudgetFits(d,stored)) {
        currentGroup.budgetBlocked=true;
        currentGroup.failedBudgetGeneration=d.coldBudgetGeneration;
        currentGroup.failedBudgetSubmissionGeneration=d.gpuSubmissionGeneration;
        ++d.snapshotFailures; d.lastSnapshotError=VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return true;
    }
    result=bindChildAppsLocked(d.handle,d,current,token.child,true);
    if(result!=VK_SUCCESS) {
        const auto restore=bindChildAppsLocked(d.handle,d,current,token.child,false);
        if(restore!=VK_SUCCESS) { d.gpuGateError=restore; d.autoEnabled=false; d.stopWorker.store(true); }
        ++d.snapshotFailures; d.lastSnapshotError=result; return true;
    }
    d.residentBytes-=rawSize; current.residentBytes-=rawSize;
    releaseBackingChild(d,current,token.child);
    current.bound=std::any_of(current.children.begin(),current.children.end(),[](VkDeviceMemory value){return value!=VK_NULL_HANDLE;});
    currentGroup=std::move(candidate); current.cold=true;
    current.coldStoredBytes+=stored; current.coldLogicalSize+=rawSize;
    d.coldBytes+=stored; d.coldLogicalBytes+=rawSize; ++d.freezeCount;
    logf("async snapshot committed raw=%llu stored=%llu",static_cast<unsigned long long>(rawSize),static_cast<unsigned long long>(stored));
    logSnapshotState("async-freeze",d);
    return true;
}
} // namespace
void snapshotWorkerLoop(const std::shared_ptr<Device>& shared) {
    auto& d=*shared;
    std::unique_lock<std::mutex> lock(d.mutex);
    while(!d.stopWorker.load()) {
        const auto deadline=d.activeEviction?
            std::chrono::steady_clock::now()+std::chrono::milliseconds(std::min<std::uint64_t>(d.idleMilliseconds,10)):
            d.lastActivity+std::chrono::milliseconds(d.idleMilliseconds);
        if(d.activeEviction) d.activity.wait_until(lock,deadline,[&]{return d.stopWorker.load();});
        else d.activity.wait_until(lock,deadline);
        if(d.stopWorker.load()) break;
        if(!d.activeEviction && std::chrono::steady_clock::now()<d.lastActivity+std::chrono::milliseconds(d.idleMilliseconds)) continue;
        std::unique_lock<std::mutex> queueLock(d.queueMutex);
        if(d.stopWorker.load()) break;
        bool allReady=false;
        VkResult r=d.activeEviction?d.autoQueues.pollActive(
            [&](VkQueue queue){d.activeRefs.retire(queue);},
            [&](VkQueue queue){d.activeRefs.cover(queue);}):d.autoQueues.ready(allReady);
        if(r!=VK_SUCCESS) {
            d.autoEnabled=false; d.stopWorker.store(true); d.gpuGateError=r; d.lastSnapshotError=r; ++d.snapshotFailures;
            d.lastActivity=std::chrono::steady_clock::now();
            queueLock.unlock(); continue;
        }
        if(!d.activeEviction && !allReady) { d.lastActivity=std::chrono::steady_clock::now(); queueLock.unlock(); continue; }
        r=d.activeEviction?VK_SUCCESS:d.autoQueues.toCopyQueue();
        if(r!=VK_SUCCESS) {
            d.autoEnabled=false; d.stopWorker.store(true); d.gpuGateError=r; d.lastSnapshotError=r; ++d.snapshotFailures;
            d.lastActivity=std::chrono::steady_clock::now();
            queueLock.unlock(); continue;
        }
        const auto start=std::chrono::steady_clock::now();
        r=d.activeEviction?VK_SUCCESS:d.snapshot.deviceWaitIdle(d.handle);
        const auto waited=std::chrono::steady_clock::now()-start;
        if(r!=VK_SUCCESS || waited>std::chrono::milliseconds(20)) {
            d.lastActivity=std::chrono::steady_clock::now(); if(r!=VK_SUCCESS) { ++d.snapshotFailures; d.lastSnapshotError=r; }
            queueLock.unlock(); continue;
        }
        bool haveAsyncCandidate=false;
        VkDeviceMemory asyncMemory{}; std::size_t asyncChild{};
        if(d.asyncCompression) {
            const auto now=std::chrono::steady_clock::now();
            for(const auto& pair:d.virtualMemory) {
                const auto& memory=pair.second;
                if(memory.children.empty() || memory.children.size()!=memory.coldGroups.size() ||
                   memory.children.size()!=memory.childSizes.size() ||
                   memory.children.size()!=memory.childGenerations.size() ||
                   memory.poolViews.size()!=memory.children.size()) continue;
                for(std::size_t i=0;i<memory.children.size();i++) {
                    const auto& group=memory.coldGroups[i];
                    if(!memory.children[i] || group.cold || group.pristine || !group.chunks.empty() ||
                       (group.budgetBlocked && group.failedBudgetGeneration==d.coldBudgetGeneration &&
                        group.failedBudgetSubmissionGeneration==d.gpuSubmissionGeneration) ||
                       !memory.childSizes[i] || memory.childSizes[i]>kAsyncSnapshotMaxRaw ||
                       memory.childSizes[i]>d.snapshot.stagingSize || d.activeRefs.busy(pair.first,i) ||
                       now<group.lastUse+std::chrono::milliseconds(d.idleMilliseconds)) continue;
                    asyncMemory=pair.first; asyncChild=i; haveAsyncCandidate=true; break;
                }
                if(haveAsyncCandidate) break;
            }
        }
        // Only value identifiers survive this scan; no map/vector references cross unlock.
        if(haveAsyncCandidate && freezeChildAsyncLocked(d,asyncMemory,asyncChild,lock,queueLock)) {
            d.lastActivity=std::chrono::steady_clock::now();
            queueLock.unlock();
            continue; // All map references were invalidated while the gates were released.
        }
        for(auto& pair:d.virtualMemory) {
            auto& memory=pair.second;
            if(d.activeEviction && !d.rangeChunkBytes && (d.activeRefs.busy(pair.first) ||
               std::chrono::steady_clock::now()<memory.lastUse+std::chrono::milliseconds(d.idleMilliseconds))) continue;
            if(memory.children.empty() || memory.children.size()!=memory.coldGroups.size() ||
               memory.poolViews.size()!=memory.children.size()) continue;
            for(std::size_t i=0;i<memory.children.size();i++) {
                if(d.rangeChunkBytes && (d.activeRefs.busy(pair.first,i) ||
                    std::chrono::steady_clock::now()<memory.coldGroups[i].lastUse+std::chrono::milliseconds(d.idleMilliseconds))) continue;
                (void)freezeChildLocked(d,memory,i);
            }
        }
        d.lastActivity=std::chrono::steady_clock::now();
        queueLock.unlock();
        const auto observedGeneration=d.gpuSubmissionGeneration;
        bool canRetry=false;
        for(const auto& pair:d.virtualMemory) {
            const auto& memory=pair.second;
            if(memory.children.empty() || memory.children.size()!=memory.coldGroups.size()) continue;
            for(std::size_t index=0;index<memory.coldGroups.size();index++) {
                const auto& group=memory.coldGroups[index];
                if(index<memory.children.size() && memory.children[index] && !group.cold &&
                   (!group.budgetBlocked || group.failedBudgetGeneration!=d.coldBudgetGeneration ||
                    group.failedBudgetSubmissionGeneration!=observedGeneration)) {
                    canRetry=true; break;
                }
            }
            if(canRetry) break;
        }
        if(!canRetry && !d.activeEviction) {
            const auto observedBudgetGeneration=d.coldBudgetGeneration;
            d.activity.wait(lock,[&]{return d.stopWorker.load() ||
                d.gpuSubmissionGeneration!=observedGeneration || d.coldBudgetGeneration!=observedBudgetGeneration;});
        }
    }
}
bool eligibleBuffer(const Device& d,const VkBufferCreateInfo* ci) {
    constexpr VkBufferUsageFlags allowed=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|
        VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return d.virtualEnabled && ci && !ci->pNext && ci->flags==0 &&
        ci->size>=1024u*1024u && ci->size%4u==0 &&
        (ci->usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)!=0 && (ci->usage&~allowed)==0 &&
        (!(ci->usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)||d.bufferDeviceAddressEnabled);
}
bool parseVirtualAllocationChain(const void* chain,VkMemoryAllocateFlags& flags,float& priority,bool& hasPriority) {
    bool hasFlags=false;
    flags=0; priority=0.5f; hasPriority=false;
    for(auto* p=static_cast<const VkBaseInStructure*>(chain);p;p=p->pNext) {
        if(p->sType==VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
            if(hasFlags) return false;
            const auto* info=reinterpret_cast<const VkMemoryAllocateFlagsInfo*>(p);
            if(info->deviceMask!=0 || (info->flags&~VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)!=0) return false;
            flags=info->flags; hasFlags=true;
#ifdef VK_EXT_memory_priority
        } else if(p->sType==VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT) {
            if(hasPriority) return false;
            priority=reinterpret_cast<const VkMemoryPriorityAllocateInfoEXT*>(p)->priority;
            hasPriority=true;
#endif
        } else return false;
    }
    return true;
}
std::vector<std::uint32_t> backingMemoryTypes(const Device& d,const VkMemoryRequirements& req) {
    std::vector<std::uint32_t> types;
    for(std::uint32_t i=0;i<d.memory.memoryTypeCount;i++) {
        if((req.memoryTypeBits&(1u<<i))==0) continue;
        const auto heap=d.memory.memoryTypes[i].heapIndex;
        if(heap>=d.memory.memoryHeapCount) continue;
        if(d.memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) types.push_back(i);
    }
    for(std::uint32_t i=0;i<d.memory.memoryTypeCount;i++) {
        if((req.memoryTypeBits&(1u<<i))==0) continue;
        const auto heap=d.memory.memoryTypes[i].heapIndex;
        if(heap<d.memory.memoryHeapCount && (d.memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)==0) types.push_back(i);
    }
    return types;
}
VkResult allocateBackingChild(Device& d,VkDevice device,VkDeviceSize size,std::uint32_t type,
                              VkMemoryAllocateFlags flags,bool hasPriority,float priority,VkDeviceMemory* out) {
    const auto started=d.gpuProfileEnabled?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    VkMemoryPriorityAllocateInfoEXT priorityInfo{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    priorityInfo.priority=priority;
    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flagsInfo.flags=flags;
    flagsInfo.deviceMask=0;
    flagsInfo.pNext=hasPriority?&priorityInfo:nullptr;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=size; allocation.memoryTypeIndex=type;
    if(flags) allocation.pNext=&flagsInfo;
    else if(hasPriority) allocation.pNext=&priorityInfo;
    const auto result=d.allocate(device,&allocation,nullptr,out);
    if(d.gpuProfileEnabled) {
        d.gpuProfileAllocCalls.fetch_add(1,std::memory_order_relaxed);
        (result==VK_SUCCESS?d.gpuProfileAllocSuccess:d.gpuProfileAllocFailures).fetch_add(1,std::memory_order_relaxed);
        d.gpuProfileAllocNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count()),std::memory_order_relaxed);
    }
    return result;
}
VkResult bindPromoted(VkDevice device,const std::shared_ptr<Device>& d,VkBuffer buffer,
                      VkDeviceMemory memory,VkDeviceSize memoryOffset) {
    bool missingRequirements=false;
    { std::lock_guard<std::mutex> lock(d->mutex); auto i=d->promotedBuffers.find(buffer); missingRequirements=i!=d->promotedBuffers.end() && i->second.requirements.size==0; }
    if(missingRequirements && d->getBufferMemoryRequirements) {
        VkMemoryRequirements req{}; d->getBufferMemoryRequirements(device,buffer,&req);
        std::lock_guard<std::mutex> lock(d->mutex); auto i=d->promotedBuffers.find(buffer); if(i!=d->promotedBuffers.end()) i->second.requirements=req;
    }
    std::lock_guard<std::mutex> lock(d->mutex);
    auto bi=d->promotedBuffers.find(buffer);
    if(bi==d->promotedBuffers.end()) return VK_ERROR_FEATURE_NOT_PRESENT;
    auto& promoted=bi->second;
    if(promoted.memory || !promoted.requirements.size ||
       promoted.requirements.size>d->virtualBytes) return VK_ERROR_FEATURE_NOT_PRESENT;
    auto vi=d->virtualMemory.find(memory);
    if(vi==d->virtualMemory.end()) {
        auto ai=d->allocations.find(memory);
        if(ai==d->allocations.end() || ai->second.bound || ai->second.type>=32 ||
           (promoted.concurrentForced && !ai->second.adoptable) ||
           (promoted.deviceAddress && !(ai->second.flags&VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)) ||
           (promoted.requirements.memoryTypeBits&(1u<<ai->second.type))==0 ||
           promoted.requirements.alignment==0 || memoryOffset%promoted.requirements.alignment!=0 ||
           memoryOffset>ai->second.size || promoted.requirements.size>ai->second.size-memoryOffset)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        const auto props=d->memory.memoryTypes[ai->second.type].propertyFlags;
        const bool gpuOnlyLocal=ai->second.local &&
            (props&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT|VK_MEMORY_PROPERTY_PROTECTED_BIT))==0;
        const bool adopt=d->autoEnabled && d->nativeWrappingAllowed && gpuOnlyLocal && ai->second.adoptable;
        if(adopt && d->rangeChunkBytes && ai->second.wrapped) {
            // No resource has ever bound this GPU-only allocation. Its contents
            // are undefined, so release its pristine backing before allocating
            // chunks rather than briefly doubling a large model's footprint.
            VirtualMemory segmented{};
            segmented.size=ai->second.size; segmented.token=ai->second.token;
            if(!assignVirtualIdentity(*d,segmented)) return VK_ERROR_OUT_OF_HOST_MEMORY;
            segmented.allocationFlags=ai->second.flags;
            segmented.priority=ai->second.priority; segmented.hasPriority=ai->second.hasPriority;
            segmented.nativeTypeBits=1u<<ai->second.type;
            const auto original=ai->second.nativeHandle;
            const auto bytes=ai->second.size;
            const auto callbacks=ai->second.callbacks;
            const bool hasCallbacks=ai->second.hasCallbacks;
            try {
                auto inserted=d->virtualMemory.emplace(memory,std::move(segmented));
                if(!inserted.second) return VK_ERROR_FEATURE_NOT_PRESENT;
                vi=inserted.first;
            } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
            d->allocations.erase(ai);
            d->free(device,original,hasCallbacks?&callbacks:nullptr);
            d->liveLocal-=std::min<std::uint64_t>(d->liveLocal,bytes);
            const auto r=bindPoolBuffer(device,*d,buffer,memory,memoryOffset,promoted.requirements);
            if(r==VK_SUCCESS) {
                promoted.memory=memory; promoted.synthetic=true;
                logf("native allocation segmented bytes=%llu children=%zu",static_cast<unsigned long long>(bytes),vi->second.children.size());
            }
            return r;
        }
        if(adopt) {
            VirtualMemory adopted{}; adopted.size=ai->second.size; adopted.allocationFlags=ai->second.flags;
            if(!assignVirtualIdentity(*d,adopted)) return VK_ERROR_OUT_OF_HOST_MEMORY;
            adopted.priority=ai->second.priority; adopted.hasPriority=ai->second.hasPriority;
            adopted.buffer=buffer; adopted.everBound=true; adopted.trackPhysicalStats=true;
            adopted.token=ai->second.token;
            adopted.adoptedCallbacks=ai->second.callbacks; adopted.hasAdoptedCallbacks=ai->second.hasCallbacks;
            VkBuffer adoptedView{};
            auto cleanupAdoptedView=[&] {
                if(adoptedView) d->snapshot.destroyBuffer(device,adoptedView,nullptr);
                adoptedView=VK_NULL_HANDLE;
            };
            try {
                adopted.children.push_back(ai->second.nativeHandle); adopted.childSizes.push_back(ai->second.size);
                adopted.childTypes.push_back(ai->second.type);
                adopted.childGenerations.push_back(1);
                adopted.coldGroups.resize(1);
                adopted.bindings.push_back({buffer,memoryOffset,promoted.requirements.size,promoted.requirements.alignment});
                adopted.bindingGeneration=1;
                adopted.backingMemoryTypeBits=promoted.requirements.memoryTypeBits;
                if(d->autoEnabled) {
                    const auto views=createPoolViews(*d,adopted,adopted.childSizes);
                    if(views!=VK_SUCCESS) return views;
                    adoptedView=adopted.poolViews.front();
                    for(auto bits:adopted.poolViewMemoryTypeBits) adopted.backingMemoryTypeBits&=bits;
                    if((adopted.backingMemoryTypeBits&(1u<<ai->second.type))==0) {
                        cleanupAdoptedView();
                        return VK_ERROR_FEATURE_NOT_PRESENT;
                    }
                }
                // Preserve the application's chosen native memory type for future pool ranges.
                adopted.backingMemoryTypeBits=1u<<ai->second.type;
                auto inserted=d->virtualMemory.emplace(memory,std::move(adopted));
                if(!inserted.second) { cleanupAdoptedView(); return VK_ERROR_FEATURE_NOT_PRESENT; }
                vi=inserted.first;
                adoptedView=VK_NULL_HANDLE;
            }
            catch(const std::bad_alloc&) { cleanupAdoptedView(); return VK_ERROR_OUT_OF_HOST_MEMORY; }
            VkSparseMemoryBind bind{}; bind.size=promoted.requirements.size; bind.memory=ai->second.nativeHandle;
            bind.memoryOffset=memoryOffset;
            const VkResult r=bindSparse(device,*d,buffer,&bind,1);
            if(r!=VK_SUCCESS) {
                if(d->gpuGateError!=VK_SUCCESS) {
                    vi->second.bound=true; vi->second.residentBytes=ai->second.size;
                    d->residentBytes+=vi->second.residentBytes;
                    d->allocations.erase(ai); promoted.memory=memory; promoted.synthetic=true;
                } else { destroyPoolViews(*d,vi->second); d->virtualMemory.erase(vi); }
                return r;
            }
            vi->second.bound=true; vi->second.residentBytes=ai->second.size;
            d->residentBytes+=vi->second.residentBytes;
            d->allocations.erase(ai); promoted.memory=memory; promoted.synthetic=true;
            logf("native allocation adopted bytes=%llu type=%u",static_cast<unsigned long long>(vi->second.size),vi->second.childTypes.front());
            return VK_SUCCESS;
        }
        VkSparseMemoryBind bind{}; bind.size=promoted.requirements.size; bind.memory=ai->second.nativeHandle; bind.memoryOffset=memoryOffset;
        VkResult r=bindSparse(device,*d,buffer,&bind,1);
        if(r==VK_SUCCESS) { ai->second.bound=true; promoted.memory=memory; promoted.synthetic=false; }
        return r;
    }
    auto& virtualMem=vi->second;
    if(promoted.deviceAddress && !(virtualMem.allocationFlags&VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(virtualMem.deferredFree) return VK_ERROR_FEATURE_NOT_PRESENT;
    const auto r=bindPoolBuffer(device,*d,buffer,memory,memoryOffset,promoted.requirements);
    if(r==VK_SUCCESS) { promoted.memory=memory; promoted.synthetic=true; }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL layerCreateBuffer(VkDevice device,const VkBufferCreateInfo* ci,const VkAllocationCallbacks* allocator,VkBuffer* out) {
    auto d=findDevice(device); if(!d || !d->createBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    if(!eligibleBuffer(*d,ci)) return d->createBuffer(device,ci,allocator,out);
    VkBufferCreateInfo copy=*ci; copy.flags|=VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
    if(d->rangeChunkBytes) copy.flags|=VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    if(d->autoEnabled) copy.usage|=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    std::vector<std::uint32_t> sharingFamilies;
    bool concurrentForced=false;
    if(d->autoEnabled && d->queueFamilies.size()>1) {
        try {
            if(ci->sharingMode==VK_SHARING_MODE_EXCLUSIVE) {
                sharingFamilies=d->queueFamilies;
                copy.sharingMode=VK_SHARING_MODE_CONCURRENT;
                copy.queueFamilyIndexCount=static_cast<std::uint32_t>(sharingFamilies.size());
                copy.pQueueFamilyIndices=sharingFamilies.data();
                concurrentForced=true;
            } else {
                sharingFamilies.assign(ci->pQueueFamilyIndices,ci->pQueueFamilyIndices+ci->queueFamilyIndexCount);
                for(auto family:d->queueFamilies)
                    if(std::find(sharingFamilies.begin(),sharingFamilies.end(),family)==sharingFamilies.end()) sharingFamilies.push_back(family);
                if(sharingFamilies.size()>ci->queueFamilyIndexCount) {
                    copy.queueFamilyIndexCount=static_cast<std::uint32_t>(sharingFamilies.size());
                    copy.pQueueFamilyIndices=sharingFamilies.data();
                }
            }
        } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    }
    VkResult r=d->createBuffer(device,&copy,allocator,out); if(r!=VK_SUCCESS) return r;
    try {
        std::lock_guard<std::mutex> lock(d->mutex);
        PromotedBuffer promoted{}; promoted.size=ci->size;
        promoted.deviceAddress=(ci->usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)!=0;
        promoted.concurrentForced=concurrentForced;
        d->promotedBuffers.emplace(*out,promoted);
    }
    catch(const std::bad_alloc&) { d->destroyBuffer(device,*out,allocator); *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL layerGetBufferMemoryRequirements(VkDevice device,VkBuffer buffer,VkMemoryRequirements* out) {
    auto d=findDevice(device); if(!d || !d->getBufferMemoryRequirements) return;
    d->getBufferMemoryRequirements(device,buffer,out);
    if(!out) return;
    std::lock_guard<std::mutex> lock(d->mutex); auto it=d->promotedBuffers.find(buffer);
    if(it!=d->promotedBuffers.end()) { it->second.requirements=*out; out->memoryTypeBits|=(1u<<d->virtualType); }
}
VKAPI_ATTR void VKAPI_CALL layerGetBufferMemoryRequirements2(VkDevice device,const VkBufferMemoryRequirementsInfo2* info,VkMemoryRequirements2* out) {
    auto d=findDevice(device); if(!d || !d->getBufferMemoryRequirements2 || !info || !out) return;
    d->getBufferMemoryRequirements2(device,info,out);
    std::lock_guard<std::mutex> lock(d->mutex); auto it=d->promotedBuffers.find(info->buffer);
    if(it!=d->promotedBuffers.end()) { it->second.requirements=out->memoryRequirements; out->memoryRequirements.memoryTypeBits|=(1u<<d->virtualType); }
}
VKAPI_ATTR VkResult VKAPI_CALL layerBindBufferMemory(VkDevice device,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize memoryOffset) {
    auto d=findDevice(device); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    bool promoted=false,synthetic=false;
    { std::lock_guard<std::mutex> lock(d->mutex); promoted=d->promotedBuffers.count(buffer)!=0; synthetic=d->virtualMemory.count(memory)!=0; }
    if(!promoted) {
        if(synthetic) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkDeviceMemory native=memory;
        { std::lock_guard<std::mutex> lock(d->mutex); native=nativeMemoryLocked(*d,memory); auto i=d->allocations.find(memory); if(i!=d->allocations.end()) i->second.bound=true; }
        const VkResult r=d->bindBufferMemory(device,buffer,native,memoryOffset);
        return r;
    }
    return bindPromoted(device,d,buffer,memory,memoryOffset);
}
VKAPI_ATTR VkResult VKAPI_CALL layerBindBufferMemory2(VkDevice device,std::uint32_t count,const VkBindBufferMemoryInfo* infos) {
    auto d=findDevice(device); if(!d || !infos) return VK_ERROR_INITIALIZATION_FAILED;
    for(std::uint32_t i=0;i<count;i++) {
        bool promoted=false,synthetic=false; { std::lock_guard<std::mutex> lock(d->mutex); promoted=d->promotedBuffers.count(infos[i].buffer)!=0; synthetic=d->virtualMemory.count(infos[i].memory)!=0; }
        VkResult r;
        if(promoted) {
            if(infos[i].pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
            r=bindPromoted(device,d,infos[i].buffer,infos[i].memory,infos[i].memoryOffset);
        } else if(synthetic) return VK_ERROR_FEATURE_NOT_PRESENT;
        else {
            VkBindBufferMemoryInfo translated=infos[i];
            { std::lock_guard<std::mutex> lock(d->mutex); translated.memory=nativeMemoryLocked(*d,infos[i].memory); auto ai=d->allocations.find(infos[i].memory); if(ai!=d->allocations.end()) ai->second.bound=true; }
            if(d->bindBufferMemory2) r=d->bindBufferMemory2(device,1,&translated);
            else r=d->bindBufferMemory(device,translated.buffer,translated.memory,translated.memoryOffset);
        }
        if(r!=VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL layerMapMemory(VkDevice device,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void** data) {
    auto d=findDevice(device); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkDeviceMemory native=memory;
    { std::lock_guard<std::mutex> lock(d->mutex); if(d->virtualMemory.count(memory)) return VK_ERROR_MEMORY_MAP_FAILED;
      auto allocation=d->allocations.find(memory); if(allocation!=d->allocations.end() && allocation->second.wrapped) return VK_ERROR_MEMORY_MAP_FAILED;
      native=nativeMemoryLocked(*d,memory); }
    auto f=reinterpret_cast<PFN_vkMapMemory>(d->gdpa(device,"vkMapMemory")); return f?f(device,native,offset,size,flags,data):VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL layerBindImageMemory(VkDevice device,VkImage image,VkDeviceMemory memory,VkDeviceSize offset) {
    auto d=findDevice(device); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    { std::lock_guard<std::mutex> lock(d->mutex); if(d->virtualMemory.count(memory)) return VK_ERROR_FEATURE_NOT_PRESENT; }
    auto f=reinterpret_cast<PFN_vkBindImageMemory>(d->gdpa(device,"vkBindImageMemory"));
    VkDeviceMemory native=memory;
    { std::lock_guard<std::mutex> lock(d->mutex); native=nativeMemoryLocked(*d,memory); auto i=d->allocations.find(memory); if(i!=d->allocations.end()) i->second.bound=true; }
    const VkResult r=f?f(device,image,native,offset):VK_ERROR_FEATURE_NOT_PRESENT;
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL layerBindImageMemory2(VkDevice device,std::uint32_t count,const VkBindImageMemoryInfo* infos) {
    auto d=findDevice(device); if(!d || !infos) return VK_ERROR_INITIALIZATION_FAILED;
    { std::lock_guard<std::mutex> lock(d->mutex); for(std::uint32_t i=0;i<count;i++) if(d->virtualMemory.count(infos[i].memory)) return VK_ERROR_FEATURE_NOT_PRESENT; }
    auto f=reinterpret_cast<PFN_vkBindImageMemory2>(d->gdpa(device,"vkBindImageMemory2")); if(!f) f=reinterpret_cast<PFN_vkBindImageMemory2>(d->gdpa(device,"vkBindImageMemory2KHR"));
    std::vector<VkBindImageMemoryInfo> translated;
    try { translated.assign(infos,infos+count); }
    catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    { std::lock_guard<std::mutex> lock(d->mutex); for(std::uint32_t i=0;i<count;i++) { translated[i].memory=nativeMemoryLocked(*d,infos[i].memory); auto a=d->allocations.find(infos[i].memory); if(a!=d->allocations.end()) a->second.bound=true; } }
    const VkResult r=f?f(device,count,translated.data()):VK_ERROR_FEATURE_NOT_PRESENT;
    return r;
}
VKAPI_ATTR void VKAPI_CALL layerGetDeviceMemoryCommitment(VkDevice device,VkDeviceMemory memory,VkDeviceSize* bytes) {
    auto d=findDevice(device); if(!d || !bytes) return;
    VkDeviceMemory native=memory;
    { std::lock_guard<std::mutex> lock(d->mutex); if(d->virtualMemory.count(memory)) { *bytes=0; return; } native=nativeMemoryLocked(*d,memory); }
    auto f=reinterpret_cast<PFN_vkGetDeviceMemoryCommitment>(d->gdpa(device,"vkGetDeviceMemoryCommitment")); if(f) f(device,native,bytes);
}
std::uint64_t memoryOpaqueCaptureAddress(VkDevice device,const VkDeviceMemoryOpaqueCaptureAddressInfo* info,const char* name) {
    auto d=findDevice(device); if(!d || !info) return 0;
    VkDeviceMemory native=info->memory;
    { std::lock_guard<std::mutex> lock(d->mutex); if(d->virtualMemory.count(info->memory)) return 0; native=nativeMemoryLocked(*d,info->memory); }
    auto f=reinterpret_cast<PFN_vkGetDeviceMemoryOpaqueCaptureAddress>(d->gdpa(device,name));
    if(!f) return 0;
    auto translated=*info; translated.memory=native;
    return f(device,&translated);
}
VKAPI_ATTR std::uint64_t VKAPI_CALL layerGetDeviceMemoryOpaqueCaptureAddress(VkDevice device,const VkDeviceMemoryOpaqueCaptureAddressInfo* info) {
    return memoryOpaqueCaptureAddress(device,info,"vkGetDeviceMemoryOpaqueCaptureAddress");
}
VKAPI_ATTR std::uint64_t VKAPI_CALL layerGetDeviceMemoryOpaqueCaptureAddressKHR(VkDevice device,const VkDeviceMemoryOpaqueCaptureAddressInfo* info) {
    return memoryOpaqueCaptureAddress(device,info,"vkGetDeviceMemoryOpaqueCaptureAddressKHR");
}
VKAPI_ATTR void VKAPI_CALL layerSetDeviceMemoryPriorityEXT(VkDevice device,VkDeviceMemory memory,float priority) {
    auto d=findDevice(device); if(!d) return;
    auto set=reinterpret_cast<PFN_vkSetDeviceMemoryPriorityEXT>(d->gdpa(device,"vkSetDeviceMemoryPriorityEXT"));
    VkDeviceMemory native=memory;
    bool isVirtual=false;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto virtualMemory=d->virtualMemory.find(memory);
        if(virtualMemory!=d->virtualMemory.end()) {
            isVirtual=true;
            virtualMemory->second.priority=priority; virtualMemory->second.hasPriority=true;
            if(set) for(auto child:virtualMemory->second.children) if(child) set(device,child,priority);
        } else {
            native=nativeMemoryLocked(*d,memory);
            auto allocation=d->allocations.find(memory);
            if(allocation!=d->allocations.end()) { allocation->second.priority=priority; allocation->second.hasPriority=true; }
        }
    }
    if(!isVirtual && set) set(device,native,priority);
}
VKAPI_ATTR VkResult VKAPI_CALL layerSetDebugUtilsObjectNameEXT(VkDevice device,const VkDebugUtilsObjectNameInfoEXT* info) {
    auto d=findDevice(device); if(!d || !info) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(d->gdpa(device,"vkSetDebugUtilsObjectNameEXT"));
    if(!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    auto translated=*info;
    if(info->objectType==VK_OBJECT_TYPE_DEVICE_MEMORY) {
        std::lock_guard<std::mutex> lock(d->mutex);
        const auto memory=tokenHandle<VkDeviceMemory>(static_cast<std::uintptr_t>(info->objectHandle));
        if(d->virtualMemory.count(memory)) return VK_ERROR_FEATURE_NOT_PRESENT;
        translated.objectHandle=static_cast<std::uint64_t>(handleToken(nativeMemoryLocked(*d,memory)));
    }
    return fn(device,&translated);
}
VKAPI_ATTR VkResult VKAPI_CALL layerSetDebugUtilsObjectTagEXT(VkDevice device,const VkDebugUtilsObjectTagInfoEXT* info) {
    auto d=findDevice(device); if(!d || !info) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=reinterpret_cast<PFN_vkSetDebugUtilsObjectTagEXT>(d->gdpa(device,"vkSetDebugUtilsObjectTagEXT"));
    if(!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    auto translated=*info;
    if(info->objectType==VK_OBJECT_TYPE_DEVICE_MEMORY) {
        std::lock_guard<std::mutex> lock(d->mutex);
        const auto memory=tokenHandle<VkDeviceMemory>(static_cast<std::uintptr_t>(info->objectHandle));
        if(d->virtualMemory.count(memory)) return VK_ERROR_FEATURE_NOT_PRESENT;
        translated.objectHandle=static_cast<std::uint64_t>(handleToken(nativeMemoryLocked(*d,memory)));
    }
    return fn(device,&translated);
}
VKAPI_ATTR VkResult VKAPI_CALL layerDebugMarkerSetObjectNameEXT(VkDevice device,const VkDebugMarkerObjectNameInfoEXT* info) {
    auto d=findDevice(device); if(!d || !info) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=reinterpret_cast<PFN_vkDebugMarkerSetObjectNameEXT>(d->gdpa(device,"vkDebugMarkerSetObjectNameEXT"));
    if(!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    auto translated=*info;
    if(info->objectType==VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT) {
        std::lock_guard<std::mutex> lock(d->mutex);
        const auto memory=tokenHandle<VkDeviceMemory>(static_cast<std::uintptr_t>(info->object));
        if(d->virtualMemory.count(memory)) return VK_ERROR_FEATURE_NOT_PRESENT;
        translated.object=static_cast<std::uint64_t>(handleToken(nativeMemoryLocked(*d,memory)));
    }
    return fn(device,&translated);
}
VKAPI_ATTR VkResult VKAPI_CALL layerDebugMarkerSetObjectTagEXT(VkDevice device,const VkDebugMarkerObjectTagInfoEXT* info) {
    auto d=findDevice(device); if(!d || !info) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=reinterpret_cast<PFN_vkDebugMarkerSetObjectTagEXT>(d->gdpa(device,"vkDebugMarkerSetObjectTagEXT"));
    if(!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    auto translated=*info;
    if(info->objectType==VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT) {
        std::lock_guard<std::mutex> lock(d->mutex);
        const auto memory=tokenHandle<VkDeviceMemory>(static_cast<std::uintptr_t>(info->object));
        if(d->virtualMemory.count(memory)) return VK_ERROR_FEATURE_NOT_PRESENT;
        translated.object=static_cast<std::uint64_t>(handleToken(nativeMemoryLocked(*d,memory)));
    }
    return fn(device,&translated);
}
bool containsVirtualMemory(const std::shared_ptr<Device>& d,VkDeviceMemory memory) {
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->virtualMemory.count(memory)!=0;
}
VKAPI_ATTR VkResult VKAPI_CALL layerBindVideoSessionMemoryKHR(VkDevice device,VkVideoSessionKHR session,
    std::uint32_t count,const VkBindVideoSessionMemoryInfoKHR* infos) {
    auto d=findDevice(device); if(!d || !infos) return VK_ERROR_INITIALIZATION_FAILED;
    for(std::uint32_t i=0;i<count;i++) if(containsVirtualMemory(d,infos[i].memory)) return VK_ERROR_FEATURE_NOT_PRESENT;
    auto fn=reinterpret_cast<PFN_vkBindVideoSessionMemoryKHR>(d->gdpa(device,"vkBindVideoSessionMemoryKHR"));
    return fn?fn(device,session,count,infos):VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR VkResult VKAPI_CALL layerBindAccelerationStructureMemoryNV(VkDevice device,std::uint32_t count,
    const VkBindAccelerationStructureMemoryInfoNV* infos) {
    auto d=findDevice(device); if(!d || !infos) return VK_ERROR_INITIALIZATION_FAILED;
    for(std::uint32_t i=0;i<count;i++) if(containsVirtualMemory(d,infos[i].memory)) return VK_ERROR_FEATURE_NOT_PRESENT;
    auto fn=reinterpret_cast<PFN_vkBindAccelerationStructureMemoryNV>(d->gdpa(device,"vkBindAccelerationStructureMemoryNV"));
    return fn?fn(device,count,infos):VK_ERROR_FEATURE_NOT_PRESENT;
}
#ifdef VK_ARM_tensors
VKAPI_ATTR VkResult VKAPI_CALL layerBindTensorMemoryARM(VkDevice device,std::uint32_t count,const VkBindTensorMemoryInfoARM* infos) {
    auto d=findDevice(device); if(!d || !infos) return VK_ERROR_INITIALIZATION_FAILED;
    for(std::uint32_t i=0;i<count;i++) if(containsVirtualMemory(d,infos[i].memory)) return VK_ERROR_FEATURE_NOT_PRESENT;
    auto fn=reinterpret_cast<PFN_vkBindTensorMemoryARM>(d->gdpa(device,"vkBindTensorMemoryARM"));
    return fn?fn(device,count,infos):VK_ERROR_FEATURE_NOT_PRESENT;
}
#endif
#ifdef VK_ARM_data_graph
VKAPI_ATTR VkResult VKAPI_CALL layerBindDataGraphPipelineSessionMemoryARM(VkDevice device,std::uint32_t count,
    const VkBindDataGraphPipelineSessionMemoryInfoARM* infos) {
    auto d=findDevice(device); if(!d || !infos) return VK_ERROR_INITIALIZATION_FAILED;
    for(std::uint32_t i=0;i<count;i++) if(containsVirtualMemory(d,infos[i].memory)) return VK_ERROR_FEATURE_NOT_PRESENT;
    auto fn=reinterpret_cast<PFN_vkBindDataGraphPipelineSessionMemoryARM>(d->gdpa(device,"vkBindDataGraphPipelineSessionMemoryARM"));
    return fn?fn(device,count,infos):VK_ERROR_FEATURE_NOT_PRESENT;
}
#endif
#ifdef VK_QCOM_tile_memory_heap
VKAPI_ATTR void VKAPI_CALL layerCmdBindTileMemoryQCOM(VkCommandBuffer commandBuffer,const VkTileMemoryBindInfoQCOM* info) {
    auto d=findDevice(reinterpret_cast<VkDevice>(commandBuffer)); if(!d || !info) return;
    if(d->selectiveRestore) { std::lock_guard<std::mutex> lock(d->mutex); d->submission.unknown(commandBuffer); }
    if(containsVirtualMemory(d,info->memory)) { logf("rejected synthetic memory in vkCmdBindTileMemoryQCOM"); return; }
    auto fn=reinterpret_cast<PFN_vkCmdBindTileMemoryQCOM>(d->gdpa(d->handle,"vkCmdBindTileMemoryQCOM"));
    if(fn) fn(commandBuffer,info);
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL layerGetSnapshotStats(VkDevice device,ZvramSnapshotStatsNX* out) {
    auto d=findDevice(device); if(!d || !out || out->structSize<sizeof(ZvramSnapshotStatsNX)) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lock(d->mutex);
    if(!d->autoInitialized) return VK_ERROR_FEATURE_NOT_PRESENT;
    out->version=1; out->coldLogicalBytes=d->coldLogicalBytes; out->coldStoredBytes=d->coldBytes;
    out->coldBudgetBytes=d->coldBudget; out->residentBytes=d->residentBytes;
    out->freezes=d->freezeCount; out->restores=d->restoreCount; out->failures=d->snapshotFailures;
    out->lastError=static_cast<std::int32_t>(d->lastSnapshotError);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL layerArmRestoreFailure(VkDevice device,std::uint32_t groups) {
    auto d=findDevice(device);
    if(!d || groups==0 || !d->autoInitialized || !std::getenv("ZVRAM_TEST_RESTORE_FAIL_AFTER_GROUPS"))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    std::lock_guard<std::mutex> lock(d->mutex);
    if(d->restoreFailureInjected) return VK_ERROR_FEATURE_NOT_PRESENT;
    d->restoreFailureAfterGroups=groups;
    // Test-only grace keeps a committed child observable after the injected
    // failure, before the normal idle worker can legitimately freeze it again.
    const auto observeUntil=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    for(auto& pair:d->virtualMemory) {
        auto& memory=pair.second;
        if(!memory.cold) continue;
        memory.lastUse=observeUntil;
        for(auto& group:memory.coldGroups) if(group.cold) group.lastUse=observeUntil;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL layerAllocateMemory(VkDevice device,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* allocator,VkDeviceMemory* out) {
    auto d=findDevice(device); if(!d || !d->allocate) return VK_ERROR_INITIALIZATION_FAILED;
    if(info && info->memoryTypeIndex==d->virtualType) {
        VkMemoryAllocateFlags flags{}; float priority{}; bool hasPriority{};
        if(!d->virtualEnabled || !out || !parseVirtualAllocationChain(info->pNext,flags,priority,hasPriority) ||
           ((flags&VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) && !d->bufferDeviceAddressEnabled)) return VK_ERROR_FEATURE_NOT_PRESENT;
        std::lock_guard<std::mutex> lock(d->mutex);
        if(info->allocationSize==0 || info->allocationSize>d->virtualBytes-d->virtualUsage) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        auto* token=new(std::nothrow) std::uint8_t{}; if(!token) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkDeviceMemory handle=tokenHandle<VkDeviceMemory>(reinterpret_cast<std::uintptr_t>(token));
        if(d->allocations.count(handle)) { delete token; return VK_ERROR_OUT_OF_HOST_MEMORY; }
        VirtualMemory entry; entry.size=info->allocationSize; entry.token=token; entry.capacityAccounted=true;
        entry.allocationFlags=flags; entry.priority=priority; entry.hasPriority=hasPriority;
        if(!assignVirtualIdentity(*d,entry)) { delete token; return VK_ERROR_OUT_OF_HOST_MEMORY; }
        try {
            const auto inserted=d->virtualMemory.emplace(handle,std::move(entry));
            if(!inserted.second) { delete token; return VK_ERROR_OUT_OF_HOST_MEMORY; }
        }
        catch(const std::bad_alloc&) { delete token; return VK_ERROR_OUT_OF_HOST_MEMORY; }
        d->virtualUsage+=info->allocationSize; *out=handle;
        logf("virtual allocate bytes=%llu heap-capacity=%llu",static_cast<unsigned long long>(info->allocationSize),static_cast<unsigned long long>(d->virtualBytes));
        return VK_SUCCESS;
    }
    VkResult r=d->allocate(device,info,allocator,out);
    std::unique_lock<std::mutex> lock(d->mutex);
    if(r!=VK_SUCCESS) { ++d->failures; if(verbose()) logf("allocation failed result=%d",static_cast<int>(r)); return r; }
    bool local=false; VkMemoryPropertyFlags typeFlags=0;
    if(info->memoryTypeIndex<d->memory.memoryTypeCount) {
        const auto heap=d->memory.memoryTypes[info->memoryTypeIndex].heapIndex;
        typeFlags=d->memory.memoryTypes[info->memoryTypeIndex].propertyFlags;
        local=heap<d->memory.memoryHeapCount && (d->memory.memoryHeaps[heap].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)!=0;
    }
    VkMemoryAllocateFlags allocationFlags{};
    float allocationPriority=0.5f; bool hasPriority=false;
    const bool adoptable=parseVirtualAllocationChain(info->pNext,allocationFlags,allocationPriority,hasPriority);
    for(auto* p=static_cast<const VkBaseInStructure*>(info->pNext);p;p=p->pNext)
        if(p->sType==VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO)
            allocationFlags=reinterpret_cast<const VkMemoryAllocateFlagsInfo*>(p)->flags;
    const bool wrap=d->autoEnabled && d->nativeWrappingAllowed && local &&
        (typeFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT|VK_MEMORY_PROPERTY_PROTECTED_BIT))==0 && adoptable;
    const VkDeviceMemory native=*out;
    if(!wrap && d->virtualMemory.count(native)) {
        lock.unlock(); d->free(device,native,allocator); *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    void* token=nullptr;
    VkDeviceMemory api=native;
    if(wrap) {
        token=new(std::nothrow) std::uint8_t{};
        if(!token) { lock.unlock(); d->free(device,native,allocator); *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
        api=tokenHandle<VkDeviceMemory>(reinterpret_cast<std::uintptr_t>(token));
        if(api==native || d->allocations.count(api) || d->virtualMemory.count(api)) {
            delete static_cast<std::uint8_t*>(token); lock.unlock(); d->free(device,native,allocator);
            *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    Allocation tracked{}; tracked.size=info->allocationSize; tracked.local=local; tracked.type=info->memoryTypeIndex;
    tracked.nativeHandle=native; tracked.token=token; tracked.wrapped=wrap;
    if(allocator) { tracked.callbacks=*allocator; tracked.hasCallbacks=true; }
    tracked.flags=allocationFlags; tracked.priority=allocationPriority; tracked.hasPriority=hasPriority; tracked.adoptable=adoptable;
    try {
        const auto inserted=d->allocations.emplace(api,tracked);
        if(!inserted.second) { delete static_cast<std::uint8_t*>(token); lock.unlock(); d->free(device,native,allocator); *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    }
    catch(const std::bad_alloc&) { ++d->failures; delete static_cast<std::uint8_t*>(token); lock.unlock(); d->free(device,native,allocator); *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    *out=api;
    auto& live=local?d->liveLocal:d->liveOther; auto& peak=local?d->peakLocal:d->peakOther; live+=info->allocationSize; peak=std::max(peak,live);
    if(verbose()) logf("allocate %s bytes=%llu%s",local?"local":"nonlocal",static_cast<unsigned long long>(info->allocationSize),wrap?" wrapped":"");
    return r;
}
VKAPI_ATTR void VKAPI_CALL layerDestroyBuffer(VkDevice device,VkBuffer buffer,const VkAllocationCallbacks* allocator) {
    auto d=findDevice(device); if(!d || !d->destroyBuffer) return;
    std::lock_guard<std::mutex> lock(d->mutex);
    auto it=d->promotedBuffers.find(buffer);
    VkDeviceMemory memory=VK_NULL_HANDLE;
    auto vi=d->virtualMemory.end();
    bool nativeAllocation=false;
    for(auto v=d->virtualMemory.begin();v!=d->virtualMemory.end();++v) {
        auto& vm=v->second;
        auto b=std::find_if(vm.bindings.begin(),vm.bindings.end(),[&](const auto& entry){return entry.buffer==buffer;});
        if(b==vm.bindings.end()) continue;
        vi=v; memory=v->first;
        if(!vm.cold) {
            std::lock_guard<std::mutex> queueLock(d->queueMutex);
            VkSparseMemoryBind unbind{}; unbind.size=b->size;
            const auto r=bindSparseLocked(device,*d,buffer,&unbind,1);
            if(r!=VK_SUCCESS) { d->gpuGateError=r; d->autoEnabled=false; d->stopWorker.store(true); }
        }
        vm.bindings.erase(b); vm.bound=!vm.bindings.empty(); bumpAsyncVersion(*d,vm.bindingGeneration);
        break;
    }
    if(vi==d->virtualMemory.end() && it!=d->promotedBuffers.end() && it->second.memory && it->second.requirements.size) {
        memory=it->second.memory;
        vi=d->virtualMemory.find(memory);
        if(vi==d->virtualMemory.end()) {
            nativeAllocation=true;
            std::lock_guard<std::mutex> queueLock(d->queueMutex);
            VkSparseMemoryBind unbind{}; unbind.size=it->second.requirements.size;
            const auto unbindResult=bindSparseLocked(device,*d,buffer,&unbind,1);
            if(unbindResult!=VK_SUCCESS) logf("native sparse unbind before buffer destroy failed result=%d",static_cast<int>(unbindResult));
        }
    }
    d->destroyBuffer(device,buffer,allocator);
    if(vi!=d->virtualMemory.end()) {
        auto& vm=vi->second;
        vm.buffer=VK_NULL_HANDLE;
        if(vm.deferredFree && vm.bindings.empty()) {
            if(vm.residentBytes) d->residentBytes-=vm.residentBytes;
            if(vm.coldStoredBytes) {
                ++d->coldBudgetGeneration; d->lastActivity=std::chrono::steady_clock::now(); d->activity.notify_all();
            }
            d->coldBytes-=vm.coldStoredBytes; d->coldLogicalBytes-=vm.coldLogicalSize;
            releaseChildren(*d,vm); if(vm.capacityAccounted) d->virtualUsage-=vm.size;
            delete static_cast<std::uint8_t*>(vm.token); d->virtualMemory.erase(vi);
        }
    } else if(nativeAllocation) {
        auto ai=d->allocations.find(memory); if(ai!=d->allocations.end()) ai->second.bound=false;
    }
    d->promotedBuffers.erase(buffer);
}
VKAPI_ATTR void VKAPI_CALL layerDestroyDevice(VkDevice device,const VkAllocationCallbacks* allocator) {
    auto d=findDevice(device); if(!d) return;
    d->stopWorker.store(true); d->activity.notify_all();
    if(d->snapshotWorker.joinable()) d->snapshotWorker.join();
    { std::lock_guard<std::mutex> lock(d->mutex); std::lock_guard<std::mutex> queueLock(d->queueMutex);
      if(!d->gpuRestoreUnsafe && d->autoInitialized && d->snapshot.deviceWaitIdle) d->snapshot.deviceWaitIdle(device);
      if(d->autoInitialized) logSnapshotState("destroy-cleanup",*d);
      for(auto& pair:d->virtualMemory) { releaseChildren(*d,pair.second); delete static_cast<std::uint8_t*>(pair.second.token); }
      d->virtualMemory.clear();
      for(auto& pair:d->allocations) delete static_cast<std::uint8_t*>(pair.second.token);
      d->allocations.clear();
      releaseSnapshotResources(*d);
      if(!d->gpuRestoreUnsafe && d->autoInitialized) d->autoQueues.destroy();
      d->autoInitialized=false;
      logf("device=%s live-local=%llu peak-local=%llu live-nonlocal=%llu peak-nonlocal=%llu allocation-failures=%llu",d->gpu.c_str(),static_cast<unsigned long long>(d->liveLocal),static_cast<unsigned long long>(d->peakLocal),static_cast<unsigned long long>(d->liveOther),static_cast<unsigned long long>(d->peakOther),static_cast<unsigned long long>(d->failures)); }
    { std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(device)); }
    if(d->destroy) d->destroy(device,allocator);
}
VKAPI_ATTR void VKAPI_CALL layerFreeMemory(VkDevice device,VkDeviceMemory memory,const VkAllocationCallbacks* allocator) {
    auto d=findDevice(device); if(!d || !d->free) return;
    { std::lock_guard<std::mutex> lock(d->mutex); auto i=d->virtualMemory.find(memory);
      if(i!=d->virtualMemory.end()) {
          if(i->second.bound) { i->second.deferredFree=true; return; }
          if(i->second.residentBytes) d->residentBytes-=i->second.residentBytes;
          if(i->second.coldStoredBytes) {
              ++d->coldBudgetGeneration; d->lastActivity=std::chrono::steady_clock::now(); d->activity.notify_all();
          }
          d->coldBytes-=i->second.coldStoredBytes; d->coldLogicalBytes-=i->second.coldLogicalSize;
          releaseChildren(*d,i->second); if(i->second.capacityAccounted) d->virtualUsage-=i->second.size;
          logf("virtual free bytes=%llu",static_cast<unsigned long long>(i->second.size));
          delete static_cast<std::uint8_t*>(i->second.token); d->virtualMemory.erase(i);
          if(d->gpuProfileEnabled && (d->gpuDecodeCalls || d->gpuDecodeFallbacks))
              logGpuProfileSummary(*d," allocation-free=1");
          return;
      } }
    Allocation a{}; bool found=false;
    { std::lock_guard<std::mutex> lock(d->mutex); auto i=d->allocations.find(memory);
      if(i!=d->allocations.end()) { a=i->second; auto& live=a.local?d->liveLocal:d->liveOther; live-=a.size; d->allocations.erase(i); found=true; } }
    if(found && verbose()) logf("free bytes=%llu",static_cast<unsigned long long>(a.size));
    if(found) delete static_cast<std::uint8_t*>(a.token);
    const VkAllocationCallbacks* nativeAllocator=found && a.wrapped?(a.hasCallbacks?&a.callbacks:nullptr):allocator;
    d->free(device,found?a.nativeHandle:memory,nativeAllocator);
}

#include "submission_hooks.inc"

template<class... Args>
VkResult restoreForQueueWithBudgetRetry(Device& d,const char* name,Args... args) {
    d.restoreBudgetRefused=false;
    auto result=restoreForQueue(d,name,args...);
    if(result!=VK_ERROR_OUT_OF_DEVICE_MEMORY || !d.restoreBudgetRefused || d.gpuGateError!=VK_SUCCESS)
        return result;
    // EXT memory-budget estimates can shrink between admission and restore.
    // Re-admit once under the current estimate; this is not a reservation.
    d.restoreBudgetRefused=false;
    result=admitForQueue(d,name,args...);
    if(result!=VK_SUCCESS) return result;
    d.restoreBudgetRefused=false;
    return restoreForQueue(d,name,args...);
}

template<class... Args>
void bumpAcceptedWriteEpochs(Device& d,const char* name,Args... args) {
    if(!d.asyncCompression) return;
    std::vector<ActiveRefs::Use> writes;
    bool known=false;
    try { known=queueMemoryAccess(d,name,writes,true,args...); }
    catch(const std::bad_alloc&) { known=false; }
    auto bump=[&](VkDeviceMemory memory,std::size_t child) {
        const auto found=d.virtualMemory.find(memory);
        if(found==d.virtualMemory.end()) return;
        auto& groups=found->second.coldGroups;
        if(child==SIZE_MAX || child>=groups.size()) {
            for(auto& group:groups) bumpAsyncVersion(d,group.writeEpoch);
        } else bumpAsyncVersion(d,groups[child].writeEpoch);
    };
    if(!known) {
        for(auto& pair:d.virtualMemory)
            for(auto& group:pair.second.coldGroups) bumpAsyncVersion(d,group.writeEpoch);
        return;
    }
    for(const auto& use:writes) bump(use.memory,use.child);
}

template<class Function,class... Args>
VkResult queueCall(VkQueue queue,const char* name,Args... args) {
    auto d=findDevice(reinterpret_cast<VkDevice>(queue)); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    auto next=reinterpret_cast<Function>(d->gdpa(d->handle,name)); if(!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if(d->virtualEnabled && d->gpuGateError!=VK_SUCCESS) return d->gpuGateError;
    if(!d->autoInitialized) {
        // Only a queue shared with internal sparse binds needs layer serialization.
        if(queue==d->sparseQueue) {
            std::lock_guard<std::mutex> queueLock(d->queueMutex);
            return next(queue,args...);
        }
        return next(queue,args...);
    }
    std::unique_lock<std::mutex> deviceLock(d->mutex,std::defer_lock);
    std::unique_lock<std::mutex> queueLock(d->queueMutex,std::defer_lock);
    if(d->virtualEnabled) { deviceLock.lock(); queueLock.lock(); }
    if(d->gpuGateError!=VK_SUCCESS) return d->gpuGateError;
    const auto admission=admitForQueue(*d,name,args...); if(admission!=VK_SUCCESS) return admission;
    const bool hasCold=std::any_of(d->virtualMemory.begin(),d->virtualMemory.end(),[](const auto& pair){return pair.second.cold;});
    // An idle wait executes no application memory accesses. Keep cold pools asleep.
    if(hasCold && std::strcmp(name,"vkQueueWaitIdle")!=0) {
        const auto r=restoreForQueueWithBudgetRetry(*d,name,args...); if(r!=VK_SUCCESS) return r;
    }
    const bool unsupportedOrdering=std::strcmp(name,"vkQueueBindSparse")==0 || std::strcmp(name,"vkQueuePresentKHR")==0;
    if(unsupportedOrdering && d->autoEnabled) {
        d->autoEnabled=false; d->stopWorker.store(true); d->activity.notify_all();
    }
    if(d->activeEviction) {
        const auto p=std::find_if(d->restoreQueueGenerations.begin(),d->restoreQueueGenerations.end(),
            [&](const auto& entry){return entry.first==queue;});
        if(p==d->restoreQueueGenerations.end()) return VK_ERROR_FEATURE_NOT_PRESENT;
        if(p->second!=d->restoreGeneration) {
            const auto visible=d->autoQueues.fromCopyQueueActive(queue);
            if(visible!=VK_SUCCESS) {
                d->gpuGateError=visible; d->autoEnabled=false; d->stopWorker.store(true); d->activity.notify_all();
                return visible;
            }
            p->second=d->restoreGeneration;
        }
    }
    const auto r=next(queue,args...);
    if(r==VK_SUCCESS && std::strcmp(name,"vkQueueWaitIdle")!=0)
        invalidateAcceptedWrites(*d,name,args...);
    if(r==VK_SUCCESS && std::strcmp(name,"vkQueueWaitIdle")!=0)
        bumpAcceptedWriteEpochs(*d,name,args...);
    if(r==VK_SUCCESS && d->residentLimitBytes && d->autoEnabled && std::strcmp(name,"vkQueueWaitIdle")==0) {
        const auto finished=d->autoQueues.finishIdleActive(queue,
            [&](VkQueue q){d->activeRefs.retire(q);},[&](VkQueue q){d->activeRefs.cover(q);});
        if(finished!=VK_SUCCESS) {
            d->gpuGateError=finished; d->autoEnabled=false; d->stopWorker.store(true); d->activity.notify_all();
        }
    }
    const bool submitted=r==VK_SUCCESS && d->autoInitialized &&
       (std::strcmp(name,"vkQueueSubmit")==0 || std::strcmp(name,"vkQueueSubmit2")==0 || std::strcmp(name,"vkQueueSubmit2KHR")==0);
    if(submitted) ++d->gpuSubmissionGeneration;
    if(submitted && !unsupportedOrdering) {
        if(d->activeEviction && d->autoEnabled) {
            try {
                std::vector<ActiveRefs::Use> memories;
                const bool known=queueMemories(*d,name,memories,args...);
                d->activeRefs.recordRanges(queue,memories,known);
                for(auto& pair:d->virtualMemory) {
                    auto& memory=pair.second;
                    const auto now=std::chrono::steady_clock::now();
                    for(std::size_t i=0;i<memory.coldGroups.size();i++)
                        if(!known || std::any_of(memories.begin(),memories.end(),[&](const auto& use){
                            return use.memory==pair.first && (use.child==SIZE_MAX || use.child==i);
                        })) { memory.coldGroups[i].lastUse=now; memory.lastUse=now; }
                }
            } catch(const std::bad_alloc&) {
                d->autoEnabled=false; d->stopWorker.store(true); d->activity.notify_all();
                logf("active Vulkan eviction disabled: tracking allocation failed");
            }
        }
        bool marked=false;
        const auto marker=d->autoQueues.submitted(queue,marked);
        if(d->activeEviction && marked) d->activeRefs.cover(queue);
        if(marker!=VK_SUCCESS) {
            d->autoEnabled=false; d->stopWorker.store(true); d->activity.notify_all();
            ++d->snapshotFailures; d->lastSnapshotError=marker;
        }
    }
    if(d->autoEnabled) { d->lastActivity=std::chrono::steady_clock::now(); d->activity.notify_all(); }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueueSubmit(VkQueue q,std::uint32_t count,const VkSubmitInfo* info,VkFence fence) {
    return queueCall<PFN_vkQueueSubmit>(q,"vkQueueSubmit",count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueueSubmit2(VkQueue q,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    return queueCall<PFN_vkQueueSubmit2>(q,"vkQueueSubmit2",count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueueSubmit2KHR(VkQueue q,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    return queueCall<PFN_vkQueueSubmit2>(q,"vkQueueSubmit2KHR",count,info,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueueBindSparse(VkQueue q,std::uint32_t count,const VkBindSparseInfo* info,VkFence fence) {
    auto d=findDevice(reinterpret_cast<VkDevice>(q)); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    if(!info || count==0) return queueCall<PFN_vkQueueBindSparse>(q,"vkQueueBindSparse",count,info,fence);
    std::vector<VkBindSparseInfo> translated;
    std::vector<std::vector<VkSparseBufferMemoryBindInfo>> bufferInfos;
    std::vector<std::vector<std::vector<VkSparseMemoryBind>>> bufferBinds;
    std::vector<std::vector<VkSparseImageOpaqueMemoryBindInfo>> opaqueInfos;
    std::vector<std::vector<std::vector<VkSparseMemoryBind>>> opaqueBinds;
    std::vector<std::vector<VkSparseImageMemoryBindInfo>> imageInfos;
    std::vector<std::vector<std::vector<VkSparseImageMemoryBind>>> imageBinds;
    try {
        translated.assign(info,info+count);
        bufferInfos.resize(count); bufferBinds.resize(count); opaqueInfos.resize(count); opaqueBinds.resize(count);
        imageInfos.resize(count); imageBinds.resize(count);
        for(std::uint32_t n=0;n<count;n++) {
            if(info[n].bufferBindCount) {
                bufferInfos[n].assign(info[n].pBufferBinds,info[n].pBufferBinds+info[n].bufferBindCount);
                bufferBinds[n].resize(info[n].bufferBindCount);
                for(std::uint32_t i=0;i<info[n].bufferBindCount;i++) {
                    const auto& src=info[n].pBufferBinds[i];
                    if(src.bindCount) bufferBinds[n][i].assign(src.pBinds,src.pBinds+src.bindCount);
                    bufferInfos[n][i].pBinds=bufferBinds[n][i].data();
                }
                translated[n].pBufferBinds=bufferInfos[n].data();
            }
            if(info[n].imageOpaqueBindCount) {
                opaqueInfos[n].assign(info[n].pImageOpaqueBinds,info[n].pImageOpaqueBinds+info[n].imageOpaqueBindCount);
                opaqueBinds[n].resize(info[n].imageOpaqueBindCount);
                for(std::uint32_t i=0;i<info[n].imageOpaqueBindCount;i++) {
                    const auto& src=info[n].pImageOpaqueBinds[i];
                    if(src.bindCount) opaqueBinds[n][i].assign(src.pBinds,src.pBinds+src.bindCount);
                    opaqueInfos[n][i].pBinds=opaqueBinds[n][i].data();
                }
                translated[n].pImageOpaqueBinds=opaqueInfos[n].data();
            }
            if(info[n].imageBindCount) {
                imageInfos[n].assign(info[n].pImageBinds,info[n].pImageBinds+info[n].imageBindCount);
                imageBinds[n].resize(info[n].imageBindCount);
                for(std::uint32_t i=0;i<info[n].imageBindCount;i++) {
                    const auto& src=info[n].pImageBinds[i];
                    if(src.bindCount) imageBinds[n][i].assign(src.pBinds,src.pBinds+src.bindCount);
                    imageInfos[n][i].pBinds=imageBinds[n][i].data();
                }
                translated[n].pImageBinds=imageInfos[n].data();
            }
        }
    } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto translate=[&](VkDeviceMemory& memory) {
            if(!memory) return VK_SUCCESS;
            if(d->virtualMemory.count(memory)) return VK_ERROR_FEATURE_NOT_PRESENT;
            const auto found=d->allocations.find(memory);
            if(found!=d->allocations.end()) { found->second.bound=true; memory=found->second.nativeHandle; }
            return VK_SUCCESS;
        };
        for(std::uint32_t n=0;n<count;n++) {
            for(auto& group:bufferBinds[n]) for(auto& bind:group) { const auto r=translate(bind.memory); if(r!=VK_SUCCESS) return r; }
            for(auto& group:opaqueBinds[n]) for(auto& bind:group) { const auto r=translate(bind.memory); if(r!=VK_SUCCESS) return r; }
            for(auto& group:imageBinds[n]) for(auto& bind:group) { const auto r=translate(bind.memory); if(r!=VK_SUCCESS) return r; }
        }
    }
    return queueCall<PFN_vkQueueBindSparse>(q,"vkQueueBindSparse",count,translated.data(),fence);
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueueWaitIdle(VkQueue q) {
    return queueCall<PFN_vkQueueWaitIdle>(q,"vkQueueWaitIdle");
}
VKAPI_ATTR VkResult VKAPI_CALL layerQueuePresent(VkQueue q,const VkPresentInfoKHR* info) {
    auto d=findDevice(reinterpret_cast<VkDevice>(q)); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    if(d->bufferPresentation && d->autoInitialized && info && !info->pNext) {
        auto next=reinterpret_cast<PFN_vkQueuePresentKHR>(d->gdpa(d->handle,"vkQueuePresentKHR"));
        if(!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
        std::unique_lock<std::mutex> deviceLock(d->mutex,std::defer_lock);
        std::unique_lock<std::mutex> queueLock(d->queueMutex,std::defer_lock);
        if(d->virtualEnabled) { deviceLock.lock(); queueLock.lock(); }
        if(d->gpuGateError!=VK_SUCCESS) return d->gpuGateError;
        // Base present reads only native swapchain images after its wait semaphores;
        // it does not access zVram's application buffer-backed sparse allocations.
        const auto result=next(q,info);
        if(result==VK_ERROR_DEVICE_LOST) {
            d->gpuGateError=result; d->autoEnabled=false; d->stopWorker.store(true); d->activity.notify_all();
        }
        return result;
    }
    return queueCall<PFN_vkQueuePresentKHR>(q,"vkQueuePresentKHR",info);
}
auto isForcedBuffer(const std::shared_ptr<Device>& d) {
    return [d](VkBuffer buffer) {
        std::lock_guard<std::mutex> lock(d->mutex);
        const auto found=d->promotedBuffers.find(buffer);
        return found!=d->promotedBuffers.end() && found->second.concurrentForced;
    };
}
VKAPI_ATTR void VKAPI_CALL layerCmdPipelineBarrier(VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStage,VkPipelineStageFlags dstStage,VkDependencyFlags flags,
    std::uint32_t memoryCount,const VkMemoryBarrier* memoryBarriers,
    std::uint32_t bufferCount,const VkBufferMemoryBarrier* bufferBarriers,
    std::uint32_t imageCount,const VkImageMemoryBarrier* imageBarriers) {
    auto d=findDevice(reinterpret_cast<VkDevice>(commandBuffer)); if(!d) return;
    auto fn=reinterpret_cast<PFN_vkCmdPipelineBarrier>(d->gdpa(d->handle,"vkCmdPipelineBarrier"));
    trackSubmission(d,[&](auto& t){for(std::uint32_t i=0;i<bufferCount;i++) t.buffer(commandBuffer,bufferBarriers[i].buffer);});
    if(!d->autoInitialized) { if(fn) fn(commandBuffer,srcStage,dstStage,flags,memoryCount,memoryBarriers,bufferCount,bufferBarriers,imageCount,imageBarriers); return; }
    auto forced=isForcedBuffer(d);
    zvram::cmdPipelineBarrier(fn,forced,commandBuffer,srcStage,dstStage,flags,memoryCount,memoryBarriers,
                              bufferCount,bufferBarriers,imageCount,imageBarriers);
}
VKAPI_ATTR void VKAPI_CALL layerCmdWaitEvents(VkCommandBuffer commandBuffer,
    std::uint32_t eventCount,const VkEvent* events,VkPipelineStageFlags srcStage,VkPipelineStageFlags dstStage,
    std::uint32_t memoryCount,const VkMemoryBarrier* memoryBarriers,
    std::uint32_t bufferCount,const VkBufferMemoryBarrier* bufferBarriers,
    std::uint32_t imageCount,const VkImageMemoryBarrier* imageBarriers) {
    auto d=findDevice(reinterpret_cast<VkDevice>(commandBuffer)); if(!d) return;
    auto fn=reinterpret_cast<PFN_vkCmdWaitEvents>(d->gdpa(d->handle,"vkCmdWaitEvents"));
    trackSubmission(d,[&](auto& t){for(std::uint32_t i=0;i<bufferCount;i++) t.buffer(commandBuffer,bufferBarriers[i].buffer);});
    if(!d->autoInitialized) { if(fn) fn(commandBuffer,eventCount,events,srcStage,dstStage,memoryCount,memoryBarriers,bufferCount,bufferBarriers,imageCount,imageBarriers); return; }
    auto forced=isForcedBuffer(d);
    zvram::cmdWaitEvents(fn,forced,commandBuffer,eventCount,events,srcStage,dstStage,memoryCount,memoryBarriers,
                         bufferCount,bufferBarriers,imageCount,imageBarriers);
}
VKAPI_ATTR void VKAPI_CALL layerCmdPipelineBarrier2(VkCommandBuffer commandBuffer,const VkDependencyInfo* dependency) {
    auto d=findDevice(reinterpret_cast<VkDevice>(commandBuffer)); if(!d) return;
    auto fn=reinterpret_cast<PFN_vkCmdPipelineBarrier2>(d->gdpa(d->handle,"vkCmdPipelineBarrier2"));
    if(!fn) fn=reinterpret_cast<PFN_vkCmdPipelineBarrier2>(d->gdpa(d->handle,"vkCmdPipelineBarrier2KHR"));
    trackSubmission(d,[&](auto& t){
        if(!dependency || dependency->pNext) { t.unknown(commandBuffer); return; }
        for(std::uint32_t i=0;i<dependency->bufferMemoryBarrierCount;i++) {
            if(dependency->pBufferMemoryBarriers[i].pNext) t.unknown(commandBuffer);
            t.buffer(commandBuffer,dependency->pBufferMemoryBarriers[i].buffer);
        }
    });
    if(!d->autoInitialized) { if(fn) fn(commandBuffer,dependency); return; }
    auto forced=isForcedBuffer(d); zvram::cmdPipelineBarrier2(fn,forced,commandBuffer,dependency);
}
VKAPI_ATTR void VKAPI_CALL layerCmdWaitEvents2(VkCommandBuffer commandBuffer,std::uint32_t eventCount,
    const VkEvent* events,const VkDependencyInfo* dependencies) {
    auto d=findDevice(reinterpret_cast<VkDevice>(commandBuffer)); if(!d) return;
    auto fn=reinterpret_cast<PFN_vkCmdWaitEvents2>(d->gdpa(d->handle,"vkCmdWaitEvents2"));
    if(!fn) fn=reinterpret_cast<PFN_vkCmdWaitEvents2>(d->gdpa(d->handle,"vkCmdWaitEvents2KHR"));
    trackSubmission(d,[&](auto& t){
        for(std::uint32_t n=0;n<eventCount;n++) {
            const auto& dep=dependencies[n];
            if(dep.pNext) t.unknown(commandBuffer);
            for(std::uint32_t i=0;i<dep.bufferMemoryBarrierCount;i++) {
                if(dep.pBufferMemoryBarriers[i].pNext) t.unknown(commandBuffer);
                t.buffer(commandBuffer,dep.pBufferMemoryBarriers[i].buffer);
            }
        }
    });
    if(!d->autoInitialized) { if(fn) fn(commandBuffer,eventCount,events,dependencies); return; }
    auto forced=isForcedBuffer(d); zvram::cmdWaitEvents2(fn,forced,commandBuffer,eventCount,events,dependencies);
}
VKAPI_ATTR VkResult VKAPI_CALL layerDeviceWaitIdle(VkDevice device) {
    auto d=findDevice(device); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    auto next=reinterpret_cast<PFN_vkDeviceWaitIdle>(d->gdpa(device,"vkDeviceWaitIdle")); if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    std::unique_lock<std::mutex> deviceLock(d->mutex,std::defer_lock);
    std::unique_lock<std::mutex> queueLock(d->queueMutex,std::defer_lock);
    if(d->virtualEnabled) { deviceLock.lock(); queueLock.lock(); }
    if(d->gpuGateError!=VK_SUCCESS) return d->gpuGateError;
    const auto r=next(device);
    if(d->autoEnabled) { d->lastActivity=std::chrono::steady_clock::now(); d->activity.notify_all(); }
    return r;
}

#include "command_hooks.inc"

PFN_vkVoidFunction lookup(const char* name) {
    if(!name) return nullptr;
    if(auto f=submissionHookLookup(name)) return f;
    if(auto f=trackedCommandLookup(name)) return f;
#define MATCH(n,f) if(std::strcmp(name,n)==0) return reinterpret_cast<PFN_vkVoidFunction>(f)
    MATCH("vkGetInstanceProcAddr",layerGetInstanceProcAddr); MATCH("vkGetDeviceProcAddr",layerGetDeviceProcAddr);
    MATCH("vk_layerGetPhysicalDeviceProcAddr",layerGetPhysicalDeviceProcAddr);
    MATCH("vkCreateInstance",layerCreateInstance); MATCH("vkDestroyInstance",layerDestroyInstance);
    MATCH("vkCreateDevice",layerCreateDevice); MATCH("vkDestroyDevice",layerDestroyDevice);
    MATCH("vkGetPhysicalDeviceMemoryProperties",layerGetPhysicalDeviceMemoryProperties);
    MATCH("vkGetPhysicalDeviceMemoryProperties2",layerGetPhysicalDeviceMemoryProperties2);
    MATCH("vkGetPhysicalDeviceMemoryProperties2KHR",layerGetPhysicalDeviceMemoryProperties2);
    MATCH("vkAllocateMemory",layerAllocateMemory); MATCH("vkFreeMemory",layerFreeMemory);
    MATCH("vkCreateBuffer",layerCreateBuffer); MATCH("vkDestroyBuffer",layerDestroyBuffer);
    MATCH("vkGetBufferMemoryRequirements",layerGetBufferMemoryRequirements);
    MATCH("vkGetBufferMemoryRequirements2",layerGetBufferMemoryRequirements2); MATCH("vkGetBufferMemoryRequirements2KHR",layerGetBufferMemoryRequirements2);
    MATCH("vkBindBufferMemory",layerBindBufferMemory);
    MATCH("vkBindBufferMemory2",layerBindBufferMemory2); MATCH("vkBindBufferMemory2KHR",layerBindBufferMemory2);
    MATCH("vkMapMemory",layerMapMemory); MATCH("vkBindImageMemory",layerBindImageMemory);
    MATCH("vkBindImageMemory2",layerBindImageMemory2); MATCH("vkBindImageMemory2KHR",layerBindImageMemory2);
    MATCH("vkGetDeviceMemoryCommitment",layerGetDeviceMemoryCommitment);
    MATCH("vkGetDeviceMemoryOpaqueCaptureAddress",layerGetDeviceMemoryOpaqueCaptureAddress);
    MATCH("vkGetDeviceMemoryOpaqueCaptureAddressKHR",layerGetDeviceMemoryOpaqueCaptureAddressKHR);
    MATCH("vkSetDeviceMemoryPriorityEXT",layerSetDeviceMemoryPriorityEXT);
    MATCH("vkSetDebugUtilsObjectNameEXT",layerSetDebugUtilsObjectNameEXT);
    MATCH("vkSetDebugUtilsObjectTagEXT",layerSetDebugUtilsObjectTagEXT);
    MATCH("vkDebugMarkerSetObjectNameEXT",layerDebugMarkerSetObjectNameEXT);
    MATCH("vkDebugMarkerSetObjectTagEXT",layerDebugMarkerSetObjectTagEXT);
    MATCH("vkBindVideoSessionMemoryKHR",layerBindVideoSessionMemoryKHR);
    MATCH("vkBindAccelerationStructureMemoryNV",layerBindAccelerationStructureMemoryNV);
#ifdef VK_ARM_tensors
    MATCH("vkBindTensorMemoryARM",layerBindTensorMemoryARM);
#endif
#ifdef VK_ARM_data_graph
    MATCH("vkBindDataGraphPipelineSessionMemoryARM",layerBindDataGraphPipelineSessionMemoryARM);
#endif
#ifdef VK_QCOM_tile_memory_heap
    MATCH("vkCmdBindTileMemoryQCOM",layerCmdBindTileMemoryQCOM);
#endif
    MATCH("vkZVramGetSnapshotStatsNX",layerGetSnapshotStats);
    MATCH("vkZVramArmRestoreFailureNX",layerArmRestoreFailure);
    MATCH("vkQueueSubmit",layerQueueSubmit); MATCH("vkQueueSubmit2",layerQueueSubmit2);
    MATCH("vkQueueSubmit2KHR",layerQueueSubmit2KHR); MATCH("vkQueueBindSparse",layerQueueBindSparse);
    MATCH("vkQueueWaitIdle",layerQueueWaitIdle); MATCH("vkQueuePresentKHR",layerQueuePresent);
    MATCH("vkCmdPipelineBarrier",layerCmdPipelineBarrier); MATCH("vkCmdWaitEvents",layerCmdWaitEvents);
    MATCH("vkCmdPipelineBarrier2",layerCmdPipelineBarrier2); MATCH("vkCmdPipelineBarrier2KHR",layerCmdPipelineBarrier2);
    MATCH("vkCmdWaitEvents2",layerCmdWaitEvents2); MATCH("vkCmdWaitEvents2KHR",layerCmdWaitEvents2);
    MATCH("vkDeviceWaitIdle",layerDeviceWaitIdle);
#undef MATCH
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetInstanceProcAddr(VkInstance instance,const char* name) {
    if(name && std::strcmp(name,"vkZVramArmRestoreFailureNX")==0) return nullptr;
    if(auto f=lookup(name)) return f;
    auto s=findInstance(key(instance)); if(s&&s->gipa) {
        auto f=s->gipa(instance,name);
        if(f && name && std::strncmp(name,"vkCmd",5)==0) unknownCommandProc=true;
        return f;
    }
    if(!instance) { PFN_vkGetInstanceProcAddr next{}; { std::lock_guard<std::mutex> lock(mapsMutex); next=globalGipa; } return next?next(VK_NULL_HANDLE,name):nullptr; }
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(VkDevice device,const char* name) {
    auto d=findDevice(device);
    if(isTrackedCommand(name) || submissionHookLookup(name)) {
        auto next=d&&d->gdpa?d->gdpa(device,name):nullptr;
        if(!next || !d->selectiveRestore) return next;
        return lookup(name);
    }
    if(name && std::strcmp(name,"vkZVramArmRestoreFailureNX")==0 &&
       (!d || !d->autoInitialized || !std::getenv("ZVRAM_TEST_RESTORE_FAIL_AFTER_GROUPS"))) return nullptr;
    const bool barrierCommand=name && (
        std::strcmp(name,"vkCmdPipelineBarrier")==0 || std::strcmp(name,"vkCmdWaitEvents")==0 ||
        std::strcmp(name,"vkCmdPipelineBarrier2")==0 || std::strcmp(name,"vkCmdPipelineBarrier2KHR")==0 ||
        std::strcmp(name,"vkCmdWaitEvents2")==0 || std::strcmp(name,"vkCmdWaitEvents2KHR")==0);
    if(barrierCommand && d && !d->autoInitialized) return d->gdpa?d->gdpa(device,name):nullptr;
    if(name && (std::strcmp(name,"vkQueuePresentKHR")==0 || std::strcmp(name,"vkQueueSubmit2")==0 || std::strcmp(name,"vkQueueSubmit2KHR")==0))
        if(!d || !d->gdpa || !d->gdpa(device,name)) return nullptr;
    if(name && (std::strcmp(name,"vkCmdPipelineBarrier2")==0 || std::strcmp(name,"vkCmdPipelineBarrier2KHR")==0))
        if(!d || !d->gdpa || (!d->gdpa(device,name) && !d->gdpa(device,"vkCmdPipelineBarrier2") && !d->gdpa(device,"vkCmdPipelineBarrier2KHR"))) return nullptr;
    if(name && (std::strcmp(name,"vkCmdWaitEvents2")==0 || std::strcmp(name,"vkCmdWaitEvents2KHR")==0))
        if(!d || !d->gdpa || (!d->gdpa(device,name) && !d->gdpa(device,"vkCmdWaitEvents2") && !d->gdpa(device,"vkCmdWaitEvents2KHR"))) return nullptr;
    if(name && (std::strcmp(name,"vkGetDeviceMemoryOpaqueCaptureAddress")==0 ||
                std::strcmp(name,"vkGetDeviceMemoryOpaqueCaptureAddressKHR")==0))
        if(!d || !d->gdpa || !d->gdpa(device,name)) return nullptr;
    if(name && std::strcmp(name,"vkSetDeviceMemoryPriorityEXT")==0)
        if(!d || !d->gdpa || !d->gdpa(device,name)) return nullptr;
    if(name && (std::strcmp(name,"vkSetDebugUtilsObjectNameEXT")==0 ||
                std::strcmp(name,"vkSetDebugUtilsObjectTagEXT")==0 ||
                std::strcmp(name,"vkDebugMarkerSetObjectNameEXT")==0 ||
                std::strcmp(name,"vkDebugMarkerSetObjectTagEXT")==0))
        if(!d || !d->gdpa || !d->gdpa(device,name)) return nullptr;
    if(name && (std::strcmp(name,"vkBindVideoSessionMemoryKHR")==0 ||
                std::strcmp(name,"vkBindAccelerationStructureMemoryNV")==0 ||
                std::strcmp(name,"vkBindTensorMemoryARM")==0 ||
                std::strcmp(name,"vkBindDataGraphPipelineSessionMemoryARM")==0 ||
                std::strcmp(name,"vkCmdBindTileMemoryQCOM")==0))
        if(!d || !d->gdpa || !d->gdpa(device,name)) return nullptr;
    if(auto f=lookup(name)) return f;
    auto f=d&&d->gdpa?d->gdpa(device,name):nullptr;
    if(f && name && std::strncmp(name,"vkCmd",5)==0) unknownCommandProc=true;
    return f;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGetPhysicalDeviceProcAddr(VkInstance instance,const char* name) {
    if(auto f=lookup(name)) return f;
    auto s=findInstance(key(instance)); if(!s) return nullptr;
    auto f=s->physProc?s->physProc(instance,name):s->gipa(instance,name);
    if(f && name && std::strncmp(name,"vkCmd",5)==0) unknownCommandProc=true;
    return f;
}
} // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if(!v || v->sType!=LAYER_NEGOTIATE_INTERFACE_STRUCT || v->loaderLayerInterfaceVersion<1) return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion=std::min(v->loaderLayerInterfaceVersion,2u);
    v->pfnGetInstanceProcAddr=layerGetInstanceProcAddr; v->pfnGetDeviceProcAddr=layerGetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr=layerGetPhysicalDeviceProcAddr; return VK_SUCCESS;
}
