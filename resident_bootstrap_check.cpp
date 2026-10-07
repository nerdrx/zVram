// CPU-only production-path check. Including the layer keeps its internal
// bootstrap/restore helpers directly testable without a Vulkan loader.
#include "layer.cpp"

#include <iostream>
#include <unordered_set>
#include <stdexcept>

namespace {
constexpr VkDeviceSize MiB = 1024ull * 1024ull;
unsigned allocations{};
unsigned sparseBinds{};
unsigned queueWaitCalls{};
int failAllocations{};
int failSparseBinds{};
unsigned failQueueWaitAt{};
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

void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }

VKAPI_ATTR VkResult VKAPI_CALL mockCreateBuffer(VkDevice,const VkBufferCreateInfo* info,
    const VkAllocationCallbacks*,VkBuffer* out) {
    *out=tokenHandle<VkBuffer>(nextHandle++); bufferSizes[*out]=info->size; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL mockDestroyBuffer(VkDevice,VkBuffer buffer,const VkAllocationCallbacks*) {
    bufferSizes.erase(buffer);
}
VKAPI_ATTR void VKAPI_CALL mockGetBufferMemoryRequirements(VkDevice,VkBuffer buffer,VkMemoryRequirements* req) {
    req->size=bufferSizes.at(buffer); req->alignment=4096; req->memoryTypeBits=1;
}
VKAPI_ATTR VkResult VKAPI_CALL mockAllocate(VkDevice,const VkMemoryAllocateInfo*,
    const VkAllocationCallbacks*,VkDeviceMemory* out) {
    ++allocations;
    if(failAllocations>0) { --failAllocations; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    *out=tokenHandle<VkDeviceMemory>(nextHandle++); liveAllocations.insert(*out); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL mockFree(VkDevice,VkDeviceMemory memory,const VkAllocationCallbacks*) {
    ++frees; liveAllocations.erase(memory);
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
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL mockQueueWait(VkQueue) {
    ++queueWaitCalls;
    return failQueueWaitAt==queueWaitCalls?VK_ERROR_DEVICE_LOST:VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL mockDeviceWait(VkDevice) { return VK_SUCCESS; }
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
VKAPI_ATTR void VKAPI_CALL mockDestroyFence(VkDevice,VkFence,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL mockFenceStatus(VkDevice,VkFence) { return VK_SUCCESS; }
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
VKAPI_ATTR VkResult VKAPI_CALL mockResetCommandPool(VkDevice,VkCommandPool,VkCommandPoolResetFlags) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL mockBeginCommandBuffer(VkCommandBuffer,const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL mockEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL mockPipelineBarrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,
    VkDependencyFlags,std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,
    std::uint32_t,const VkImageMemoryBarrier*) {}
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
        allocations=sparseBinds=queueWaitCalls=frees=budgetQueries=presentCalls=submitCalls=0; failAllocations=failSparseBinds=0; failQueueWaitAt=0; nextHandle=0x1000; bufferSizes.clear(); liveAllocations.clear(); sparseBindCalls.clear(); mockBudgetSequence.clear(); applicationSubmitPnext=nullptr; applicationSubmitInfo=nullptr;
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

void checkBootstrapAdmissionAndRestore() {
    Fixture f;
    const auto first=f.bind(0,MiB);
    auto& m=f.state();
    require(allocations==0 && f.device.residentBytes==0,"bootstrap allocated resident backing");
    require(m.children.size()==4 && m.coldGroups.size()==4 && m.bindings.size()==1,
            "bootstrap did not create four cold range groups");
    require(m.cold && f.device.coldLogicalBytes==4*MiB,"bootstrap cold accounting mismatch");
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
    checkAsyncSnapshotTransactionDecisions();
    checkAsyncSnapshotEncodingRoundTrip();
    checkAsyncEncoderReleasesBothGates();
    checkNoActionCommandHooksStaySelective();
    checkAllocationFailureRetry();
    checkBindFailureAccounting();
    checkMergedRestoreMapping();
    std::cout<<"PASS: pristine bootstrap, cold aliases, cap/budget accounting, rollback, and retry\n";
    return 0;
} catch(const std::exception& e) {
    std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1;
}
