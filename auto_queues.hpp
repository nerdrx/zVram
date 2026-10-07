#pragma once

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

// Caller serializes application queue entry and these downstream operations.
// Fence markers detect completion without waiting on an unresolved app semaphore.
class ZvramAutoQueues {
    struct Queue {
        VkQueue handle{};
        VkFence marker{};
        VkSemaphore bridge{};
        std::uint64_t generation{}, markedGeneration{};
        bool pending{};
    };
    VkDevice device_{};
    VkQueue copy_{};
    std::vector<Queue> queues_;
    std::vector<VkSemaphore> waits_;
    std::vector<VkPipelineStageFlags> stages_;
    VkSemaphore beforeSparse_{}, afterSparse_{};
    PFN_vkCreateFence createFence_{};
    PFN_vkDestroyFence destroyFence_{};
    PFN_vkGetFenceStatus fenceStatus_{};
    PFN_vkResetFences resetFences_{};
    PFN_vkCreateSemaphore createSemaphore_{};
    PFN_vkDestroySemaphore destroySemaphore_{};
    PFN_vkQueueSubmit submit_{};
    PFN_vkQueueBindSparse bindSparse_{};
    PFN_vkQueueWaitIdle waitIdle_{};
    VkResult error_{VK_SUCCESS};
    VkResult checked(VkResult result) {
        if(result!=VK_SUCCESS) error_=result;
        return result;
    }

    VkResult marker(Queue& q) {
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        const auto result=submit_(q.handle,1,&info,q.marker);
        if(result==VK_SUCCESS) { q.pending=true; q.markedGeneration=q.generation; }
        return checked(result);
    }
public:
    ZvramAutoQueues()=default;
    ZvramAutoQueues(const ZvramAutoQueues&)=delete;
    ZvramAutoQueues& operator=(const ZvramAutoQueues&)=delete;

    VkResult init(VkDevice device,PFN_vkGetDeviceProcAddr get,VkQueue copy,
                  const std::vector<VkQueue>& handles) {
        device_=device; copy_=copy;
#define LOAD(field,type,name) field=reinterpret_cast<type>(get(device,name))
        LOAD(createFence_,PFN_vkCreateFence,"vkCreateFence");
        LOAD(destroyFence_,PFN_vkDestroyFence,"vkDestroyFence");
        LOAD(fenceStatus_,PFN_vkGetFenceStatus,"vkGetFenceStatus");
        LOAD(resetFences_,PFN_vkResetFences,"vkResetFences");
        LOAD(createSemaphore_,PFN_vkCreateSemaphore,"vkCreateSemaphore");
        LOAD(destroySemaphore_,PFN_vkDestroySemaphore,"vkDestroySemaphore");
        LOAD(submit_,PFN_vkQueueSubmit,"vkQueueSubmit");
        LOAD(bindSparse_,PFN_vkQueueBindSparse,"vkQueueBindSparse");
        LOAD(waitIdle_,PFN_vkQueueWaitIdle,"vkQueueWaitIdle");
#undef LOAD
        if(!copy || !createFence_ || !destroyFence_ || !fenceStatus_ || !resetFences_ ||
           !createSemaphore_ || !destroySemaphore_ || !submit_ || !bindSparse_ || !waitIdle_)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        try {
            queues_.reserve(handles.size()); waits_.reserve(handles.size()); stages_.reserve(handles.size());
            for(auto handle:handles) {
                if(!handle) return VK_ERROR_INITIALIZATION_FAILED;
                if(std::any_of(queues_.begin(),queues_.end(),[&](const Queue& q){return q.handle==handle;})) continue;
                queues_.push_back(Queue{}); auto& q=queues_.back(); q.handle=handle;
                VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                auto result=createFence_(device_,&fence,nullptr,&q.marker); if(result!=VK_SUCCESS) return result;
                if(handle!=copy_) {
                    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                    result=createSemaphore_(device_,&semaphore,nullptr,&q.bridge); if(result!=VK_SUCCESS) return result;
                    waits_.push_back(q.bridge); stages_.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                }
            }
        } catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        if(std::none_of(queues_.begin(),queues_.end(),[&](const Queue& q){return q.handle==copy_;}))
            return VK_ERROR_INITIALIZATION_FAILED;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        auto result=createSemaphore_(device_,&semaphore,nullptr,&beforeSparse_); if(result!=VK_SUCCESS) return result;
        return createSemaphore_(device_,&semaphore,nullptr,&afterSparse_);
    }

    // A marker failure must disable future snapshots, not change the result of
    // already accepted app work (which the app could otherwise submit twice).
    VkResult submitted(VkQueue handle) {
        if(error_!=VK_SUCCESS) return error_;
        auto found=std::find_if(queues_.begin(),queues_.end(),[&](const Queue& q){return q.handle==handle;});
        if(found==queues_.end()) return checked(VK_ERROR_FEATURE_NOT_PRESENT);
        auto& q=*found;
        if(q.generation==std::numeric_limits<std::uint64_t>::max()) return checked(VK_ERROR_UNKNOWN);
        ++q.generation;
        return q.pending?VK_SUCCESS:marker(q);
    }

    VkResult ready(bool& allReady) {
        if(error_!=VK_SUCCESS) { allReady=false; return error_; }
        allReady=true;
        for(auto& q:queues_) {
            if(!q.pending) continue;
            auto result=fenceStatus_(device_,q.marker);
            if(result==VK_NOT_READY) { allReady=false; continue; }
            if(result!=VK_SUCCESS) return checked(result);
            if(q.markedGeneration==q.generation) continue;
            result=resetFences_(device_,1,&q.marker); if(result!=VK_SUCCESS) return checked(result);
            q.pending=false;
            result=marker(q); if(result!=VK_SUCCESS) return result;
            allReady=false;
        }
        return VK_SUCCESS;
    }

    VkResult toCopyQueue() {
        if(error_!=VK_SUCCESS) return error_;
        if(waits_.empty()) return VK_SUCCESS;
        for(auto& q:queues_) if(q.bridge) {
            VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            signal.signalSemaphoreCount=1; signal.pSignalSemaphores=&q.bridge;
            const auto result=submit_(q.handle,1,&signal,VK_NULL_HANDLE); if(result!=VK_SUCCESS) return checked(result);
        }
        VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        wait.waitSemaphoreCount=static_cast<std::uint32_t>(waits_.size()); wait.pWaitSemaphores=waits_.data();
        wait.pWaitDstStageMask=stages_.data();
        auto result=submit_(copy_,1,&wait,VK_NULL_HANDLE);
        return checked(result==VK_SUCCESS?waitIdle_(copy_):result);
    }

    VkResult fromCopyQueue() {
        if(error_!=VK_SUCCESS) return error_;
        if(waits_.empty()) return VK_SUCCESS;
        VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        signal.signalSemaphoreCount=static_cast<std::uint32_t>(waits_.size()); signal.pSignalSemaphores=waits_.data();
        auto result=submit_(copy_,1,&signal,VK_NULL_HANDLE); if(result!=VK_SUCCESS) return checked(result);
        const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        for(auto& q:queues_) if(q.bridge) {
            VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            wait.waitSemaphoreCount=1; wait.pWaitSemaphores=&q.bridge; wait.pWaitDstStageMask=&stage;
            result=submit_(q.handle,1,&wait,VK_NULL_HANDLE); if(result!=VK_SUCCESS) return checked(result);
        }
        // App work submitted next is ordered after these waits. The next cold
        // cycle must pass ready() before reusing the bridges in toCopyQueue().
        // Never wait here on an app queue whose later signal may need another
        // intercepted submission from the calling thread.
        return VK_SUCCESS;
    }

    VkResult sparseBind(const VkSparseBufferMemoryBindInfo& buffer) {
        if(error_!=VK_SUCCESS) return error_;
        VkSubmitInfo before{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        before.signalSemaphoreCount=1; before.pSignalSemaphores=&beforeSparse_;
        auto result=submit_(copy_,1,&before,VK_NULL_HANDLE); if(result!=VK_SUCCESS) return checked(result);
        VkBindSparseInfo bind{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        bind.waitSemaphoreCount=1; bind.pWaitSemaphores=&beforeSparse_;
        bind.signalSemaphoreCount=1; bind.pSignalSemaphores=&afterSparse_;
        bind.bufferBindCount=1; bind.pBufferBinds=&buffer;
        result=bindSparse_(copy_,1,&bind,VK_NULL_HANDLE); if(result!=VK_SUCCESS) return checked(result);
        const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo after{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        after.waitSemaphoreCount=1; after.pWaitSemaphores=&afterSparse_; after.pWaitDstStageMask=&stage;
        result=submit_(copy_,1,&after,VK_NULL_HANDLE);
        return checked(result==VK_SUCCESS?waitIdle_(copy_):result);
    }

    // Only after all private operations have completed, or at device teardown.
    void destroy() {
        for(auto& q:queues_) {
            if(q.marker && destroyFence_) destroyFence_(device_,q.marker,nullptr);
            if(q.bridge && destroySemaphore_) destroySemaphore_(device_,q.bridge,nullptr);
        }
        if(beforeSparse_ && destroySemaphore_) destroySemaphore_(device_,beforeSparse_,nullptr);
        if(afterSparse_ && destroySemaphore_) destroySemaphore_(device_,afterSparse_,nullptr);
        queues_.clear(); waits_.clear(); stages_.clear(); beforeSparse_={}; afterSparse_={};
    }
};
