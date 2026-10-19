// CPU-only production-path check. Including the layer keeps its internal
// bootstrap/restore helpers directly testable without a Vulkan loader.
#include "layer.cpp"

#include <iostream>
#include <unordered_set>
#include <stdexcept>
#include <condition_variable>

namespace zvram::gdeflate::gpu {
class DecoderTeardownTestAccess {
public:
    static void seedPoisonedDecoder(Decoder& decoder,VkDevice device,VkBuffer buffer,
                                    VkDeviceMemory memory,PFN_vkDestroyBuffer destroyBuffer,
                                    PFN_vkFreeMemory freeMemory) {
        decoder.device_=device;
        decoder.api_.destroyBuffer=destroyBuffer;
        decoder.api_.freeMemory=freeMemory;
        decoder.input_.buffer=buffer;
        decoder.input_.memory=memory;
        decoder.initialized_=true;
        decoder.markPoisoned();
    }
};
}

namespace {
constexpr VkDeviceSize MiB = 1024ull * 1024ull;
unsigned allocations{};
unsigned sparseBinds{};
unsigned queueWaitCalls{};
int failAllocations{};
std::unordered_set<std::uint32_t> failAllocationTypes;
std::vector<std::uint32_t> allocatedTypes;
int failSparseBinds{};
unsigned failQueueWaitAt{};
VkResult failQueueWaitResult{VK_ERROR_DEVICE_LOST};
unsigned failSparseBindAt{};
VkResult mockEndCommandResult{VK_SUCCESS};
std::uint32_t mockPoolViewTypeBits{1};
unsigned frees{};
unsigned budgetQueries{};
unsigned submitCalls{};
VkDeviceSize mockHeapBudget{};
VkDeviceSize mockHeapUsage{};
VkDeviceSize mockNativeHeapSize{};
std::vector<std::pair<VkDeviceSize,VkDeviceSize>> mockBudgetSequence;
std::vector<VkQueue> mockWaitedQueues;
std::unordered_map<VkQueue,VkResult> mockQueueWaitResults;
const void* applicationSubmitPnext{};
const VkSubmitInfo* applicationSubmitInfo{};
unsigned presentCalls{};
unsigned samplerHeapBindCalls{};
unsigned copyBufferToImageForwards{};
const VkCopyBufferToImageInfo2* forwardedCopyBufferToImage{};
VkResult presentResult{VK_SUCCESS};
const VkPresentInfoKHR* forwardedPresent{};
std::uintptr_t nextHandle{0x1000};
std::unordered_map<VkBuffer,VkDeviceSize> bufferSizes;
std::unordered_set<VkDeviceMemory> liveAllocations;
unsigned trackedSyncForwards{};
unsigned trackedLabelForwards{};
unsigned unsupportedCommandForwards{};
unsigned pipelineBarrierForwards{},waitEventsForwards{},pipelineBarrier2Forwards{},waitEvents2Forwards{};
struct CapturedBufferBind { VkBuffer buffer{}; std::vector<VkSparseMemoryBind> binds; };
std::vector<std::vector<CapturedBufferBind>> sparseBindCalls;
struct CapturedMemoryBarrier {
    VkPipelineStageFlags srcStage{},dstStage{};
    VkAccessFlags srcAccess{},dstAccess{};
};
std::vector<CapturedMemoryBarrier> capturedMemoryBarriers;
std::mutex mockFenceMutex;
std::condition_variable mockFenceCondition;
bool mockFenceWaitEntered{},mockFenceWaitRelease{};
VkResult mockFenceWaitResult{VK_SUCCESS};
std::mutex workerFreeMutex;
std::condition_variable workerFreeCondition;
bool observeWorkerFrees{};
unsigned observedWorkerFrees{};
std::vector<VkDeviceMemory> observedWorkerFreedMemory;
std::uint32_t mockDeviceWaitCalls{},mockResetCommandPoolCalls{};
VkResult mockDeviceWaitResult{VK_SUCCESS};
std::uint32_t destroyedMockBuffers{},destroyedMockFences{};
const void* lastDestroyedBufferUserData{};
const void* lastFreedMemoryUserData{};
std::unordered_map<VkDeviceMemory,const void*> freedMemoryUserData;
Device* destroyObservedDevice{};
bool destroyObservedDecoderUnsafe{};
bool destroyObservedDecoderPresent{};
unsigned destroyDeviceCalls{};
std::uint32_t destroyedBuffersAtNativeDeviceDestroy{},freesAtNativeDeviceDestroy{};
bool flipUnknownCommandOnBudgetQuery{};
std::mutex admissionSignalMutex;
std::condition_variable admissionSignal;
std::atomic<bool> signalBudgetQuery{};
bool budgetQueryEntered{};

void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void requireColdLogicalMatchesState(const Device& d,const char* message) {
    VkDeviceSize expected=0;
    bool anyCold=false;
    for(const auto& pair:d.virtualMemory) if(pair.second.cold) {
        anyCold=true;
        expected+=pair.second.coldLogicalSize;
    }
    require((d.coldLogicalBytes!=0)==anyCold && d.coldLogicalBytes==expected,message);
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateBuffer(VkDevice,const VkBufferCreateInfo* info,
    const VkAllocationCallbacks*,VkBuffer* out) {
    *out=tokenHandle<VkBuffer>(nextHandle++); bufferSizes[*out]=info->size; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL mockDestroyBuffer(VkDevice,VkBuffer buffer,const VkAllocationCallbacks* allocator) {
    ++destroyedMockBuffers; lastDestroyedBufferUserData=allocator?allocator->pUserData:nullptr;
    bufferSizes.erase(buffer);
}
VKAPI_ATTR void VKAPI_CALL mockGetBufferMemoryRequirements(VkDevice,VkBuffer buffer,VkMemoryRequirements* req) {
    req->size=bufferSizes.at(buffer); req->alignment=4096; req->memoryTypeBits=mockPoolViewTypeBits;
}
VKAPI_ATTR VkResult VKAPI_CALL mockAllocate(VkDevice,const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks*,VkDeviceMemory* out) {
    ++allocations;
    allocatedTypes.push_back(info->memoryTypeIndex);
    if(failAllocations>0) { --failAllocations; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    if(failAllocationTypes.count(info->memoryTypeIndex)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out=tokenHandle<VkDeviceMemory>(nextHandle++); liveAllocations.insert(*out); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL mockFree(VkDevice,VkDeviceMemory memory,const VkAllocationCallbacks* allocator) {
    ++frees; lastFreedMemoryUserData=allocator?allocator->pUserData:nullptr; liveAllocations.erase(memory);
    freedMemoryUserData[memory]=lastFreedMemoryUserData;
    { std::lock_guard<std::mutex> lock(workerFreeMutex);
      if(observeWorkerFrees) {
          ++observedWorkerFrees; observedWorkerFreedMemory.push_back(memory);
          workerFreeCondition.notify_all();
      } }
}
VKAPI_ATTR VkResult VKAPI_CALL mockSparse(VkQueue,std::uint32_t count,const VkBindSparseInfo* infos,VkFence) {
    ++sparseBinds;
    std::vector<CapturedBufferBind> call;
    for(std::uint32_t i=0;i<count;i++) {
        for(std::uint32_t j=0;j<infos[i].bufferBindCount;j++) {
            CapturedBufferBind captured; captured.buffer=infos[i].pBufferBinds[j].buffer;
            const auto& source=infos[i].pBufferBinds[j];
            captured.binds.assign(source.pBinds,source.pBinds+source.bindCount);
            call.push_back(std::move(captured));
        }
    }
    sparseBindCalls.push_back(std::move(call));
    if(failSparseBinds>0) { --failSparseBinds; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    if(failSparseBindAt && sparseBinds==failSparseBindAt) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL mockQueueWait(VkQueue queue) {
    ++queueWaitCalls;
    mockWaitedQueues.push_back(queue);
    const auto configured=mockQueueWaitResults.find(queue);
    if(configured!=mockQueueWaitResults.end()) return configured->second;
    return failQueueWaitAt==queueWaitCalls?failQueueWaitResult:VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL mockDeviceWait(VkDevice) { ++mockDeviceWaitCalls; return mockDeviceWaitResult; }
VKAPI_ATTR void VKAPI_CALL mockDestroyDeviceObservingDecoder(VkDevice,const VkAllocationCallbacks*) {
    ++destroyDeviceCalls;
    destroyObservedDecoderPresent=destroyObservedDevice && bool(destroyObservedDevice->gpuDecoder);
    destroyObservedDecoderUnsafe=destroyObservedDevice && destroyObservedDevice->gpuDecoder &&
        destroyObservedDevice->gpuDecoder->unsafe();
    destroyedBuffersAtNativeDeviceDestroy=destroyedMockBuffers;
    freesAtNativeDeviceDestroy=frees;
}
VKAPI_ATTR void VKAPI_CALL mockBudgetProperties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties2* out) {
    ++budgetQueries;
    out->memoryProperties.memoryHeapCount=2;
    out->memoryProperties.memoryHeaps[0].size=mockNativeHeapSize;
    out->memoryProperties.memoryHeaps[0].flags=VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    out->memoryProperties.memoryHeaps[1].size=64*MiB; // synthetic slot must not affect native budget
    auto* budget=reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(out->pNext);
    require(budget && budget->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
            "production budget query omitted the budget structure");
    auto heapBudget=mockHeapBudget, heapUsage=mockHeapUsage;
    if(!mockBudgetSequence.empty()) {
        const auto index=std::min<std::size_t>(budgetQueries-1,mockBudgetSequence.size()-1);
        heapBudget=mockBudgetSequence[index].first; heapUsage=mockBudgetSequence[index].second;
    }
    budget->heapBudget[0]=heapBudget; budget->heapUsage[0]=heapUsage;
    budget->heapBudget[1]=64*MiB; budget->heapUsage[1]=0;
    if(flipUnknownCommandOnBudgetQuery) unknownCommandProc.store(true);
    if(signalBudgetQuery.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(admissionSignalMutex);
        budgetQueryEntered=true; admissionSignal.notify_all();
    }
}
VKAPI_ATTR VkResult VKAPI_CALL mockPresent(VkQueue,const VkPresentInfoKHR* info) {
    ++presentCalls; forwardedPresent=info; return presentResult;
}
VKAPI_ATTR void VKAPI_CALL mockSetEvent(VkCommandBuffer,VkEvent,VkPipelineStageFlags) { ++trackedSyncForwards; }
VKAPI_ATTR void VKAPI_CALL mockResetEvent(VkCommandBuffer,VkEvent,VkPipelineStageFlags) { ++trackedSyncForwards; }
VKAPI_ATTR void VKAPI_CALL mockBeginLabel(VkCommandBuffer,const VkDebugUtilsLabelEXT*) { ++trackedLabelForwards; }
VKAPI_ATTR void VKAPI_CALL mockEndLabel(VkCommandBuffer) { ++trackedLabelForwards; }
VKAPI_ATTR void VKAPI_CALL mockInsertLabel(VkCommandBuffer,const VkDebugUtilsLabelEXT*) { ++trackedLabelForwards; }
VKAPI_ATTR void VKAPI_CALL mockSetLineWidth(VkCommandBuffer,float) { ++unsupportedCommandForwards; }
VKAPI_ATTR VkResult VKAPI_CALL mockCreateFence(VkDevice,const VkFenceCreateInfo*,const VkAllocationCallbacks*,VkFence* out) {
    *out=tokenHandle<VkFence>(nextHandle++); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL mockDestroyFence(VkDevice,VkFence,const VkAllocationCallbacks*) { ++destroyedMockFences; }
VKAPI_ATTR VkResult VKAPI_CALL mockFenceStatus(VkDevice,VkFence) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL mockWaitForFences(VkDevice,std::uint32_t,const VkFence*,VkBool32,std::uint64_t) {
    std::unique_lock<std::mutex> lock(mockFenceMutex);
    mockFenceWaitEntered=true; mockFenceCondition.notify_all();
    if(!mockFenceCondition.wait_for(lock,std::chrono::seconds(5),[]{return mockFenceWaitRelease;}))
        return VK_TIMEOUT;
    return mockFenceWaitResult;
}
VKAPI_ATTR VkResult VKAPI_CALL mockResetFences(VkDevice,std::uint32_t,const VkFence*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL mockCreateSemaphore(VkDevice,const VkSemaphoreCreateInfo*,const VkAllocationCallbacks*,VkSemaphore* out) {
    *out=tokenHandle<VkSemaphore>(nextHandle++); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL mockDestroySemaphore(VkDevice,VkSemaphore,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL mockSubmit(VkQueue,std::uint32_t count,const VkSubmitInfo* infos,VkFence) {
    if(count && infos && ((applicationSubmitPnext && infos[0].pNext==applicationSubmitPnext) ||
                          (applicationSubmitInfo && infos==applicationSubmitInfo))) ++submitCalls;
    return VK_SUCCESS;
}
struct QueueOperation { char kind{}; VkQueue queue{}; VkSemaphore wait{},signal{}; };
std::vector<QueueOperation> tracedQueueOperations;
VKAPI_ATTR VkResult VKAPI_CALL traceQueueSubmit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* infos,VkFence) {
    QueueOperation op{'S',queue};
    if(count && infos) {
        if(infos[0].waitSemaphoreCount) op.wait=infos[0].pWaitSemaphores[0];
        if(infos[0].signalSemaphoreCount) op.signal=infos[0].pSignalSemaphores[0];
    }
    tracedQueueOperations.push_back(op);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL traceQueueBindSparse(VkQueue queue,std::uint32_t count,const VkBindSparseInfo* infos,VkFence) {
    QueueOperation op{'B',queue};
    if(count && infos) {
        if(infos[0].waitSemaphoreCount) op.wait=infos[0].pWaitSemaphores[0];
        if(infos[0].signalSemaphoreCount) op.signal=infos[0].pSignalSemaphores[0];
    }
    tracedQueueOperations.push_back(op);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL traceQueueWaitIdle(VkQueue queue) {
    tracedQueueOperations.push_back({'I',queue});
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL traceQueueGetDeviceProcAddr(VkDevice,const char* name) {
    if(std::strcmp(name,"vkCreateFence")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockCreateFence);
    if(std::strcmp(name,"vkDestroyFence")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockDestroyFence);
    if(std::strcmp(name,"vkGetFenceStatus")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockFenceStatus);
    if(std::strcmp(name,"vkResetFences")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockResetFences);
    if(std::strcmp(name,"vkCreateSemaphore")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockCreateSemaphore);
    if(std::strcmp(name,"vkDestroySemaphore")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockDestroySemaphore);
    if(std::strcmp(name,"vkQueueSubmit")==0) return reinterpret_cast<PFN_vkVoidFunction>(traceQueueSubmit);
    if(std::strcmp(name,"vkQueueBindSparse")==0) return reinterpret_cast<PFN_vkVoidFunction>(traceQueueBindSparse);
    if(std::strcmp(name,"vkQueueWaitIdle")==0) return reinterpret_cast<PFN_vkVoidFunction>(traceQueueWaitIdle);
    return nullptr;
}
VKAPI_ATTR VkResult VKAPI_CALL mockResetCommandPool(VkDevice,VkCommandPool,VkCommandPoolResetFlags) {
    ++mockResetCommandPoolCalls; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL mockBeginCommandBuffer(VkCommandBuffer,const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL mockEndCommandBuffer(VkCommandBuffer) {
    const auto result=mockEndCommandResult; mockEndCommandResult=VK_SUCCESS; return result;
}
VKAPI_ATTR void VKAPI_CALL mockPipelineBarrier(VkCommandBuffer,VkPipelineStageFlags srcStage,
    VkPipelineStageFlags dstStage,VkDependencyFlags,std::uint32_t memoryBarrierCount,
    const VkMemoryBarrier* memoryBarriers,std::uint32_t,const VkBufferMemoryBarrier*,
    std::uint32_t,const VkImageMemoryBarrier*) {
    ++pipelineBarrierForwards;
    for(std::uint32_t i=0;i<memoryBarrierCount;i++)
        capturedMemoryBarriers.push_back({srcStage,dstStage,memoryBarriers[i].srcAccessMask,
                                         memoryBarriers[i].dstAccessMask});
}
VKAPI_ATTR void VKAPI_CALL mockWaitEvents(VkCommandBuffer,std::uint32_t,const VkEvent*,
    VkPipelineStageFlags,VkPipelineStageFlags,std::uint32_t,const VkMemoryBarrier*,
    std::uint32_t,const VkBufferMemoryBarrier*,std::uint32_t,const VkImageMemoryBarrier*) {
    ++waitEventsForwards;
}
VKAPI_ATTR void VKAPI_CALL mockPipelineBarrier2(VkCommandBuffer,const VkDependencyInfo*) {
    ++pipelineBarrier2Forwards;
}
VKAPI_ATTR void VKAPI_CALL mockWaitEvents2(VkCommandBuffer,std::uint32_t,const VkEvent*,
    const VkDependencyInfo*) { ++waitEvents2Forwards; }
VKAPI_ATTR void VKAPI_CALL mockCopyBuffer(VkCommandBuffer,VkBuffer,VkBuffer,std::uint32_t,const VkBufferCopy*) {}
#if defined(VK_VERSION_1_3) || defined(VK_KHR_copy_commands2)
VKAPI_ATTR void VKAPI_CALL mockCopyBufferToImage2(VkCommandBuffer,const VkCopyBufferToImageInfo2* info) {
    ++copyBufferToImageForwards; forwardedCopyBufferToImage=info;
}
#endif
#ifdef VK_EXT_descriptor_heap
VKAPI_ATTR void VKAPI_CALL mockBindSamplerHeapEXT(VkCommandBuffer,const VkBindHeapInfoEXT*) { ++samplerHeapBindCalls; }
#endif
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mockGetDeviceProcAddr(VkDevice,const char* name) {
    if(std::strcmp(name,"vkQueuePresentKHR")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockPresent);
    if(std::strcmp(name,"vkCmdSetEvent")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockSetEvent);
    if(std::strcmp(name,"vkCmdResetEvent")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockResetEvent);
#ifdef VK_EXT_debug_utils
    if(std::strcmp(name,"vkCmdBeginDebugUtilsLabelEXT")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockBeginLabel);
    if(std::strcmp(name,"vkCmdEndDebugUtilsLabelEXT")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockEndLabel);
    if(std::strcmp(name,"vkCmdInsertDebugUtilsLabelEXT")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockInsertLabel);
#endif
    if(std::strcmp(name,"vkCmdSetLineWidth")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockSetLineWidth);
    if(std::strcmp(name,"vkCreateFence")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockCreateFence);
    if(std::strcmp(name,"vkDestroyFence")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockDestroyFence);
    if(std::strcmp(name,"vkWaitForFences")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockWaitForFences);
    if(std::strcmp(name,"vkGetFenceStatus")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockFenceStatus);
    if(std::strcmp(name,"vkResetFences")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockResetFences);
    if(std::strcmp(name,"vkCreateSemaphore")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockCreateSemaphore);
    if(std::strcmp(name,"vkDestroySemaphore")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockDestroySemaphore);
    if(std::strcmp(name,"vkQueueSubmit")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockSubmit);
    if(std::strcmp(name,"vkResetCommandPool")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockResetCommandPool);
    if(std::strcmp(name,"vkBeginCommandBuffer")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockBeginCommandBuffer);
    if(std::strcmp(name,"vkEndCommandBuffer")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockEndCommandBuffer);
    if(std::strcmp(name,"vkCmdPipelineBarrier")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockPipelineBarrier);
    if(std::strcmp(name,"vkCmdWaitEvents")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockWaitEvents);
    if(std::strcmp(name,"vkCmdPipelineBarrier2")==0 || std::strcmp(name,"vkCmdPipelineBarrier2KHR")==0)
        return reinterpret_cast<PFN_vkVoidFunction>(mockPipelineBarrier2);
    if(std::strcmp(name,"vkCmdWaitEvents2")==0 || std::strcmp(name,"vkCmdWaitEvents2KHR")==0)
        return reinterpret_cast<PFN_vkVoidFunction>(mockWaitEvents2);
    if(std::strcmp(name,"vkCmdCopyBuffer")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockCopyBuffer);
#if defined(VK_VERSION_1_3)
    if(std::strcmp(name,"vkCmdCopyBufferToImage2")==0)
        return reinterpret_cast<PFN_vkVoidFunction>(mockCopyBufferToImage2);
#endif
#ifdef VK_KHR_copy_commands2
    if(std::strcmp(name,"vkCmdCopyBufferToImage2KHR")==0)
        return reinterpret_cast<PFN_vkVoidFunction>(mockCopyBufferToImage2);
#endif
#ifdef VK_EXT_descriptor_heap
    if(std::strcmp(name,"vkCmdBindSamplerHeapEXT")==0)
        return reinterpret_cast<PFN_vkVoidFunction>(mockBindSamplerHeapEXT);
#endif
    if(std::strcmp(name,"vkQueueBindSparse")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockSparse);
    if(std::strcmp(name,"vkQueueWaitIdle")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockQueueWait);
    return nullptr;
}

struct Fixture {
    Device device{};
    VkDevice handle{tokenHandle<VkDevice>(1)};
    std::uintptr_t dispatchWord{0xdeadbeef};
    VkDeviceMemory memory{tokenHandle<VkDeviceMemory>(2)};
    VkMemoryRequirements req{MiB,4096,1};

    explicit Fixture(VkDeviceSize bytes=4*MiB,VkDeviceSize limit=2*MiB) {
        allocations=sparseBinds=queueWaitCalls=frees=budgetQueries=presentCalls=submitCalls=samplerHeapBindCalls=0; failAllocations=failSparseBinds=0; failAllocationTypes.clear(); allocatedTypes.clear(); failQueueWaitAt=failSparseBindAt=0; failQueueWaitResult=VK_ERROR_DEVICE_LOST; mockEndCommandResult=VK_SUCCESS; mockPoolViewTypeBits=1; nextHandle=0x1000; bufferSizes.clear(); liveAllocations.clear(); freedMemoryUserData.clear(); sparseBindCalls.clear(); capturedMemoryBarriers.clear(); mockBudgetSequence.clear(); mockWaitedQueues.clear(); mockQueueWaitResults.clear(); applicationSubmitPnext=nullptr; applicationSubmitInfo=nullptr; mockDeviceWaitCalls=mockResetCommandPoolCalls=destroyedMockBuffers=destroyedMockFences=0; lastDestroyedBufferUserData=lastFreedMemoryUserData=nullptr; mockDeviceWaitResult=VK_SUCCESS;
        pipelineBarrierForwards=waitEventsForwards=pipelineBarrier2Forwards=waitEvents2Forwards=0;
        { std::lock_guard<std::mutex> fenceLock(mockFenceMutex); mockFenceWaitEntered=false; mockFenceWaitRelease=false; mockFenceWaitResult=VK_SUCCESS; }
        presentResult=VK_SUCCESS; forwardedPresent=nullptr;
        mockHeapBudget=mockHeapUsage=mockNativeHeapSize=0;
        device.handle=handle; device.autoEnabled=true; device.lazyBacking=true;
        device.rangeChunkBytes=MiB; device.residentLimitBytes=limit; device.residentAdmissionArmed=true;
        device.activeEviction=true; device.allocate=mockAllocate; device.free=mockFree;
        device.queueBindSparse=mockSparse; device.queueWaitIdle=mockQueueWait;
        device.sparseQueue=tokenHandle<VkQueue>(3);
        device.snapshot.deviceWaitIdle=mockDeviceWait;
        device.snapshot.createBuffer=mockCreateBuffer; device.snapshot.destroyBuffer=mockDestroyBuffer;
        device.snapshot.getBufferMemoryRequirements=mockGetBufferMemoryRequirements;
        device.snapshot.resetCommandPool=mockResetCommandPool; device.snapshot.beginCommandBuffer=mockBeginCommandBuffer;
        device.snapshot.endCommandBuffer=mockEndCommandBuffer; device.snapshot.cmdPipelineBarrier=mockPipelineBarrier;
        device.snapshot.cmdCopyBuffer=mockCopyBuffer; device.snapshot.queueSubmit=mockSubmit;
        device.snapshot.commandPool=tokenHandle<VkCommandPool>(10); device.snapshot.commandBuffer=tokenHandle<VkCommandBuffer>(11);
        device.memory.memoryTypeCount=1; device.memory.memoryHeapCount=1;
        device.memory.memoryTypes[0].heapIndex=0;
        device.memory.memoryHeaps[0].flags=VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        VirtualMemory memoryState{}; memoryState.size=bytes; memoryState.nativeTypeBits=1;
        device.virtualMemory.emplace(memory,memoryState);
    }

    VkBuffer bind(VkDeviceSize offset,VkDeviceSize size) {
        auto buffer=tokenHandle<VkBuffer>(nextHandle++);
        auto requirements=req; requirements.size=size;
        require(bindPoolBuffer(handle,device,buffer,memory,offset,requirements)==VK_SUCCESS,
                "production bindPoolBuffer failed");
        return buffer;
    }
    VirtualMemory& state() { return device.virtualMemory.at(memory); }
};

#ifdef VK_KHR_maintenance6
unsigned maintenance6BindCalls{};
VkBuffer maintenance6FailBuffer{};
std::vector<VkBuffer> maintenance6BoundBuffers;
VKAPI_ATTR VkResult VKAPI_CALL mockMaintenance6BindBufferMemory2(VkDevice,std::uint32_t count,const VkBindBufferMemoryInfo* infos) {
    ++maintenance6BindCalls;
    for(std::uint32_t i=0;i<count;i++) {
        maintenance6BoundBuffers.push_back(infos[i].buffer);
        for(auto* p=static_cast<const VkBaseInStructure*>(infos[i].pNext);p;p=p->pNext)
#ifdef VK_KHR_maintenance6
            if(p->sType==VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR)
                *reinterpret_cast<const VkBindMemoryStatusKHR*>(p)->pResult=
                    infos[i].buffer==maintenance6FailBuffer?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;
#endif
    }
    return infos[0].buffer==maintenance6FailBuffer?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;
}
#endif

void checkMaintenance6BindStatus() {
#ifdef VK_KHR_maintenance6
    VkPhysicalDeviceMaintenance6FeaturesKHR features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
    VkDeviceCreateInfo deviceCreate{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; deviceCreate.pNext=&features;
    features.maintenance6=VK_TRUE;
    require(maintenance6Requested(&deviceCreate),"KHR maintenance6 enabled feature was not detected");
    features.maintenance6=VK_FALSE;
    require(!maintenance6Requested(&deviceCreate),"disabled KHR maintenance6 feature was detected");
#endif
#ifdef VK_VERSION_1_4
    VkPhysicalDeviceVulkan14Features coreFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkDeviceCreateInfo coreCreate{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; coreCreate.pNext=&coreFeatures;
    coreFeatures.maintenance6=VK_TRUE;
    require(maintenance6Requested(&coreCreate),"core 1.4 maintenance6 enabled feature was not detected");
#endif
#ifdef VK_KHR_maintenance6
    {
        Fixture f(2*MiB,2*MiB); f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord);
        auto& d=f.device; d.handle=f.handle; d.maintenance6Enabled=true; d.virtualBytes=2*MiB;
        const auto prior=f.bind(0,MiB);
        (void)prior;
        const auto buffer=tokenHandle<VkBuffer>(0x6b01);
        PromotedBuffer promoted{}; promoted.size=MiB; promoted.requirements={MiB,4096,1};
        d.promotedBuffers.emplace(buffer,promoted);
        { std::lock_guard<std::mutex> lock(mapsMutex); devices[key(d.handle)]=std::shared_ptr<Device>(&d,[](Device*){}); }
        VkResult statusResult=VK_ERROR_UNKNOWN;
        VkBindMemoryStatusKHR status{VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR}; status.pResult=&statusResult;
        VkBindBufferMemoryInfo info{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};
        info.pNext=&status; info.buffer=buffer; info.memory=f.memory; info.memoryOffset=MiB;
        require(layerBindBufferMemory2(d.handle,1,&info)==VK_SUCCESS && statusResult==VK_SUCCESS,
                "promoted buffer bind did not report maintenance6 status success");

        VkBaseInStructure unsupported{}; unsupported.sType=VK_STRUCTURE_TYPE_MAX_ENUM;
        unsupported.pNext=reinterpret_cast<const VkBaseInStructure*>(&status);
        const auto rejected=tokenHandle<VkBuffer>(0x6b02);
        PromotedBuffer rejectedPromoted{}; rejectedPromoted.size=MiB; rejectedPromoted.requirements={MiB,4096,1};
        d.promotedBuffers.emplace(rejected,rejectedPromoted);
        info.buffer=rejected; info.pNext=&unsupported; statusResult=VK_ERROR_UNKNOWN;
        const auto sparseBefore=sparseBinds;
        require(layerBindBufferMemory2(d.handle,1,&info)==VK_ERROR_FEATURE_NOT_PRESENT &&
                statusResult==VK_ERROR_FEATURE_NOT_PRESENT && sparseBinds==sparseBefore,
                "promoted bind bypassed unsupported pNext rejection or omitted per-bind error status");
        { std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(d.handle)); }
    }
    {
        Fixture f; f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord);
        auto& d=f.device; d.handle=f.handle; d.maintenance6Enabled=true;
        d.bindBufferMemory2=mockMaintenance6BindBufferMemory2;
        { std::lock_guard<std::mutex> lock(mapsMutex); devices[key(d.handle)]=std::shared_ptr<Device>(&d,[](Device*){}); }
        const auto first=tokenHandle<VkBuffer>(0x6b10),local=tokenHandle<VkBuffer>(0x6b11),last=tokenHandle<VkBuffer>(0x6b12);
        VkResult firstStatus=VK_ERROR_UNKNOWN,localStatus=VK_ERROR_UNKNOWN,lastStatus=VK_ERROR_UNKNOWN;
        VkBindMemoryStatusKHR firstInfoStatus{VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR}; firstInfoStatus.pResult=&firstStatus;
        VkBindMemoryStatusKHR localInfoStatus{VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR}; localInfoStatus.pResult=&localStatus;
        VkBindMemoryStatusKHR lastInfoStatus{VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR}; lastInfoStatus.pResult=&lastStatus;
        VkBindBufferMemoryInfo infos[3]{};
        infos[0]={VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,&firstInfoStatus,first,tokenHandle<VkDeviceMemory>(0x6b20),0};
        infos[1]={VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,&localInfoStatus,local,f.memory,0};
        infos[2]={VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,&lastInfoStatus,last,tokenHandle<VkDeviceMemory>(0x6b21),0};
        maintenance6BindCalls=0; maintenance6FailBuffer=first; maintenance6BoundBuffers.clear();
        const auto batch=layerBindBufferMemory2(d.handle,3,infos);
        require(batch==VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                maintenance6BindCalls==2 && maintenance6BoundBuffers==std::vector<VkBuffer>{first,last} &&
                firstStatus==VK_ERROR_OUT_OF_DEVICE_MEMORY && localStatus==VK_ERROR_FEATURE_NOT_PRESENT &&
                lastStatus==VK_SUCCESS,
                "maintenance6 buffer batch did not attempt later binds or report each result");

        d.maintenance6Enabled=false;
        firstStatus=lastStatus=VK_ERROR_UNKNOWN; maintenance6BindCalls=0; maintenance6BoundBuffers.clear();
        require(layerBindBufferMemory2(d.handle,2,infos)==VK_ERROR_OUT_OF_DEVICE_MEMORY && maintenance6BindCalls==1 &&
                firstStatus==VK_ERROR_OUT_OF_DEVICE_MEMORY && lastStatus==VK_ERROR_UNKNOWN,
                "disabled maintenance6 did not preserve early-exit batch behavior");
        { std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(d.handle)); }
    }
#endif
}

bool maintenance4CoreAvailable=true,maintenance4KhrAvailable=true;
unsigned maintenance4CoreCalls{},maintenance4KhrCalls{};
VkBufferCreateInfo maintenance4QueriedCreateInfo{};
std::vector<std::uint32_t> maintenance4QueriedFamilies;
const void* maintenance4OuterPnext{};
VkBufferCreateInfo maintenance4CreatedInfo{};
std::vector<std::uint32_t> maintenance4CreatedFamilies;
std::unordered_set<std::string> availableProcNames;
VKAPI_ATTR void VKAPI_CALL mockAvailableCommand() {}
PFN_vkVoidFunction mockProcAddress(const char* name) {
    return name && availableProcNames.count(name)
        ?reinterpret_cast<PFN_vkVoidFunction>(mockAvailableCommand):nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mockAvailabilityGIPA(VkInstance,const char* name) {
    if(name && (std::strcmp(name,"vkEnumerateInstanceVersion")==0 ||
                std::strcmp(name,"vkEnumerateInstanceExtensionProperties")==0 ||
                std::strcmp(name,"vkEnumerateInstanceLayerProperties")==0))
        return reinterpret_cast<PFN_vkVoidFunction>(mockAvailableCommand);
    return mockProcAddress(name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mockAvailabilityPhysicalProc(VkInstance,const char* name) {
    return mockProcAddress(name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mockAvailabilityGDPA(VkDevice,const char* name) {
    return mockProcAddress(name);
}
void checkProcAddressAvailability() {
    availableProcNames.clear();
    std::uintptr_t instanceWord=0x5151;
    const auto instance=reinterpret_cast<VkInstance>(&instanceWord);
    auto state=std::make_shared<Instance>(); state->handle=instance;
    state->gipa=mockAvailabilityGIPA; state->physProc=mockAvailabilityPhysicalProc;
    PFN_vkGetInstanceProcAddr previousGlobal{};
    { std::lock_guard<std::mutex> lock(mapsMutex); instances[key(instance)]=state; previousGlobal=globalGipa; globalGipa=mockAvailabilityGIPA; }
    require(layerGetInstanceProcAddr(VK_NULL_HANDLE,"vkCreateInstance")!=nullptr &&
            layerGetInstanceProcAddr(VK_NULL_HANDLE,"vkGetInstanceProcAddr")!=nullptr &&
            layerGetInstanceProcAddr(VK_NULL_HANDLE,"vkEnumerateInstanceVersion")!=nullptr,
            "null-instance GIPA lost loader bootstrap globals");
    require(layerGetInstanceProcAddr(VK_NULL_HANDLE,"vkCreateDevice")==nullptr &&
            layerGetInstanceProcAddr(VK_NULL_HANDLE,"vkGetPhysicalDeviceMemoryProperties2")==nullptr &&
            layerGetInstanceProcAddr(VK_NULL_HANDLE,"vkGetPhysicalDeviceMemoryProperties2KHR")==nullptr,
            "null-instance GIPA exposed non-global commands");
    require(layerGetInstanceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties2")==nullptr &&
            layerGetPhysicalDeviceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties2KHR")==nullptr,
            "unavailable properties2 aliases were exposed");
    availableProcNames.insert("vkGetPhysicalDeviceMemoryProperties2");
    require(layerGetInstanceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties2")!=nullptr &&
            layerGetInstanceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties2KHR")==nullptr &&
            layerGetPhysicalDeviceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties2")==nullptr &&
            layerGetPhysicalDeviceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties2KHR")==nullptr,
            "GIPA properties2 core availability leaked to PFP or its unavailable KHR alias");
    availableProcNames.clear(); availableProcNames.insert("vkGetPhysicalDeviceMemoryProperties2KHR");
    require(layerGetInstanceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties2")==nullptr &&
            layerGetInstanceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties2KHR")!=nullptr &&
            layerGetPhysicalDeviceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties2")==nullptr &&
            layerGetPhysicalDeviceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties2KHR")!=nullptr,
            "properties2 KHR availability was not kept exact across GIPA and PFP");
    availableProcNames.clear();
    require(layerGetInstanceProcAddr(instance,"vkBindBufferMemory2")==nullptr &&
            layerGetInstanceProcAddr(instance,"vkBindBufferMemory2KHR")==nullptr &&
            layerGetInstanceProcAddr(instance,"vkCmdPipelineBarrier2KHR")==nullptr,
            "GIPA exposed unavailable intercepted device commands");
    availableProcNames.insert("vkBindBufferMemory2");
    availableProcNames.insert("vkCmdPipelineBarrier2");
    require(layerGetInstanceProcAddr(instance,"vkBindBufferMemory2")!=nullptr &&
            layerGetInstanceProcAddr(instance,"vkBindBufferMemory2KHR")==nullptr &&
            layerGetInstanceProcAddr(instance,"vkCmdPipelineBarrier2")!=nullptr &&
            layerGetInstanceProcAddr(instance,"vkCmdPipelineBarrier2KHR")==nullptr,
            "GIPA device alias availability was not checked by exact name");
    require(layerGetPhysicalDeviceProcAddr(instance,"vkBindBufferMemory2")==nullptr &&
            layerGetPhysicalDeviceProcAddr(instance,"vkZVramGetSnapshotStatsNX")==nullptr,
            "physical-device proc lookup exposed non-physical commands");

    Fixture f; f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord);
    auto& d=f.device; d.handle=f.handle; d.gdpa=mockAvailabilityGDPA;
    d.autoInitialized=true; d.selectiveRestore=true;
    { std::lock_guard<std::mutex> lock(mapsMutex); devices[key(d.handle)]=std::shared_ptr<Device>(&d,[](Device*){}); }
    availableProcNames.clear();
    require(layerGetDeviceProcAddr(d.handle,"vkBindBufferMemory2")==nullptr &&
            layerGetDeviceProcAddr(d.handle,"vkBindBufferMemory2KHR")==nullptr &&
            layerGetDeviceProcAddr(d.handle,"vkGetBufferMemoryRequirements2KHR")==nullptr &&
            layerGetDeviceProcAddr(d.handle,"vkCmdPipelineBarrier2KHR")==nullptr,
            "unavailable device aliases were exposed");
    availableProcNames.insert("vkBindBufferMemory2");
    availableProcNames.insert("vkGetBufferMemoryRequirements2KHR");
    availableProcNames.insert("vkCmdPipelineBarrier2");
    require(layerGetDeviceProcAddr(d.handle,"vkBindBufferMemory2")==
                reinterpret_cast<PFN_vkVoidFunction>(layerBindBufferMemory2) &&
            layerGetDeviceProcAddr(d.handle,"vkBindBufferMemory2KHR")==nullptr &&
            layerGetDeviceProcAddr(d.handle,"vkGetBufferMemoryRequirements2")==nullptr &&
            layerGetDeviceProcAddr(d.handle,"vkGetBufferMemoryRequirements2KHR")==
                reinterpret_cast<PFN_vkVoidFunction>(layerGetBufferMemoryRequirements2) &&
            layerGetDeviceProcAddr(d.handle,"vkCmdPipelineBarrier2")==
                reinterpret_cast<PFN_vkVoidFunction>(layerCmdPipelineBarrier2) &&
            layerGetDeviceProcAddr(d.handle,"vkCmdPipelineBarrier2KHR")==nullptr,
            "device alias availability was not checked by exact name");
    availableProcNames.insert("vkQueueSubmit2KHR");
    require(layerGetDeviceProcAddr(d.handle,"vkQueueSubmit2KHR")==
                reinterpret_cast<PFN_vkVoidFunction>(layerQueueSubmit2KHR) &&
            layerGetDeviceProcAddr(d.handle,"vkQueueSubmit2")==nullptr,
            "submit2 exact-name availability regressed");
    require(layerGetDeviceProcAddr(d.handle,"vkZVramGetSnapshotStatsNX")!=nullptr,
            "layer-owned device command was hidden by Vulkan availability gates");
    { std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(d.handle)); instances.erase(key(instance)); globalGipa=previousGlobal; }
    availableProcNames.clear();
}

VKAPI_ATTR void VKAPI_CALL mockMaintenance4Query(VkDevice,const VkDeviceBufferMemoryRequirements* info,VkMemoryRequirements2* out) {
    maintenance4QueriedCreateInfo=*info->pCreateInfo;
    maintenance4QueriedFamilies.clear();
    if(info->pCreateInfo->queueFamilyIndexCount) maintenance4QueriedFamilies.assign(info->pCreateInfo->pQueueFamilyIndices,
        info->pCreateInfo->pQueueFamilyIndices+info->pCreateInfo->queueFamilyIndexCount);
    maintenance4OuterPnext=info->pNext;
    out->memoryRequirements={8*MiB,8192,0x5};
    auto* dedicated=reinterpret_cast<VkMemoryDedicatedRequirements*>(out->pNext);
    if(dedicated) { dedicated->prefersDedicatedAllocation=VK_TRUE; dedicated->requiresDedicatedAllocation=VK_FALSE; }
}
VKAPI_ATTR void VKAPI_CALL mockMaintenance4QueryCore(VkDevice d,const VkDeviceBufferMemoryRequirements* info,VkMemoryRequirements2* out) {
    ++maintenance4CoreCalls; mockMaintenance4Query(d,info,out);
}
VKAPI_ATTR void VKAPI_CALL mockMaintenance4QueryKhr(VkDevice d,const VkDeviceBufferMemoryRequirements* info,VkMemoryRequirements2* out) {
    ++maintenance4KhrCalls; mockMaintenance4Query(d,info,out);
}
VKAPI_ATTR VkResult VKAPI_CALL mockMaintenance4Create(VkDevice,const VkBufferCreateInfo* info,const VkAllocationCallbacks*,VkBuffer* out) {
    maintenance4CreatedInfo=*info;
    maintenance4CreatedFamilies.assign(info->pQueueFamilyIndices,info->pQueueFamilyIndices+info->queueFamilyIndexCount);
    *out=tokenHandle<VkBuffer>(nextHandle++); return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mockMaintenance4GDPA(VkDevice,const char* name) {
    if(std::strcmp(name,"vkGetDeviceBufferMemoryRequirements")==0 && maintenance4CoreAvailable)
        return reinterpret_cast<PFN_vkVoidFunction>(mockMaintenance4QueryCore);
    if(std::strcmp(name,"vkGetDeviceBufferMemoryRequirementsKHR")==0 && maintenance4KhrAvailable)
        return reinterpret_cast<PFN_vkVoidFunction>(mockMaintenance4QueryKhr);
    return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mockMaintenance4GIPA(VkInstance,const char* name) {
    return mockMaintenance4GDPA(VK_NULL_HANDLE,name);
}
void checkMaintenance4BufferRequirements() {
    Fixture f; f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord);
    auto& d=f.device; d.handle=f.handle; d.virtualEnabled=true; d.virtualType=3; d.gdpa=mockMaintenance4GDPA;
    d.getDeviceBufferMemoryRequirements=mockMaintenance4QueryCore;
    d.getDeviceBufferMemoryRequirementsKHR=mockMaintenance4QueryKhr;
    d.createBuffer=mockMaintenance4Create; d.queueFamilies={2,5};
    maintenance4CoreAvailable=maintenance4KhrAvailable=true;
    maintenance4CoreCalls=maintenance4KhrCalls=0;
    {
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices[key(d.handle)]=std::shared_ptr<Device>(&d,[](Device*){});
    }
    VkBufferCreateInfo create{}; create.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create.size=MiB; create.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer actual{};
    require(layerCreateBuffer(d.handle,&create,nullptr,&actual)==VK_SUCCESS,"maintenance4 comparison buffer create failed");

    VkBaseOutStructure inputPnext{}; inputPnext.sType=VK_STRUCTURE_TYPE_MAX_ENUM;
    VkDeviceBufferMemoryRequirements info{}; info.sType=VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS;
    info.pNext=&inputPnext; info.pCreateInfo=&create;
    VkMemoryDedicatedRequirements dedicated{}; dedicated.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 output{}; output.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2; output.pNext=&dedicated;
    auto core=reinterpret_cast<PFN_vkGetDeviceBufferMemoryRequirements>(layerGetDeviceProcAddr(d.handle,"vkGetDeviceBufferMemoryRequirements"));
    require(core!=nullptr,"core maintenance4 entrypoint was not wrapped");
    core(d.handle,&info,&output);
    require(maintenance4CoreCalls==1 && maintenance4QueriedCreateInfo.flags==maintenance4CreatedInfo.flags &&
            maintenance4QueriedCreateInfo.usage==maintenance4CreatedInfo.usage &&
            maintenance4QueriedCreateInfo.sharingMode==maintenance4CreatedInfo.sharingMode &&
            maintenance4QueriedFamilies==maintenance4CreatedFamilies && maintenance4OuterPnext==&inputPnext &&
            output.memoryRequirements.memoryTypeBits==(0x5u|(1u<<3)) && output.memoryRequirements.size==8*MiB &&
            output.memoryRequirements.alignment==8192 && dedicated.prefersDedicatedAllocation==VK_TRUE,
            "core maintenance4 query diverged from actual transformed buffer or damaged outputs");

    output={}; output.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2; output.pNext=&dedicated;
    auto khr=reinterpret_cast<PFN_vkGetDeviceBufferMemoryRequirementsKHR>(layerGetDeviceProcAddr(d.handle,"vkGetDeviceBufferMemoryRequirementsKHR"));
    require(khr!=nullptr,"KHR maintenance4 entrypoint was not wrapped");
    khr(d.handle,&info,&output);
    require(maintenance4KhrCalls==1 && maintenance4QueriedCreateInfo.flags==maintenance4CreatedInfo.flags &&
            maintenance4QueriedCreateInfo.usage==maintenance4CreatedInfo.usage &&
            maintenance4QueriedFamilies==maintenance4CreatedFamilies &&
            output.memoryRequirements.memoryTypeBits==(0x5u|(1u<<3)),
            "KHR maintenance4 query diverged from actual transformed buffer");

    VkBufferCreateInfo ineligible=create; ineligible.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.pCreateInfo=&ineligible; output={}; output.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    core(d.handle,&info,&output);
    require(maintenance4QueriedCreateInfo.usage==ineligible.usage && output.memoryRequirements.memoryTypeBits==0x5u,
            "ineligible maintenance4 query did not pass through unchanged");

    maintenance4CoreAvailable=false; maintenance4KhrAvailable=true;
    require(layerGetDeviceProcAddr(d.handle,"vkGetDeviceBufferMemoryRequirements")==nullptr &&
            layerGetDeviceProcAddr(d.handle,"vkGetDeviceBufferMemoryRequirementsKHR")!=nullptr,
            "maintenance4 GDPA exposed unsupported alias");
    std::uintptr_t instanceDispatchWord=0x3141;
    VkInstance instance=reinterpret_cast<VkInstance>(&instanceDispatchWord); auto instanceState=std::make_shared<Instance>();
    instanceState->handle=instance; instanceState->gipa=mockMaintenance4GIPA;
    { std::lock_guard<std::mutex> lock(mapsMutex); instances[key(instance)]=instanceState; }
    require(layerGetInstanceProcAddr(instance,"vkGetDeviceBufferMemoryRequirements")==nullptr &&
            layerGetInstanceProcAddr(instance,"vkGetDeviceBufferMemoryRequirementsKHR")!=nullptr,
            "maintenance4 GIPA exposed unsupported alias");
    { std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(d.handle)); instances.erase(key(instance)); }
    maintenance4CoreAvailable=maintenance4KhrAvailable=true;
}

void configureNativeTypeFallback(Fixture& f) {
    auto& d=f.device; auto& m=f.state();
    d.memory.memoryTypeCount=3; d.memory.memoryHeapCount=3;
    for(std::uint32_t i=0;i<3;i++) {
        d.memory.memoryTypes[i].heapIndex=i;
        d.memory.memoryHeaps[i].flags=i<2?VK_MEMORY_HEAP_DEVICE_LOCAL_BIT:0;
        d.memory.memoryHeaps[i].size=8*MiB;
    }
    m.nativeTypeBits=1u<<1; // Original native type: local type 1.
    f.req.memoryTypeBits=7;
    mockPoolViewTypeBits=7;
}

void checkNativeBackingFallbackPolicy() {
    {
        Fixture f(MiB,2*MiB); configureNativeTypeFallback(f);
        f.device.lazyBacking=false;
        failAllocationTypes.insert(1);
        const auto r=bindPoolBuffer(f.handle,f.device,tokenHandle<VkBuffer>(0xb001),f.memory,0,f.req);
        require(r==VK_ERROR_OUT_OF_DEVICE_MEMORY && allocatedTypes==std::vector<std::uint32_t>{1} &&
                f.state().nativeTypeBits==(1u<<1),
                "default native backing escaped the application's original type");
        failAllocationTypes.clear();
    }
    {
        Fixture f(MiB,2*MiB); configureNativeTypeFallback(f);
        f.device.lazyBacking=false; f.device.coldCycleRecovery=true;
        failAllocationTypes.insert(0); failAllocationTypes.insert(1);
        require(bindPoolBuffer(f.handle,f.device,tokenHandle<VkBuffer>(0xb002),f.memory,0,f.req)==VK_SUCCESS,
                "opt-in native backing did not fall back to a compatible nonlocal type");
        const auto& m=f.state();
        require(allocatedTypes==std::vector<std::uint32_t>({1,0,2}) &&
                m.children[0] && m.childTypes[0]==2 && m.nativeTypeBits==(1u<<1) &&
                m.backingMemoryTypeBits==7,
                "opt-in fallback did not prefer original type or preserve candidate provenance");
        failAllocationTypes.clear();
    }
    {
        Fixture f(2*MiB,3*MiB); configureNativeTypeFallback(f);
        f.device.coldCycleRecovery=true;
        auto& m=f.state();
        f.bind(0,MiB);
        f.req.memoryTypeBits=6; // Preserve the original type and permit nonlocal type 2.
        f.bind(MiB,MiB);
        require(m.backingMemoryTypeBits==6 && m.coldGroups[0].cold && m.coldGroups[1].cold,
                "native alias did not monotonically narrow the cold backing mask");
        failAllocationTypes.insert(1);
        require(restoreColdLocked(f.handle,f.device,f.memory,0)==VK_SUCCESS &&
                m.children[0] && m.childTypes[0]==2 && m.backingMemoryTypeBits==6 &&
                allocatedTypes==std::vector<std::uint32_t>({1,2}),
                "cold restore escaped the narrowed multi-alias compatible type set");
        failAllocationTypes.clear();
    }
    {
        Fixture f(2*MiB,3*MiB); configureNativeTypeFallback(f);
        f.device.coldCycleRecovery=true;
        auto& m=f.state(); f.bind(0,MiB);
        f.req.memoryTypeBits=5; // Omits the application's original native type 1.
        const auto before=allocatedTypes.size();
        const auto result=bindPoolBuffer(f.handle,f.device,tokenHandle<VkBuffer>(0xb004),
                                         f.memory,MiB,f.req);
        require(result==VK_ERROR_FEATURE_NOT_PRESENT && allocatedTypes.size()==before &&
                m.bindings.size()==1 && m.backingMemoryTypeBits==7 &&
                m.coldGroups[0].cold && m.coldGroups[1].cold && f.device.residentBytes==0,
                "native alias incompatible with the original app type mutated backing state");
    }
    {
        Fixture f(MiB,2*MiB); configureNativeTypeFallback(f);
        f.device.lazyBacking=false; f.device.coldCycleRecovery=true;
        f.device.memory.memoryTypes[2].propertyFlags=VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
        failAllocationTypes.insert(0); failAllocationTypes.insert(1);
        require(bindPoolBuffer(f.handle,f.device,tokenHandle<VkBuffer>(0xb003),f.memory,0,f.req)==
                    VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                allocatedTypes==std::vector<std::uint32_t>({1,0}),
                "opt-in native fallback admitted a lazily allocated type");
        failAllocationTypes.clear();
    }
}

void checkBootstrapAdmissionAndRestore() {
    Fixture f;
    const auto first=f.bind(0,MiB);
    auto& m=f.state();
    require(allocations==0 && f.device.residentBytes==0,"bootstrap allocated resident backing");
    require(m.children.size()==4 && m.coldGroups.size()==4 && m.bindings.size()==1,
            "bootstrap did not create four cold range groups");
    require(m.cold && f.device.coldLogicalBytes==4*MiB,"bootstrap cold accounting mismatch");
    requireColdLogicalMatchesState(f.device,"lazy bootstrap cold counter diverged from memory state");
    require(f.device.coldBytes==0 && f.device.cacheBytes==0 && m.coldStoredBytes==0 &&
            m.cacheStoredBytes==0 && f.device.freezeCount==0,
            "pristine bootstrap incorrectly counted stored or frozen bytes");
    for(const auto& group:m.coldGroups)
        require(group.cold && group.pristine && group.logicalBytes==MiB && group.storedBytes==0 && group.chunks.empty(),
                "bootstrap group is not pristine cold");
    f.bind(MiB,MiB); // Adding a second alias while cold must not force restore.
    require(m.bindings.size()==2 && allocations==0 && sparseBinds==0,
            "adding an alias restored or allocated cold backing");
    require(restoreColdLocked(f.handle,f.device,f.memory,0)==VK_SUCCESS,
            "single pristine group restore failed");
    require(allocations==1 && f.device.residentBytes==MiB && m.residentBytes==MiB,
            "single restore resident accounting mismatch");
    require(!m.coldGroups[0].cold && !m.coldGroups[0].pristine && m.coldGroups[1].cold,
            "single restore changed the wrong groups");
    require(sparseBinds==1 && m.bound,"restore did not bind only the app alias over the resident child");
    require(f.device.coldBytes==0 && f.device.cacheBytes==0 && m.coldStoredBytes==0 &&
            m.cacheStoredBytes==0 && f.device.freezeCount==0,
            "pristine materialization changed stored/cache/freeze counters");
    requireColdLogicalMatchesState(f.device,"partial restore cold counter diverged from memory state");
    const auto allocsBefore=allocations;
    require(restoreColdLocked(f.handle,f.device,f.memory)==VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "over-capacity restore-all was not refused");
    require(allocations==allocsBefore && f.device.residentBytes==MiB,
            "restore preflight allocated before refusing over-capacity restore");
    (void)first;
}

void checkExactCapRestore() {
    Fixture f(3*MiB,3*MiB); f.bind(0,3*MiB); auto& m=f.state();
    require(allocations==0 && f.device.residentBytes==0,"exact-cap bootstrap allocated eagerly");
    require(restoreColdLocked(f.handle,f.device,f.memory)==VK_SUCCESS,
            "restore at exact resident cap was refused");
    require(allocations==3 && f.device.residentBytes==3*MiB && m.residentBytes==3*MiB &&
            !m.cold && f.device.coldLogicalBytes==0,"exact-cap restore accounting mismatch");
    requireColdLogicalMatchesState(f.device,"full restore cold counter diverged from memory state");
    require(f.device.coldBytes==0 && f.device.cacheBytes==0 && m.coldStoredBytes==0 &&
            m.cacheStoredBytes==0 && f.device.freezeCount==0,
            "pristine exact-cap restore counted stored or frozen bytes");
}

void checkOneByteBelowCap() {
    Fixture f(2*MiB,2*MiB-1); f.bind(0,2*MiB); auto& m=f.state();
    require(restoreColdLocked(f.handle,f.device,f.memory)==VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "restore one byte over resident cap was accepted");
    require(allocations==0 && liveAllocations.empty() && f.device.residentBytes==0 &&
            m.cold && f.device.coldLogicalBytes==2*MiB,
            "one-byte-over-cap preflight allocated or changed accounting");
    requireColdLogicalMatchesState(f.device,"restore refusal changed cold counter/state correspondence");
}

void checkBootstrapAlignmentRollback() {
    Fixture f;
    const auto buffer=tokenHandle<VkBuffer>(nextHandle++);
    auto req=f.req; req.size=MiB+1; req.alignment=4096;
    require(bindPoolBuffer(f.handle,f.device,buffer,f.memory,0,req)==VK_ERROR_FEATURE_NOT_PRESENT,
            "misaligned partial child range was accepted");
    const auto& m=f.state();
    require(!m.everBound && m.children.empty() && m.childSizes.empty() && m.childTypes.empty() &&
            m.coldGroups.empty() && m.poolViews.empty() && m.bindings.empty() && bufferSizes.empty(),
            "failed bootstrap binding did not roll back child/view metadata");
    require(allocations==0 && liveAllocations.empty() && frees==0 && f.device.residentBytes==0 &&
            f.device.coldLogicalBytes==0 && m.coldLogicalSize==0 && f.device.coldBytes==0 &&
            f.device.cacheBytes==0 && f.device.freezeCount==0,
            "failed bootstrap binding did not roll back cold/resident counters");
}

void checkMergedRestoreMapping() {
    auto prepare=[](Fixture& f) {
        const auto half=MiB/2;
        f.bind(0,half);
        f.bind(half,half);
        auto& m=f.state(); auto& group=m.coldGroups[0];
        const auto child=tokenHandle<VkDeviceMemory>(0xface);
        m.children[0]=child; m.childTypes[0]=0; m.residentBytes=MiB; f.device.residentBytes=MiB;
        group.pristine=false; group.restoreBound=true;
        VirtualMemory::ColdChunk chunk; chunk.bytes.assign(MiB,0x5a); chunk.rawSize=MiB; chunk.compressed=false;
        group.chunks.push_back(std::move(chunk)); group.storedBytes=MiB;
        m.coldStoredBytes=MiB; f.device.coldBytes=MiB;
        f.device.residentAdmissionArmed=false;
        auto staging=std::make_unique<std::vector<std::uint8_t>>(MiB);
        f.device.snapshot.mapped=staging->data(); f.device.snapshot.stagingSize=MiB;
        f.device.snapshot.chunkSize=MiB; f.device.snapshot.stagingBuffer=tokenHandle<VkBuffer>(0xbabe);
        f.device.snapshot.commandPool=tokenHandle<VkCommandPool>(0xbeef);
        f.device.snapshot.commandBuffer=tokenHandle<VkCommandBuffer>(0xcafe);
        f.device.copyQueue=tokenHandle<VkQueue>(0xabc);
        return staging;
    };

    {
        Fixture f; auto staging=prepare(f); auto& m=f.state();
        const auto result=restoreColdLocked(f.handle,f.device,f.memory,0);
        require(result==VK_SUCCESS,"merged private-view/app restore bind failed");
        require(sparseBinds==1 && queueWaitCalls==2 && sparseBindCalls.size()==1,
                "final mapping did not use one sparse call and one bind completion wait");
        const auto& binds=sparseBindCalls[0];
        require(binds.size()==3,"merged bind must contain view unbind and two app aliases");
        require(binds[0].buffer==m.poolViews[0] && binds[0].binds.size()==1 &&
                binds[0].binds[0].memory==VK_NULL_HANDLE && binds[0].binds[0].resourceOffset==0 &&
                binds[0].binds[0].size==MiB,"private view release bind was malformed or not first");
        require(binds[1].buffer==m.bindings[0].buffer && binds[2].buffer==m.bindings[1].buffer,
                "merged app alias bind order did not follow the recorded aliases");
        require(binds[1].binds.size()==1 && binds[1].binds[0].memory==m.children[0] &&
                binds[1].binds[0].resourceOffset==0 && binds[1].binds[0].memoryOffset==0 &&
                binds[1].binds[0].size==MiB/2,"first app alias mapping was incorrect");
        require(binds[2].binds.size()==1 && binds[2].binds[0].memory==m.children[0] &&
                binds[2].binds[0].resourceOffset==0 && binds[2].binds[0].memoryOffset==MiB/2 &&
                binds[2].binds[0].size==MiB/2,"second app alias mapping was incorrect");
        require(!m.coldGroups[0].cold && !m.coldGroups[0].restoreBound &&
                m.coldGroups[0].logicalBytes==0 && m.coldGroups[0].chunks.empty() &&
                f.device.coldLogicalBytes==3*MiB && f.device.coldBytes==0,
                "successful merged restore did not retire the cold snapshot");
        f.device.snapshot.mapped=nullptr;
    }
    {
        Fixture f; auto staging=prepare(f); auto& m=f.state(); auto& group=m.coldGroups[0];
        const auto logicalBefore=f.device.coldLogicalBytes;
        failSparseBinds=1;
        const auto result=restoreColdLocked(f.handle,f.device,f.memory,0);
        require(result==VK_ERROR_OUT_OF_DEVICE_MEMORY,"injected merged-bind failure was not returned");
        require(sparseBinds==1 && queueWaitCalls==1 && sparseBindCalls.size()==1 &&
                sparseBindCalls[0].size()==3,"failed transition did not issue exactly one merged sparse call");
        require(f.device.gpuGateError==VK_ERROR_OUT_OF_DEVICE_MEMORY && group.cold && group.restoreBound &&
                group.logicalBytes==MiB && group.storedBytes==MiB && group.chunks.size()==1,
                "failed merged transition did not preserve cold state behind the sticky gate");
        require(m.children[0]==tokenHandle<VkDeviceMemory>(0xface) && m.residentBytes==MiB &&
                f.device.residentBytes==MiB && f.device.coldLogicalBytes==logicalBefore &&
                f.device.coldBytes==MiB && frees==0,
                "failed merged transition changed backing/cold accounting or freed memory");
        f.device.snapshot.mapped=nullptr;
    }
    {
        Fixture f; auto staging=prepare(f); auto& m=f.state(); auto& group=m.coldGroups[0];
        failQueueWaitAt=2; // Copy completed; final mapping was accepted but its wait fails.
        require(restoreColdLocked(f.handle,f.device,f.memory,0)==VK_ERROR_DEVICE_LOST,
                "merged completion failure was not returned");
        require(sparseBinds==1 && queueWaitCalls==2 &&
                f.device.gpuGateError==VK_ERROR_DEVICE_LOST && group.cold && group.restoreBound &&
                group.chunks.size()==1 && group.storedBytes==MiB &&
                f.device.coldBytes==MiB && f.device.coldLogicalBytes==4*MiB &&
                m.children[0]==tokenHandle<VkDeviceMemory>(0xface) && frees==0,
                "completion failure retired cold data or freed potentially referenced backing");
        f.device.snapshot.mapped=nullptr;
    }
}

struct ColdCycleFixture {
    Fixture f{MiB,2*MiB};
    std::unique_ptr<std::vector<std::uint8_t>> staging;
    VkDeviceMemory nonlocal{tokenHandle<VkDeviceMemory>(0xc001)};

    ColdCycleFixture() {
        f.device.memory.memoryTypeCount=2; f.device.memory.memoryHeapCount=2;
        f.device.memory.memoryHeaps[0].size=8*MiB;
        f.device.memory.memoryHeaps[0].flags=VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        f.device.memory.memoryHeaps[1].size=8*MiB;
        f.device.memory.memoryHeaps[1].flags=0;
        f.device.memory.memoryTypes[0].heapIndex=0;
        f.device.memory.memoryTypes[1].heapIndex=1;
        f.device.budgetProperties=mockBudgetProperties; f.device.budgetHeap=0;
        f.device.budgetReserveBytes=1; f.device.physical=tokenHandle<VkPhysicalDevice>(4);
        f.device.coldCycleRecovery=true; f.device.pressureOnly=true;
        f.device.budgetProperties=mockBudgetProperties;
        mockNativeHeapSize=mockHeapBudget=8*MiB; mockHeapUsage=0;
        auto& m=f.state();
        f.bind(0,MiB);
        m.backingMemoryTypeBits=3; m.poolViewMemoryTypeBits[0]=3;
        m.children[0]=nonlocal; m.childTypes[0]=1; m.coldGroups[0].cold=false;
        m.coldGroups[0].pristine=false;
        m.coldGroups[0].logicalBytes=0; m.cold=false; m.coldLogicalSize=0;
        f.device.coldLogicalBytes=0;
        m.coldGroups[0].lastUse=std::chrono::steady_clock::now()-std::chrono::seconds(5);
        m.residentBytes=MiB; f.device.residentBytes=MiB; f.device.liveOther=MiB;
        liveAllocations.insert(nonlocal);
        f.device.idleMilliseconds=1000; f.device.coldBudget=2*MiB;
        f.device.minSavingsPercent=1; f.device.snapshot.chunkSize=MiB;
        f.device.snapshot.stagingSize=MiB; f.device.snapshot.stagingBuffer=tokenHandle<VkBuffer>(0xc002);
        f.device.snapshot.commandPool=tokenHandle<VkCommandPool>(0xc003);
        f.device.snapshot.commandBuffer=tokenHandle<VkCommandBuffer>(0xc004);
        f.device.copyQueue=tokenHandle<VkQueue>(0xc005);
        staging=std::make_unique<std::vector<std::uint8_t>>(MiB);
        f.device.snapshot.mapped=staging->data();
    }
};

void checkGatedRecoveryTeardownCleanup() {
    for(const auto idleResult:{VK_SUCCESS,VK_ERROR_DEVICE_LOST}) {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        d.autoInitialized=true; d.gpuRestoreUnsafe=true; d.gpuGateError=VK_ERROR_DEVICE_LOST;
        d.snapshot.destroyFence=mockDestroyFence;
        d.pendingWarmRecovery.fence=tokenHandle<VkFence>(0xc031);
        const auto replacement=tokenHandle<VkDeviceMemory>(0xc032);
        const auto destination=tokenHandle<VkBuffer>(0xc033);
        liveAllocations.insert(replacement); bufferSizes[destination]=MiB;
        m.trackPhysicalStats=true; d.liveOther=MiB; d.liveLocal=MiB;
        d.retainedWarmRecoveries.push_back({c.nonlocal,replacement,m.poolViews[0],destination,1,0,MiB});
        mockDeviceWaitResult=idleResult;
        require(waitForDeviceTeardown(d) && d.teardownGpuIdleProven && d.gpuRestoreUnsafe,
                "success/device-lost teardown did not authorize gated resource cleanup while preserving gate");
        cleanupRetainedWarmRecoveriesAfterIdle(d);
        releaseChildren(d,m);
        require(frees==2 && !liveAllocations.count(c.nonlocal) && !liveAllocations.count(replacement) &&
                destroyedMockBuffers==2 && bufferSizes.empty() && destroyedMockFences==1 &&
                d.retainedWarmRecoveries.empty() && !d.pendingWarmRecovery.fence &&
                d.liveLocal==0 && d.liveOther==0 && d.gpuGateError==VK_ERROR_DEVICE_LOST,
                "safe teardown did not destroy each retained/ordinary resource exactly once");
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        d.autoInitialized=true; d.gpuRestoreUnsafe=true; d.gpuGateError=VK_ERROR_DEVICE_LOST;
        d.snapshot.destroyFence=mockDestroyFence;
        d.pendingWarmRecovery.fence=tokenHandle<VkFence>(0xc041);
        const auto replacement=tokenHandle<VkDeviceMemory>(0xc042);
        const auto destination=tokenHandle<VkBuffer>(0xc043);
        liveAllocations.insert(replacement); bufferSizes[destination]=MiB;
        d.retainedWarmRecoveries.push_back({c.nonlocal,replacement,m.poolViews[0],destination,1,0,MiB});
        mockDeviceWaitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
        require(!waitForDeviceTeardown(d) && !d.teardownGpuIdleProven && d.gpuRestoreUnsafe &&
                d.gpuGateError==VK_ERROR_DEVICE_LOST,
                "unrelated wait failure incorrectly authorized gated GPU cleanup or overwrote the gate diagnostic");
        cleanupRetainedWarmRecoveriesAfterIdle(d);
        require(frees==0 && destroyedMockBuffers==0 && destroyedMockFences==0 &&
                liveAllocations.count(c.nonlocal) && liveAllocations.count(replacement) &&
                bufferSizes.count(m.poolViews[0]) && bufferSizes.count(destination) &&
                d.retainedWarmRecoveries.size()==1 && d.pendingWarmRecovery.fence,
                "unproven idle teardown destroyed a retained backing, view, or fence prematurely");
    }
}

void configureTeardownQueueInventory(Device& d) {
    const auto app0=tokenHandle<VkQueue>(0xc061), app1=tokenHandle<VkQueue>(0xc062);
    const auto privateQueue=tokenHandle<VkQueue>(0xc063);
    d.appQueues={app0,app1,app0};
    d.copyQueue=privateQueue; d.sparseQueue=app1;
    d.queueInventoryComplete=true; d.queueWaitIdle=mockQueueWait;
}

void checkQueueIdleTeardownFallback() {
    {
        Fixture f; auto& d=f.device;
        d.autoInitialized=true; d.snapshot.deviceWaitIdle=mockDeviceWait;
        configureTeardownQueueInventory(d);
        mockDeviceWaitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
        require(waitForDeviceTeardown(d) && d.teardownGpuIdleProven && mockDeviceWaitCalls==1 &&
                mockWaitedQueues.size()==3 && mockWaitedQueues[0]==d.appQueues[0] &&
                mockWaitedQueues[1]==d.appQueues[1] && mockWaitedQueues[2]==d.copyQueue,
                "complete all-success queue fallback did not prove idle once per distinct queue");
    }
    {
        Fixture f; auto& d=f.device;
        d.autoInitialized=true; d.snapshot.deviceWaitIdle=mockDeviceWait;
        configureTeardownQueueInventory(d);
        mockDeviceWaitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
        mockQueueWaitResults[d.appQueues[1]]=VK_ERROR_OUT_OF_HOST_MEMORY;
        require(!waitForDeviceTeardown(d) && !d.teardownGpuIdleProven && mockWaitedQueues.size()==3 &&
                mockWaitedQueues[2]==d.copyQueue,
                "partial queue-wait error stopped enumeration or incorrectly proved idle");
    }
    {
        Fixture f; auto& d=f.device;
        d.autoInitialized=true; d.gpuRestoreUnsafe=true; d.snapshot.deviceWaitIdle=mockDeviceWait;
        d.snapshot.destroyFence=mockDestroyFence; d.snapshot.destroyBuffer=mockDestroyBuffer;
        d.destroyBuffer=mockDestroyBuffer; d.free=mockFree;
        configureTeardownQueueInventory(d);
        const auto app0=d.appQueues[0], app1=d.appQueues[1], privateQueue=d.copyQueue;
        const auto oldBacking=tokenHandle<VkDeviceMemory>(0xc071);
        const auto replacement=tokenHandle<VkDeviceMemory>(0xc072);
        const auto sourceView=tokenHandle<VkBuffer>(0xc073);
        const auto destinationView=tokenHandle<VkBuffer>(0xc074);
        const auto deferredMemory=tokenHandle<VkDeviceMemory>(0xc075);
        const auto deferredBuffer=tokenHandle<VkBuffer>(0xc076);
        int memoryTag{},bufferTag{}; VkAllocationCallbacks memoryCallbacks{},bufferCallbacks{};
        memoryCallbacks.pUserData=&memoryTag; bufferCallbacks.pUserData=&bufferTag;
        liveAllocations.insert(oldBacking); liveAllocations.insert(replacement);
        bufferSizes[sourceView]=MiB; bufferSizes[destinationView]=MiB; bufferSizes[deferredBuffer]=MiB;
        d.retainedWarmRecoveries.push_back({oldBacking,replacement,sourceView,destinationView,0,0,MiB});
        d.pendingWarmRecovery.fence=tokenHandle<VkFence>(0xc077);
        d.promotedBuffers[deferredBuffer].deferredDestroy=true;
        d.promotedBuffers[deferredBuffer].deferredDestroyCallbacks=bufferCallbacks;
        d.promotedBuffers[deferredBuffer].hasDeferredDestroyCallbacks=true;
        Allocation deferred{}; deferred.size=MiB; deferred.local=true; deferred.deferredFree=true;
        deferred.hasDeferredFreeCallbacks=true; deferred.deferredFreeCallbacks=memoryCallbacks;
        d.allocations.emplace(deferredMemory,deferred); liveAllocations.insert(deferredMemory);
        d.liveLocal=3*MiB;
        mockDeviceWaitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
        mockQueueWaitResults[app1]=VK_ERROR_OUT_OF_HOST_MEMORY;
        require(!waitForDeviceTeardown(d),"partial-error cleanup fixture unexpectedly proved idle");
        destroyDeferredPromotedBuffersAfterIdle(d);
        releaseDeferredNativeFreesAfterIdle(d);
        cleanupRetainedWarmRecoveriesAfterIdle(d);
        require(frees==0 && destroyedMockBuffers==0 && destroyedMockFences==0 &&
                liveAllocations.count(oldBacking) && liveAllocations.count(replacement) &&
                liveAllocations.count(deferredMemory) && d.pendingWarmRecovery.fence &&
                d.retainedWarmRecoveries.size()==1 && d.promotedBuffers.count(deferredBuffer) &&
                d.allocations.count(deferredMemory),
                "partial queue-idle failure let cleanup release retained GPU resources");
        mockQueueWaitResults.clear(); mockWaitedQueues.clear(); queueWaitCalls=0;
        require(waitForDeviceTeardown(d) && mockWaitedQueues.size()==3 &&
                mockWaitedQueues[0]==app0 && mockWaitedQueues[1]==app1 &&
                mockWaitedQueues[2]==privateQueue,
                "subsequent all-queue success did not establish teardown proof");
        destroyDeferredPromotedBuffersAfterIdle(d);
        require(lastDestroyedBufferUserData==&bufferTag,
                "deferred application buffer lost its copied destroy callback");
        releaseDeferredNativeFreesAfterIdle(d);
        cleanupRetainedWarmRecoveriesAfterIdle(d);
        require(frees==3 && destroyedMockBuffers==3 && destroyedMockFences==1 &&
                !liveAllocations.count(oldBacking) && !liveAllocations.count(replacement) &&
                !liveAllocations.count(deferredMemory) && !d.pendingWarmRecovery.fence &&
                d.retainedWarmRecoveries.empty() && !d.promotedBuffers.count(deferredBuffer) &&
                !d.allocations.count(deferredMemory) && d.liveLocal==0 &&
                freedMemoryUserData.at(deferredMemory)==&memoryTag,
                "queue fallback proof failed to release deferred callbacks/resources exactly once");
    }
    {
        Fixture f; auto& d=f.device;
        d.autoInitialized=true; d.snapshot.deviceWaitIdle=mockDeviceWait;
        configureTeardownQueueInventory(d);
        mockDeviceWaitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
        mockQueueWaitResults[d.appQueues[1]]=VK_ERROR_DEVICE_LOST;
        require(waitForDeviceTeardown(d) && d.teardownGpuIdleProven && mockWaitedQueues.size()==3,
                "real queue DEVICE_LOST was not accepted as an idle proof");
    }
    {
        Fixture f; auto& d=f.device;
        d.autoInitialized=true; d.snapshot.deviceWaitIdle=mockDeviceWait;
        configureTeardownQueueInventory(d); d.queueInventoryComplete=false;
        mockDeviceWaitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
        require(!waitForDeviceTeardown(d) && !d.teardownGpuIdleProven && mockWaitedQueues.empty(),
                "incomplete queue inventory was used to claim idle");
    }
    {
        Fixture f; auto& d=f.device;
        d.autoInitialized=true; d.snapshot.deviceWaitIdle=mockDeviceWait;
        d.queueInventoryComplete=true; d.queueWaitIdle=mockQueueWait;
        mockDeviceWaitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
        require(!waitForDeviceTeardown(d) && !d.teardownGpuIdleProven && mockWaitedQueues.empty(),
                "empty queue inventory vacuously proved device idle");
    }
    {
        Fixture f; auto& d=f.device;
        d.autoInitialized=true; d.snapshot.deviceWaitIdle=mockDeviceWait;
        configureTeardownQueueInventory(d);
        mockDeviceWaitResult=VK_SUCCESS;
        require(waitForDeviceTeardown(d) && mockWaitedQueues.empty(),
                "successful device wait unnecessarily fell back to individual queues");
    }
}

void checkDeferredPromotedBufferDestroy() {
    ColdCycleFixture c; c.f.handle=reinterpret_cast<VkDevice>(&c.f.dispatchWord);
    auto& d=c.f.device; d.handle=c.f.handle;
    d.autoInitialized=true; d.gpuGateError=VK_ERROR_DEVICE_LOST;
    d.snapshot.deviceWaitIdle=mockDeviceWait; d.destroyBuffer=mockDestroyBuffer;
    const auto buffer=tokenHandle<VkBuffer>(0xc051);
    bufferSizes[buffer]=MiB;
    PromotedBuffer promoted{}; promoted.size=MiB;
    d.promotedBuffers.emplace(buffer,promoted);
    int allocatorTag{}; VkAllocationCallbacks callbacks{}; callbacks.pUserData=&allocatorTag;
    {
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices[key(d.handle)]=std::shared_ptr<Device>(&d,[](Device*){});
    }
    layerDestroyBuffer(d.handle,buffer,&callbacks);
    require(d.promotedBuffers.count(buffer)==1 && d.promotedBuffers.at(buffer).deferredDestroy &&
            destroyedMockBuffers==0 && bufferSizes.count(buffer),
            "gated application destroy was not retained without touching the buffer");
    const auto untrackedBuffer=tokenHandle<VkBuffer>(0xc054);
    bufferSizes[untrackedBuffer]=MiB;
    layerDestroyBuffer(d.handle,untrackedBuffer,nullptr);
    require(destroyedMockBuffers==1 && !bufferSizes.count(untrackedBuffer),
            "ordinary untracked buffer destroy was swallowed by the recovery gate");
    const auto memory=tokenHandle<VkDeviceMemory>(0xc052);
    Allocation allocation{}; allocation.size=MiB; allocation.local=true; allocation.type=0;
    allocation.nativeHandle=memory; d.allocations.emplace(memory,allocation);
    const auto memoryNull=tokenHandle<VkDeviceMemory>(0xc055);
    Allocation unwrappedNull{}; unwrappedNull.size=MiB; unwrappedNull.local=true; unwrappedNull.type=0;
    unwrappedNull.nativeHandle=memoryNull; d.allocations.emplace(memoryNull,unwrappedNull);
    const auto memoryWrapped=tokenHandle<VkDeviceMemory>(0xc056);
    const auto wrappedNative=tokenHandle<VkDeviceMemory>(0xc057);
    int originalAllocatorTag{}; VkAllocationCallbacks originalCallbacks{};
    originalCallbacks.pUserData=&originalAllocatorTag;
    Allocation wrapped{}; wrapped.size=MiB; wrapped.local=true; wrapped.type=0;
    wrapped.nativeHandle=wrappedNative; wrapped.wrapped=true; wrapped.hasCallbacks=true;
    wrapped.callbacks=originalCallbacks; d.allocations.emplace(memoryWrapped,wrapped);
    d.liveLocal=3*MiB; liveAllocations.insert(memory); liveAllocations.insert(memoryNull);
    liveAllocations.insert(wrappedNative);
    layerFreeMemory(d.handle,memory,&callbacks);
    layerFreeMemory(d.handle,memoryNull,nullptr);
    layerFreeMemory(d.handle,memoryWrapped,&callbacks);
    require(d.allocations.count(memory)==1 && d.allocations.at(memory).deferredFree &&
            d.allocations.at(memoryNull).deferredFree && d.allocations.at(memoryWrapped).deferredFree &&
            frees==0,
            "gated native free was not deferred with the allocation still tracked");
    mockDeviceWaitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
    require(!waitForDeviceTeardown(d),"unrelated wait error incorrectly proved safe teardown");
    destroyDeferredPromotedBuffersAfterIdle(d);
    releaseDeferredNativeFreesAfterIdle(d);
    require(d.promotedBuffers.count(buffer)==1 && d.allocations.count(memory)==1 &&
            d.allocations.count(memoryNull)==1 && d.allocations.count(memoryWrapped)==1 &&
            destroyedMockBuffers==1 && frees==0 && bufferSizes.count(buffer) &&
            liveAllocations.count(memory) && liveAllocations.count(memoryNull) &&
            liveAllocations.count(wrappedNative),
            "deferred app buffer or memory was destroyed before idle/lost proof");
    configureTeardownQueueInventory(d);
    mockDeviceWaitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
    require(waitForDeviceTeardown(d),"all-queue fallback did not prove deferred callback cleanup safe");
    destroyDeferredPromotedBuffersAfterIdle(d);
    releaseDeferredNativeFreesAfterIdle(d);
    require(d.promotedBuffers.count(buffer)==0 && d.allocations.count(memory)==0 &&
            d.allocations.count(memoryNull)==0 && d.allocations.count(memoryWrapped)==0 &&
            destroyedMockBuffers==2 && frees==3 && !bufferSizes.count(buffer) &&
            !liveAllocations.count(memory) && !liveAllocations.count(memoryNull) &&
            !liveAllocations.count(wrappedNative) && lastDestroyedBufferUserData==&allocatorTag &&
            freedMemoryUserData.at(memory)==&allocatorTag && freedMemoryUserData.at(memoryNull)==nullptr &&
            freedMemoryUserData.at(wrappedNative)==&originalAllocatorTag && d.liveLocal==0,
            "safe teardown did not honor deferred buffer/memory releases and their callbacks exactly once");
    std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(d.handle));
}

void checkColdCyclePromotionAndFailureRetry() {
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        require(coldCyclePromoteLocked(d,c.f.memory,0,2*MiB),
                "direct recovery did not copy the nonlocal child into local memory");
        require(m.children[0] && m.childTypes[0]==0 && d.liveLocal==MiB && d.liveOther==0 &&
                d.residentBytes==MiB && !m.coldGroups[0].cold && d.coldLogicalBytes==0 &&
                d.freezeCount==0 && d.restoreCount==0 && d.snapshot.copyCalls==1 &&
                d.snapshot.copyBytes==MiB && !d.gpuRestoreUnsafe && d.retainedWarmRecoveries.empty(),
                "direct recovery changed cold counters or failed to commit one GPU copy");
        requireColdLogicalMatchesState(d,"successful cold-cycle left stale cold accounting");
        require(m.backingMemoryTypeBits==3 && allocatedTypes.size()==1 && allocatedTypes[0]==0 &&
                frees==1 && bufferSizes.size()==m.poolViews.size(),
                "direct recovery failed to preserve masks or retire only old backing and temporary view");
        require(sparseBinds==2 && queueWaitCalls==3 && sparseBindCalls.size()==2 &&
                sparseBindCalls.back().size()==m.bindings.size()+2,
                "direct recovery did not use one final sparse batch and completion wait");
        const auto& initialBatch=sparseBindCalls.front();
        const auto& finalBatch=sparseBindCalls.back();
        require(initialBatch.size()==m.bindings.size()+2 &&
                initialBatch[0].buffer==m.poolViews[0] && initialBatch[0].binds.size()==1 &&
                initialBatch[0].binds[0].memory==c.nonlocal && initialBatch[0].binds[0].size==MiB &&
                initialBatch[1].binds.size()==1 && initialBatch[1].binds[0].memory==finalBatch[2].binds[0].memory &&
                initialBatch[1].binds[0].size==MiB &&
                initialBatch[2].buffer==m.bindings[0].buffer && initialBatch[2].binds.size()==1 &&
                initialBatch[2].binds[0].memory==VK_NULL_HANDLE && initialBatch[2].binds[0].size==MiB,
                "initial sparse batch did not bind both private views while detaching every app alias");
        const auto& sourceRelease=finalBatch[0];
        const auto& destinationRelease=finalBatch[1];
        const auto& aliasBind=finalBatch[2];
        require(sourceRelease.buffer==m.poolViews[0] && sourceRelease.binds.size()==1 &&
                sourceRelease.binds[0].memory==VK_NULL_HANDLE && sourceRelease.binds[0].size==MiB &&
                destinationRelease.buffer==initialBatch[1].buffer &&
                destinationRelease.binds.size()==1 && destinationRelease.binds[0].memory==VK_NULL_HANDLE &&
                destinationRelease.binds[0].size==MiB && aliasBind.buffer==m.bindings[0].buffer &&
                aliasBind.binds.size()==1 && aliasBind.binds[0].memory==m.children[0] &&
                aliasBind.binds[0].size==MiB,
                "final sparse batch did not unbind both private views and map the app alias to replacement");
        require(capturedMemoryBarriers.size()==2 &&
                capturedMemoryBarriers[0].srcStage==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
                capturedMemoryBarriers[0].dstStage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
                capturedMemoryBarriers[0].srcAccess==VK_ACCESS_MEMORY_WRITE_BIT &&
                capturedMemoryBarriers[0].dstAccess==VK_ACCESS_TRANSFER_READ_BIT &&
                capturedMemoryBarriers[1].srcStage==VK_PIPELINE_STAGE_TRANSFER_BIT &&
                capturedMemoryBarriers[1].dstStage==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
                capturedMemoryBarriers[1].srcAccess==VK_ACCESS_TRANSFER_WRITE_BIT &&
                capturedMemoryBarriers[1].dstAccess==(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT),
                "direct recovery copy omitted source-write or destination-app visibility barriers");
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        const auto attemptStarted=std::chrono::steady_clock::now();
        failAllocationTypes.insert(0);
        require(!coldCyclePromoteLocked(d,c.f.memory,0,2*MiB),
                "direct recovery unexpectedly succeeded when local allocation was refused");
        require(!m.coldGroups[0].cold && m.children[0]==c.nonlocal &&
                m.backingMemoryTypeBits==3 && d.liveOther==MiB && d.liveLocal==0 &&
                d.residentBytes==MiB && frees==0 && d.retainedWarmRecoveries.empty() &&
                sparseBinds==0 && bufferSizes.size()==m.poolViews.size() &&
                d.coldCycleBackoffUntil>=attemptStarted+std::chrono::milliseconds(250),
                "allocation refusal mutated aliases, counters, or old backing ownership");
        const auto attempted=allocations;
        require(!coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && allocations==attempted,
                "local allocation refusal retried before the 250ms recovery backoff expired");
        require(bufferSizes.size()==m.poolViews.size(),
                "local allocation backoff created another temporary sparse view");
        failAllocationTypes.clear();
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        mockEndCommandResult=VK_ERROR_UNKNOWN; // Fail before queue submission; rollback is safe.
        require(!coldCyclePromoteLocked(d,c.f.memory,0,2*MiB),
                "direct recovery ignored a pre-submit copy failure");
        require(m.children[0]==c.nonlocal && !m.coldGroups[0].cold &&
                m.backingMemoryTypeBits==3 && d.liveOther==MiB && d.liveLocal==0 &&
                d.residentBytes==MiB && d.coldBytes==0 && d.coldLogicalBytes==0 &&
                d.gpuGateError==VK_SUCCESS && !d.gpuRestoreUnsafe && frees==1 &&
                d.retainedWarmRecoveries.empty() && bufferSizes.size()==m.poolViews.size(),
                "pre-submit copy failure did not restore aliases and release temporary resources");
        requireColdLogicalMatchesState(d,"pre-commit failure changed cold accounting");
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        failQueueWaitAt=2; // Combined initial setup, then copy submission completes ambiguously.
        require(!coldCyclePromoteLocked(d,c.f.memory,0,2*MiB),
                "direct recovery ignored ambiguous copy completion");
        const auto& retained=d.retainedWarmRecoveries.at(0);
        require(m.children[0]==c.nonlocal && retained.oldBacking==c.nonlocal &&
                retained.replacement && retained.sourceView==m.poolViews[0] && retained.destinationView &&
                liveAllocations.count(c.nonlocal) && liveAllocations.count(retained.replacement) &&
                bufferSizes.count(retained.destinationView) && d.gpuRestoreUnsafe &&
                d.gpuGateError==VK_ERROR_DEVICE_LOST && frees==0,
                "ambiguous copy completion did not retain every potentially referenced resource");
    }
}

void checkWarmRecoveryInitialBatchMapsDisjointAliases() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    constexpr VkDeviceSize quarter=MiB/4;
    m.bindings[0].memoryOffset=0; m.bindings[0].size=quarter;
    m.bindings.push_back({tokenHandle<VkBuffer>(0xc701),quarter,quarter,4096});
    m.bindings.push_back({tokenHandle<VkBuffer>(0xc702),2*quarter,2*quarter,4096});
    require(coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && sparseBinds==2 &&
            queueWaitCalls==3 && sparseBindCalls.size()==2 &&
            sparseBindCalls[0].size()==m.bindings.size()+2 &&
            sparseBindCalls[1].size()==m.bindings.size()+2,
            "recovery did not combine the initial setup and final alias remap into single batches");
    const auto& setup=sparseBindCalls[0]; const auto& final=sparseBindCalls[1];
    const auto replacement=final[2].binds[0].memory;
    require(setup[0].buffer==m.poolViews[0] && setup[0].binds[0].memory==c.nonlocal &&
            setup[0].binds[0].size==MiB && setup[1].buffer==final[1].buffer &&
            setup[1].binds[0].memory==replacement && setup[1].binds[0].size==MiB,
            "initial combined batch omitted either source or destination private bind");
    for(std::size_t i=0;i<m.bindings.size();++i) {
        const auto& unbind=setup[i+2]; const auto& bind=final[i+2];
        const auto offset=m.bindings[i].memoryOffset;
        const auto size=m.bindings[i].size;
        require(unbind.buffer==m.bindings[i].buffer && unbind.binds.size()==1 &&
                unbind.binds[0].memory==VK_NULL_HANDLE && unbind.binds[0].resourceOffset==0 &&
                unbind.binds[0].size==size && bind.buffer==m.bindings[i].buffer &&
                bind.binds.size()==1 && bind.binds[0].memory==replacement &&
                bind.binds[0].resourceOffset==0 && bind.binds[0].memoryOffset==offset &&
                bind.binds[0].size==size,
                "combined sparse plan lost a disjoint alias or its child-relative offset");
    }
}

void checkColdCycleActiveReferenceGuard() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    d.coldCycleQuietMilliseconds=0;
    m.coldGroups[0].lastUse=std::chrono::steady_clock::now();
    d.activeRefs.recordRanges(tokenHandle<VkQueue>(0xc006),{{c.f.memory,0}},true);
    require(!coldCyclePromoteLocked(d,c.f.memory,0,2*MiB),
            "cold-cycle ignored an in-flight child reference");
    require(allocations==0 && sparseBinds==0 && m.children[0]==c.nonlocal &&
            !m.coldGroups[0].cold && d.residentBytes==MiB,
            "in-flight guard mutated or froze the referenced child");
}

void checkColdCycleHotCompletedChild() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    d.coldCycleQuietMilliseconds=0;
    m.coldGroups[0].lastUse=std::chrono::steady_clock::now();
    require(coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && m.children[0] &&
            m.childTypes[0]==0 && !m.coldGroups[0].cold,
            "zero-quiet recovery did not promote a recent completed child");
}

void checkRecoveryPriorityOverridesQuietAgeOnly() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    d.coldCycleQuietMilliseconds=1000;
    const auto now=std::chrono::steady_clock::now();
    m.coldGroups[0].lastUse=now-std::chrono::milliseconds(100);
    VkDeviceMemory candidate{}; std::size_t child{};
    require(!selectColdCycleCandidate(d,64*MiB,now,candidate,child) &&
            candidate==VK_NULL_HANDLE,
            "default recovery selected a child before its quiet age elapsed");
    d.liveControl.state.recoveryPriority=true;
    require(selectColdCycleCandidate(d,64*MiB,now,candidate,child) &&
            candidate==c.f.memory && child==0,
            "recovery priority did not admit an otherwise eligible recent child");
    d.activeRefs.recordRanges(tokenHandle<VkQueue>(0xc005),{{c.f.memory,0}},true);
    require(!selectColdCycleCandidate(d,64*MiB,now,candidate,child) &&
            candidate==VK_NULL_HANDLE,
            "recovery priority bypassed the in-flight child guard");
    d.liveControl.state.recoveryPriority=false;
    d.activeRefs.cover(tokenHandle<VkQueue>(0xc005));
    d.activeRefs.retire(tokenHandle<VkQueue>(0xc005));
    require(!selectColdCycleCandidate(d,64*MiB,now,candidate,child) &&
            candidate==VK_NULL_HANDLE,
            "disabling recovery priority did not restore the default quiet-age gate");
}

void checkColdCycleCandidateSkipsUnpromotableOldest() {
    for(bool adoptedCallbacks:{false,true}) {
        ColdCycleFixture c; auto& d=c.f.device; auto& old=c.f.state();
        d.coldCycleQuietMilliseconds=0;
        const auto now=std::chrono::steady_clock::now();
        old.coldGroups[0].lastUse=now-std::chrono::seconds(5);
        old.hasAdoptedCallbacks=adoptedCallbacks;
        if(!adoptedCallbacks) {
            old.childSizes[0]=kAsyncSnapshotMaxRaw+1;
            d.liveOther=old.childSizes[0];
        }

        const auto youngerHandle=tokenHandle<VkDeviceMemory>(adoptedCallbacks?0xc201:0xc202);
        auto younger=old;
        younger.hasAdoptedCallbacks=false;
        younger.children[0]=tokenHandle<VkDeviceMemory>(adoptedCallbacks?0xc203:0xc204);
        younger.childSizes[0]=MiB;
        younger.childTypes[0]=1;
        younger.coldGroups[0].lastUse=now-std::chrono::seconds(2);
        d.virtualMemory.emplace(youngerHandle,std::move(younger));
        d.liveOther+=MiB;

        VkDeviceMemory candidate{}; std::size_t child{};
        require(selectColdCycleCandidate(d,64*MiB,now,candidate,child) &&
                candidate==youngerHandle && child==0,
                adoptedCallbacks?
                    "older callback-backed pool starved a younger promotable child":
                    "older oversized child starved a younger promotable child");
        d.virtualMemory.erase(youngerHandle);
        d.liveOther-=MiB;
        require(!selectColdCycleCandidate(d,64*MiB,now,candidate,child) &&
                candidate==VK_NULL_HANDLE && child==0,
                "recovery selection retained a stale candidate when only refused pools remained");
    }
}

void checkColdCycleCandidatePrefersMostRecentEligible() {
    ColdCycleFixture c; auto& d=c.f.device; auto& older=c.f.state();
    d.coldCycleQuietMilliseconds=1000;
    const auto now=std::chrono::steady_clock::now();
    older.coldGroups[0].lastUse=now-std::chrono::seconds(2);

    const auto recentHandle=tokenHandle<VkDeviceMemory>(0xc221);
    auto recent=older;
    recent.children[0]=tokenHandle<VkDeviceMemory>(0xc222);
    recent.coldGroups[0].lastUse=now-std::chrono::milliseconds(100);
    d.virtualMemory.emplace(recentHandle,std::move(recent));
    d.liveOther+=MiB;

    // This older child has no compatible local pool type, so it must not win
    // merely because its last-use timestamp ranks first.
    const auto incompatibleHandle=tokenHandle<VkDeviceMemory>(0xc223);
    auto incompatible=older;
    incompatible.children[0]=tokenHandle<VkDeviceMemory>(0xc224);
    incompatible.backingMemoryTypeBits=2;
    incompatible.poolViewMemoryTypeBits[0]=2;
    incompatible.coldGroups[0].lastUse=now-std::chrono::milliseconds(1500);
    d.virtualMemory.emplace(incompatibleHandle,std::move(incompatible));
    d.liveOther+=MiB;

    VkDeviceMemory candidate{}; std::size_t child{};
    require(selectColdCycleCandidate(d,64*MiB,now,candidate,child) &&
            candidate==c.f.memory && child==0,
            "quiet-age or local-pool compatibility exclusion was bypassed during cold-cycle selection");

    d.coldCycleQuietMilliseconds=0;
    require(selectColdCycleCandidate(d,64*MiB,now,candidate,child) &&
            candidate==recentHandle && child==0,
            "cold-cycle selection did not prefer the most recently used eligible child");

    d.activeRefs.recordRanges(tokenHandle<VkQueue>(0xc225),{{recentHandle,0}},true);
    require(selectColdCycleCandidate(d,64*MiB,now,candidate,child) &&
            candidate==c.f.memory && child==0,
            "busy recent child blocked fallback to an older eligible child");
}

void checkRecoveryUseHeatSelectionAndDecay() {
    ColdCycleFixture c; auto& d=c.f.device; auto& older=c.f.state();
    d.coldCycleQuietMilliseconds=0;
    const auto used=std::chrono::steady_clock::now();
    const auto recentHandle=tokenHandle<VkDeviceMemory>(0xc231);
    auto recent=older;
    recent.children[0]=tokenHandle<VkDeviceMemory>(0xc232);
    const auto newerUse=used+std::chrono::milliseconds(1);
    d.virtualMemory.emplace(recentHandle,std::move(recent)); d.liveOther+=MiB;
    for(unsigned i=0;i<5;i++) markSubmittedUseTimes(d,{{c.f.memory,0}},true,used);
    markSubmittedUseTimes(d,{{recentHandle,0}},true,newerUse);

    VkDeviceMemory candidate{}; std::size_t child{};
    require(older.coldGroups[0].lastUse<d.virtualMemory.at(recentHandle).coldGroups[0].lastUse &&
            recoveryUseHeat(older.coldGroups[0],newerUse)==5 &&
            recoveryUseHeat(d.virtualMemory.at(recentHandle).coldGroups[0],newerUse)==1 &&
            selectColdCycleCandidate(d,64*MiB,newerUse,candidate,child) && candidate==c.f.memory,
            "repeated known uses did not outrank one newer use for recovery");

    const auto busyQueue=tokenHandle<VkQueue>(0xc233);
    d.activeRefs.recordRanges(busyQueue,{{c.f.memory,0}},true);
    require(selectColdCycleCandidate(d,64*MiB,newerUse,candidate,child) && candidate==recentHandle,
            "busy hot recovery candidate blocked an eligible cooler child");
    d.activeRefs.cover(busyQueue); d.activeRefs.retire(busyQueue);

    const auto fadedAt=used+std::chrono::seconds(5);
    markSubmittedUseTimes(d,{{recentHandle,0}},true,fadedAt-std::chrono::milliseconds(100));
    require(recoveryUseHeat(older.coldGroups[0],fadedAt)==0 &&
            recoveryUseHeat(d.virtualMemory.at(recentHandle).coldGroups[0],fadedAt)==1 &&
            selectColdCycleCandidate(d,64*MiB,fadedAt,candidate,child) && candidate==recentHandle,
            "recovery heat did not decay after five seconds or yield to newer use");

    auto& tracked=d.virtualMemory.at(recentHandle);
    const auto savedUses=tracked.coldGroups[0].recoveryUses;
    const auto savedEpoch=tracked.coldGroups[0].recoveryUseEpoch;
    const auto savedUse=tracked.coldGroups[0].lastUse;
    markSubmittedUseTimes(d,{},true,fadedAt+std::chrono::seconds(1));
    require(tracked.coldGroups[0].recoveryUses==savedUses &&
            tracked.coldGroups[0].recoveryUseEpoch==savedEpoch && tracked.coldGroups[0].lastUse==savedUse,
            "known empty submission changed recovery use metadata");
    markSubmittedUseTimes(d,{},false,fadedAt+std::chrono::seconds(1));
    require(tracked.coldGroups[0].recoveryUses==savedUses && tracked.coldGroups[0].recoveryUseEpoch==savedEpoch &&
            tracked.coldGroups[0].lastUse==fadedAt+std::chrono::seconds(1),
            "unknown submission changed heat or failed to retain conservative last-use tracking");

    VirtualMemory::ColdGroup spaced{};
    const auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<12;i++) recordRecoveryUse(spaced,start+std::chrono::milliseconds(900*i));
    require(recoveryUseHeat(spaced,start+std::chrono::milliseconds(9900))<=3,
            "900 ms use cadence accumulated unbounded recovery heat");
    VirtualMemory::ColdGroup capped{};
    for(unsigned i=0;i<32;i++) recordRecoveryUse(capped,start);
    require(capped.recoveryUses==16 && recoveryUseHeat(capped,start)==16 &&
            recoveryUseHeat(capped,start+std::chrono::milliseconds(999))==16 &&
            recoveryUseHeat(capped,start+std::chrono::seconds(1))==8 &&
            recoveryUseHeat(capped,start+std::chrono::seconds(5))==0,
            "recovery heat exceeded its cap or missed whole-second/five-second decay boundaries");

    {
        Fixture tracked(2*MiB,2*MiB); auto& state=tracked.device; auto& memory=tracked.state();
        const auto buffer=tracked.bind(0,2*MiB);
        state.selectiveRestore=true; state.narrowDescriptorRanges=true;
        state.promotedBuffers[buffer]=PromotedBuffer{};
        state.promotedBuffers[buffer].size=2*MiB;
        state.promotedBuffers[buffer].memory=tracked.memory;
        const auto command=tokenHandle<VkCommandBuffer>(0xc252);
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool=tokenHandle<VkCommandPool>(0xc253); allocate.commandBufferCount=1;
        state.submission.allocateCommands(&allocate,&command);
        state.submission.beginCommand(command);
        state.submission.bufferRange(command,buffer,0,MiB,true);
        state.submission.bufferRange(command,buffer,0,MiB,true);
        const auto priorUnknown=unknownCommandProc.exchange(false);
        std::vector<ActiveRefs::Use> uses;
        const bool known=commandMemories(state,{command},uses,false);
        unknownCommandProc.store(priorUnknown);
        const auto touched=std::chrono::steady_clock::now();
        if(known) markSubmittedUseTimes(state,uses,true,touched);
        require(known && uses.size()==1 && uses[0].memory==tracked.memory && uses[0].child==0 &&
                memory.coldGroups[0].recoveryUses==1 && memory.coldGroups[1].recoveryUses==0,
                "duplicate known command ranges incremented recovery heat more than once");
    }
}

void checkColdCycleCandidateZeroLiveOther() {
    ColdCycleFixture c; auto& d=c.f.device;
    d.liveOther=0;
    VkDeviceMemory candidate=tokenHandle<VkDeviceMemory>(0xc2ff); std::size_t child=99;
    require(!selectColdCycleCandidate(d,64*MiB,std::chrono::steady_clock::now(),candidate,child) &&
            candidate==VK_NULL_HANDLE && child==0,
            "zero tracked nonlocal bytes did not clear recovery candidate outputs");
}

void checkActiveWorkerDelayBounds() {
    require(activeWorkerDelayMilliseconds(10,false)==10 &&
            activeWorkerDelayMilliseconds(10,true)==1,
            "active worker delay did not cap idle waits or shorten after progress");
    require(activeWorkerDelayMilliseconds(5,false)==5 &&
            activeWorkerDelayMilliseconds(5,true)==1,
            "active worker delay changed the idle cadence below its cap");
    require(activeWorkerDelayMilliseconds(0,false)==1 &&
            activeWorkerDelayMilliseconds(0,true)==1,
            "zero idle interval allowed a busy-spin worker cadence");
    require(activeWorkerDelayMilliseconds(UINT64_MAX,false)==10 &&
            activeWorkerDelayMilliseconds(UINT64_MAX,true)==1,
            "large idle interval overflowed or bypassed the bounded active cadence");
}

void checkActiveSnapshotWorkerDrainsPressureAndStops() {
    ColdCycleFixture c; auto& d=c.f.device; auto& busy=c.f.state();
    d.handle=reinterpret_cast<VkDevice>(&c.f.dispatchWord); d.gdpa=mockGetDeviceProcAddr;
    d.autoInitialized=true; d.autoEnabled=true; d.activeEviction=true; d.pressureOnly=true;
    d.residentLimitBytes=MiB; d.residentBytes=3*MiB; d.liveOther=3*MiB;
    mockNativeHeapSize=mockHeapBudget=8*MiB; mockHeapUsage=0;

    const auto now=std::chrono::steady_clock::now();
    busy.coldGroups[0].lastUse=now-std::chrono::seconds(10);
    const auto queue=tokenHandle<VkQueue>(0xc901);
    d.activeRefs.recordRanges(queue,{{c.f.memory,0}},true);
    std::vector<VkDeviceMemory> eligible;
    const auto makeSibling=[&](std::uintptr_t base,std::chrono::seconds age,unsigned heat) {
        const auto memoryHandle=tokenHandle<VkDeviceMemory>(base);
        auto sibling=busy;
        sibling.children[0]=tokenHandle<VkDeviceMemory>(base+1);
        sibling.childTypes[0]=1; sibling.childSizes[0]=MiB;
        sibling.poolViews[0]=tokenHandle<VkBuffer>(base+2);
        sibling.poolViewMemoryTypeBits[0]=3; sibling.backingMemoryTypeBits=3;
        sibling.bindings[0].buffer=tokenHandle<VkBuffer>(base+3);
        sibling.coldGroups[0].lastUse=now-age;
        sibling.coldGroups[0].recoveryUses=static_cast<std::uint8_t>(heat);
        sibling.coldGroups[0].recoveryUseEpoch=now;
        sibling.residentBytes=MiB;
        bufferSizes[sibling.poolViews[0]]=MiB;
        bufferSizes[sibling.bindings[0].buffer]=MiB;
        liveAllocations.insert(sibling.children[0]);
        eligible.push_back(sibling.children[0]);
        d.virtualMemory.emplace(memoryHandle,std::move(sibling));
    };
    makeSibling(0xc910,std::chrono::seconds(5),0);
    makeSibling(0xc920,std::chrono::seconds(4),16);

    require(d.autoQueues.init(d.handle,d.gdpa,d.copyQueue,{d.copyQueue},true)==VK_SUCCESS,
            "active worker mock queue initialization failed");
    {
        std::lock_guard<std::mutex> lock(workerFreeMutex);
        observedWorkerFrees=0; observedWorkerFreedMemory.clear();
        observedWorkerFreedMemory.reserve(4); observeWorkerFrees=true;
    }
    auto shared=std::shared_ptr<Device>(&d,[](Device*){});
    std::mutex stoppedMutex; std::condition_variable stoppedCondition; bool stopped=false;
    std::thread worker([&] {
        snapshotWorkerLoop(shared);
        { std::lock_guard<std::mutex> lock(stoppedMutex); stopped=true; }
        stoppedCondition.notify_all();
    });

    bool drained=false;
    {
        std::unique_lock<std::mutex> lock(workerFreeMutex);
        drained=workerFreeCondition.wait_for(lock,std::chrono::seconds(10),[]{return observedWorkerFrees>=2;});
    }
    bool committed=false;
    {
        std::unique_lock<std::mutex> lock(d.mutex);
        const auto busyFound=d.virtualMemory.find(c.f.memory);
        committed=busyFound!=d.virtualMemory.end() && busyFound->second.children[0]==c.nonlocal &&
            d.residentBytes==MiB && d.coldLogicalBytes==2*MiB && d.freezeCount==2 &&
            d.liveOther==MiB && !busyFound->second.coldGroups[0].cold &&
            busyFound->second.coldGroups[0].lastUse==now-std::chrono::seconds(10) &&
            d.virtualMemory.at(tokenHandle<VkDeviceMemory>(0xc910)).coldGroups[0].recoveryUses==0 &&
            d.virtualMemory.at(tokenHandle<VkDeviceMemory>(0xc920)).coldGroups[0].recoveryUses==16;
        d.stopWorker.store(true); d.activity.notify_all();
    }
    bool stoppedInTime=false;
    {
        std::unique_lock<std::mutex> lock(stoppedMutex);
        stoppedInTime=stoppedCondition.wait_for(lock,std::chrono::seconds(5),[&]{return stopped;});
    }
    worker.join();
    std::vector<VkDeviceMemory> freedOrder;
    { std::lock_guard<std::mutex> lock(workerFreeMutex);
      observeWorkerFrees=false; freedOrder=observedWorkerFreedMemory; }
    require(drained && committed && stoppedInTime && freedOrder.size()==2 &&
            freedOrder[0]==eligible[0] && freedOrder[1]==eligible[1],
            "active snapshot worker failed to drain eligible pressure backlog, preserve a busy child, or stop");
}

void checkPressureBudgetRetryIgnoresUnrelatedSubmissions() {
    ColdCycleFixture c; auto& d=c.f.device; auto& older=c.f.state();
    d.minSavingsPercent=100; // Keep the 1 MiB snapshot raw and above the cold quota.
    d.coldBudget=MiB/2;

    const auto youngerHandle=tokenHandle<VkDeviceMemory>(0xc931);
    auto younger=older;
    younger.children[0]=tokenHandle<VkDeviceMemory>(0xc932);
    younger.childSizes[0]=MiB/4;
    younger.childTypes[0]=1;
    younger.poolViews[0]=tokenHandle<VkBuffer>(0xc933);
    younger.poolViewMemoryTypeBits[0]=3;
    younger.backingMemoryTypeBits=3;
    younger.bindings[0].buffer=tokenHandle<VkBuffer>(0xc934);
    younger.bindings[0].size=MiB/4;
    younger.residentBytes=MiB/4;
    younger.coldGroups[0].lastUse+=std::chrono::seconds(1);
    require(older.coldGroups[0].lastUse<younger.coldGroups[0].lastUse,
            "pressure retry fixture does not rank the refusing child oldest");
    bufferSizes[younger.poolViews[0]]=MiB/4;
    bufferSizes[younger.bindings[0].buffer]=MiB/4;
    liveAllocations.insert(younger.children[0]);
    d.virtualMemory.emplace(youngerHandle,std::move(younger));
    d.liveOther+=MiB/4; d.residentBytes+=MiB/4;

    require(freezeChildLocked(d,older,0)==VK_ERROR_OUT_OF_DEVICE_MEMORY &&
            older.coldGroups[0].budgetBlocked &&
            older.coldGroups[0].failedBudgetGeneration==d.coldBudgetGeneration,
            "oversized oldest child did not establish a real cold-quota refusal");

    VkDeviceMemory candidate{}; std::size_t child{};
    require(selectPressureCandidate(d,candidate,child) && candidate==youngerHandle && child==0,
            "same-generation budget refusal did not let the smaller child proceed");

    // A successful submission from unrelated application work must not release
    // this candidate's quota refusal and repeatedly put it ahead of the fit child.
    ++d.gpuSubmissionGeneration;
    require(selectPressureCandidate(d,candidate,child) && candidate==youngerHandle && child==0,
            "unrelated successful submission retried the oldest refused child ahead of a fitting child");

    ++d.coldBudgetGeneration;
    require(selectPressureCandidate(d,candidate,child) && candidate==c.f.memory && child==0,
            "cold-quota generation change did not make the refused child retryable");
    require(freezeChildLocked(d,older,0)==VK_ERROR_OUT_OF_DEVICE_MEMORY &&
            older.coldGroups[0].budgetBlocked &&
            older.coldGroups[0].failedBudgetGeneration==d.coldBudgetGeneration,
            "quota-generation retry did not record the renewed cold-quota refusal");

    auto& smaller=d.virtualMemory.at(youngerHandle);
    require(selectPressureCandidate(d,candidate,child) && candidate==youngerHandle && child==0,
            "renewed same-generation refusal did not allow smaller child to proceed");
    require(freezeChildLocked(d,smaller,0)==VK_SUCCESS && smaller.coldGroups[0].cold &&
            d.coldLogicalBytes==MiB/4,
            "smaller child did not fit the remaining cold quota after the older refusal");
}

void checkPressureBudgetBlockWriteInvalidation() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    d.selectiveRestore=true; d.narrowDescriptorRanges=true;
    d.budgetRefusalsSeen=true;
    const auto priorUnknown=unknownCommandProc.exchange(false);
    m.children.push_back(tokenHandle<VkDeviceMemory>(0xc941));
    m.childSizes.push_back(MiB); m.childTypes.push_back(1); m.childGenerations.push_back(1);
    m.coldGroups.emplace_back(); m.poolViews.push_back(tokenHandle<VkBuffer>(0xc942));
    m.poolViewMemoryTypeBits.push_back(3); m.backingMemoryTypeBits=3;
    const auto first=m.bindings[0].buffer;
    const auto second=tokenHandle<VkBuffer>(0xc943);
    m.bindings.push_back({second,MiB,MiB,1});
    for(const auto buffer:{first,second}) {
        PromotedBuffer promoted{}; promoted.size=MiB; promoted.memory=c.f.memory;
        d.promotedBuffers[buffer]=promoted;
    }

    struct AccessResult { bool known; std::size_t writes; };
    auto accepted=[&](VkBuffer buffer,bool writes,const void* pNext=nullptr) {
        const auto command=tokenHandle<VkCommandBuffer>(nextHandle++);
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool=tokenHandle<VkCommandPool>(0xc944); allocate.commandBufferCount=1;
        d.submission.allocateCommands(&allocate,&command);
        d.submission.beginCommand(command);
        d.submission.bufferRange(command,buffer,0,MiB,writes);
        std::vector<VkSubmissionTracker::BufferRange> ranges; const char* reason=nullptr;
        require(d.submission.collectRanges(1,&command,ranges,&reason),
                reason?reason:"pressure retry command ranges were not tracked");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext=pNext; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
        std::vector<ActiveRefs::Use> writeUses;
        const auto* submitInfo=static_cast<const VkSubmitInfo*>(&submit);
        const auto knownAccess=queueMemoryAccess(d,"vkQueueSubmit",writeUses,true,
            std::uint32_t{1},submitInfo,VkFence{});
        bumpAcceptedWriteEpochs(d,"vkQueueSubmit",std::uint32_t{1},submitInfo,VkFence{});
        return AccessResult{knownAccess,writeUses.size()};
    };

    auto& firstGroup=m.coldGroups[0]; auto& secondGroup=m.coldGroups[1];
    firstGroup.budgetBlocked=secondGroup.budgetBlocked=true;
    firstGroup.failedBudgetGeneration=secondGroup.failedBudgetGeneration=d.coldBudgetGeneration;
    firstGroup.writeEpoch=7;
    d.reportedAccessFallback=false;
    const auto read=accepted(first,false); // Known read only.
    require(read.known,"read-only accepted access became unknown");
    require(read.writes==0,"read-only access was classified as a write");
    require(firstGroup.budgetBlocked && secondGroup.budgetBlocked,
            "read-only accepted work cleared a cold-quota refusal");

    VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const auto* emptyInfo=static_cast<const VkSubmitInfo*>(&empty);
    bumpAcceptedWriteEpochs(d,"vkQueueSubmit",std::uint32_t{1},emptyInfo,VkFence{});
    require(firstGroup.budgetBlocked && secondGroup.budgetBlocked,
            "known empty accepted work cleared a cold-quota refusal");

    const auto otherWrite=accepted(second,true); // A write to another child must stay scoped.
    require(otherWrite.known && otherWrite.writes==1 && firstGroup.budgetBlocked &&
            !secondGroup.budgetBlocked && firstGroup.writeEpoch==7,
            "accepted write to another child cleared the wrong quota refusal");

    secondGroup.budgetBlocked=true;
    const auto ownWrite=accepted(first,true); // Exact refused child.
    require(ownWrite.known && ownWrite.writes==1 && !firstGroup.budgetBlocked &&
            secondGroup.budgetBlocked && firstGroup.writeEpoch==7,
            "sync-only accepted write did not clear only the matching quota refusal");

    firstGroup.budgetBlocked=secondGroup.budgetBlocked=true;
    unknownCommandProc.store(true);
    const auto unknown=accepted(first,true);
    unknownCommandProc.store(false);
    require(!unknown.known && !firstGroup.budgetBlocked && !secondGroup.budgetBlocked,
            "unknown accepted access did not conservatively clear all quota refusals");

    firstGroup.budgetBlocked=true; firstGroup.writeEpoch=19; d.asyncCompression=true;
    const auto asyncOwnWrite=accepted(first,true);
    require(asyncOwnWrite.known && asyncOwnWrite.writes==1 && !firstGroup.budgetBlocked &&
            firstGroup.writeEpoch==20 && d.asyncCompression,
            "async accepted write did not clear the refusal and advance its write epoch");

    firstGroup.budgetBlocked=true; firstGroup.writeEpoch=UINT64_MAX;
    const auto saturatedWrite=accepted(first,true);
    require(saturatedWrite.known && saturatedWrite.writes==1 && !firstGroup.budgetBlocked &&
            firstGroup.writeEpoch==UINT64_MAX && !d.asyncCompression,
            "write-epoch saturation did not fail closed while clearing the quota refusal");
    unknownCommandProc.store(priorUnknown);
}

PendingWarmRecovery pendingWarmRecoveryFor(ColdCycleFixture& c) {
    auto& m=c.f.state();
    PendingWarmRecovery pending{};
    pending.memory=c.f.memory; pending.allocationGeneration=m.identityGeneration;
    pending.bindingGeneration=m.bindingGeneration; pending.childGeneration=m.childGenerations[0];
    pending.child=0; pending.oldBacking=m.children[0]; pending.sourceView=m.poolViews[0];
    pending.destinationView=tokenHandle<VkBuffer>(0xc2fe); pending.oldType=m.childTypes[0];
    pending.newType=0; pending.size=MiB; pending.fence=tokenHandle<VkFence>(0xc2fd);
    return pending;
}

void checkWarmRecoveryFinalPlanFailureRetainsOwnership() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    auto pending=pendingWarmRecoveryFor(c);
    pending.replacement=tokenHandle<VkDeviceMemory>(0xc2e1);
    liveAllocations.insert(pending.replacement);
    bufferSizes[pending.destinationView]=MiB;
    d.liveLocal=MiB;
    d.retainedWarmRecoveries.push_back({pending.oldBacking,pending.replacement,pending.sourceView,
        pending.destinationView,pending.oldType,pending.newType,pending.size});
    m.bindings[0].alignment=0; // Fail final plan construction after the copy transaction.
    require(!finalizeWarmRecoveryLocked(d,pending) && d.gpuRestoreUnsafe &&
            d.gpuGateError==VK_ERROR_FEATURE_NOT_PRESENT && sparseBinds==0 && queueWaitCalls==0 &&
            m.children[0]==c.nonlocal && m.childTypes[0]==pending.oldType && frees==0 &&
            liveAllocations.count(pending.oldBacking) && liveAllocations.count(pending.replacement) &&
            bufferSizes.count(pending.destinationView) && d.retainedWarmRecoveries.size()==1,
            "final sparse-plan refusal freed or published resources before any driver call");
}

void checkWarmRecoveryVisibilityFailureRetainsOldBacking() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    auto pending=pendingWarmRecoveryFor(c);
    pending.replacement=tokenHandle<VkDeviceMemory>(0xc2e2);
    liveAllocations.insert(pending.replacement);
    bufferSizes[pending.destinationView]=MiB;
    d.liveLocal=MiB;
    d.retainedWarmRecoveries.push_back({pending.oldBacking,pending.replacement,pending.sourceView,
        pending.destinationView,pending.oldType,pending.newType,pending.size});
    d.gdpa=mockGetDeviceProcAddr;
    d.autoInitialized=true; d.activeEviction=true;
    d.restoreGeneration=std::numeric_limits<std::uint64_t>::max();
    const auto appQueue=tokenHandle<VkQueue>(0xc2e3);
    const std::vector<VkQueue> queues{appQueue,d.copyQueue};
    require(d.autoQueues.init(d.handle,d.gdpa,d.copyQueue,queues,true)==VK_SUCCESS,
            "visibility-failure fixture failed to initialize automatic queues");
    require(!finalizeWarmRecoveryLocked(d,pending) && d.gpuRestoreUnsafe &&
            d.gpuGateError==VK_ERROR_UNKNOWN && sparseBinds==1 &&
            m.children[0]==pending.replacement && m.childTypes[0]==pending.newType && frees==0 &&
            liveAllocations.count(pending.oldBacking) && liveAllocations.count(pending.replacement) &&
            bufferSizes.count(pending.destinationView) && d.retainedWarmRecoveries.size()==1,
            "restore-visibility failure freed old backing or dropped retained recovery ownership");
}

void checkPendingWarmRecoveryTokenAndQueueGates() {
    ColdCycleFixture c; auto& d=c.f.device;
    d.pendingWarmRecovery=pendingWarmRecoveryFor(c); d.pendingWarmRecovery.active=true;
    const auto valid=d.pendingWarmRecovery;
    require(pendingWarmRecoveryTokenValid(d,valid),"valid recovery lifetime key was rejected");
    auto stale=valid; ++stale.allocationGeneration;
    require(!pendingWarmRecoveryTokenValid(d,stale),"recovery token survived allocation replacement");
    stale=valid; ++stale.childGeneration;
    require(!pendingWarmRecoveryTokenValid(d,stale),"recovery token survived child replacement");
    stale=valid; ++stale.bindingGeneration;
    require(!pendingWarmRecoveryTokenValid(d,stale),"recovery token survived alias mutation");
    stale=valid; stale.oldBacking=tokenHandle<VkDeviceMemory>(0xc2fc);
    require(!pendingWarmRecoveryTokenValid(d,stale),"recovery token survived backing replacement");
    auto& pendingMemory=c.f.state();
    pendingMemory.children.push_back(tokenHandle<VkDeviceMemory>(0xc2fb));
    pendingMemory.childSizes.push_back(MiB); pendingMemory.childTypes.push_back(1);
    pendingMemory.childGenerations.push_back(2);
    pendingMemory.poolViews.push_back(tokenHandle<VkBuffer>(0xc2f9));
    pendingMemory.poolViewMemoryTypeBits.push_back(3);
    pendingMemory.coldGroups.emplace_back();
    const std::vector<ActiveRefs::Use> empty{};
    const std::vector<ActiveRefs::Use> hotDisjoint{{c.f.memory,1}};
    const std::vector<ActiveRefs::Use> exact{{c.f.memory,0}};
    const std::vector<ActiveRefs::Use> whole{{c.f.memory,SIZE_MAX}};
    require(!warmRecoveryBlocksUses(d,true,empty) &&
            warmRecoveryKnownHotUses(d,true,hotDisjoint) &&
            !warmRecoveryBlocksUses(d,true,hotDisjoint,true) &&
            warmRecoveryBlocksUses(d,false,empty) &&
            warmRecoveryBlocksUses(d,true,exact) && warmRecoveryBlocksUses(d,true,whole) &&
            warmRecoveryBlocksUses(d,true,{{c.f.memory,2}}),
            "pending recovery queue-use gate mishandled empty, unknown, matching, or disjoint uses");
    pendingMemory.coldGroups[1].cold=true;
    require(!warmRecoveryKnownHotUses(d,true,hotDisjoint) &&
            warmRecoveryBlocksUses(d,true,hotDisjoint),
            "a selected cold child was allowed through a pending recovery");
    pendingMemory.coldGroups[1].cold=false;
    const auto childGenerations=pendingMemory.childGenerations;
    pendingMemory.childGenerations.pop_back();
    require(warmRecoveryBlocksUses(d,true,hotDisjoint),
            "missing child metadata did not block a pending recovery submit");
    pendingMemory.childGenerations=childGenerations;
    const auto poolTypeBits=pendingMemory.poolViewMemoryTypeBits[1];
    pendingMemory.poolViewMemoryTypeBits[1]=0;
    require(warmRecoveryBlocksUses(d,true,hotDisjoint),
            "incompatible pool-view memory type did not block a pending recovery submit");
    pendingMemory.poolViewMemoryTypeBits[1]=poolTypeBits;
    const auto childType=pendingMemory.childTypes[1];
    pendingMemory.childTypes[1]=UINT32_MAX;
    require(warmRecoveryBlocksUses(d,true,hotDisjoint),
            "out-of-range child memory type did not block a pending recovery submit");
    pendingMemory.childTypes[1]=childType;
    d.coldLogicalBytes=1;
    require(!warmRecoveryBlocksUses(d,true,empty) &&
            !warmRecoveryBlocksUses(d,true,hotDisjoint,true),
            "cold peer blocked a fully validated disjoint hot submission");
    const auto replacement=tokenHandle<VkDeviceMemory>(0xc2fa);
    d.retainedWarmRecoveries.push_back({valid.oldBacking,replacement,valid.sourceView,
                                        valid.destinationView,valid.oldType,valid.newType,valid.size});
    d.pendingWarmRecovery=valid; d.pendingWarmRecovery.active=true;
    stale=valid; ++stale.bindingGeneration;
    require(!finalizeWarmRecoveryLocked(d,stale) && d.gpuRestoreUnsafe &&
            d.gpuGateError==VK_ERROR_DEVICE_LOST && sparseBinds==0 && frees==0 &&
            d.retainedWarmRecoveries.size()==1,
            "stale pending recovery token rebound or freed owned backing resources");
}

void checkPendingKnownEmptyQueueCallSkipsColdRestore() {
    Fixture f(MiB,MiB); auto& d=f.device;
    f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord); d.handle=f.handle;
    d.gdpa=mockGetDeviceProcAddr; d.virtualEnabled=true; d.autoInitialized=true;
    d.residentAdmissionArmed=true; d.activeEviction=false; d.selectiveRestore=true;
    const auto queue=reinterpret_cast<VkQueue>(f.handle);
    const std::vector<VkQueue> queues{queue,d.sparseQueue};
    require(d.autoQueues.init(d.handle,d.gdpa,d.sparseQueue,queues,false)==VK_SUCCESS,
            "empty-signal pending fixture failed to initialize automatic queues");
    VirtualMemory cold{}; cold.size=MiB; cold.children={VK_NULL_HANDLE};
    cold.childSizes={MiB}; cold.childTypes={0}; cold.childGenerations={1};
    cold.poolViews={tokenHandle<VkBuffer>(0xd100)}; cold.poolViewMemoryTypeBits={1};
    cold.coldGroups.resize(1); cold.cold=true; cold.coldLogicalSize=MiB;
    cold.coldGroups[0].cold=true; cold.coldGroups[0].pristine=true;
    cold.coldGroups[0].logicalBytes=MiB; cold.coldGroups[0].lastUse=std::chrono::steady_clock::now();
    cold.backingMemoryTypeBits=1;
    d.virtualMemory.at(f.memory)=std::move(cold); d.coldLogicalBytes=MiB;
    d.pendingWarmRecovery.active=true; d.pendingWarmRecovery.memory=tokenHandle<VkDeviceMemory>(0xd101);
    VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const VkSubmitInfo* emptyInfo=&empty;
    applicationSubmitInfo=&empty;
    {
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices[key(f.handle)]=std::shared_ptr<Device>(&d,[](Device*){});
    }
    const auto oldUnknownProc=unknownCommandProc.exchange(false);
    std::vector<ActiveRefs::Use> emptyUses;
    const bool emptyKnown=queueMemories(d,"vkQueueSubmit",emptyUses,std::uint32_t{1},emptyInfo,VkFence{});
    if(!emptyKnown || !emptyUses.empty()) {
        unknownCommandProc.store(oldUnknownProc);
        std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(f.handle));
        applicationSubmitInfo=nullptr;
        require(false,"empty signal was not classified as known by the queue tracker");
    }
    const auto result=queueCall<PFN_vkQueueSubmit>(queue,"vkQueueSubmit",std::uint32_t{1},emptyInfo,VkFence{});
    unknownCommandProc.store(oldUnknownProc);
    {
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices.erase(key(f.handle));
    }
    applicationSubmitInfo=nullptr;
    const auto coldFound=d.virtualMemory.find(f.memory);
    const bool coldGroupRemains=coldFound!=d.virtualMemory.end() &&
        !coldFound->second.coldGroups.empty() && coldFound->second.coldGroups[0].cold;
    require(result==VK_SUCCESS && submitCalls==1 && mockDeviceWaitCalls==0 &&
            mockResetCommandPoolCalls==0 && allocations==0 && d.coldLogicalBytes==MiB &&
            coldGroupRemains && d.pendingWarmRecovery.active,
            "known-empty submit touched the cold restore/shared command-pool path while recovery was pending");
}

void checkPendingAdmissionRequestsFullPreflightRetry() {
    Fixture f(MiB,MiB); auto& d=f.device;
    d.pendingWarmRecovery.active=true; d.pendingWarmRecovery.memory=tokenHandle<VkDeviceMemory>(0xd201);
    VirtualMemory memory{}; memory.children={VK_NULL_HANDLE}; memory.childSizes={MiB}; memory.childTypes={0};
    d.virtualMemory.at(f.memory)=std::move(memory);
    d.residentBytes=MiB; d.coldLogicalBytes=MiB;
    VkBaseInStructure unknown{static_cast<VkStructureType>(0x7fffffff),nullptr};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext=&unknown;
    const auto result=admitTrackedSubmit(d,"vkQueueSubmit",1,&submit,VkFence{});
    require(result==VK_NOT_READY && d.pendingRecoveryAdmissionWait && allocations==0 &&
            d.residentBytes==MiB && d.coldLogicalBytes==MiB,
            "pending admission eviction did not request queue preflight retry without mutation");
}

void checkUnlockedRecoveryAbortsAfterExternalGate() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    d.unlockedWarmRecoveryWait=true;
    d.snapshot.createFence=mockCreateFence; d.snapshot.destroyFence=mockDestroyFence;
    d.snapshot.waitForFences=mockWaitForFences;
    require(coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && d.pendingWarmRecovery.active,
            "external-gate fixture did not begin fenced recovery");
    const auto sparseBeforeWait=sparseBinds;
    std::atomic<bool> finished{true};
    std::thread worker([&] {
        std::unique_lock<std::mutex> deviceLock(d.mutex),queueLock(d.queueMutex);
        finished=finishPendingWarmRecoveryLocked(d,deviceLock,queueLock);
    });
    bool fenceEntered=false;
    {
        std::unique_lock<std::mutex> lock(mockFenceMutex);
        fenceEntered=mockFenceCondition.wait_for(lock,std::chrono::seconds(5),[]{return mockFenceWaitEntered;});
    }
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.gpuGateError=VK_ERROR_DEVICE_LOST;
        d.autoEnabled=false; d.stopWorker.store(true);
    }
    {
        std::lock_guard<std::mutex> lock(mockFenceMutex);
        mockFenceWaitRelease=true;
    }
    mockFenceCondition.notify_all(); worker.join();
    const auto& retained=d.retainedWarmRecoveries.at(0);
    require(fenceEntered && !finished && !d.pendingWarmRecovery.active && d.gpuRestoreUnsafe &&
            d.gpuGateError==VK_ERROR_DEVICE_LOST && m.children[0]==c.nonlocal && m.childTypes[0]==1 &&
            liveAllocations.count(retained.oldBacking) && liveAllocations.count(retained.replacement) &&
            bufferSizes.count(retained.destinationView) && frees==0 && sparseBinds==sparseBeforeWait,
            "external device gate during fence wait allowed sparse commit or released retained backing");
}

void checkUnlockedRecoveryFenceWaitReleasesLocks() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    d.unlockedWarmRecoveryWait=true;
    d.snapshot.createFence=mockCreateFence; d.snapshot.destroyFence=mockDestroyFence;
    d.snapshot.waitForFences=mockWaitForFences;
    require(coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && d.pendingWarmRecovery.active &&
            m.children[0]==c.nonlocal && m.childTypes[0]==1 && d.retainedWarmRecoveries.size()==1,
            "unlocked recovery did not retain the old backing before waiting");
    std::atomic<bool> completed{false};
    std::thread worker([&] {
        std::unique_lock<std::mutex> deviceLock(d.mutex),queueLock(d.queueMutex);
        completed=finishPendingWarmRecoveryLocked(d,deviceLock,queueLock);
    });
    bool fenceEntered=false;
    {
        std::unique_lock<std::mutex> lock(mockFenceMutex);
        fenceEntered=mockFenceCondition.wait_for(lock,std::chrono::seconds(5),[]{return mockFenceWaitEntered;});
    }
    std::unique_lock<std::mutex> deviceProbe(d.mutex,std::try_to_lock);
    std::unique_lock<std::mutex> queueProbe(d.queueMutex,std::try_to_lock);
    const bool locksFree=deviceProbe.owns_lock() && queueProbe.owns_lock();
    if(deviceProbe.owns_lock()) deviceProbe.unlock();
    if(queueProbe.owns_lock()) queueProbe.unlock();
    std::atomic<bool> waiterStarted{false};
    VkResult waitResult=VK_ERROR_UNKNOWN;
    std::thread mutator([&] {
        std::unique_lock<std::mutex> lock(d.mutex);
        waiterStarted=true;
        waitResult=waitForPendingWarmRecovery(d,lock);
    });
    while(!waiterStarted.load()) std::this_thread::yield();
    bool mutexReleased=false;
    bool waiterCounted=false;
    for(unsigned i=0;i<1000;i++) {
        std::unique_lock<std::mutex> probe(d.mutex,std::try_to_lock);
        mutexReleased=probe.owns_lock();
#ifdef ZVRAM_TEST_ASYNC_HOOK
        if(mutexReleased) waiterCounted=d.warmRecoveryWaiters==1;
#endif
        if(mutexReleased) probe.unlock();
#ifdef ZVRAM_TEST_ASYNC_HOOK
        if(mutexReleased && waiterCounted) break;
#else
        if(mutexReleased) break;
#endif
        std::this_thread::yield();
    }
    {
        std::lock_guard<std::mutex> lock(mockFenceMutex);
        mockFenceWaitRelease=true;
    }
    mockFenceCondition.notify_all();
    worker.join(); mutator.join();
    require(fenceEntered && locksFree && mutexReleased && completed && waitResult==VK_SUCCESS && !d.pendingWarmRecovery.active &&
            m.childTypes[0]==0 && m.children[0]!=c.nonlocal && d.retainedWarmRecoveries.empty() &&
            !d.gpuRestoreUnsafe && d.liveOther==0,
            "fence completion failed to commit and release waiting mutators");
#ifdef ZVRAM_TEST_ASYNC_HOOK
    require(waiterCounted && d.warmRecoveryWaiters==0,
            "test-only recovery waiter counter did not track wait entry and wake");
#endif
}

void checkUnlockedRecoveryLateCapRollbackAndFenceFailure() {
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        d.unlockedWarmRecoveryWait=true;
        d.snapshot.createFence=mockCreateFence; d.snapshot.destroyFence=mockDestroyFence;
        d.snapshot.waitForFences=mockWaitForFences;
        require(coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && d.pendingWarmRecovery.active,
                "late-cap fixture did not begin a fenced recovery");
        d.liveControl.state.result=1; d.liveControl.state.requestedMiB=0;
        { std::lock_guard<std::mutex> lock(mockFenceMutex); mockFenceWaitRelease=true; }
        std::unique_lock<std::mutex> deviceLock(d.mutex),queueLock(d.queueMutex);
        require(!finishPendingWarmRecoveryLocked(d,deviceLock,queueLock) &&
                !d.pendingWarmRecovery.active && !d.gpuRestoreUnsafe && m.children[0]==c.nonlocal &&
                m.childTypes[0]==1 && d.liveLocal==0 && d.liveOther==MiB && frees==1 &&
                d.retainedWarmRecoveries.empty(),
                "late live-cap decrease committed a recovery instead of safely rolling it back");
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        d.unlockedWarmRecoveryWait=true;
        d.snapshot.createFence=mockCreateFence; d.snapshot.destroyFence=mockDestroyFence;
        d.snapshot.waitForFences=mockWaitForFences;
        require(coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && d.pendingWarmRecovery.active,
                "fence-failure fixture did not begin a fenced recovery");
        std::atomic<bool> finished{true};
        std::thread worker([&] {
            std::unique_lock<std::mutex> deviceLock(d.mutex),queueLock(d.queueMutex);
            finished=finishPendingWarmRecoveryLocked(d,deviceLock,queueLock);
        });
        bool fenceEntered=false;
        {
            std::unique_lock<std::mutex> lock(mockFenceMutex);
            fenceEntered=mockFenceCondition.wait_for(lock,std::chrono::seconds(5),[]{return mockFenceWaitEntered;});
            mockFenceWaitResult=VK_ERROR_DEVICE_LOST; mockFenceWaitRelease=true;
        }
        mockFenceCondition.notify_all(); worker.join();
        const auto& retained=d.retainedWarmRecoveries.at(0);
        require(fenceEntered && !finished && !d.pendingWarmRecovery.active && d.gpuRestoreUnsafe &&
                d.gpuGateError==VK_ERROR_DEVICE_LOST && m.children[0]==c.nonlocal &&
                liveAllocations.count(retained.oldBacking) && liveAllocations.count(retained.replacement) &&
                bufferSizes.count(retained.destinationView) && frees==0,
                "ambiguous copy-fence failure failed to gate or retain both backings and the view");
    }
}

void checkColdCycleIgnoresSnapshotBudgetBlock() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    d.coldCycleQuietMilliseconds=0; d.coldBudget=0;
    auto& group=m.coldGroups[0]; group.budgetBlocked=true;
    group.failedBudgetGeneration=d.coldBudgetGeneration;
    require(coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && m.childTypes[0]==0 &&
            d.coldBytes==0 && d.coldLogicalBytes==0 && d.freezeCount==0 && d.restoreCount==0,
            "snapshot-quota block incorrectly prevented direct recovery copy");
}
#ifdef ZVRAM_TEST_ASYNC_HOOK
void checkColdCycleTestPause() {
    ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
    d.testRecoveryPaused=true;
    VkDeviceMemory candidate{}; std::size_t child{};
    require(!selectColdCycleCandidate(d,2*MiB,std::chrono::steady_clock::now(),candidate,child) &&
            candidate==VK_NULL_HANDLE && child==0 &&
            !coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && allocations==0 && sparseBinds==0 &&
            m.children[0]==c.nonlocal && d.liveOther==MiB && d.liveLocal==0,
            "test-only pause did not hold recovery without changing the original backing");
}
#endif

void checkColdCycleQuietEnvParsing() {
    constexpr const char* name="ZVRAM_VULKAN_RECOVER_LOCAL_QUIET_MS";
    const char* previous=std::getenv(name);
    const bool hadPrevious=previous!=nullptr;
    const std::string saved=previous?previous:"";
    std::uint32_t value=1000;
    unsetenv(name);
    require(uint32EnvAllowZero(name,value) && value==1000,
            "unset recovery quiet delay changed the one-second default");
    setenv(name,"0",1);
    require(uint32EnvAllowZero(name,value) && value==0,
            "recovery quiet delay rejected zero");
    setenv(name,"4294967295",1);
    require(uint32EnvAllowZero(name,value) && value==UINT32_MAX,
            "recovery quiet delay rejected uint32 maximum");
    setenv(name,"4294967296",1);
    require(!uint32EnvAllowZero(name,value),"recovery quiet delay accepted uint32 overflow");
    value=1000;
    setenv(name,"-18446744073709551615",1);
    require(!uint32EnvAllowZero(name,value) && value==1000,
            "recovery quiet delay accepted a negative wrapped value or changed output");
    setenv(name,"+1",1);
    require(!uint32EnvAllowZero(name,value) && value==1000,
            "recovery quiet delay accepted a signed value or changed output");
    setenv(name," 1",1);
    require(!uint32EnvAllowZero(name,value) && value==1000,
            "recovery quiet delay accepted whitespace or changed output");
    setenv(name,"10ms",1);
    require(!uint32EnvAllowZero(name,value) && value==1000,
            "recovery quiet delay accepted malformed input or changed output");
    if(hadPrevious) setenv(name,saved.c_str(),1); else unsetenv(name);
}

void checkColdCycleSparseFailurePreservesBacking() {
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        m.bindings[0].alignment=0; // Reject before any VkQueueBindSparse call.
        require(!coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && sparseBinds==0 &&
                d.gpuGateError==VK_SUCCESS && !d.gpuRestoreUnsafe && m.children[0]==c.nonlocal &&
                d.liveOther==MiB && d.liveLocal==0 && frees==1 && d.retainedWarmRecoveries.empty() &&
                bufferSizes.size()==m.poolViews.size(),
                "pre-driver alias-plan failure gated the device or leaked temporary recovery resources");
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        failQueueWaitAt=1; // Initial combined alias-detach/private-view-bind batch is ambiguous.
        require(!coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) &&
                d.gpuRestoreUnsafe && d.gpuGateError==VK_ERROR_DEVICE_LOST &&
                sparseBinds==1 && queueWaitCalls==1 && m.children[0]==c.nonlocal &&
                m.childTypes[0]==1 && d.retainedWarmRecoveries.size()==1 && frees==0 &&
                liveAllocations.count(c.nonlocal) &&
                liveAllocations.count(d.retainedWarmRecoveries[0].replacement) &&
                bufferSizes.count(d.retainedWarmRecoveries[0].destinationView),
                "initial combined sparse wait failure published or freed potentially referenced resources");
    }
    for(unsigned stage=1;stage<=2;stage++) {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        failSparseBindAt=stage; // combined initial setup, then combined final transition
        require(!coldCyclePromoteLocked(d,c.f.memory,0,2*MiB),
                "direct recovery ignored an injected sparse transition failure");
        require(d.gpuRestoreUnsafe && d.gpuGateError==VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                d.retainedWarmRecoveries.size()==1 && frees==0,
                "sparse failure did not gate and retain replacement ownership");
        const auto& retained=d.retainedWarmRecoveries.front();
        require(retained.oldBacking==c.nonlocal && retained.replacement && retained.sourceView &&
                retained.destinationView && liveAllocations.count(retained.oldBacking) &&
                liveAllocations.count(retained.replacement) && bufferSizes.count(retained.destinationView),
                "sparse failure dropped an allocation or private view handle");
        require(m.children[0]==c.nonlocal,
                "failed sparse batch published the replacement child before completion");
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        failQueueWaitAt=3; // combined initial setup, copy, final combined sparse batch.
        require(!coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && d.gpuRestoreUnsafe &&
                d.gpuGateError==VK_ERROR_DEVICE_LOST && sparseBinds==2 && queueWaitCalls==3 &&
                m.children[0]==c.nonlocal && d.retainedWarmRecoveries.size()==1 && frees==0 &&
                liveAllocations.count(c.nonlocal) &&
                liveAllocations.count(d.retainedWarmRecoveries[0].replacement) &&
                bufferSizes.count(d.retainedWarmRecoveries[0].destinationView),
                "final sparse completion failure freed or published potentially referenced resources");
    }
}

void checkColdCycleRefusalPolicies() {
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        d.coldBudget=0; // Warm copy has no host-side raw snapshot quota.
        require(coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && m.childTypes[0]==0 &&
                d.coldBytes==0 && d.coldLogicalBytes==0 && d.freezeCount==0 && d.restoreCount==0,
                "direct warm promotion remained dependent on host cold-storage quota");
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        m.poolViewMemoryTypeBits[0]=2;
        require(!coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && allocations==0 &&
                m.children[0]==c.nonlocal && !m.coldGroups[0].cold,
                "cold-cycle ignored an incompatible pool-view type mask");
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        m.coldGroups[0].lastUse=std::chrono::steady_clock::now();
        require(d.coldCycleQuietMilliseconds==1000 &&
                !coldCyclePromoteLocked(d,c.f.memory,0,2*MiB) && allocations==0 &&
                m.children[0]==c.nonlocal,
                "cold-cycle ignored the quiet delay");
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        m.childSizes[0]=kAsyncSnapshotMaxRaw+MiB;
        require(!coldCyclePromoteLocked(d,c.f.memory,0,40*MiB) && allocations==0 &&
                m.children[0]==c.nonlocal && !m.coldGroups[0].cold,
                "cold-cycle ignored the per-pass size ceiling");
    }
}

void checkPressureOnlyAsyncCommitRechecksCurrentCap() {
    Fixture f(3*MiB,2*MiB); auto& d=f.device;
    d.pressureOnly=true; d.residentBytes=MiB;
    d.liveControl.state.result=1; d.liveControl.state.requestedMiB=1;
    VkDeviceSize target{};
    require(!pressureOnlyFreezeNeeded(d,target) && target==MiB,
            "pressure-only post-encode check ignored a pending live cap at current residency");
    d.liveControl.state.result=0; d.residentBytes=3*MiB;
    require(pressureOnlyFreezeNeeded(d,target) && target==2*MiB,
            "pressure-only post-encode check rejected a still-needed hard-cap eviction");
    d.pressureOnly=true; d.budgetReserveBytes=1; d.budgetProperties=mockBudgetProperties;
    d.budgetHeap=UINT32_MAX;
    require(!pressureOnlyFreezeNeeded(d,target),
            "pressure-only async commit proceeded after native-budget query failure");
    d.pressureOnly=false;
    require(!pressureOnlyFreezeNeeded(d,target),
            "pressure-only recheck changed legacy idle-mode behavior");
}

void checkSubmitUseTimestampScope() {
    Fixture f; auto& d=f.device; auto& memory=f.state();
    const auto old=std::chrono::steady_clock::time_point{}+std::chrono::seconds(1);
    const auto touched=old+std::chrono::seconds(1);
    memory.coldGroups.resize(3);
    memory.coldGroups[0].lastUse=memory.coldGroups[1].lastUse=memory.coldGroups[2].lastUse=old;
    memory.lastUse=old;
    require(memory.children.empty(),"timestamp fixture unexpectedly has resident children");
    const auto emptyHandle=tokenHandle<VkDeviceMemory>(0xd001);
    VirtualMemory empty{}; empty.lastUse=old;
    d.virtualMemory.emplace(emptyHandle,std::move(empty));
    const auto unrelatedHandle=tokenHandle<VkDeviceMemory>(0xd003);
    VirtualMemory unrelated{}; unrelated.lastUse=old; unrelated.coldGroups.resize(2);
    for(auto& group:unrelated.coldGroups) group.lastUse=old;
    d.virtualMemory.emplace(unrelatedHandle,std::move(unrelated));

    markSubmittedUseTimes(d,{{f.memory,1}},true,touched);
    require(memory.lastUse==touched && memory.coldGroups[0].lastUse==old &&
            memory.coldGroups[1].lastUse==touched && memory.coldGroups[2].lastUse==old &&
            memory.coldGroups[0].recoveryUses==0 && memory.coldGroups[1].recoveryUses==1 &&
            memory.coldGroups[2].recoveryUses==0 &&
            d.virtualMemory.at(unrelatedHandle).lastUse==old &&
            std::all_of(d.virtualMemory.at(unrelatedHandle).coldGroups.begin(),
                        d.virtualMemory.at(unrelatedHandle).coldGroups.end(),
                        [&](const auto& group){return group.lastUse==old;}),
            "known child use timestamped unrelated groups or missed its cold child");

    memory.lastUse=old;
    for(auto& group:memory.coldGroups) group.lastUse=old;
    markSubmittedUseTimes(d,{{f.memory,SIZE_MAX}},true,touched);
    require(memory.lastUse==touched && std::all_of(memory.coldGroups.begin(),memory.coldGroups.end(),
            [&](const auto& group){return group.lastUse==touched;}),
            "known wildcard use did not timestamp every group");

    memory.lastUse=old;
    for(auto& group:memory.coldGroups) group.lastUse=old;
    std::vector<std::pair<std::uint8_t,std::chrono::steady_clock::time_point>> beforeKnownEmpty;
    for(const auto& group:memory.coldGroups)
        beforeKnownEmpty.emplace_back(group.recoveryUses,group.recoveryUseEpoch);
    markSubmittedUseTimes(d,{},true,touched);
    require(memory.lastUse==old && d.virtualMemory.at(unrelatedHandle).lastUse==old &&
            std::all_of(memory.coldGroups.begin(),memory.coldGroups.end(),
                        [&](const auto& group){return group.lastUse==old;}) &&
            std::equal(memory.coldGroups.begin(),memory.coldGroups.end(),beforeKnownEmpty.begin(),
                [](const auto& group,const auto& prior) {
                    return group.recoveryUses==prior.first && group.recoveryUseEpoch==prior.second;
                }),
            "known empty use set mutated tracked timestamps");

    markSubmittedUseTimes(d,{{f.memory,99},{emptyHandle,SIZE_MAX},
                              {tokenHandle<VkDeviceMemory>(0xd002),0}},true,touched);
    require(memory.lastUse==old && d.virtualMemory.at(emptyHandle).lastUse==old &&
            d.virtualMemory.at(unrelatedHandle).lastUse==old &&
            std::all_of(memory.coldGroups.begin(),memory.coldGroups.end(),
            [&](const auto& group){return group.lastUse==old;}) &&
            std::all_of(d.virtualMemory.at(unrelatedHandle).coldGroups.begin(),
                        d.virtualMemory.at(unrelatedHandle).coldGroups.end(),
                        [&](const auto& group){return group.lastUse==old;}),
            "invalid, empty, or missing known use mutated timestamps");

    std::vector<std::pair<std::uint8_t,std::chrono::steady_clock::time_point>> beforeUnknown;
    for(const auto& group:memory.coldGroups)
        beforeUnknown.emplace_back(group.recoveryUses,group.recoveryUseEpoch);
    markSubmittedUseTimes(d,{},false,touched);
    require(memory.lastUse==touched && std::all_of(memory.coldGroups.begin(),memory.coldGroups.end(),
            [&](const auto& group){return group.lastUse==touched;}) &&
            std::equal(memory.coldGroups.begin(),memory.coldGroups.end(),beforeUnknown.begin(),
                [](const auto& group,const auto& prior) {
                    return group.recoveryUses==prior.first && group.recoveryUseEpoch==prior.second;
                }) &&
            d.virtualMemory.at(emptyHandle).lastUse==old &&
            d.virtualMemory.at(unrelatedHandle).lastUse==touched &&
            std::all_of(d.virtualMemory.at(unrelatedHandle).coldGroups.begin(),
                        d.virtualMemory.at(unrelatedHandle).coldGroups.end(),
                        [&](const auto& group){return group.lastUse==touched;}),
            "unknown-use fallback did not timestamp every nonempty tracked memory");
}

void checkUnknownSubmitAdmission() {
    Fixture f(3*MiB,2*MiB); f.bind(0,3*MiB);
    // Unknown submit chains must admit the entire allocation conservatively.
    VkBaseInStructure unknown{}; unknown.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext=&unknown;
    require(admitTrackedSubmit(f.device,"vkQueueSubmit",1,&submit,VK_NULL_HANDLE)==VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "unknown submit bypassed the resident cap");
    require(allocations==0 && f.device.residentBytes==0 && f.device.coldLogicalBytes==3*MiB,
            "rejected submit materialized pristine backing");
    f.device.residentLimitBytes=3*MiB;
    require(admitTrackedSubmit(f.device,"vkQueueSubmit",1,&submit,VK_NULL_HANDLE)==VK_SUCCESS,
            "exact-cap unknown submit was not admitted");
    require(restoreForQueue(f.device,"vkQueueSubmit",1,&submit,VK_NULL_HANDLE)==VK_SUCCESS,
            "admitted unknown submit did not conservatively restore pristine backing");
    require(f.device.residentBytes==3*MiB && !f.state().cold,
            "admitted unknown submit restored the wrong backing amount");
}

void checkBudgetPreflightRetry() {
    auto setup=[&](Fixture& f,bool activeEviction=false) {
        f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord);
        auto& d=f.device; d.handle=f.handle; d.gdpa=mockGetDeviceProcAddr;
        d.virtualEnabled=true; d.autoInitialized=true; d.activeEviction=activeEviction;
        d.residentAdmissionArmed=true; d.budgetProperties=mockBudgetProperties; d.budgetHeap=0;
        d.budgetReserveBytes=1; d.memory.memoryHeapCount=1; d.memory.memoryHeaps[0].size=2*MiB;
        mockNativeHeapSize=2*MiB;
        const auto queue=reinterpret_cast<VkQueue>(f.handle);
        const std::vector<VkQueue> queues{queue,d.sparseQueue};
        require(d.autoQueues.init(d.handle,d.gdpa,d.sparseQueue,queues,activeEviction)==VK_SUCCESS,
                "mock automatic queue setup failed");
        if(activeEviction) d.restoreQueueGenerations.emplace_back(queue,0);
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices[key(f.handle)]=std::shared_ptr<Device>(&d,[](Device*){});
        return queue;
    };
    auto unregister=[](Fixture& f) {
        std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(f.handle));
    };
    auto submit=[](VkBaseInStructure& chain) {
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO}; info.pNext=&chain;
        return info;
    };

    {
        Fixture f(MiB,MiB); f.bind(0,MiB); auto& d=f.device; const auto queue=setup(f);
        VkBaseInStructure unknown{static_cast<VkStructureType>(0x7fffffff),nullptr};
        auto info=submit(unknown); applicationSubmitPnext=&unknown;
        const auto* infoPtr=static_cast<const VkSubmitInfo*>(&info);
        mockBudgetSequence={{2*MiB,0},{MiB,0},{2*MiB,0},{2*MiB,0}};
        const auto result=queueCall<PFN_vkQueueSubmit>(queue,"vkQueueSubmit",std::uint32_t{1},infoPtr,VkFence{});
        require(result==VK_SUCCESS && submitCalls==1 && budgetQueries==4,
                "fresh admission did not retry a preflight-only refusal exactly once");
        require(allocations==1 && d.residentBytes==MiB && !f.state().cold &&
                d.gpuGateError==VK_SUCCESS && !d.restoreBudgetRefused,
                "successful budget retry did not restore cleanly");
        unregister(f); applicationSubmitPnext=nullptr;
    }
    {
        Fixture f(3*MiB,3*MiB); const auto buffer=f.bind(0,3*MiB);
        auto& d=f.device; const auto queue=setup(f,true); auto& m=f.state();
        d.memory.memoryHeaps[0].size=4*MiB; mockNativeHeapSize=4*MiB;
        d.snapshot.stagingSize=MiB; d.snapshot.chunkSize=MiB;
        d.snapshot.stagingBuffer=tokenHandle<VkBuffer>(0x8800);
        d.snapshot.commandPool=tokenHandle<VkCommandPool>(0x8801);
        d.snapshot.commandBuffer=tokenHandle<VkCommandBuffer>(0x8802);
        auto staging=std::make_unique<std::vector<std::uint8_t>>(MiB);
        d.snapshot.mapped=staging->data();
        d.coldBudget=4*MiB; d.minSavingsPercent=0; d.narrowDescriptorRanges=true;
        for(std::size_t i=0;i<2;i++) {
            auto& group=m.coldGroups[i]; group.pristine=false;
            VirtualMemory::ColdChunk chunk; chunk.bytes.assign(MiB,static_cast<std::uint8_t>(0x31+i));
            chunk.rawSize=MiB; group.chunks.push_back(std::move(chunk)); group.storedBytes=MiB;
            m.coldStoredBytes+=MiB; d.coldBytes+=MiB;
        }
        // Child 2 starts resident but unselected; the budget retry must evict it.
        m.children[2]=tokenHandle<VkDeviceMemory>(0x8820); m.childTypes[2]=0;
        m.coldGroups[2].cold=false; m.coldGroups[2].pristine=false;
        m.coldGroups[2].logicalBytes=0; m.coldLogicalSize-=MiB; d.coldLogicalBytes-=MiB;
        m.residentBytes=MiB; d.residentBytes=MiB;
        d.selectiveRestore=true;
        d.promotedBuffers[buffer]=PromotedBuffer{};
        d.promotedBuffers[buffer].size=2*MiB; d.promotedBuffers[buffer].memory=f.memory;
        const auto command=tokenHandle<VkCommandBuffer>(0x8830);
        VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandAllocation.commandPool=tokenHandle<VkCommandPool>(0x8831);
        commandAllocation.commandBufferCount=1; d.submission.allocateCommands(&commandAllocation,&command);
        d.submission.beginCommand(command); d.submission.bufferRange(command,buffer,0,2*MiB,true);
        mockBudgetSequence={{4*MiB,MiB},{4*MiB,MiB},{2*MiB+MiB/2+1,MiB},
                            {2*MiB+MiB/2+1,2*MiB},{2*MiB+MiB/2+1,MiB}};
        VkSubmitInfo tracked{VK_STRUCTURE_TYPE_SUBMIT_INFO}; tracked.commandBufferCount=1;
        tracked.pCommandBuffers=&command; applicationSubmitInfo=&tracked;
        const auto* trackedInfo=static_cast<const VkSubmitInfo*>(&tracked);
        const auto result=queueCall<PFN_vkQueueSubmit>(queue,"vkQueueSubmit",std::uint32_t{1},trackedInfo,VkFence{});
        require(result==VK_SUCCESS && submitCalls==1 && budgetQueries==5,
                "budget shrink after partial selective restore did not re-admit and retry once");
        require(m.children[0] && !m.coldGroups[0].cold && m.children[1] && !m.coldGroups[1].cold,
                "retry lost a selected child already restored or failed to restore the next child");
        require(!m.children[2] && m.coldGroups[2].cold && m.coldGroups[2].logicalBytes==MiB &&
                d.residentBytes==2*MiB && d.residentBytes<=2*MiB+MiB/2,
                "re-admission did not evict the unselected victim within the refreshed budget");
        require(d.gpuGateError==VK_SUCCESS && !d.restoreBudgetRefused,
                "successful partial-restore retry left a gate or budget marker set");
        unregister(f); applicationSubmitInfo=nullptr;
        d.snapshot.mapped=nullptr;
    }
    {
        Fixture f(MiB,MiB); f.bind(0,MiB); auto& d=f.device; const auto queue=setup(f);
        VkBaseInStructure unknown{static_cast<VkStructureType>(0x7fffffff),nullptr};
        auto info=submit(unknown); applicationSubmitPnext=&unknown;
        const auto* infoPtr=static_cast<const VkSubmitInfo*>(&info);
        mockBudgetSequence={{2*MiB,0},{MiB,0},{MiB,0}};
        const auto result=queueCall<PFN_vkQueueSubmit>(queue,"vkQueueSubmit",std::uint32_t{1},infoPtr,VkFence{});
        require(result==VK_ERROR_OUT_OF_DEVICE_MEMORY && submitCalls==0 && budgetQueries==3 &&
                allocations==0 && f.state().cold,
                "persistent budget refusal retried more than once or reached the app submit");
        unregister(f); applicationSubmitPnext=nullptr;
    }
    {
        Fixture f(MiB,MiB); f.bind(0,MiB); auto& d=f.device; const auto queue=setup(f);
        VkBaseInStructure unknown{static_cast<VkStructureType>(0x7fffffff),nullptr};
        auto info=submit(unknown); applicationSubmitPnext=&unknown;
        const auto* infoPtr=static_cast<const VkSubmitInfo*>(&info);
        mockBudgetSequence={{2*MiB,0},{2*MiB,0}}; failAllocations=1;
        const auto result=queueCall<PFN_vkQueueSubmit>(queue,"vkQueueSubmit",std::uint32_t{1},infoPtr,VkFence{});
        require(result==VK_ERROR_OUT_OF_DEVICE_MEMORY && allocations==1 && budgetQueries==2 &&
                submitCalls==0 && d.gpuGateError==VK_SUCCESS && !d.restoreBudgetRefused && f.state().cold,
                "non-budget allocation failure was retried or altered the cold group");
        unregister(f); applicationSubmitPnext=nullptr;
    }
    {
        Fixture f(MiB,MiB); f.bind(0,MiB); auto& d=f.device; const auto queue=setup(f);
        VkBaseInStructure unknown{static_cast<VkStructureType>(0x7fffffff),nullptr};
        auto info=submit(unknown); applicationSubmitPnext=&unknown;
        const auto* infoPtr=static_cast<const VkSubmitInfo*>(&info);
        mockBudgetSequence={{2*MiB,0},{2*MiB,0}}; failSparseBinds=1;
        const auto result=queueCall<PFN_vkQueueSubmit>(queue,"vkQueueSubmit",std::uint32_t{1},infoPtr,VkFence{});
        require(result==VK_ERROR_OUT_OF_DEVICE_MEMORY && allocations==1 && budgetQueries==2 &&
                submitCalls==0 && d.gpuGateError==VK_ERROR_DEVICE_LOST && !d.restoreBudgetRefused,
                "sticky sparse failure was retried or app work was forwarded");
        unregister(f); applicationSubmitPnext=nullptr;
    }
}

void checkResidentBudgetAccounting() {
    Fixture f(4*MiB,12*MiB);
    auto& d=f.device;
    d.budgetProperties=mockBudgetProperties; d.budgetHeap=0; d.budgetReserveBytes=MiB;
    d.memory.memoryHeapCount=2;
    d.memory.memoryHeaps[0].size=10*MiB; // clamp budget to native heap size
    d.memory.memoryHeaps[1].size=64*MiB; // synthetic Vulkan heap is deliberately much larger
    d.memory.memoryTypes[0].heapIndex=0;
    VirtualMemory tracked{}; tracked.size=3*MiB; tracked.nativeTypeBits=1;
    tracked.children={tokenHandle<VkDeviceMemory>(11)};
    tracked.childSizes={3*MiB}; tracked.childTypes={0};
    d.virtualMemory.emplace(tokenHandle<VkDeviceMemory>(12),std::move(tracked));
    mockNativeHeapSize=10*MiB; mockHeapBudget=12*MiB; mockHeapUsage=7*MiB;
    VkDeviceSize limit{};
    require(residentAdmissionLimit(d,limit)==VK_SUCCESS,"native budget query failed");
    // Native total is min(12 MiB budget, 10 MiB heap); usage contains 3 MiB
    // tracked + 4 MiB untracked. Reserve 1 MiB leaves 5 MiB for our backing.
    require(limit==5*MiB && budgetQueries==1,
            "native heap clamp, tracked subtraction, or untracked usage accounting is wrong");
}

void checkBudgetRefusalBeforeAllocation() {
    Fixture f(2*MiB,2*MiB); auto& d=f.device;
    f.bind(0,2*MiB);
    d.budgetProperties=mockBudgetProperties; d.budgetHeap=0; d.budgetReserveBytes=MiB;
    d.memory.memoryHeapCount=1; d.memory.memoryHeaps[0].size=8*MiB;
    mockNativeHeapSize=8*MiB; mockHeapBudget=8*MiB; mockHeapUsage=7*MiB;
    require(restoreColdLocked(f.handle,d,f.memory)==VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "raw budget did not refuse restore exceeding available backing budget");
    require(allocations==0 && liveAllocations.empty() && d.residentBytes==0 &&
            d.coldLogicalBytes==2*MiB && budgetQueries==1,
            "raw-budget refusal allocated backing or changed cold accounting");
}

void checkDefaultBudgetDoesNotQuery() {
    Fixture f(2*MiB,MiB);
    f.device.budgetProperties=mockBudgetProperties; // zero reserve disables the optional query
    VkDeviceSize limit{};
    require(residentAdmissionLimit(f.device,limit)==VK_SUCCESS && limit==MiB && budgetQueries==0,
            "default resident admission queried hardware budget or changed the hard cap");
}

void checkAutoSparseBindUsesDedicatedQueue() {
    tracedQueueOperations.clear();
    const auto device=tokenHandle<VkDevice>(0x1a01);
    const auto appQueue=tokenHandle<VkQueue>(0x1a02);
    const auto copyQueue=tokenHandle<VkQueue>(0x1a03);
    const auto sparseQueue=tokenHandle<VkQueue>(0x1a04);
    ZvramAutoQueues queues;
    require(queues.init(device,traceQueueGetDeviceProcAddr,copyQueue,{appQueue,copyQueue},false,sparseQueue)==VK_SUCCESS,
            "dedicated sparse queue fixture failed to initialize");
    VkSparseMemoryBind memoryBind{};
    memoryBind.size=4096;
    VkSparseBufferMemoryBindInfo bufferBind{};
    bufferBind.buffer=tokenHandle<VkBuffer>(0x1a05);
    bufferBind.bindCount=1;
    bufferBind.pBinds=&memoryBind;
    require(queues.sparseBind(bufferBind)==VK_SUCCESS,"dedicated sparse bind failed");
    require(tracedQueueOperations.size()==4,"dedicated sparse bind emitted an unexpected queue sequence");
    const auto& before=tracedQueueOperations[0];
    const auto& bind=tracedQueueOperations[1];
    const auto& after=tracedQueueOperations[2];
    const auto& idle=tracedQueueOperations[3];
    require(before.kind=='S' && before.queue==copyQueue && before.signal,
            "pre-bind semaphore signal did not stay on the copy queue");
    require(bind.kind=='B' && bind.queue==sparseQueue && bind.wait==before.signal && bind.signal,
            "sparse bind did not use the dedicated queue and semaphore bridge");
    require(after.kind=='S' && after.queue==copyQueue && after.wait==bind.signal,
            "post-bind semaphore wait did not return to the copy queue");
    require(idle.kind=='I' && idle.queue==copyQueue,
            "sparse-bind completion did not wait on the copy queue bridge");
    bool marked=false;
    require(queues.submitted(sparseQueue,marked)==VK_ERROR_FEATURE_NOT_PRESENT &&
            tracedQueueOperations.size()==4,
            "internal sparse queue was incorrectly added to application queue markers");
    queues.destroy();
}

void registerPresentFixture(Fixture& f,bool enabled=true) {
    // Dispatchable Vulkan handles point at objects whose first word is a
    // dispatch pointer; key() intentionally follows that same representation.
    f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord);
    auto& d=f.device;
    d.handle=f.handle; d.gdpa=mockGetDeviceProcAddr; d.autoInitialized=true; d.autoEnabled=true;
    d.bufferPresentation=enabled; d.virtualEnabled=true; d.activeEviction=true;
    const auto queue=reinterpret_cast<VkQueue>(f.handle);
    d.copyQueue=tokenHandle<VkQueue>(0x5555);
    d.restoreQueueGenerations.emplace_back(queue,0);
    require(d.autoQueues.init(f.handle,mockGetDeviceProcAddr,d.copyQueue,{queue,d.copyQueue},true)==VK_SUCCESS,
            "mock auto-queue initialization failed");
    std::lock_guard<std::mutex> lock(mapsMutex);
    devices[key(f.handle)]=std::shared_ptr<Device>(&d,[](Device*){});
}
void unregisterPresentFixture(Fixture& f) {
    std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(f.handle));
}
VkPresentInfoKHR makePresentInfo(VkSemaphore& semaphore,VkSwapchainKHR& swapchain,std::uint32_t& index) {
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount=1; info.pWaitSemaphores=&semaphore;
    info.swapchainCount=1; info.pSwapchains=&swapchain; info.pImageIndices=&index;
    return info;
}

void checkBasePresentPassthrough() {
    Fixture f(2*MiB,MiB); f.bind(0,2*MiB); registerPresentFixture(f);
    auto& d=f.device; const auto q=reinterpret_cast<VkQueue>(f.handle);
    VkSemaphore semaphore=tokenHandle<VkSemaphore>(31); VkSwapchainKHR swapchain=tokenHandle<VkSwapchainKHR>(32);
    std::uint32_t index=1; auto info=makePresentInfo(semaphore,swapchain,index);
    presentResult=VK_SUBOPTIMAL_KHR;
    require(layerQueuePresent(q,&info)==VK_SUBOPTIMAL_KHR && presentCalls==1 && forwardedPresent==&info,
            "base present did not forward the original info/status");
    require(info.pWaitSemaphores==&semaphore && info.pSwapchains==&swapchain && info.pImageIndices==&index &&
            semaphore==tokenHandle<VkSemaphore>(31) && swapchain==tokenHandle<VkSwapchainKHR>(32) && index==1,
            "base present modified caller semaphore or swapchain data");
    presentResult=VK_ERROR_OUT_OF_DATE_KHR;
    require(layerQueuePresent(q,&info)==VK_ERROR_OUT_OF_DATE_KHR && presentCalls==2 && forwardedPresent==&info,
            "base present changed OUT_OF_DATE status");
    require(allocations==0 && sparseBinds==0 && d.residentBytes==0 && d.coldLogicalBytes==2*MiB &&
            f.state().cold && d.autoEnabled,
            "base present restored cold buffers or disabled paging");
    unregisterPresentFixture(f);
}

void checkPresentGateAndFallback() {
    {
        Fixture f(2*MiB,MiB); f.bind(0,2*MiB); registerPresentFixture(f);
        auto& d=f.device; d.gpuGateError=VK_ERROR_DEVICE_LOST;
        VkSemaphore semaphore{}; VkSwapchainKHR swapchain{}; std::uint32_t index{};
        auto info=makePresentInfo(semaphore,swapchain,index);
        require(layerQueuePresent(reinterpret_cast<VkQueue>(f.handle),&info)==VK_ERROR_DEVICE_LOST && presentCalls==0,
                "present ignored a poisoned device gate");
        unregisterPresentFixture(f);
    }
    {
        Fixture f(2*MiB,MiB); f.bind(0,2*MiB); registerPresentFixture(f);
        auto& d=f.device; VkSemaphore semaphore{}; VkSwapchainKHR swapchain{}; std::uint32_t index{};
        auto info=makePresentInfo(semaphore,swapchain,index); presentResult=VK_ERROR_DEVICE_LOST;
        const auto q=reinterpret_cast<VkQueue>(f.handle);
        require(layerQueuePresent(q,&info)==VK_ERROR_DEVICE_LOST && presentCalls==1 &&
                d.gpuGateError==VK_ERROR_DEVICE_LOST && !d.autoEnabled,
                "downstream device loss did not poison the present path");
        require(layerQueuePresent(q,&info)==VK_ERROR_DEVICE_LOST && presentCalls==1,
                "poisoned present path called downstream again");
        unregisterPresentFixture(f);
    }
    for(bool unknownChain:{false,true}) {
        Fixture f(2*MiB,2*MiB); f.bind(0,2*MiB);
        auto& d=f.device; registerPresentFixture(f,unknownChain);
        const auto q=reinterpret_cast<VkQueue>(f.handle);
        VkSemaphore semaphore{}; VkSwapchainKHR swapchain{}; std::uint32_t index{};
        auto info=makePresentInfo(semaphore,swapchain,index);
        VkBaseInStructure extension{static_cast<VkStructureType>(0x7fffffff),nullptr};
        if(unknownChain) info.pNext=&extension;
        require(layerQueuePresent(q,&info)==VK_SUCCESS && presentCalls==1 && forwardedPresent==&info,
                "default/unknown present fallback did not forward downstream");
        require(allocations==2 && !d.coldLogicalBytes && d.residentBytes==2*MiB && !d.autoEnabled,
                "default/unknown present fallback did not restore and disable paging");
        unregisterPresentFixture(f);
    }
}

void checkPresentMetadataAllowlist() {
    for(unsigned accepted=0;accepted<3;accepted++) {
        Fixture f(2*MiB,2*MiB); f.bind(0,2*MiB); registerPresentFixture(f);
        auto& d=f.device;
        VkSemaphore semaphore{}; VkSwapchainKHR swapchain{}; std::uint32_t index{};
        auto info=makePresentInfo(semaphore,swapchain,index);
        VkPresentIdKHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
        VkPresentRegionsKHR regions{VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
        VkPresentRegionKHR region{};
        presentId.swapchainCount=regions.swapchainCount=info.swapchainCount;
        regions.pRegions=&region;
        // The present-ID array is optional; the region array has one empty entry.
        if(accepted==0) info.pNext=&presentId;
        else if(accepted==1) info.pNext=&regions;
        else { presentId.pNext=&regions; info.pNext=&presentId; }
        const auto originalChain=info.pNext;
        const auto q=reinterpret_cast<VkQueue>(f.handle);
        require(layerQueuePresent(q,&info)==VK_SUCCESS && presentCalls==1 && forwardedPresent==&info &&
                info.pNext==originalChain,
                "allowlisted present metadata did not forward the original chain");
        require(allocations==0 && f.state().cold && d.autoEnabled,
                "allowlisted present metadata restored cold backing or disabled paging");
        unregisterPresentFixture(f);
    }
    for(unsigned rejected=0;rejected<6;rejected++) {
        Fixture f(2*MiB,2*MiB); f.bind(0,2*MiB); registerPresentFixture(f);
        auto& d=f.device;
        VkSemaphore semaphore{}; VkSwapchainKHR swapchain{}; std::uint32_t index{};
        auto info=makePresentInfo(semaphore,swapchain,index);
        VkPresentIdKHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
        VkPresentRegionsKHR regions{VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
        VkBaseInStructure unknown{static_cast<VkStructureType>(0x7fffffff),nullptr};
        presentId.swapchainCount=regions.swapchainCount=info.swapchainCount;
        VkPresentIdKHR duplicate{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
        duplicate.swapchainCount=info.swapchainCount;
        if(rejected==0) info.pNext=&unknown;
        else if(rejected==1) { presentId.pNext=&duplicate; info.pNext=&presentId; }
        else if(rejected==2) { presentId.pNext=&presentId; info.pNext=&presentId; }
        else if(rejected==3) { presentId.pNext=&regions; regions.pNext=&presentId; info.pNext=&presentId; }
        else if(rejected==4) { presentId.swapchainCount++; info.pNext=&presentId; }
        else { regions.swapchainCount++; info.pNext=&regions; }
        const auto originalChain=info.pNext;
        const auto q=reinterpret_cast<VkQueue>(f.handle);
        require(layerQueuePresent(q,&info)==VK_SUCCESS && presentCalls==1 && forwardedPresent==&info &&
                info.pNext==originalChain,
                "rejected present chain was not forwarded unchanged by the fallback");
        require(allocations==2 && d.autoEnabled==false && d.residentBytes==2*MiB &&
                d.coldLogicalBytes==0,
                "rejected present chain bypassed conservative restore/disable fallback");
        unregisterPresentFixture(f);
    }
    auto checkAccepted=[&](const void* chain) {
        Fixture f(2*MiB,2*MiB); f.bind(0,2*MiB); registerPresentFixture(f);
        VkSemaphore semaphore{}; VkSwapchainKHR swapchain{}; std::uint32_t index{};
        auto info=makePresentInfo(semaphore,swapchain,index); info.pNext=chain;
        const auto q=reinterpret_cast<VkQueue>(f.handle);
        require(layerQueuePresent(q,&info)==VK_SUCCESS && presentCalls==1 && forwardedPresent==&info &&
                info.pNext==chain && allocations==0 && f.state().cold && f.device.autoEnabled,
                "supported modern present metadata did not preserve paging fast path");
        unregisterPresentFixture(f);
    };
#ifdef VK_KHR_present_id2
    {
        const std::uint64_t ids[]{0};
        VkPresentId2KHR id{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR}; id.swapchainCount=1; id.pPresentIds=ids;
        checkAccepted(&id);
    }
#endif
#ifdef VK_GOOGLE_display_timing
    {
        const VkPresentTimeGOOGLE times[]{{0,0}};
        VkPresentTimesInfoGOOGLE timing{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE}; timing.swapchainCount=1; timing.pTimes=times;
        checkAccepted(&timing);
    }
#endif
#ifdef VK_EXT_present_timing
    {
        const VkPresentTimingInfoEXT times[]{{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT}};
        VkPresentTimingsInfoEXT timing{VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT}; timing.swapchainCount=1; timing.pTimingInfos=times;
        checkAccepted(&timing);
    }
#endif
#if defined(VK_KHR_swapchain_maintenance1)
    {
        const VkFence fences[]{VK_NULL_HANDLE}; const VkPresentModeKHR modes[]{VK_PRESENT_MODE_FIFO_KHR};
        VkSwapchainPresentFenceInfoKHR fence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR};
        VkSwapchainPresentModeInfoKHR mode{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR};
        fence.swapchainCount=mode.swapchainCount=1; fence.pFences=fences; mode.pPresentModes=modes;
        fence.pNext=&mode;
        checkAccepted(&fence);
    }
#elif defined(VK_EXT_swapchain_maintenance1)
    {
        const VkFence fences[]{VK_NULL_HANDLE}; const VkPresentModeKHR modes[]{VK_PRESENT_MODE_FIFO_KHR};
        VkSwapchainPresentFenceInfoEXT fence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        VkSwapchainPresentModeInfoEXT mode{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT};
        fence.swapchainCount=mode.swapchainCount=1; fence.pFences=fences; mode.pPresentModes=modes;
        fence.pNext=&mode;
        checkAccepted(&fence);
    }
#endif
    for(unsigned rejected=0;rejected<3;rejected++) {
        Fixture f(2*MiB,2*MiB); f.bind(0,2*MiB); registerPresentFixture(f);
        VkSemaphore semaphore{}; VkSwapchainKHR swapchain{}; std::uint32_t index{};
        auto info=makePresentInfo(semaphore,swapchain,index);
        VkPresentIdKHR id{VK_STRUCTURE_TYPE_PRESENT_ID_KHR}; id.swapchainCount=1;
        VkPresentRegionsKHR regions{VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR}; regions.swapchainCount=1;
        VkPresentIdKHR duplicate{VK_STRUCTURE_TYPE_PRESENT_ID_KHR}; duplicate.swapchainCount=1;
        if(rejected==0) { id.pNext=&duplicate; info.pNext=&id; }
        else if(rejected==1) { id.pNext=&regions; regions.pNext=&id; info.pNext=&id; }
        else { id.swapchainCount=2; info.pNext=&id; }
        const auto q=reinterpret_cast<VkQueue>(f.handle);
        require(layerQueuePresent(q,&info)==VK_SUCCESS && presentCalls==1 && !f.device.autoEnabled &&
                allocations==2,
                "duplicate, cyclic, or mismatched present metadata bypassed conservative fallback");
        unregisterPresentFixture(f);
    }
#ifdef VK_KHR_present_id2
    for(bool duplicate:{false,true}) {
        Fixture f(2*MiB,2*MiB); f.bind(0,2*MiB); registerPresentFixture(f);
        VkSemaphore semaphore{}; VkSwapchainKHR swapchain{}; std::uint32_t index{};
        const std::uint64_t ids[]{0};
        VkPresentId2KHR first{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR}; first.swapchainCount=1; first.pPresentIds=ids;
        VkPresentId2KHR second{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR}; second.swapchainCount=duplicate?1:2; second.pPresentIds=ids;
        if(duplicate) first.pNext=&second;
        auto info=makePresentInfo(semaphore,swapchain,index); info.pNext=duplicate?static_cast<const void*>(&first):static_cast<const void*>(&second);
        require(layerQueuePresent(reinterpret_cast<VkQueue>(f.handle),&info)==VK_SUCCESS &&
                presentCalls==1 && !f.device.autoEnabled && !f.device.coldLogicalBytes,
                "duplicate or mismatched present-id2 metadata bypassed conservative fallback");
        unregisterPresentFixture(f);
    }
#endif
#if defined(VK_KHR_swapchain_maintenance1) || defined(VK_EXT_swapchain_maintenance1)
    {
        Fixture f(2*MiB,2*MiB); f.bind(0,2*MiB); registerPresentFixture(f);
        VkSemaphore semaphore{}; VkSwapchainKHR swapchain{}; std::uint32_t index{};
        const VkPresentModeKHR modes[]{VK_PRESENT_MODE_FIFO_KHR};
#if defined(VK_KHR_swapchain_maintenance1)
        VkSwapchainPresentModeInfoKHR first{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR};
        VkSwapchainPresentModeInfoKHR second{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR};
#else
        VkSwapchainPresentModeInfoEXT first{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT};
        VkSwapchainPresentModeInfoEXT second{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT};
#endif
        first.swapchainCount=second.swapchainCount=1; first.pPresentModes=second.pPresentModes=modes;
        first.pNext=&second;
        auto info=makePresentInfo(semaphore,swapchain,index); info.pNext=&first;
        require(layerQueuePresent(reinterpret_cast<VkQueue>(f.handle),&info)==VK_SUCCESS &&
                presentCalls==1 && !f.device.autoEnabled && !f.device.coldLogicalBytes,
                "duplicate present-mode metadata bypassed conservative fallback");
        unregisterPresentFixture(f);
    }
#endif
}

void checkDescriptorHeapRemainsUnmanaged() {
#if defined(VK_EXT_descriptor_heap) && ((defined(VK_KHR_extended_flags) || defined(VK_KHR_maintenance5)) && (defined(VK_KHR_buffer_device_address) || defined(VK_VERSION_1_2)) || defined(VK_VERSION_1_4))
    Fixture f(2*MiB,2*MiB);
    f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord);
    auto& d=f.device; d.handle=f.handle; d.gdpa=mockGetDeviceProcAddr;
    d.createBuffer=mockCreateBuffer; d.virtualEnabled=true; d.bufferDeviceAddressEnabled=true;
    {
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices[key(f.handle)]=std::shared_ptr<Device>(&d,[](Device*){});
    }
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size=8*MiB;
    ci.usage=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT;
    VkBuffer buffer{};
    require(!eligibleBuffer(d,&ci) && layerCreateBuffer(f.handle,&ci,nullptr,&buffer)==VK_SUCCESS &&
            buffer && bufferSizes.count(buffer) && !d.promotedBuffers.count(buffer),
            "descriptor-heap buffer was incorrectly promoted into sparse recovery");
    d.selectiveRestore=true;
    const auto commandBuffer=reinterpret_cast<VkCommandBuffer>(&f.dispatchWord);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandBufferCount=1;
    d.submission.allocateCommands(&allocate,&commandBuffer);
    d.submission.beginCommand(commandBuffer);
    const auto command=reinterpret_cast<PFN_vkCmdBindSamplerHeapEXT>(layerGetDeviceProcAddr(f.handle,"vkCmdBindSamplerHeapEXT"));
    VkBindHeapInfoEXT bindInfo{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    const auto oldUnknown=unknownCommandProc.exchange(false);
    require(command==trackedvkCmdBindSamplerHeapEXT &&
            reinterpret_cast<PFN_vkCmdBindSamplerHeapEXT>(lookup("vkCmdBindSamplerHeapEXT"))==command,
            "descriptor heap command lost its conservative tracker wrapper");
    command(commandBuffer,&bindInfo);
    std::vector<ActiveRefs::Use> uses;
    require(samplerHeapBindCalls==1 && !commandMemories(d,{commandBuffer},uses,false) && uses.empty(),
            "descriptor heap command did not force conservative unknown-access fallback");
    unknownCommandProc.store(oldUnknown);
    { std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(f.handle)); }
#endif
}

void checkAsyncSnapshotTransactionDecisions() {
    Fixture f; auto& d=f.device; const auto memoryHandle=f.memory;
    d.asyncCompression=true; d.autoEnabled=true; d.coldBudget=10;
    auto& memory=d.virtualMemory.at(memoryHandle);
    memory.identityGeneration=9; memory.bindingGeneration=4;
    memory.children={tokenHandle<VkDeviceMemory>(40)}; memory.childSizes={MiB};
    memory.childTypes={0}; memory.childGenerations={12}; memory.poolViews={tokenHandle<VkBuffer>(41)};
    memory.coldGroups.resize(1); memory.coldGroups[0].cold=false; memory.coldGroups[0].writeEpoch=17;
    d.rangeChunkBytes=MiB; d.narrowDescriptorRanges=true; d.selectiveRestore=true;
    const auto trackedBuffer=tokenHandle<VkBuffer>(42);
    const auto command=tokenHandle<VkCommandBuffer>(44);
    PromotedBuffer promoted{}; promoted.size=MiB; promoted.memory=memoryHandle;
    d.promotedBuffers[trackedBuffer]=promoted;
    memory.bindings.push_back({trackedBuffer,0,MiB,1});
    d.submission.beginCommand(command);
    d.submission.bufferRange(command,trackedBuffer,0,MiB,true);
    auto token=[&] { return AsyncFreezeToken{memoryHandle,memory.identityGeneration,0,memory.children[0],
        memory.childGenerations[0],memory.bindingGeneration,memory.coldGroups[0].writeEpoch}; };
    require(asyncFreezeTokenValid(d,token()),"fresh async snapshot token was rejected");

    VkBaseInStructure unknown{static_cast<VkStructureType>(0x7fffffff),nullptr};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext=&unknown;
    auto stale=token();
    bumpAcceptedWriteEpochs(d,"vkQueueSubmit",1,&submit,VK_NULL_HANDLE);
    require(!asyncFreezeTokenValid(d,stale),"unknown accepted access did not invalidate the snapshot epoch");
    memory.coldGroups[0].writeEpoch=17;

    stale=token();
    d.gpuGateError=VK_SUCCESS;
    VkSubmitInfo trackedSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    trackedSubmit.commandBufferCount=1; trackedSubmit.pCommandBuffers=&command;
    bumpAcceptedWriteEpochs(d,"vkQueueSubmit",1,&trackedSubmit,VK_NULL_HANDLE);
    require(!asyncFreezeTokenValid(d,stale) && memory.coldGroups[0].writeEpoch==18,
            "known accepted write did not invalidate its target child");
    memory.coldGroups[0].writeEpoch=17;

    stale=token(); require(assignVirtualIdentity(d,memory),"replacement allocation identity assignment failed");
    require(!asyncFreezeTokenValid(d,stale),"allocation reuse did not invalidate the token");
    memory.identityGeneration=9;

    stale=token(); bumpAsyncVersion(d,memory.bindingGeneration);
    require(!asyncFreezeTokenValid(d,stale),"buffer bind mutation did not invalidate the token");
    memory.bindingGeneration=4;
    stale=token(); bumpAsyncVersion(d,memory.childGenerations[0]);
    require(!asyncFreezeTokenValid(d,stale),"backing replacement did not invalidate the token");
    memory.childGenerations[0]=12;

    stale=token(); d.activeRefs.recordRanges(tokenHandle<VkQueue>(43),{{memoryHandle,0}},true);
    require(!asyncFreezeTokenValid(d,stale),"active resource reference did not block commit");
    d.activeRefs.retire(tokenHandle<VkQueue>(43));
    stale=token(); d.gpuGateError=VK_ERROR_DEVICE_LOST;
    require(!asyncFreezeTokenValid(d,stale),"device-loss gate did not block commit");
    d.gpuGateError=VK_SUCCESS;
    stale=token(); d.stopWorker.store(true);
    require(!asyncFreezeTokenValid(d,stale),"worker stop did not block commit");
    d.stopWorker.store(false);

    require(asyncColdBudgetFits(d,3),"candidate at exact cold-budget headroom was rejected");
    d.coldBytes=4; d.cacheBytes=3;
    require(!asyncColdBudgetFits(d,4),"candidate exceeding current cold budget was accepted");
    d.coldBudget=6;
    require(!asyncColdBudgetFits(d,0),"candidate ignored a cold-budget shrink below current use");

    // Model free/reallocation of the same Vulkan handle while the worker is
    // outside both locks. A new map entry must not inherit the old token.
    stale=token();
    std::unique_lock<std::mutex> deviceLock(d.mutex),queueLock(d.queueMutex);
    runAsyncEncoderUnlocked(deviceLock,queueLock,[&] {
        d.virtualMemory.erase(memoryHandle);
        VirtualMemory replacement{}; replacement.identityGeneration=10;
        replacement.bindingGeneration=4; replacement.children={tokenHandle<VkDeviceMemory>(40)};
        replacement.childSizes={MiB}; replacement.childGenerations={12};
        replacement.coldGroups.resize(1); replacement.coldGroups[0].writeEpoch=17;
        d.virtualMemory.emplace(memoryHandle,std::move(replacement));
    });
    require(!asyncFreezeTokenValid(d,stale),"free/reallocation during unlocked encode reused a stale token");
}

void checkAsyncSnapshotEncodingRoundTrip() {
    std::vector<std::uint8_t> raw(64*1024);
    for(std::size_t i=0;i<raw.size();++i) raw[i]=static_cast<std::uint8_t>((i%97)<80?0:(i*31u));
    VirtualMemory::ColdGroup candidate; VkDeviceSize stored=0;
    require(encodeAsyncSnapshot(raw.data(),raw.size(),16*1024,0,0,zvram::snapshot::Codec::Zstd,1,candidate,stored),
            "async snapshot candidate encoding failed");
    require(candidate.cold && candidate.logicalBytes==raw.size() && candidate.storedBytes==stored &&
            candidate.chunks.size()==4,"async snapshot candidate metadata mismatch");
    std::vector<std::uint8_t> decoded(raw.size()); std::size_t offset=0;
    for(const auto& chunk:candidate.chunks) {
        const zvram::snapshot::EncodedChunk view{chunk.bytes.data(),chunk.bytes.size(),
            static_cast<std::size_t>(chunk.rawSize),chunk.compressed,chunk.byteShuffle,chunk.codec};
        require(zvram::snapshot::decodeOne(view,decoded.data()+offset),"async candidate chunk decode failed");
        offset+=chunk.rawSize;
    }
    require(decoded==raw,"async candidate round-trip changed bytes");
    const std::uint8_t sentinel{};
    VirtualMemory::ColdGroup oversized; VkDeviceSize oversizedStored=0;
    require(!encodeAsyncSnapshot(&sentinel,static_cast<std::size_t>(kAsyncSnapshotMaxRaw+1),16*1024,
            0,0,zvram::snapshot::Codec::Zstd,1,oversized,oversizedStored),
            "async encoder accepted a child above its hard raw bound");
}

void checkFreezePreservesUseTimestamp() {
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        const auto used=std::chrono::steady_clock::now()-std::chrono::seconds(5);
        m.coldGroups[0].lastUse=used; m.coldGroups[0].recoveryUses=7;
        m.coldGroups[0].recoveryUseEpoch=used;
        require(freezeChildLocked(d,m,0)==VK_SUCCESS && m.coldGroups[0].cold &&
                m.coldGroups[0].lastUse==used && m.coldGroups[0].recoveryUses==7 &&
                m.coldGroups[0].recoveryUseEpoch==used,
                "synchronous freeze replaced use-tracking metadata");
    }
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        d.asyncCompression=true; d.autoEnabled=true; d.coldBudget=2*MiB; d.residentLimitBytes=MiB/2;
        m.childGenerations.assign(m.children.size(),1);
        const auto used=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        m.coldGroups[0].lastUse=used; m.coldGroups[0].recoveryUses=11;
        m.coldGroups[0].recoveryUseEpoch=used-std::chrono::seconds(1);
        std::unique_lock<std::mutex> deviceLock(d.mutex),queueLock(d.queueMutex);
        require(freezeChildAsyncLocked(d,c.f.memory,0,deviceLock,queueLock) &&
                m.coldGroups[0].cold && m.coldGroups[0].lastUse==used &&
                m.coldGroups[0].recoveryUses==11 &&
                m.coldGroups[0].recoveryUseEpoch==used-std::chrono::seconds(1),
                "asynchronous freeze replaced use-tracking metadata");
    }
#ifdef ZVRAM_TEST_ASYNC_HOOK
    {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        d.asyncCompression=true; d.autoEnabled=true; d.coldBudget=2*MiB; d.residentLimitBytes=MiB/2;
        m.childGenerations.assign(m.children.size(),1);
        const auto used=std::chrono::steady_clock::now()-std::chrono::seconds(3);
        const auto touched=std::chrono::steady_clock::now();
        m.coldGroups[0].lastUse=used;
        m.coldGroups[0].recoveryUses=4; m.coldGroups[0].recoveryUseEpoch=used;
        auto expected=m.coldGroups[0]; recordRecoveryUse(expected,touched);
        struct TimestampUpdate { Device* device; VkDeviceMemory memory; std::chrono::steady_clock::time_point time; } update{
            &d,c.f.memory,touched};
        d.asyncEncodeTestHook=[](void* opaque) {
            auto& state=*static_cast<TimestampUpdate*>(opaque);
            std::lock_guard<std::mutex> lock(state.device->mutex);
            markSubmittedUseTimes(*state.device,{{state.memory,0}},true,state.time);
        };
        d.asyncEncodeTestUserdata=&update;
        std::unique_lock<std::mutex> deviceLock(d.mutex),queueLock(d.queueMutex);
        require(freezeChildAsyncLocked(d,c.f.memory,0,deviceLock,queueLock) &&
                m.coldGroups[0].cold && m.coldGroups[0].lastUse==touched &&
                m.coldGroups[0].recoveryUses==expected.recoveryUses &&
                m.coldGroups[0].recoveryUseEpoch==expected.recoveryUseEpoch,
                "async freeze did not preserve current use metadata changed during encoding");
    }
#endif
}

void checkAsyncEncoderReleasesBothGates() {
    std::mutex deviceMutex,queueMutex,signalMutex;
    std::condition_variable signal;
    bool acquired=false;
    std::unique_lock<std::mutex> deviceLock(deviceMutex);
    std::unique_lock<std::mutex> queueLock(queueMutex);
    runAsyncEncoderUnlocked(deviceLock,queueLock,[&] {
        std::thread contender([&] {
            std::unique_lock<std::mutex> deviceGuard(deviceMutex,std::try_to_lock);
            std::unique_lock<std::mutex> queueGuard(queueMutex,std::try_to_lock);
            { std::lock_guard<std::mutex> signalGuard(signalMutex);
              acquired=deviceGuard.owns_lock() && queueGuard.owns_lock(); }
            signal.notify_one();
        });
        std::unique_lock<std::mutex> signalGuard(signalMutex);
        const bool woke=signal.wait_for(signalGuard,std::chrono::seconds(1),[&]{return acquired;});
        signalGuard.unlock(); contender.join();
        require(woke && acquired,"encoder callback blocked while either production gate remained held");
    });
    require(deviceLock.owns_lock() && queueLock.owns_lock(),"encoder gates were not reacquired in order");
}

void checkNoActionCommandHooksStaySelective() {
    Fixture f(4*MiB,4*MiB); f.bind(0,4*MiB);
    f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord);
    auto& d=f.device; d.handle=f.handle; d.gdpa=mockGetDeviceProcAddr;
    d.virtualEnabled=true; d.selectiveRestore=true; d.rangeChunkBytes=MiB;
    d.narrowDescriptorRanges=true;
    const auto command=reinterpret_cast<VkCommandBuffer>(&f.dispatchWord);
    const auto pool=tokenHandle<VkCommandPool>(81);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool=pool; allocate.commandBufferCount=1;
    d.submission.allocateCommands(&allocate,&command);
    d.submission.beginCommand(command);
    const auto buffer=tokenHandle<VkBuffer>(82);
    d.promotedBuffers[buffer]=PromotedBuffer{};
    d.promotedBuffers[buffer].size=4*MiB; d.promotedBuffers[buffer].memory=f.memory;
    d.virtualMemory.at(f.memory).bindings.push_back({buffer,0,4*MiB,1});
    d.submission.bufferRange(command,buffer,MiB+16,64);
    {
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices[key(f.handle)]=std::shared_ptr<Device>(&d,[](Device*){});
    }

    trackedSyncForwards=trackedLabelForwards=unsupportedCommandForwards=0;
    const auto event=tokenHandle<VkEvent>(83);
    trackedvkCmdSetEvent(command,event,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    trackedvkCmdResetEvent(command,event,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
#ifdef VK_EXT_debug_utils
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    trackedvkCmdBeginDebugUtilsLabelEXT(command,&label);
    trackedvkCmdEndDebugUtilsLabelEXT(command);
    trackedvkCmdInsertDebugUtilsLabelEXT(command,&label);
#endif
    require(trackedSyncForwards==2,"event no-action hooks did not forward downstream");
#ifdef VK_EXT_debug_utils
    require(trackedLabelForwards==3,"debug-label no-action hooks did not forward downstream");
#endif

    std::vector<ActiveRefs::Use> uses;
    std::vector<VkSubmissionTracker::BufferRange> ranges;
    const char* reason=nullptr;
    require(d.submission.collectRanges(1,&command,ranges,&reason) && !reason && ranges.size()==1,
            "no-action hooks changed command-buffer tracking");
    require(commandMemories(d,{command},uses,false) &&
            uses.size()==1 && uses[0].memory==f.memory && uses[0].child==1,
            "event/debug-label hooks widened a known sparse range to whole-pool fallback");
    require(d.submission.collectRanges(1,&command,ranges,&reason) && !reason && ranges.size()==1 &&
            ranges[0].offset==MiB+16 && ranges[0].size==64,
            "event/debug-label hooks changed tracked command ranges");

    trackedvkCmdSetLineWidth(command,1.0f);
    require(unsupportedCommandForwards==1 &&
            !commandMemories(d,{command},uses,false),
            "unsupported state command did not preserve unknown-access fallback");
    require(!d.submission.collectRanges(1,&command,ranges,&reason) && reason &&
            std::strcmp(reason,"vkCmdSetLineWidth")==0,
            "unsupported command lost its specific diagnostic");
    { std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(f.handle)); }
}

void checkCopyBufferToImage2TracksSourceRead() {
#if defined(VK_VERSION_1_3) || defined(VK_KHR_copy_commands2)
    Fixture f(4*MiB,4*MiB);
    f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord);
    auto& d=f.device; d.handle=f.handle; d.gdpa=mockGetDeviceProcAddr;
    d.virtualEnabled=true; d.selectiveRestore=true; d.rangeChunkBytes=MiB;
    const auto command=reinterpret_cast<VkCommandBuffer>(&f.dispatchWord);
    const auto pool=tokenHandle<VkCommandPool>(0xb200);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool=pool; allocate.commandBufferCount=1;
    d.submission.allocateCommands(&allocate,&command);
    const auto source=tokenHandle<VkBuffer>(0xb201);
    d.promotedBuffers[source].size=4*MiB; d.promotedBuffers[source].memory=f.memory;
    auto& memory=d.virtualMemory.at(f.memory);
    memory.children.push_back(tokenHandle<VkDeviceMemory>(0xb202));
    memory.childSizes.push_back(4*MiB); memory.childTypes.push_back(0);
    memory.childGenerations.push_back(1); memory.coldGroups.resize(1);
    memory.bindings.push_back({source,0,4*MiB,1});
    {
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices[key(f.handle)]=std::shared_ptr<Device>(&d,[](Device*){});
    }
    copyBufferToImageForwards=0; forwardedCopyBufferToImage=nullptr;
    const auto previousUnknown=unknownCommandProc.exchange(false);
    auto invoke=[&](const char* name,const VkCopyBufferToImageInfo2* info) {
        d.submission.beginCommand(command);
        const auto fn=reinterpret_cast<PFN_vkCmdCopyBufferToImage2>(trackedCommandLookup(name));
        require(fn!=nullptr,"CopyBufferToImage2 tracking wrapper unavailable");
        fn(command,info);
        require(forwardedCopyBufferToImage==info,"CopyBufferToImage2 info was not forwarded unchanged");
    };
    auto requireReadOnlySource=[&](const char* name,const VkCopyBufferToImageInfo2* info) {
        invoke(name,info);
        std::vector<ActiveRefs::Use> reads,writes;
        require(commandMemories(d,{command},reads,false) && reads.size()==1 &&
                reads[0].memory==f.memory && reads[0].child==0,
                "CopyBufferToImage2 source buffer was not tracked for restore");
        require(commandMemories(d,{command},writes,true) && writes.empty(),
                "CopyBufferToImage2 source read was marked as a write");
    };
    VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    VkCopyBufferToImageInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
    info.srcBuffer=source; info.regionCount=1; info.pRegions=&region;
#ifdef VK_VERSION_1_3
    requireReadOnlySource("vkCmdCopyBufferToImage2",&info);
#endif
#ifdef VK_KHR_copy_commands2
    requireReadOnlySource("vkCmdCopyBufferToImage2KHR",&info);
#endif
    require(copyBufferToImageForwards==
                static_cast<unsigned>(
#if defined(VK_VERSION_1_3) && defined(VK_KHR_copy_commands2)
                    2
#else
                    1
#endif
                ),"core/KHR CopyBufferToImage2 command did not reach downstream");

    VkBaseInStructure extension{static_cast<VkStructureType>(0x7fffffff),nullptr};
    auto requireUnknown=[&](const char* name,const VkCopyBufferToImageInfo2* invalid) {
        invoke(name,invalid);
        std::vector<ActiveRefs::Use> uses;
        require(!commandMemories(d,{command},uses,false) && uses.empty(),
                "unsupported CopyBufferToImage2 metadata did not keep conservative fallback");
    };
#ifdef VK_VERSION_1_3
    info.pNext=&extension; requireUnknown("vkCmdCopyBufferToImage2",&info); info.pNext=nullptr;
    requireUnknown("vkCmdCopyBufferToImage2",nullptr);
    info.pRegions=nullptr; requireUnknown("vkCmdCopyBufferToImage2",&info); info.pRegions=&region;
    region.pNext=&extension; requireUnknown("vkCmdCopyBufferToImage2",&info); region.pNext=nullptr;
#endif
#ifdef VK_KHR_copy_commands2
    info.pNext=&extension; requireUnknown("vkCmdCopyBufferToImage2KHR",&info); info.pNext=nullptr;
    requireUnknown("vkCmdCopyBufferToImage2KHR",nullptr);
    region.pNext=&extension; requireUnknown("vkCmdCopyBufferToImage2KHR",&info); region.pNext=nullptr;
#endif
    info.regionCount=0;
#ifdef VK_VERSION_1_3
    invoke("vkCmdCopyBufferToImage2",&info);
#elif defined(VK_KHR_copy_commands2)
    invoke("vkCmdCopyBufferToImage2KHR",&info);
#endif
    std::vector<ActiveRefs::Use> empty;
    require(commandMemories(d,{command},empty,false) && empty.empty(),
            "empty CopyBufferToImage2 regions recorded an unnecessary source use");
    unknownCommandProc.store(previousUnknown);
    { std::lock_guard<std::mutex> lock(mapsMutex); devices.erase(key(f.handle)); }
#endif
}

struct BarrierRangeFixture {
    Fixture f{16*MiB,16*MiB};
    VkBuffer buffer{};
    VkCommandBuffer command{};
    explicit BarrierRangeFixture(VkDeviceSize bindOffset=0) {
        auto& d=f.device;
        d.rangeChunkBytes=4*MiB; d.narrowDescriptorRanges=true;
        buffer=f.bind(bindOffset,8*MiB);
        d.promotedBuffers[buffer]=PromotedBuffer{};
        d.promotedBuffers[buffer].size=8*MiB;
        d.promotedBuffers[buffer].memory=f.memory;
        f.handle=reinterpret_cast<VkDevice>(&f.dispatchWord);
        d.handle=f.handle; d.gdpa=mockGetDeviceProcAddr;
        d.virtualEnabled=true; d.selectiveRestore=true;
        command=reinterpret_cast<VkCommandBuffer>(&f.dispatchWord);
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool=tokenHandle<VkCommandPool>(0xb100); allocate.commandBufferCount=1;
        d.submission.allocateCommands(&allocate,&command);
        d.submission.beginCommand(command);
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices[key(f.handle)]=std::shared_ptr<Device>(&d,[](Device*){});
    }
    ~BarrierRangeFixture() {
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices.erase(key(f.handle));
    }
    void begin() { f.device.submission.beginCommand(command); }
    void expectChild(std::uint32_t expected,const char* message,bool mayWrite=false) {
        std::vector<VkSubmissionTracker::BufferRange> ranges;
        std::vector<ActiveRefs::Use> uses;
        const char* reason=nullptr;
        require(f.device.submission.collectRanges(1,&command,ranges,&reason) && !reason && ranges.size()==1,
                "finite barrier did not produce one known tracked range");
        require(ranges[0].mayWrite==mayWrite,"barrier access masks produced incorrect write classification");
        require(commandMemories(f.device,{command},uses,false) && uses.size()==1 &&
                uses[0].memory==f.memory && uses[0].child==expected,message);
        uses.clear();
        const bool knownWrites=commandMemories(f.device,{command},uses,true);
        require(knownWrites && (mayWrite ? uses.size()==1 && uses[0].child==expected : uses.empty()),
                "writes-only tracking disagreed with barrier access classification");
    }
    void expectWhole(const char* message) {
        std::vector<VkSubmissionTracker::BufferRange> ranges;
        std::vector<ActiveRefs::Use> uses;
        const char* reason=nullptr;
        require(f.device.submission.collectRanges(1,&command,ranges,&reason) && !reason && ranges.size()==1,
                "fallback barrier did not retain a known whole-buffer range");
        require(ranges[0].offset==0 && ranges[0].size==VK_WHOLE_SIZE,
                "fallback barrier narrowed an unsupported range");
        require(ranges[0].mayWrite,"fallback barrier was incorrectly classified as read-only");
        require(commandMemories(f.device,{command},uses,false) && uses.size()==2 &&
                uses[0].child==0 && uses[1].child==1,message);
        uses.clear();
        require(commandMemories(f.device,{command},uses,true) && uses.size()==2,
                "fallback barrier stopped tracking writes conservatively");
    }
    void expectUnknown(const char* message) {
        std::vector<VkSubmissionTracker::BufferRange> ranges;
        std::vector<ActiveRefs::Use> uses;
        const char* reason=nullptr;
        require(!f.device.submission.collectRanges(1,&command,ranges,&reason) && reason &&
                !commandMemories(f.device,{command},uses,false),message);
    }
};

void checkWarmRecoveryFinalBatchMapsAllAliases() {
    BarrierRangeFixture x;
    auto& d=x.f.device; auto& m=x.f.state();
    require(m.childSizes.size()>1 && m.poolViews.size()>1 && m.childTypes.size()>1,
            "multi-alias recovery fixture did not create a second child");
    d.memory.memoryTypeCount=2; d.memory.memoryHeapCount=2;
    d.memory.memoryTypes[0].heapIndex=0; d.memory.memoryTypes[1].heapIndex=1;
    d.memory.memoryHeaps[0].flags=VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    d.memory.memoryHeaps[1].flags=0;
    const auto child=std::size_t{1};
    const auto oldBacking=tokenHandle<VkDeviceMemory>(0xb8a1);
    const auto replacement=tokenHandle<VkDeviceMemory>(0xb8a2);
    const auto destinationView=tokenHandle<VkBuffer>(0xb8a3);
    m.children[child]=oldBacking; m.childTypes[child]=1;
    m.backingMemoryTypeBits=3; m.poolViewMemoryTypeBits[child]=3;
    const VkDeviceSize childBase=m.childSizes[0], childSize=m.childSizes[child];
    require(childBase==4*MiB && childSize==4*MiB,
            "multi-alias recovery fixture has unexpected child geometry");
    m.bindings[0].memoryOffset=2*MiB; m.bindings[0].size=3*MiB;
    m.bindings.push_back({tokenHandle<VkBuffer>(0xb8a4),5*MiB,2*MiB,4096});
    m.bindings.push_back({tokenHandle<VkBuffer>(0xb8a5),7*MiB,2*MiB,4096});
    liveAllocations.insert(oldBacking); liveAllocations.insert(replacement);
    bufferSizes[destinationView]=childSize;
    d.liveOther=childSize; d.liveLocal=childSize;
    PendingWarmRecovery pending{};
    pending.memory=x.f.memory; pending.allocationGeneration=m.identityGeneration;
    pending.bindingGeneration=m.bindingGeneration; pending.childGeneration=m.childGenerations[child];
    pending.child=child; pending.oldBacking=oldBacking; pending.replacement=replacement;
    pending.sourceView=m.poolViews[child]; pending.destinationView=destinationView;
    pending.oldType=1; pending.newType=0; pending.size=childSize;
    d.retainedWarmRecoveries.push_back({oldBacking,replacement,pending.sourceView,destinationView,1,0,childSize});
    require(finalizeWarmRecoveryLocked(d,pending) && sparseBinds==1 && queueWaitCalls==1 &&
            m.children[child]==replacement && m.childTypes[child]==0 && frees==1 &&
            d.retainedWarmRecoveries.empty() && sparseBindCalls.size()==1 &&
            sparseBindCalls[0].size()==5,
            "multi-alias recovery did not finish in one final sparse batch");
    const auto& batch=sparseBindCalls[0];
    require(batch[0].buffer==pending.sourceView && batch[0].binds[0].memory==VK_NULL_HANDLE &&
            batch[0].binds[0].size==childSize && batch[1].buffer==destinationView &&
            batch[1].binds[0].memory==VK_NULL_HANDLE && batch[1].binds[0].size==childSize &&
            batch[2].buffer==m.bindings[0].buffer && batch[2].binds[0].memory==replacement &&
            batch[2].binds[0].resourceOffset==2*MiB && batch[2].binds[0].memoryOffset==0 &&
            batch[2].binds[0].size==MiB &&
            batch[3].buffer==m.bindings[1].buffer && batch[3].binds[0].memory==replacement &&
            batch[3].binds[0].resourceOffset==0 && batch[3].binds[0].memoryOffset==MiB &&
            batch[3].binds[0].size==2*MiB &&
            batch[4].buffer==m.bindings[2].buffer && batch[4].binds[0].memory==replacement &&
            batch[4].binds[0].resourceOffset==0 && batch[4].binds[0].memoryOffset==3*MiB &&
            batch[4].binds[0].size==MiB,
            "combined batch lost an alias or mapped a cross-child offset incorrectly");
}

void checkPendingKnownHotQueueCallSkipsColdRestore() {
    for(const bool flipUnknown:{false,true}) {
        BarrierRangeFixture x;
        auto& d=x.f.device; auto& memory=x.f.state();
        d.activeEviction=true; d.autoEnabled=true; d.autoInitialized=true;
        d.residentLimitBytes=16*MiB; d.residentAdmissionArmed=true;
        d.budgetProperties=mockBudgetProperties; d.budgetHeap=0; d.budgetReserveBytes=1;
        d.memory.memoryTypeCount=2; d.memory.memoryHeapCount=1;
        d.memory.memoryHeaps[0].size=64*MiB; d.memory.memoryTypes[0].heapIndex=0;
        d.memory.memoryTypes[1].heapIndex=0; d.physical=tokenHandle<VkPhysicalDevice>(0xd301);
        mockNativeHeapSize=mockHeapBudget=64*MiB; mockHeapUsage=0;

        memory.children[0]=tokenHandle<VkDeviceMemory>(0xd302);
        memory.childTypes[0]=1; memory.poolViewMemoryTypeBits[0]=3;
        memory.children[1]=tokenHandle<VkDeviceMemory>(0xd303);
        memory.childTypes[1]=0;
        for(auto& group:memory.coldGroups) { group.cold=false; group.pristine=false; }
        memory.cold=false; memory.coldLogicalSize=0; memory.residentBytes=8*MiB;
        d.residentBytes=8*MiB;

        const auto coldHandle=tokenHandle<VkDeviceMemory>(0xd304);
        VirtualMemory cold{}; cold.size=MiB; cold.identityGeneration=77;
        cold.children={VK_NULL_HANDLE}; cold.childSizes={MiB}; cold.childTypes={0};
        cold.childGenerations={1}; cold.poolViews={tokenHandle<VkBuffer>(0xd305)};
        cold.poolViewMemoryTypeBits={1}; cold.coldGroups.resize(1); cold.cold=true;
        cold.coldLogicalSize=MiB; cold.coldGroups[0].cold=true;
        cold.coldGroups[0].pristine=true; cold.coldGroups[0].logicalBytes=MiB;
        d.virtualMemory.emplace(coldHandle,std::move(cold)); d.coldLogicalBytes=MiB;

        auto& pending=d.pendingWarmRecovery; pending.active=true; pending.memory=x.f.memory;
        pending.child=0; pending.oldBacking=memory.children[0];
        pending.allocationGeneration=memory.identityGeneration;
        pending.bindingGeneration=memory.bindingGeneration;
        pending.childGeneration=memory.childGenerations[0];

        const auto queue=reinterpret_cast<VkQueue>(x.f.handle);
        const std::vector<VkQueue> queues{queue,d.sparseQueue};
        require(d.autoQueues.init(d.handle,d.gdpa,d.sparseQueue,queues,true)==VK_SUCCESS,
                "hot pending queue fixture failed to initialize automatic queues");
        d.restoreQueueGenerations.emplace_back(queue,d.restoreGeneration);
        x.f.device.submission.bufferRange(x.command,x.buffer,4*MiB,4*MiB,true);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount=1; submit.pCommandBuffers=&x.command;
        const auto* info=static_cast<const VkSubmitInfo*>(&submit);
        applicationSubmitInfo=info;
        const auto oldUnknown=unknownCommandProc.exchange(false);
        std::vector<ActiveRefs::Use> selected;
        require(queueMemories(d,"vkQueueSubmit",selected,std::uint32_t{1},info,VkFence{}) &&
                selected.size()==1 && selected[0].memory==x.f.memory && selected[0].child==1 &&
                warmRecoveryKnownHotUses(d,true,selected),
                "finite queue submit did not resolve to one validated disjoint hot child");
        flipUnknownCommandOnBudgetQuery=flipUnknown;
        const auto beforeAllocations=allocations,beforeSparse=sparseBinds;
        const auto result=queueCall<PFN_vkQueueSubmit>(queue,"vkQueueSubmit",std::uint32_t{1},info,VkFence{});
        flipUnknownCommandOnBudgetQuery=false;
        unknownCommandProc.store(oldUnknown);
        applicationSubmitInfo=nullptr;

        const auto status=d.activeRefs.status(queue);
        require(result==VK_SUCCESS && submitCalls==1 && pending.active &&
                d.coldLogicalBytes==MiB && d.virtualMemory.at(coldHandle).cold &&
                mockResetCommandPoolCalls==0 && allocations==beforeAllocations &&
                sparseBinds==beforeSparse &&
                (flipUnknown ? status.blocksAll && d.activeRefs.busy(x.f.memory,0)
                             : !status.blocksAll && !d.activeRefs.busy(x.f.memory,0) &&
                               d.activeRefs.busy(x.f.memory,1)),
                flipUnknown ?
                    "late unknown access lost conservative tracking or restored the cold peer during recovery":
                    "validated disjoint hot submit restored/reset the cold peer during recovery");
    }
}

void checkPendingAdmissionWaitRetriesThroughQueueCall() {
    BarrierRangeFixture x;
    auto& d=x.f.device; auto& memory=x.f.state();
    d.activeEviction=true; d.autoEnabled=true; d.autoInitialized=true;
    d.residentLimitBytes=8*MiB; d.residentAdmissionArmed=true;
    d.budgetProperties=mockBudgetProperties; d.budgetHeap=0; d.budgetReserveBytes=1;
    d.memory.memoryTypeCount=1; d.memory.memoryHeapCount=1;
    d.memory.memoryHeaps[0].size=64*MiB; d.memory.memoryTypes[0].heapIndex=0;
    d.physical=tokenHandle<VkPhysicalDevice>(0xd311);
    mockNativeHeapSize=mockHeapBudget=64*MiB; mockHeapUsage=0;
    memory.children[0]=tokenHandle<VkDeviceMemory>(0xd312);
    memory.children[1]=tokenHandle<VkDeviceMemory>(0xd313);
    memory.childTypes[0]=memory.childTypes[1]=0;
    memory.poolViewMemoryTypeBits[0]=memory.poolViewMemoryTypeBits[1]=1;
    for(auto& group:memory.coldGroups) { group.cold=false; group.pristine=false; }
    memory.cold=false; memory.coldLogicalSize=0; memory.coldStoredBytes=0;
    memory.residentBytes=8*MiB; d.coldLogicalBytes=d.coldBytes=0;
    d.residentBytes=9*MiB; // Force the selected hot child to await the in-flight promotion.
    d.pendingWarmRecovery.active=true; d.pendingWarmRecovery.memory=x.f.memory;
    d.pendingWarmRecovery.child=0; d.pendingWarmRecovery.oldBacking=memory.children[0];
    d.pendingWarmRecovery.childGeneration=memory.childGenerations[0];
    d.pendingWarmRecovery.allocationGeneration=memory.identityGeneration;
    d.pendingWarmRecovery.bindingGeneration=memory.bindingGeneration;
    const auto queue=reinterpret_cast<VkQueue>(x.f.handle);
    const std::vector<VkQueue> queues{queue,d.sparseQueue};
    require(d.autoQueues.init(d.handle,d.gdpa,d.sparseQueue,queues,true)==VK_SUCCESS,
            "pending admission retry fixture failed to initialize automatic queues");
    d.restoreQueueGenerations.emplace_back(queue,d.restoreGeneration);
    x.f.device.submission.bufferRange(x.command,x.buffer,4*MiB,4*MiB,true);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount=1; submit.pCommandBuffers=&x.command;
    const auto* info=static_cast<const VkSubmitInfo*>(&submit); applicationSubmitInfo=info;
    const auto oldUnknown=unknownCommandProc.exchange(false);
    std::vector<ActiveRefs::Use> selected;
    const bool tracked=queueMemories(d,"vkQueueSubmit",selected,std::uint32_t{1},info,VkFence{});
    const bool knownHot=warmRecoveryKnownHotUses(d,tracked,selected);
    require(tracked && selected.size()==1 && selected[0].child==1 && knownHot,
            "admission CV fixture did not classify a valid disjoint hot child");
    {
        std::lock_guard<std::mutex> lock(admissionSignalMutex);
        budgetQueryEntered=false;
    }
    signalBudgetQuery=true;
    std::atomic<VkResult> result{VK_ERROR_UNKNOWN};
    std::atomic<bool> callerDone{};
    std::thread caller([&] {
        result=queueCall<PFN_vkQueueSubmit>(queue,"vkQueueSubmit",std::uint32_t{1},info,VkFence{});
        callerDone.store(true,std::memory_order_release);
    });
    bool sawBudgetQuery=false;
    {
        std::unique_lock<std::mutex> lock(admissionSignalMutex);
        sawBudgetQuery=admissionSignal.wait_for(lock,std::chrono::seconds(3),[]{return budgetQueryEntered;});
    }
    bool joinedPendingAdmission=false;
    {
        std::unique_lock<std::mutex> lock(d.mutex);
        joinedPendingAdmission=sawBudgetQuery && d.pendingWarmRecovery.active &&
            !callerDone.load(std::memory_order_acquire);
        d.pendingWarmRecovery.active=false;
        d.residentLimitBytes=16*MiB;
        signalBudgetQuery.store(false,std::memory_order_release);
    }
    d.activity.notify_all(); caller.join();
    unknownCommandProc.store(oldUnknown); applicationSubmitInfo=nullptr;
    if(!(sawBudgetQuery && joinedPendingAdmission && result==VK_SUCCESS && submitCalls==1 &&
         !d.pendingWarmRecovery.active && !d.pendingRecoveryAdmissionWait &&
         d.residentLimitBytes==16*MiB && d.coldLogicalBytes==0))
        throw std::runtime_error("queueCall cap retry: signaled="+std::to_string(sawBudgetQuery)+
            " joined="+std::to_string(joinedPendingAdmission)+" result="+
            std::to_string(result.load())+" submits="+std::to_string(submitCalls)+
            " pending="+std::to_string(d.pendingWarmRecovery.active)+" admissionWait="+
            std::to_string(d.pendingRecoveryAdmissionWait)+" limit="+
            std::to_string(d.residentLimitBytes)+" cold="+std::to_string(d.coldLogicalBytes)+
            " done="+std::to_string(callerDone.load()));
}

void checkFiniteBufferBarrierTracking() {
    const VkEvent event=tokenHandle<VkEvent>(0xb200);
    {
        BarrierRangeFixture x;
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=x.buffer; b.offset=4*MiB; b.size=4*MiB;
        layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,&b,0,nullptr);
        x.expectChild(1,"legacy pipeline barrier did not select child 1");
        require(pipelineBarrierForwards==1,"legacy pipeline barrier was not forwarded");
    }
    {
        BarrierRangeFixture x(4*MiB);
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=x.buffer; b.offset=0; b.size=4*MiB;
        layerCmdWaitEvents(x.command,1,&event,0,0,0,nullptr,1,&b,0,nullptr);
        x.expectChild(1,"legacy wait-events range ignored buffer binding offset");
        require(waitEventsForwards==1,"legacy wait-events was not forwarded");
    }
    {
        BarrierRangeFixture x;
        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=x.buffer; b.offset=0; b.size=4*MiB;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount=1; dep.pBufferMemoryBarriers=&b;
        layerCmdPipelineBarrier2(x.command,&dep);
        x.expectChild(0,"sync2 pipeline barrier did not select child 0");
        require(pipelineBarrier2Forwards==1,"sync2 pipeline barrier was not forwarded");
    }
    {
        BarrierRangeFixture x;
        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=x.buffer; b.offset=4*MiB; b.size=4*MiB;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount=1; dep.pBufferMemoryBarriers=&b;
        layerCmdWaitEvents2(x.command,1,&event,&dep);
        x.expectChild(1,"sync2 wait-events range did not select child 1");
        require(waitEvents2Forwards==1,"sync2 wait-events was not forwarded");
    }
    {
        BarrierRangeFixture x;
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=x.buffer; b.offset=0; b.size=VK_WHOLE_SIZE;
        layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,&b,0,nullptr);
        x.expectWhole("whole-size barrier did not conservatively select all children");
        x.begin(); b.offset=UINT64_MAX-4; b.size=16;
        layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,&b,0,nullptr);
        x.expectWhole("overflowing barrier range was not widened");
        x.begin(); b.offset=4*MiB; b.size=8*MiB;
        layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,&b,0,nullptr);
        x.expectWhole("out-of-buffer barrier range was not widened");
        x.begin(); b.offset=8*MiB; b.size=1;
        layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,&b,0,nullptr);
        x.expectWhole("barrier beginning at buffer end was not widened");
        x.begin(); b.offset=0; b.size=0;
        layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,&b,0,nullptr);
        x.expectWhole("zero-size barrier was not widened");
        x.begin(); b.offset=0; b.size=4*MiB; b.dstQueueFamilyIndex=0;
        layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,&b,0,nullptr);
        x.expectWhole("queue-family ownership transfer was narrowed");
        x.begin(); b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        VkBaseInStructure ext{static_cast<VkStructureType>(0x7ffffffe),nullptr}; b.pNext=&ext;
        layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,&b,0,nullptr);
        x.expectUnknown("barrier extension chain did not preserve unknown fallback");
        x.begin(); b.pNext=nullptr;
        layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,nullptr,0,nullptr);
        x.expectUnknown("null barrier array with nonzero count was not marked unknown");
        x.begin(); VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.pNext=&ext;
        layerCmdPipelineBarrier2(x.command,&dep);
        x.expectUnknown("sync2 dependency extension did not preserve unknown fallback");
        x.begin();
        x.f.device.promotedBuffers.erase(x.buffer);
        b.pNext=nullptr; b.offset=0; b.size=4*MiB;
        layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,&b,0,nullptr);
        std::vector<VkSubmissionTracker::BufferRange> ranges;
        const char* reason=nullptr;
        require(x.f.device.submission.collectRanges(1,&x.command,ranges,&reason) && !reason &&
                ranges.size()==1 && ranges[0].offset==0 && ranges[0].size==VK_WHOLE_SIZE,
                "missing promoted-buffer size metadata was narrowed");
    }
}

void issueBarrierAccess(BarrierRangeFixture& x,unsigned api,VkAccessFlags2 src,VkAccessFlags2 dst,
                        bool extension=false) {
    const VkEvent event=tokenHandle<VkEvent>(0xb220);
    VkBaseInStructure ext{static_cast<VkStructureType>(0x7ffffffe),nullptr};
    if(api<2) {
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask=static_cast<VkAccessFlags>(src);
        b.dstAccessMask=static_cast<VkAccessFlags>(dst);
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=x.buffer; b.offset=4*MiB; b.size=4*MiB;
        if(extension) b.pNext=&ext;
        if(api==0) layerCmdPipelineBarrier(x.command,0,0,0,0,nullptr,1,&b,0,nullptr);
        else layerCmdWaitEvents(x.command,1,&event,0,0,0,nullptr,1,&b,0,nullptr);
    } else {
        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcAccessMask=src; b.dstAccessMask=dst;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=x.buffer; b.offset=4*MiB; b.size=4*MiB;
        if(extension) b.pNext=&ext;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount=1; dep.pBufferMemoryBarriers=&b;
        if(api==2) layerCmdPipelineBarrier2(x.command,&dep);
        else layerCmdWaitEvents2(x.command,1,&event,&dep);
    }
}
void seedBarrierCleanCache(BarrierRangeFixture& x) {
    auto& d=x.f.device; auto& m=x.f.state();
    d.cleanCache=true; d.asyncCompression=true; d.cacheBytes=17; d.cacheInvalidations=0;
    m.cacheStoredBytes=17;
    for(auto& group:m.coldGroups) { group.cold=false; group.pristine=false; }
    auto& target=m.coldGroups[1]; target.storedBytes=17; target.logicalBytes=4*MiB;
    target.writeEpoch=9; target.chunks.emplace_back(); target.chunks.back().bytes.resize(17);
}
void acceptedBarrierSubmit(BarrierRangeFixture& x) {
    auto& d=x.f.device;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount=1; submit.pCommandBuffers=&x.command;
    const auto* info=static_cast<const VkSubmitInfo*>(&submit);
    const auto queue=reinterpret_cast<VkQueue>(x.f.handle);
    const auto oldInfo=applicationSubmitInfo;
    const auto oldUnknown=unknownCommandProc.exchange(false);
    applicationSubmitInfo=info;
    invalidateAcceptedWrites(d,"vkQueueSubmit",std::uint32_t{1},info,VkFence{});
    bumpAcceptedWriteEpochs(d,"vkQueueSubmit",std::uint32_t{1},info,VkFence{});
    applicationSubmitInfo=oldInfo;
    unknownCommandProc.store(oldUnknown);
    (void)queue;
}
void checkBarrierReadWriteClassification() {
    for(unsigned api=0;api<4;api++) {
        BarrierRangeFixture x; auto& d=x.f.device; auto& m=x.f.state();
        seedBarrierCleanCache(x);
        const auto read=(api%2)?VkAccessFlags2{0}:VK_ACCESS_2_TRANSFER_READ_BIT;
        issueBarrierAccess(x,api,api==2?VK_ACCESS_2_TRANSFER_READ_BIT:0,read);
        x.expectChild(1,"read barrier did not retain a selected child",false);
        std::vector<ActiveRefs::Use> refs,writes;
        const bool knownRefs=commandMemories(d,{x.command},refs,false);
        const bool knownWrites=commandMemories(d,{x.command},writes,true);
        require(knownRefs && refs.size()==1 && refs[0].child==1 && knownWrites && writes.empty(),
                "read-only barrier lost references or entered writes-only tracking");
        acceptedBarrierSubmit(x);
        require(d.cacheBytes==17 && m.coldGroups[1].storedBytes==17 &&
                d.cacheInvalidations==0 && m.coldGroups[1].writeEpoch==9,
                "accepted transfer-read barrier invalidated clean data or bumped the write epoch");
    }
    const VkAccessFlags2 writes[]{VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_ACCESS_2_HOST_WRITE_BIT,static_cast<VkAccessFlags2>(1ull<<63)};
    for(unsigned api=0;api<4;api++) {
        BarrierRangeFixture x; auto& d=x.f.device; auto& m=x.f.state();
        seedBarrierCleanCache(x);
        issueBarrierAccess(x,api,0,writes[api]);
        x.expectChild(1,"write-capable barrier did not retain its child",true);
        acceptedBarrierSubmit(x);
        require(d.cacheBytes==0 && m.coldGroups[1].storedBytes==0 &&
                d.cacheInvalidations==1 && m.coldGroups[1].writeEpoch==10,
                "accepted write-capable barrier preserved cache or failed to bump its epoch");
    }
    {
        BarrierRangeFixture x; auto& d=x.f.device; auto& m=x.f.state();
        seedBarrierCleanCache(x);
        issueBarrierAccess(x,2,VK_ACCESS_2_UNIFORM_READ_BIT,0);
        x.expectChild(1,"non-allowlisted read access was incorrectly treated as read-only",true);
        acceptedBarrierSubmit(x);
        require(d.cacheBytes==0 && d.cacheInvalidations==1 && m.coldGroups[1].writeEpoch==10,
                "non-allowlisted read access did not conservatively invalidate cache and bump epoch");
    }
    {
        BarrierRangeFixture x; auto& d=x.f.device; auto& m=x.f.state();
        seedBarrierCleanCache(x);
        issueBarrierAccess(x,0,VkAccessFlags2{0},VkAccessFlags2{0});
        d.submission.bufferRange(x.command,x.buffer,4*MiB,4*MiB,true); // Later fill/copy destination.
        std::vector<ActiveRefs::Use> writes;
        require(commandMemories(d,{x.command},writes,true) && writes.size()==1 && writes[0].child==1,
                "later write access was suppressed by an earlier read-only barrier");
        acceptedBarrierSubmit(x);
        require(d.cacheBytes==0 && d.cacheInvalidations==1 && m.coldGroups[1].writeEpoch==10,
                "accepted independent copy/fill write failed to invalidate clean data");
    }
    {
        BarrierRangeFixture x; auto& d=x.f.device; auto& m=x.f.state();
        seedBarrierCleanCache(x); m.coldGroups[0].storedBytes=13; m.cacheStoredBytes+=13; d.cacheBytes+=13;
        m.coldGroups[0].logicalBytes=4*MiB; m.coldGroups[0].chunks.emplace_back();
        m.coldGroups[0].chunks.back().bytes.resize(13); m.coldGroups[0].writeEpoch=4;
        issueBarrierAccess(x,2,0,VkAccessFlags2{0},true);
        x.expectUnknown("extension barrier did not preserve unknown-access fallback");
        acceptedBarrierSubmit(x);
        require(d.cacheBytes==0 && d.cacheInvalidations==2 && m.coldGroups[0].writeEpoch==5 &&
                m.coldGroups[1].writeEpoch==10,
                "accepted unknown barrier did not conservatively invalidate cache and bump epochs");
    }
}

void checkAllocationFailureRetry() {
    Fixture f(2*MiB,2*MiB); f.bind(0,2*MiB); auto& m=f.state();
    failAllocations=1;
    require(restoreColdLocked(f.handle,f.device,f.memory,0)==VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "injected child allocation failure was not returned");
    require(allocations==1 && f.device.residentBytes==0 && m.coldGroups[0].cold &&
            m.coldGroups[0].pristine && f.device.coldLogicalBytes==2*MiB,
            "allocation failure corrupted pristine cold accounting");
    require(restoreColdLocked(f.handle,f.device,f.memory,0)==VK_SUCCESS,
            "retry after allocation failure did not succeed");
    require(allocations==2 && f.device.residentBytes==MiB && !m.coldGroups[0].cold &&
            f.device.coldLogicalBytes==MiB,"allocation retry accounting mismatch");
}

void checkBindFailureAccounting() {
    Fixture f(2*MiB,2*MiB); f.bind(0,2*MiB); auto& m=f.state();
    failSparseBinds=1;
    require(restoreColdLocked(f.handle,f.device,f.memory,0)==VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "injected sparse bind failure was not returned");
    require(allocations==1 && f.device.residentBytes==MiB && m.children[0] &&
            m.coldGroups[0].cold && m.coldGroups[0].pristine &&
            f.device.coldLogicalBytes==2*MiB,"bind failure corrupted retryable cold accounting");
    require(f.device.gpuGateError==VK_ERROR_DEVICE_LOST,
            "failed sparse bind did not retain the production unsafe gate");
    const auto retry=restoreColdLocked(f.handle,f.device,f.memory,0);
    require(retry==VK_ERROR_DEVICE_LOST && allocations==1 && f.device.residentBytes==MiB &&
            m.coldGroups[0].cold && f.device.coldLogicalBytes==2*MiB,
            "blocked retry changed failed-bind resident/cold accounting");
}

void checkDecoderAbandonmentPrecedesNativeDeviceDestroy() {
    for(const auto idleResult:{VK_SUCCESS,VK_ERROR_DEVICE_LOST,VK_ERROR_OUT_OF_HOST_MEMORY}) {
        auto d=std::make_shared<Device>();
        std::uintptr_t dispatchWord=0;
        d->handle=reinterpret_cast<VkDevice>(&dispatchWord);
        d->autoInitialized=true;
        d->gpuRestoreUnsafe=true;
        d->snapshot.deviceWaitIdle=mockDeviceWait;
        d->destroy=mockDestroyDeviceObservingDecoder;
        d->gpuDecoder=std::make_unique<zvram::gdeflate::gpu::Decoder>();
        zvram::gdeflate::gpu::DecoderTeardownTestAccess::seedPoisonedDecoder(
            *d->gpuDecoder,d->handle,tokenHandle<VkBuffer>(0xd001),
            tokenHandle<VkDeviceMemory>(0xd002),mockDestroyBuffer,mockFree);
        destroyObservedDevice=d.get();
        destroyObservedDecoderUnsafe=false; destroyObservedDecoderPresent=false; destroyDeviceCalls=0;
        const auto beforeBuffers=destroyedMockBuffers,beforeFrees=frees;
        mockDeviceWaitResult=idleResult;
        {
            std::lock_guard<std::mutex> lock(mapsMutex);
            devices[key(d->handle)]=d;
        }
        layerDestroyDevice(d->handle,nullptr);
        bool mapEntryRemains{};
        { std::lock_guard<std::mutex> lock(mapsMutex); mapEntryRemains=devices.count(key(d->handle))!=0; }
        const bool idleProven=idleResult==VK_SUCCESS || idleResult==VK_ERROR_DEVICE_LOST;
        require(destroyDeviceCalls==1 &&
                (destroyObservedDecoderPresent==!idleProven) &&
                (destroyObservedDecoderUnsafe==!idleProven) &&
                destroyedBuffersAtNativeDeviceDestroy==beforeBuffers+(idleProven?1:0) &&
                freesAtNativeDeviceDestroy==beforeFrees+(idleProven?1:0) &&
                !mapEntryRemains,
                "device destroy did not clean poisoned decoder only after real idle proof");
        destroyObservedDevice=nullptr;
        const auto afterNativeBuffers=destroyedMockBuffers,afterNativeFrees=frees;
        d->gpuDecoder.reset();
        require(destroyedMockBuffers==afterNativeBuffers && frees==afterNativeFrees,
                "decoder destructor made late driver calls after native device destroy");
    }
}
} // namespace

int main() try {
    checkProcAddressAvailability();
    checkMaintenance4BufferRequirements();
    checkMaintenance6BindStatus();
    checkGatedRecoveryTeardownCleanup();
    checkQueueIdleTeardownFallback();
    checkDeferredPromotedBufferDestroy();
    checkDecoderAbandonmentPrecedesNativeDeviceDestroy();
    checkBootstrapAdmissionAndRestore();
    checkExactCapRestore();
    checkOneByteBelowCap();
    checkBootstrapAlignmentRollback();
    checkUnknownSubmitAdmission();
    checkBudgetPreflightRetry();
    checkResidentBudgetAccounting();
    checkBudgetRefusalBeforeAllocation();
    checkDefaultBudgetDoesNotQuery();
    checkAutoSparseBindUsesDedicatedQueue();
    checkBasePresentPassthrough();
    checkPresentGateAndFallback();
    checkPresentMetadataAllowlist();
    checkDescriptorHeapRemainsUnmanaged();
    checkAsyncSnapshotTransactionDecisions();
    checkAsyncSnapshotEncodingRoundTrip();
    checkFreezePreservesUseTimestamp();
    checkAsyncEncoderReleasesBothGates();
    checkNoActionCommandHooksStaySelective();
    checkCopyBufferToImage2TracksSourceRead();
    checkFiniteBufferBarrierTracking();
    checkBarrierReadWriteClassification();
    checkPendingKnownHotQueueCallSkipsColdRestore();
    checkPendingAdmissionWaitRetriesThroughQueueCall();
    checkAllocationFailureRetry();
    checkBindFailureAccounting();
    checkMergedRestoreMapping();
    checkColdCyclePromotionAndFailureRetry();
    checkWarmRecoveryInitialBatchMapsDisjointAliases();
    checkColdCycleActiveReferenceGuard();
    checkRecoveryPriorityOverridesQuietAgeOnly();
    checkColdCycleHotCompletedChild();
    checkColdCycleCandidateSkipsUnpromotableOldest();
    checkColdCycleCandidatePrefersMostRecentEligible();
    checkRecoveryUseHeatSelectionAndDecay();
    checkColdCycleCandidateZeroLiveOther();
    checkActiveWorkerDelayBounds();
    checkActiveSnapshotWorkerDrainsPressureAndStops();
    checkPressureBudgetRetryIgnoresUnrelatedSubmissions();
    checkPressureBudgetBlockWriteInvalidation();
    checkWarmRecoveryFinalPlanFailureRetainsOwnership();
    checkWarmRecoveryVisibilityFailureRetainsOldBacking();
    checkWarmRecoveryFinalBatchMapsAllAliases();
    checkPendingWarmRecoveryTokenAndQueueGates();
    checkPendingKnownEmptyQueueCallSkipsColdRestore();
    checkPendingAdmissionRequestsFullPreflightRetry();
    checkUnlockedRecoveryFenceWaitReleasesLocks();
    checkUnlockedRecoveryAbortsAfterExternalGate();
    checkUnlockedRecoveryLateCapRollbackAndFenceFailure();
    checkColdCycleIgnoresSnapshotBudgetBlock();
#ifdef ZVRAM_TEST_ASYNC_HOOK
    checkColdCycleTestPause();
#endif
    checkColdCycleQuietEnvParsing();
    checkColdCycleSparseFailurePreservesBacking();
    checkColdCycleRefusalPolicies();
    checkNativeBackingFallbackPolicy();
    checkPressureOnlyAsyncCommitRechecksCurrentCap();
    checkSubmitUseTimestampScope();
    std::cout<<"PASS: pristine bootstrap, cold aliases, cap/budget accounting, rollback, and retry\n";
    return 0;
} catch(const std::exception& e) {
    std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1;
}
