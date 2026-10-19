// Device-side metadata hooks. Internal snapshot calls use downstream pointers.
template<class Action>
void trackSubmission(const std::shared_ptr<Device>& d,Action action) {
    if(!d || !d->selectiveRestore) return;
    std::lock_guard<std::mutex> lock(d->mutex);
    if(!d->selectiveRestore) return;
    try { action(d->submission); }
    catch(const std::bad_alloc&) {
        d->selectiveRestore=false;
        logf("selective restore disabled: tracking allocation failed; using full restore");
    }
}

#define NEXT(type,name) auto d=findDevice(device); auto next=d&&d->gdpa?reinterpret_cast<type>(d->gdpa(device,name)):nullptr
VKAPI_ATTR VkResult VKAPI_CALL trackedCreateShaderModule(VkDevice device,const VkShaderModuleCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkShaderModule* out) {
    NEXT(PFN_vkCreateShaderModule,"vkCreateShaderModule"); if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(device,info,allocator,out);
    if(r==VK_SUCCESS) trackSubmission(d,[&](auto& t){t.shader(*out,info);});
    return r;
}
VKAPI_ATTR void VKAPI_CALL trackedDestroyShaderModule(VkDevice device,VkShaderModule shader,const VkAllocationCallbacks* allocator) {
    NEXT(PFN_vkDestroyShaderModule,"vkDestroyShaderModule");
    trackSubmission(d,[&](auto& t){t.eraseShader(shader);}); if(next) next(device,shader,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL trackedCreateComputePipelines(VkDevice device,VkPipelineCache cache,std::uint32_t count,
    const VkComputePipelineCreateInfo* infos,const VkAllocationCallbacks* allocator,VkPipeline* out) {
    NEXT(PFN_vkCreateComputePipelines,"vkCreateComputePipelines"); if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(device,cache,count,infos,allocator,out);
    if(out && infos) trackSubmission(d,[&](auto& t){for(std::uint32_t i=0;i<count;i++) if(out[i]) t.computePipeline(out[i],infos[i]);});
    return r;
}
VKAPI_ATTR void VKAPI_CALL trackedDestroyPipeline(VkDevice device,VkPipeline pipeline,const VkAllocationCallbacks* allocator) {
    NEXT(PFN_vkDestroyPipeline,"vkDestroyPipeline");
    trackSubmission(d,[&](auto& t){t.erasePipeline(pipeline);}); if(next) next(device,pipeline,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL trackedCreateDescriptorSetLayout(VkDevice device,const VkDescriptorSetLayoutCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkDescriptorSetLayout* out) {
    NEXT(PFN_vkCreateDescriptorSetLayout,"vkCreateDescriptorSetLayout"); if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(device,info,allocator,out);
    if(r==VK_SUCCESS) trackSubmission(d,[&](auto& t){t.layout(*out,*info);}); return r;
}
VKAPI_ATTR void VKAPI_CALL trackedDestroyDescriptorSetLayout(VkDevice device,VkDescriptorSetLayout layout,const VkAllocationCallbacks* allocator) {
    NEXT(PFN_vkDestroyDescriptorSetLayout,"vkDestroyDescriptorSetLayout");
    trackSubmission(d,[&](auto& t){t.eraseLayout(layout);}); if(next) next(device,layout,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL trackedAllocateDescriptorSets(VkDevice device,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* out) {
    NEXT(PFN_vkAllocateDescriptorSets,"vkAllocateDescriptorSets"); if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(device,info,out); if(r==VK_SUCCESS) trackSubmission(d,[&](auto& t){t.allocateSets(info,out);}); return r;
}
VKAPI_ATTR VkResult VKAPI_CALL trackedFreeDescriptorSets(VkDevice device,VkDescriptorPool pool,std::uint32_t count,const VkDescriptorSet* sets) {
    NEXT(PFN_vkFreeDescriptorSets,"vkFreeDescriptorSets"); if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(device,pool,count,sets); if(r==VK_SUCCESS) trackSubmission(d,[&](auto& t){t.freeSets(count,sets);}); return r;
}
VKAPI_ATTR VkResult VKAPI_CALL trackedResetDescriptorPool(VkDevice device,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags) {
    NEXT(PFN_vkResetDescriptorPool,"vkResetDescriptorPool"); if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(device,pool,flags); if(r==VK_SUCCESS) trackSubmission(d,[&](auto& t){t.resetDescriptorPool(pool);}); return r;
}
VKAPI_ATTR void VKAPI_CALL trackedDestroyDescriptorPool(VkDevice device,VkDescriptorPool pool,const VkAllocationCallbacks* allocator) {
    NEXT(PFN_vkDestroyDescriptorPool,"vkDestroyDescriptorPool");
    trackSubmission(d,[&](auto& t){t.resetDescriptorPool(pool);}); if(next) next(device,pool,allocator);
}
VKAPI_ATTR void VKAPI_CALL trackedUpdateDescriptorSets(VkDevice device,std::uint32_t writeCount,const VkWriteDescriptorSet* writes,
    std::uint32_t copyCount,const VkCopyDescriptorSet* copies) {
    NEXT(PFN_vkUpdateDescriptorSets,"vkUpdateDescriptorSets"); if(!next) return;
    next(device,writeCount,writes,copyCount,copies);
    trackSubmission(d,[&](auto& t){t.updateSets(writeCount,writes,copyCount,copies);});
}
VKAPI_ATTR VkResult VKAPI_CALL trackedAllocateCommandBuffers(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
    NEXT(PFN_vkAllocateCommandBuffers,"vkAllocateCommandBuffers"); if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(device,info,out);
    if(r==VK_SUCCESS) {
        if(d->selectiveRestore && d->setDeviceLoaderData)
            for(std::uint32_t i=0;i<info->commandBufferCount;i++) {
                if(d->setDeviceLoaderData(device,out[i])!=VK_SUCCESS) { d->selectiveRestore=false; break; }
            }
        trackSubmission(d,[&](auto& t){t.allocateCommands(info,out);});
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL trackedFreeCommandBuffers(VkDevice device,VkCommandPool pool,std::uint32_t count,const VkCommandBuffer* commands) {
    NEXT(PFN_vkFreeCommandBuffers,"vkFreeCommandBuffers");
    trackSubmission(d,[&](auto& t){t.freeCommands(count,commands);}); if(next) next(device,pool,count,commands);
}
VKAPI_ATTR VkResult VKAPI_CALL trackedBeginCommandBuffer(VkCommandBuffer command,const VkCommandBufferBeginInfo* info) {
    auto d=findDevice(reinterpret_cast<VkDevice>(command));
    auto next=d&&d->gdpa?reinterpret_cast<PFN_vkBeginCommandBuffer>(d->gdpa(d->handle,"vkBeginCommandBuffer")):nullptr;
    if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(command,info);
    trackSubmission(d,[&](auto& t){
        t.beginCommand(command);
        if(r!=VK_SUCCESS || !info || info->pNext || (info->flags&VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) t.unknown(command);
    }); return r;
}
VKAPI_ATTR VkResult VKAPI_CALL trackedEndCommandBuffer(VkCommandBuffer command) {
    auto d=findDevice(reinterpret_cast<VkDevice>(command));
    auto next=d&&d->gdpa?reinterpret_cast<PFN_vkEndCommandBuffer>(d->gdpa(d->handle,"vkEndCommandBuffer")):nullptr;
    if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(command); if(r!=VK_SUCCESS) trackSubmission(d,[&](auto& t){t.unknown(command);}); return r;
}
VKAPI_ATTR VkResult VKAPI_CALL trackedResetCommandBuffer(VkCommandBuffer command,VkCommandBufferResetFlags flags) {
    auto d=findDevice(reinterpret_cast<VkDevice>(command));
    auto next=d&&d->gdpa?reinterpret_cast<PFN_vkResetCommandBuffer>(d->gdpa(d->handle,"vkResetCommandBuffer")):nullptr;
    if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(command,flags); if(r==VK_SUCCESS) trackSubmission(d,[&](auto& t){t.resetCommand(command);}); return r;
}
VKAPI_ATTR VkResult VKAPI_CALL trackedResetCommandPool(VkDevice device,VkCommandPool pool,VkCommandPoolResetFlags flags) {
    NEXT(PFN_vkResetCommandPool,"vkResetCommandPool"); if(!next) return VK_ERROR_INITIALIZATION_FAILED;
    auto r=next(device,pool,flags); if(r==VK_SUCCESS) trackSubmission(d,[&](auto& t){t.resetCommandPool(pool);}); return r;
}
VKAPI_ATTR void VKAPI_CALL trackedDestroyCommandPool(VkDevice device,VkCommandPool pool,const VkAllocationCallbacks* allocator) {
    NEXT(PFN_vkDestroyCommandPool,"vkDestroyCommandPool");
    trackSubmission(d,[&](auto& t){t.eraseCommandPool(pool);}); if(next) next(device,pool,allocator);
}
#undef NEXT

PFN_vkVoidFunction submissionHookLookup(const char* name) {
    if(!name) return nullptr;
#define HOOK(api) if(std::strcmp(name,"vk" #api)==0) return reinterpret_cast<PFN_vkVoidFunction>(tracked##api)
    HOOK(CreateShaderModule); HOOK(DestroyShaderModule); HOOK(CreateComputePipelines); HOOK(DestroyPipeline);
    HOOK(CreateDescriptorSetLayout); HOOK(DestroyDescriptorSetLayout); HOOK(AllocateDescriptorSets); HOOK(FreeDescriptorSets);
    HOOK(ResetDescriptorPool); HOOK(DestroyDescriptorPool); HOOK(UpdateDescriptorSets);
    HOOK(AllocateCommandBuffers); HOOK(FreeCommandBuffers); HOOK(BeginCommandBuffer); HOOK(EndCommandBuffer);
    HOOK(ResetCommandBuffer); HOOK(ResetCommandPool); HOOK(DestroyCommandPool);
#undef HOOK
    return nullptr;
}

bool commandMemories(Device&,const std::vector<VkCommandBuffer>&,std::vector<ActiveRefs::Use>&,bool writesOnly=false);

// Copy only frame descriptors. The caller keeps device/queue gates held and
// joins before modifying the next cold group or releasing either staging slot.
bool prepareRangeDecode(Device& d,const ActiveRefs::Use& use,std::size_t slot,
                        zvram::snapshot::DecodeAhead& ahead,bool asynchronous) {
    const auto p=d.virtualMemory.find(use.memory);
    if(p==d.virtualMemory.end() || use.child==SIZE_MAX || use.child>=p->second.coldGroups.size()) return false;
    const auto& group=p->second.coldGroups[use.child];
    auto& s=d.snapshot;
    if(d.gpuRawHostInput && d.residentAdmissionArmed && d.gpuDecoder &&
       use.child<p->second.childSizes.size() && group.chunks.size()==1) {
        const auto& chunk=group.chunks.front();
        if(!chunk.compressed && !chunk.byteShuffle && !chunk.imported &&
           chunk.rawSize==group.logicalBytes && chunk.rawSize==p->second.childSizes[use.child] &&
           chunk.size()==chunk.rawSize &&
           chunk.rawSize<=s.chunkSize && chunk.rawSize<=zvram::bp16::MaxRawBytes &&
           chunk.rawSize%zvram::bp16::RawBytesPerBlock==0)
            return false; // Keep eligible RAW groups on the synchronous direct-transfer path.
    }
    if(!group.cold || !group.logicalBytes || group.logicalBytes>s.stagingSize ||
       group.chunks.empty() || group.chunks.size()>4) return false;
    std::array<zvram::snapshot::EncodedChunk,4> chunks{};
    VkDeviceSize bytes=0;
    for(std::size_t i=0;i<group.chunks.size();++i) {
        const auto& chunk=group.chunks[i];
        if(d.gpuRestoreEnabled && chunk.compressed && chunk.codec!=zvram::snapshot::Codec::Zstd) return false;
        if(!chunk.rawSize || chunk.rawSize>s.chunkSize || chunk.rawSize>group.logicalBytes-bytes) return false;
        bytes+=chunk.rawSize;
        chunks[i]={chunk.data(),chunk.size(),static_cast<std::size_t>(chunk.rawSize),chunk.compressed,chunk.byteShuffle,chunk.codec};
    }
    if(bytes!=group.logicalBytes) return false;
    auto* mapped=static_cast<std::uint8_t*>(slot?s.lookaheadMapped:s.mapped);
    if(ahead.start(chunks,group.chunks.size(),mapped,static_cast<std::size_t>(s.stagingSize),
                   static_cast<std::size_t>(s.chunkSize),asynchronous)) ++s.prefetchLaunches;
    return true;
}

// Called with the existing device and queue gates held.
VkResult restoreTrackedCommands(Device& d,const std::vector<VkCommandBuffer>& commands) {
    if(!d.selectiveRestore || unknownCommandProc) return restoreColdLocked(d.handle,d);
    try {
        std::vector<ActiveRefs::Use> selected;
        if(!commandMemories(d,commands,selected)) {
            logf("selective restore fallback: untracked command, descriptor or address shader");
            return restoreColdLocked(d.handle,d);
        }
        std::vector<ActiveRefs::Use> cold;
        cold.reserve(selected.size());
        for(const auto& use:selected) {
            const auto p=d.virtualMemory.find(use.memory);
            if(p==d.virtualMemory.end() || !p->second.cold) continue;
            if(use.child!=SIZE_MAX && (use.child>=p->second.coldGroups.size() || !p->second.coldGroups[use.child].cold)) continue;
            if(std::none_of(cold.begin(),cold.end(),[&](const auto& entry){
                return entry.memory==use.memory && entry.child==use.child;
            })) cold.push_back(use);
        }
        auto& s=d.snapshot;
        const bool pipeline=s.lookaheadMapped && std::none_of(cold.begin(),cold.end(),[](const auto& use){return use.child==SIZE_MAX;});
        zvram::snapshot::DecodeAhead ahead;
        std::size_t preparedFor=SIZE_MAX, slot=0, restored=0;
        auto finishDecode=[&] {
            const auto start=std::chrono::steady_clock::now();
            const auto result=ahead.take();
            s.prefetchWaitNanoseconds+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
            s.decodeBytes+=result.bytes; s.decodeNanoseconds+=result.nanoseconds;
            return result;
        };
        for(std::size_t n=0;n<cold.size();++n) {
            const auto& use=cold[n];
            const auto p=d.virtualMemory.find(use.memory);
            if(p==d.virtualMemory.end() || !p->second.cold ||
               (use.child!=SIZE_MAX && !p->second.coldGroups[use.child].cold)) continue;
            bool prepared=preparedFor==n;
            preparedFor=SIZE_MAX;
            if(!prepared && pipeline) prepared=prepareRangeDecode(d,use,slot,ahead,false);
            zvram::snapshot::DecodeResult decoded{};
            if(prepared) {
                decoded=finishDecode();
                if(!decoded.ok || decoded.bytes!=p->second.coldGroups[use.child].logicalBytes) return VK_ERROR_UNKNOWN;
            }
            // Current decode is complete before launching the only look-ahead:
            // at most four CPU decode workers are active, and no worker uses Vulkan.
            if(prepared && n+1<cold.size() && prepareRangeDecode(d,cold[n+1],1-slot,ahead,true)) preparedFor=n+1;
            const auto buffer=prepared?(slot?s.lookaheadBuffer:s.stagingBuffer):VK_NULL_HANDLE;
            const auto r=restoreColdLocked(d.handle,d,use.memory,use.child,buffer,decoded.bytes);
            if(r!=VK_SUCCESS) {
                if(preparedFor!=SIZE_MAX) (void)finishDecode();
                return r;
            }
            if(prepared) ++s.preparedRestores;
            ++restored;
            slot=preparedFor!=SIZE_MAX?1-slot:0;
        }
        std::size_t remaining=0;
        for(const auto& p:d.virtualMemory) remaining+=p.second.cold;
        if(restored) {
            if(d.rangeChunkBytes) logf("selective range restore selected-chunks=%zu restored-chunks=%zu cold-pools-left=%zu",selected.size(),restored,remaining);
            else logf("selective restore selected-pools=%zu restored-pools=%zu cold-pools-left=%zu",selected.size(),restored,remaining);
            if(pipeline) logf("snapshot pipeline prefetches=%llu prepared-restores=%llu wait-ns=%llu",
                static_cast<unsigned long long>(s.prefetchLaunches),static_cast<unsigned long long>(s.preparedRestores),
                static_cast<unsigned long long>(s.prefetchWaitNanoseconds));
        }
        return VK_SUCCESS;
    } catch(const std::bad_alloc&) {
        d.selectiveRestore=false;
        return restoreColdLocked(d.handle,d);
    }
}
bool trackedSubmitChain(const void* chain) {
    for(auto* p=static_cast<const VkBaseInStructure*>(chain);p;p=p->pNext)
        if(p->sType!=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) return false;
    return true;
}
template<class... Args>
VkResult restoreForQueue(Device& d,const char*,Args...) { return restoreColdLocked(d.handle,d); }
VkResult restoreForQueue(Device& d,const char*,std::uint32_t count,const VkSubmitInfo* infos,VkFence) {
    if(!d.selectiveRestore || unknownCommandProc) return restoreColdLocked(d.handle,d);
    try {
        std::vector<VkCommandBuffer> commands;
        for(std::uint32_t i=0;i<count;i++) {
            if(!infos || !trackedSubmitChain(infos[i].pNext)) return restoreColdLocked(d.handle,d);
            if(infos[i].commandBufferCount) commands.insert(commands.end(),infos[i].pCommandBuffers,infos[i].pCommandBuffers+infos[i].commandBufferCount);
        }
        return restoreTrackedCommands(d,commands);
    } catch(const std::bad_alloc&) { d.selectiveRestore=false; return restoreColdLocked(d.handle,d); }
}
VkResult restoreForQueue(Device& d,const char*,std::uint32_t count,const VkSubmitInfo2* infos,VkFence) {
    if(!d.selectiveRestore || unknownCommandProc) return restoreColdLocked(d.handle,d);
    try {
        std::vector<VkCommandBuffer> commands;
        for(std::uint32_t i=0;i<count;i++) {
            if(!infos || infos[i].pNext) return restoreColdLocked(d.handle,d);
            for(std::uint32_t j=0;j<infos[i].commandBufferInfoCount;j++) {
                if(infos[i].pCommandBufferInfos[j].pNext) return restoreColdLocked(d.handle,d);
                commands.push_back(infos[i].pCommandBufferInfos[j].commandBuffer);
            }
        }
        return restoreTrackedCommands(d,commands);
    } catch(const std::bad_alloc&) { d.selectiveRestore=false; return restoreColdLocked(d.handle,d); }
}

// Resolve live descriptor contents before recording an accepted submission.
// Unknown accesses protect every pool until that queue epoch retires.
template<class... Args>
bool queueMemories(Device&,const char*,std::vector<ActiveRefs::Use>&,Args...) { return false; }
bool commandMemories(Device& d,const std::vector<VkCommandBuffer>& commands,std::vector<ActiveRefs::Use>& memories,bool writesOnly) {
    auto fallback=[&](const char* reason) {
        if(!d.reportedAccessFallback) {
            d.reportedAccessFallback=true;
            logf("access tracking fallback reason=%s command-count=%zu",reason,commands.size());
        }
        return false;
    };
    if(!d.selectiveRestore) return fallback("selective-disabled");
    if(unknownCommandProc) return fallback("unwrapped-command-procedure");
    std::vector<VkSubmissionTracker::BufferRange> ranges;
    const char* trackingReason=nullptr;
    if(!d.submission.collectRanges(static_cast<std::uint32_t>(commands.size()),commands.data(),ranges,&trackingReason)) return fallback(trackingReason ? trackingReason : "command-resources-unknown");
    if(!d.rangeChunkBytes || !d.narrowDescriptorRanges)
        for(auto& range:ranges) { range.offset=0; range.size=VK_WHOLE_SIZE; }
    for(const auto& range:ranges) {
        if(writesOnly && !range.mayWrite) continue;
        const auto p=d.promotedBuffers.find(range.buffer);
        if(p==d.promotedBuffers.end() || !p->second.memory) continue;
        const auto memory=p->second.memory;
        const auto v=d.virtualMemory.find(memory);
        if(v==d.virtualMemory.end()) continue;
        auto add=[&](std::size_t child) {
            ActiveRefs::Use use{memory,child};
            if(std::find(memories.begin(),memories.end(),use)==memories.end()) memories.push_back(use);
        };
        if(!d.rangeChunkBytes) { add(SIZE_MAX); continue; }
        const auto binding=std::find_if(v->second.bindings.begin(),v->second.bindings.end(),
            [&](const auto& entry){return entry.buffer==range.buffer;});
        if(binding==v->second.bindings.end() || range.offset>=p->second.size) return fallback("buffer-binding-or-offset-invalid");
        const auto bytes=range.size==VK_WHOLE_SIZE?p->second.size-range.offset:range.size;
        if(!bytes || bytes>p->second.size-range.offset) return fallback("buffer-range-invalid");
        auto relativeStart=range.offset;
        auto relativeEnd=range.offset+bytes;
        const auto alignment=d.robustRangeAlignment;
        relativeStart-=relativeStart%alignment;
        const auto padding=(alignment-relativeEnd%alignment)%alignment;
        relativeEnd+=std::min(padding,p->second.size-relativeEnd);
        const auto start=binding->memoryOffset+relativeStart;
        const auto end=binding->memoryOffset+relativeEnd;
        VkDeviceSize base=0;
        for(std::size_t i=0;i<v->second.childSizes.size();i++) {
            const auto next=base+v->second.childSizes[i];
            if(start<next && base<end) add(i);
            base=next;
        }
    }
    return true;
}
bool queueMemoryAccess(Device& d,const char*,std::vector<ActiveRefs::Use>& memories,bool writesOnly,std::uint32_t count,const VkSubmitInfo* infos,VkFence) {
    std::vector<VkCommandBuffer> commands;
    for(std::uint32_t i=0;i<count;i++) {
        if(!infos || !trackedSubmitChain(infos[i].pNext)) return false;
        if(infos[i].commandBufferCount) commands.insert(commands.end(),infos[i].pCommandBuffers,infos[i].pCommandBuffers+infos[i].commandBufferCount);
    }
    return commandMemories(d,commands,memories,writesOnly);
}
bool queueMemoryAccess(Device& d,const char*,std::vector<ActiveRefs::Use>& memories,bool writesOnly,std::uint32_t count,const VkSubmitInfo2* infos,VkFence) {
    std::vector<VkCommandBuffer> commands;
    for(std::uint32_t i=0;i<count;i++) {
        if(!infos || infos[i].pNext) return false;
        for(std::uint32_t j=0;j<infos[i].commandBufferInfoCount;j++) {
            if(infos[i].pCommandBufferInfos[j].pNext) return false;
            commands.push_back(infos[i].pCommandBufferInfos[j].commandBuffer);
        }
    }
    return commandMemories(d,commands,memories,writesOnly);
}
template<class... Args>
bool queueMemoryAccess(Device&,const char*,std::vector<ActiveRefs::Use>&,bool,Args...) { return false; }
bool queueMemories(Device& d,const char* name,std::vector<ActiveRefs::Use>& memories,std::uint32_t count,const VkSubmitInfo* infos,VkFence fence) {
    return queueMemoryAccess(d,name,memories,false,count,infos,fence);
}
bool queueMemories(Device& d,const char* name,std::vector<ActiveRefs::Use>& memories,std::uint32_t count,const VkSubmitInfo2* infos,VkFence fence) {
    return queueMemoryAccess(d,name,memories,false,count,infos,fence);
}
// Called after downstream acceptance, while both layer gates remain held.
// Failed submissions leave clean copies intact; uncertain accepted work drops
// every clean copy, including aliases outside its known descriptor ranges.
template<class... Args>
void invalidateAcceptedWrites(Device& d,const char* name,Args... args) {
    if(!d.cleanCache || !d.cacheBytes) return;
    std::vector<ActiveRefs::Use> writes;
    bool known=false;
    try { known=queueMemoryAccess(d,name,writes,true,args...); }
    catch(const std::bad_alloc&) { known=false; }
    for(auto& pair:d.virtualMemory)
        for(std::size_t i=0;i<pair.second.coldGroups.size();++i)
            if(!known || std::any_of(writes.begin(),writes.end(),[&](const auto& use){
                return use.memory==pair.first && (use.child==SIZE_MAX || use.child==i);
            })) discardCleanCacheLocked(d,pair.second,i);
}

// Pressure admission never waits for an application queue: a future timeline
// signal may be needed to release it. Only completed unselected children can
// be displaced. The initial bind and untracked allocations are outside this cap.
template<class... Args>
VkResult admitForQueue(Device&,const char*,Args...) { return VK_SUCCESS; }
template<class Info>
VkResult admitTrackedSubmit(Device& d,const char* name,std::uint32_t count,const Info* infos,VkFence fence) {
    if(!d.residentLimitBytes) return VK_SUCCESS;
    if(!d.rangeChunkBytes) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(!d.residentAdmissionArmed) {
        if(d.residentBytes || !d.coldLogicalBytes) return VK_SUCCESS;
        d.residentAdmissionArmed=true;
        logf("resident admission armed after complete cold transition");
    }
    try {
        std::vector<ActiveRefs::Use> selected;
        const bool known=d.autoEnabled && queueMemories(d,name,selected,count,infos,fence);
        // Empty known submits only advance ordering; do not deny a signal that
        // can release already accepted work behind a timeline wait.
        if(known && selected.empty()) return VK_SUCCESS;
        if(!known) {
            selected.clear();
            for(const auto& pair:d.virtualMemory)
                for(std::size_t i=0;i<pair.second.childSizes.size();++i) selected.push_back({pair.first,i});
        }
        VkDeviceSize limit{};
        const auto budgetResult=residentAdmissionLimit(d,limit); if(budgetResult!=VK_SUCCESS) return budgetResult;
        VkDeviceSize needed=0,incoming=0;
        for(const auto& use:selected) {
            const auto p=d.virtualMemory.find(use.memory);
            if(p==d.virtualMemory.end() || use.child>=p->second.childSizes.size()) return VK_ERROR_UNKNOWN;
            const auto size=p->second.childSizes[use.child];
            if(size>limit-needed) {
                logf("resident admission refused: submission working set exceeds limit-bytes=%llu known=%d",
                     static_cast<unsigned long long>(limit),known);
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
            needed+=size;
            if(!p->second.children[use.child]) incoming+=size;
        }
        if(d.residentBytes<=limit-incoming) return VK_SUCCESS;
        if(d.pendingWarmRecovery.active) {
            d.pendingRecoveryAdmissionWait=true;
            return VK_NOT_READY; // queueCall releases both locks and retries after the copy fence.
        }
        if(!d.autoEnabled) {
            logf("resident admission refused: automatic paging disabled and full backing exceeds limit");
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto ready=d.autoQueues.pollActive(
            [&](VkQueue q){d.activeRefs.retire(q);},[&](VkQueue q){d.activeRefs.cover(q);});
        if(ready!=VK_SUCCESS) {
            d.gpuGateError=ready; d.autoEnabled=false; d.stopWorker.store(true); d.activity.notify_all();
            return ready;
        }
        struct Victim {
            VkDeviceMemory memory;
            std::size_t child;
            std::chrono::steady_clock::time_point used;
            bool cleanSnapshot;
        };
        std::vector<Victim> victims;
        std::size_t busyCount=0;
        VkDeviceSize busyBytes=0,candidateBytes=0;
        const auto addBytes=[](VkDeviceSize& total,VkDeviceSize value) {
            total=value>std::numeric_limits<VkDeviceSize>::max()-total?
                std::numeric_limits<VkDeviceSize>::max():total+value;
        };
        for(const auto& pair:d.virtualMemory) {
            const auto& memory=pair.second;
            for(std::size_t i=0;i<memory.children.size();++i) {
                const auto& group=memory.coldGroups[i];
                const auto childBytes=i<memory.childSizes.size()?memory.childSizes[i]:VkDeviceSize{0};
                if(!memory.children[i] || group.cold) continue;
                if(d.activeRefs.busy(pair.first,i)) {
                    ++busyCount; addBytes(busyBytes,childBytes);
                    continue;
                }
                const ActiveRefs::Use use{pair.first,i};
                if(std::find(selected.begin(),selected.end(),use)==selected.end()) {
                    victims.push_back({pair.first,i,group.lastUse,d.cleanCache && !group.chunks.empty()});
                    addBytes(candidateBytes,childBytes);
                }
            }
        }
        std::sort(victims.begin(),victims.end(),[&](const auto& a,const auto& b){
            if(d.cleanFirstEviction && a.cleanSnapshot!=b.cleanSnapshot) return a.cleanSnapshot;
            return d.mruEviction?a.used>b.used:a.used<b.used;
        });
        const VkDeviceSize residentBeforeEviction=d.residentBytes;
        std::size_t evicted=0,freezeAttempts=0,freezeFailures=0,freezeVkFailures=0;
        for(const auto& victim:victims) {
            if(d.residentBytes<=limit-incoming) break;
            auto& memory=d.virtualMemory.at(victim.memory);
            ++freezeAttempts;
            const auto result=freezeChildLocked(d,memory,victim.child);
            if(d.gpuGateError!=VK_SUCCESS) return d.gpuGateError;
            if(result!=VK_SUCCESS) ++freezeVkFailures;
            if(result==VK_SUCCESS && memory.coldGroups[victim.child].cold) ++evicted;
            else ++freezeFailures;
        }
        if(d.residentBytes>limit-incoming) {
            logf("resident admission refused: completed evictable backing or cold budget unavailable known=%d selected=%zu needed=%llu incoming=%llu limit=%llu resident=%llu active-busy=%zu/%llu candidates=%zu/%llu freeze-attempts=%zu freeze-failures=%zu freeze-vk-failures=%zu evicted=%zu remaining-resident=%llu",
                 known,selected.size(),static_cast<unsigned long long>(needed),
                 static_cast<unsigned long long>(incoming),static_cast<unsigned long long>(limit),
                 static_cast<unsigned long long>(residentBeforeEviction),busyCount,
                 static_cast<unsigned long long>(busyBytes),victims.size(),
                 static_cast<unsigned long long>(candidateBytes),freezeAttempts,freezeFailures,freezeVkFailures,evicted,
                 static_cast<unsigned long long>(d.residentBytes));
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        logf("resident admission selected-chunks=%zu evicted-chunks=%zu resident-before-restore=%llu incoming-bytes=%llu limit-bytes=%llu",
             selected.size(),evicted,static_cast<unsigned long long>(d.residentBytes),
             static_cast<unsigned long long>(incoming),static_cast<unsigned long long>(limit));
        return VK_SUCCESS;
    } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
VkResult admitForQueue(Device& d,const char* name,std::uint32_t count,const VkSubmitInfo* infos,VkFence fence) {
    return admitTrackedSubmit(d,name,count,infos,fence);
}
VkResult admitForQueue(Device& d,const char* name,std::uint32_t count,const VkSubmitInfo2* infos,VkFence fence) {
    return admitTrackedSubmit(d,name,count,infos,fence);
}
