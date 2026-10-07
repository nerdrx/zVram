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

// Called with the existing device and queue gates held.
VkResult restoreTrackedCommands(Device& d,const std::vector<VkCommandBuffer>& commands) {
    if(!d.selectiveRestore || unknownCommandProc) return restoreColdLocked(d.handle,d);
    try {
        std::vector<VkBuffer> buffers;
        if(!d.submission.collect(static_cast<std::uint32_t>(commands.size()),commands.data(),buffers)) {
            logf("selective restore fallback: untracked command, descriptor or address shader");
            return restoreColdLocked(d.handle,d);
        }
        std::vector<VkDeviceMemory> selected;
        for(auto buffer:buffers) {
            const auto p=d.promotedBuffers.find(buffer);
            if(p!=d.promotedBuffers.end() && p->second.memory &&
               std::find(selected.begin(),selected.end(),p->second.memory)==selected.end()) selected.push_back(p->second.memory);
        }
        std::size_t restored=0;
        for(auto memory:selected) {
            const auto p=d.virtualMemory.find(memory);
            if(p==d.virtualMemory.end() || !p->second.cold) continue;
            const auto r=restoreColdLocked(d.handle,d,memory); if(r!=VK_SUCCESS) return r;
            ++restored;
        }
        std::size_t remaining=0;
        for(const auto& p:d.virtualMemory) remaining+=p.second.cold;
        if(restored) logf("selective restore selected-pools=%zu restored-pools=%zu cold-pools-left=%zu",selected.size(),restored,remaining);
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
