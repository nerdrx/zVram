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

bool commandMemories(Device&,const std::vector<VkCommandBuffer>&,std::vector<ActiveRefs::Use>&);

// Called with the existing device and queue gates held.
VkResult restoreTrackedCommands(Device& d,const std::vector<VkCommandBuffer>& commands) {
    if(!d.selectiveRestore || unknownCommandProc) return restoreColdLocked(d.handle,d);
    try {
        std::vector<ActiveRefs::Use> selected;
        if(!commandMemories(d,commands,selected)) {
            logf("selective restore fallback: untracked command, descriptor or address shader");
            return restoreColdLocked(d.handle,d);
        }
        std::size_t restored=0;
        for(const auto& use:selected) {
            const auto memory=use.memory;
            const auto p=d.virtualMemory.find(memory);
            if(p==d.virtualMemory.end() || !p->second.cold ||
               (use.child!=SIZE_MAX && !p->second.coldGroups[use.child].cold)) continue;
            const auto r=restoreColdLocked(d.handle,d,memory,use.child); if(r!=VK_SUCCESS) return r;
            ++restored;
        }
        std::size_t remaining=0;
        for(const auto& p:d.virtualMemory) remaining+=p.second.cold;
        if(restored) {
            if(d.rangeChunkBytes) logf("selective range restore selected-chunks=%zu restored-chunks=%zu cold-pools-left=%zu",selected.size(),restored,remaining);
            else logf("selective restore selected-pools=%zu restored-pools=%zu cold-pools-left=%zu",selected.size(),restored,remaining);
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
bool commandMemories(Device& d,const std::vector<VkCommandBuffer>& commands,std::vector<ActiveRefs::Use>& memories) {
    if(!d.selectiveRestore || unknownCommandProc) return false;
    std::vector<VkSubmissionTracker::BufferRange> ranges;
    if(d.rangeChunkBytes && d.narrowDescriptorRanges) {
        if(!d.submission.collectRanges(static_cast<std::uint32_t>(commands.size()),commands.data(),ranges)) return false;
    } else {
        std::vector<VkBuffer> buffers;
        if(!d.submission.collect(static_cast<std::uint32_t>(commands.size()),commands.data(),buffers)) return false;
        for(auto buffer:buffers) ranges.push_back({buffer,0,VK_WHOLE_SIZE});
    }
    for(const auto& range:ranges) {
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
        if(binding==v->second.bindings.end() || range.offset>=p->second.size) return false;
        const auto bytes=range.size==VK_WHOLE_SIZE?p->second.size-range.offset:range.size;
        if(!bytes || bytes>p->second.size-range.offset) return false;
        const auto start=binding->memoryOffset+range.offset;
        const auto end=start+bytes;
        VkDeviceSize base=0;
        for(std::size_t i=0;i<v->second.childSizes.size();i++) {
            const auto next=base+v->second.childSizes[i];
            if(start<next && base<end) add(i);
            base=next;
        }
    }
    return true;
}
bool queueMemories(Device& d,const char*,std::vector<ActiveRefs::Use>& memories,std::uint32_t count,const VkSubmitInfo* infos,VkFence) {
    std::vector<VkCommandBuffer> commands;
    for(std::uint32_t i=0;i<count;i++) {
        if(!infos || !trackedSubmitChain(infos[i].pNext)) return false;
        if(infos[i].commandBufferCount) commands.insert(commands.end(),infos[i].pCommandBuffers,infos[i].pCommandBuffers+infos[i].commandBufferCount);
    }
    return commandMemories(d,commands,memories);
}
bool queueMemories(Device& d,const char*,std::vector<ActiveRefs::Use>& memories,std::uint32_t count,const VkSubmitInfo2* infos,VkFence) {
    std::vector<VkCommandBuffer> commands;
    for(std::uint32_t i=0;i<count;i++) {
        if(!infos || infos[i].pNext) return false;
        for(std::uint32_t j=0;j<infos[i].commandBufferInfoCount;j++) {
            if(infos[i].pCommandBufferInfos[j].pNext) return false;
            commands.push_back(infos[i].pCommandBufferInfos[j].commandBuffer);
        }
    }
    return commandMemories(d,commands,memories);
}
