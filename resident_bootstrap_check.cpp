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
int failAllocations{};
int failSparseBinds{};
unsigned frees{};
unsigned budgetQueries{};
VkDeviceSize mockHeapBudget{};
VkDeviceSize mockHeapUsage{};
VkDeviceSize mockNativeHeapSize{};
unsigned presentCalls{};
VkResult presentResult{VK_SUCCESS};
const VkPresentInfoKHR* forwardedPresent{};
std::uintptr_t nextHandle{0x1000};
std::unordered_map<VkBuffer,VkDeviceSize> bufferSizes;
std::unordered_set<VkDeviceMemory> liveAllocations;

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
VKAPI_ATTR VkResult VKAPI_CALL mockSparse(VkQueue,std::uint32_t,const VkBindSparseInfo*,VkFence) {
    ++sparseBinds;
    if(failSparseBinds>0) { --failSparseBinds; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL mockQueueWait(VkQueue) { return VK_SUCCESS; }
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
    budget->heapBudget[0]=mockHeapBudget; budget->heapUsage[0]=mockHeapUsage;
    budget->heapBudget[1]=64*MiB; budget->heapUsage[1]=0;
}
VKAPI_ATTR VkResult VKAPI_CALL mockPresent(VkQueue,const VkPresentInfoKHR* info) {
    ++presentCalls; forwardedPresent=info; return presentResult;
}
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
VKAPI_ATTR VkResult VKAPI_CALL mockSubmit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence) { return VK_SUCCESS; }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mockGetDeviceProcAddr(VkDevice,const char* name) {
    if(std::strcmp(name,"vkQueuePresentKHR")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockPresent);
    if(std::strcmp(name,"vkCreateFence")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockCreateFence);
    if(std::strcmp(name,"vkDestroyFence")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockDestroyFence);
    if(std::strcmp(name,"vkGetFenceStatus")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockFenceStatus);
    if(std::strcmp(name,"vkResetFences")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockResetFences);
    if(std::strcmp(name,"vkCreateSemaphore")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockCreateSemaphore);
    if(std::strcmp(name,"vkDestroySemaphore")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockDestroySemaphore);
    if(std::strcmp(name,"vkQueueSubmit")==0) return reinterpret_cast<PFN_vkVoidFunction>(mockSubmit);
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
        allocations=sparseBinds=frees=budgetQueries=presentCalls=0; failAllocations=failSparseBinds=0; nextHandle=0x1000; bufferSizes.clear(); liveAllocations.clear();
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
    checkResidentBudgetAccounting();
    checkBudgetRefusalBeforeAllocation();
    checkDefaultBudgetDoesNotQuery();
    checkBasePresentPassthrough();
    checkPresentGateAndFallback();
    checkAsyncSnapshotTransactionDecisions();
    checkAsyncSnapshotEncodingRoundTrip();
    checkAsyncEncoderReleasesBothGates();
    checkAllocationFailureRetry();
    checkBindFailureAccounting();
    std::cout<<"PASS: pristine bootstrap, cold aliases, cap/budget accounting, rollback, and retry\n";
    return 0;
} catch(const std::exception& e) {
    std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1;
}
