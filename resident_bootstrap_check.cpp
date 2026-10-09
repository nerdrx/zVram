// CPU-only production-path check. Including the layer keeps its internal
// bootstrap/restore helpers directly testable without a Vulkan loader.
#include "layer.cpp"

#include <iostream>
#include <unordered_set>
#include <stdexcept>
#include <condition_variable>

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
const void* applicationSubmitPnext{};
const VkSubmitInfo* applicationSubmitInfo{};
unsigned presentCalls{};
VkResult presentResult{VK_SUCCESS};
const VkPresentInfoKHR* forwardedPresent{};
std::uintptr_t nextHandle{0x1000};
std::unordered_map<VkBuffer,VkDeviceSize> bufferSizes;
std::unordered_set<VkDeviceMemory> liveAllocations;
unsigned trackedSyncForwards{};
unsigned trackedLabelForwards{};
unsigned unsupportedCommandForwards{};
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
std::uint32_t mockDeviceWaitCalls{},mockResetCommandPoolCalls{};
VkResult mockDeviceWaitResult{VK_SUCCESS};
std::uint32_t destroyedMockBuffers{},destroyedMockFences{};
const void* lastDestroyedBufferUserData{};
const void* lastFreedMemoryUserData{};
std::unordered_map<VkDeviceMemory,const void*> freedMemoryUserData;

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
VKAPI_ATTR VkResult VKAPI_CALL mockQueueWait(VkQueue) {
    ++queueWaitCalls;
    return failQueueWaitAt==queueWaitCalls?failQueueWaitResult:VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL mockDeviceWait(VkDevice) { ++mockDeviceWaitCalls; return mockDeviceWaitResult; }
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
    for(std::uint32_t i=0;i<memoryBarrierCount;i++)
        capturedMemoryBarriers.push_back({srcStage,dstStage,memoryBarriers[i].srcAccessMask,
                                         memoryBarriers[i].dstAccessMask});
}
VKAPI_ATTR void VKAPI_CALL mockCopyBuffer(VkCommandBuffer,VkBuffer,VkBuffer,std::uint32_t,const VkBufferCopy*) {}
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
    if(std::strcmp(name,"vkCmdCopyBuffer")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockCopyBuffer);
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
        allocations=sparseBinds=queueWaitCalls=frees=budgetQueries=presentCalls=submitCalls=0; failAllocations=failSparseBinds=0; failAllocationTypes.clear(); allocatedTypes.clear(); failQueueWaitAt=failSparseBindAt=0; failQueueWaitResult=VK_ERROR_DEVICE_LOST; mockEndCommandResult=VK_SUCCESS; mockPoolViewTypeBits=1; nextHandle=0x1000; bufferSizes.clear(); liveAllocations.clear(); freedMemoryUserData.clear(); sparseBindCalls.clear(); capturedMemoryBarriers.clear(); mockBudgetSequence.clear(); applicationSubmitPnext=nullptr; applicationSubmitInfo=nullptr; mockDeviceWaitCalls=mockResetCommandPoolCalls=destroyedMockBuffers=destroyedMockFences=0; lastDestroyedBufferUserData=lastFreedMemoryUserData=nullptr; mockDeviceWaitResult=VK_SUCCESS;
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
    mockDeviceWaitResult=VK_ERROR_DEVICE_LOST;
    require(waitForDeviceTeardown(d),"device-lost teardown was not accepted as completion proof");
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
        failQueueWaitAt=3; // App detach, two-view bind, then copy submit completes ambiguously.
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

void checkColdCycleCandidateZeroLiveOther() {
    ColdCycleFixture c; auto& d=c.f.device;
    d.liveOther=0;
    VkDeviceMemory candidate=tokenHandle<VkDeviceMemory>(0xc2ff); std::size_t child=99;
    require(!selectColdCycleCandidate(d,64*MiB,std::chrono::steady_clock::now(),candidate,child) &&
            candidate==VK_NULL_HANDLE && child==0,
            "zero tracked nonlocal bytes did not clear recovery candidate outputs");
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
    const std::vector<ActiveRefs::Use> empty{};
    const std::vector<ActiveRefs::Use> hotDisjoint{{tokenHandle<VkDeviceMemory>(0xc2fb),0}};
    const std::vector<ActiveRefs::Use> exact{{c.f.memory,0}};
    const std::vector<ActiveRefs::Use> whole{{c.f.memory,SIZE_MAX}};
    require(!warmRecoveryBlocksUses(d,true,empty) &&
            !warmRecoveryBlocksUses(d,true,hotDisjoint) &&
            warmRecoveryBlocksUses(d,false,empty) &&
            warmRecoveryBlocksUses(d,true,exact) && warmRecoveryBlocksUses(d,true,whole),
            "pending recovery queue-use gate mishandled empty, unknown, matching, or disjoint uses");
    d.coldLogicalBytes=1;
    require(!warmRecoveryBlocksUses(d,true,empty) && warmRecoveryBlocksUses(d,true,hotDisjoint),
            "cold restore did not block nonempty submissions while preserving empty signals");
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
    group.failedBudgetSubmissionGeneration=d.gpuSubmissionGeneration;
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
    for(unsigned stage=1;stage<=4;stage++) {
        ColdCycleFixture c; auto& d=c.f.device; auto& m=c.f.state();
        failSparseBindAt=stage; // app unbind, view bind, view unbind, app rebind
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
        if(stage<4) require(m.children[0]==c.nonlocal,
                            "pre-commit sparse failure replaced the original child handle");
        else require(m.children[0]==retained.replacement,
                     "ambiguous app rebind did not retain the possibly visible replacement");
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
    markSubmittedUseTimes(d,{},true,touched);
    require(memory.lastUse==old && d.virtualMemory.at(unrelatedHandle).lastUse==old &&
            std::all_of(memory.coldGroups.begin(),memory.coldGroups.end(),
                        [&](const auto& group){return group.lastUse==old;}),
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

    markSubmittedUseTimes(d,{},false,touched);
    require(memory.lastUse==touched && std::all_of(memory.coldGroups.begin(),memory.coldGroups.end(),
            [&](const auto& group){return group.lastUse==touched;}) &&
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
        presentId.swapchainCount=regions.swapchainCount=info.swapchainCount;
        // Null metadata arrays are valid and remain the driver's responsibility.
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
} // namespace

int main() try {
    checkGatedRecoveryTeardownCleanup();
    checkDeferredPromotedBufferDestroy();
    checkBootstrapAdmissionAndRestore();
    checkExactCapRestore();
    checkOneByteBelowCap();
    checkBootstrapAlignmentRollback();
    checkUnknownSubmitAdmission();
    checkBudgetPreflightRetry();
    checkResidentBudgetAccounting();
    checkBudgetRefusalBeforeAllocation();
    checkDefaultBudgetDoesNotQuery();
    checkBasePresentPassthrough();
    checkPresentGateAndFallback();
    checkPresentMetadataAllowlist();
    checkAsyncSnapshotTransactionDecisions();
    checkAsyncSnapshotEncodingRoundTrip();
    checkAsyncEncoderReleasesBothGates();
    checkNoActionCommandHooksStaySelective();
    checkAllocationFailureRetry();
    checkBindFailureAccounting();
    checkMergedRestoreMapping();
    checkColdCyclePromotionAndFailureRetry();
    checkColdCycleActiveReferenceGuard();
    checkColdCycleHotCompletedChild();
    checkColdCycleCandidateSkipsUnpromotableOldest();
    checkColdCycleCandidateZeroLiveOther();
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
