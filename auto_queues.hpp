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
        VkSemaphore activeCompletion{};
        std::uint64_t generation{}, markedGeneration{};
        bool pending{};
    };
    struct RestoreWait {
        VkSemaphore semaphore{};
        VkFence fence{};
        bool submitted{};
    };
    VkDevice device_{};
    VkQueue copy_{};
    std::vector<Queue> queues_;
    std::vector<VkSemaphore> waits_;
    std::vector<VkPipelineStageFlags> stages_;
    std::vector<RestoreWait> restoreWaits_;
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
    bool activeMarkers_{};
    VkResult checked(VkResult result) {
        if(result!=VK_SUCCESS) error_=result;
        return result;
    }

    VkResult marker(Queue& q) {
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if(activeMarkers_ && q.activeCompletion) {
            info.signalSemaphoreCount=1;
            info.pSignalSemaphores=&q.activeCompletion;
        }
        const auto result=submit_(q.handle,1,&info,q.marker);
        if(result==VK_SUCCESS) { q.pending=true; q.markedGeneration=q.generation; }
        return checked(result);
    }
public:
    ZvramAutoQueues()=default;
    ZvramAutoQueues(const ZvramAutoQueues&)=delete;
    ZvramAutoQueues& operator=(const ZvramAutoQueues&)=delete;

    VkResult init(VkDevice device,PFN_vkGetDeviceProcAddr get,VkQueue copy,
                  const std::vector<VkQueue>& handles,bool activeMarkers=false) {
        device_=device; copy_=copy; activeMarkers_=activeMarkers;
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
                    if(activeMarkers_) {
                        result=createSemaphore_(device_,&semaphore,nullptr,&q.activeCompletion);
                        if(result!=VK_SUCCESS) return result;
                    } else {
                        result=createSemaphore_(device_,&semaphore,nullptr,&q.bridge); if(result!=VK_SUCCESS) return result;
                        waits_.push_back(q.bridge); stages_.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                    }
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
    VkResult submitted(VkQueue handle,bool& marked) {
        marked=false;
        if(error_!=VK_SUCCESS) return error_;
        auto found=std::find_if(queues_.begin(),queues_.end(),[&](const Queue& q){return q.handle==handle;});
        if(found==queues_.end()) return checked(VK_ERROR_FEATURE_NOT_PRESENT);
        auto& q=*found;
        if(activeMarkers_ && handle==copy_) return VK_SUCCESS;
        if(q.generation==std::numeric_limits<std::uint64_t>::max()) return checked(VK_ERROR_UNKNOWN);
        ++q.generation;
        if(q.pending) return VK_SUCCESS;
        const auto result=marker(q);
        marked=result==VK_SUCCESS;
        return result;
    }

    VkResult submitted(VkQueue handle) {
        bool marked{};
        return submitted(handle,marked);
    }

    VkResult ready(bool& allReady) {
        if(activeMarkers_) { allReady=false; return checked(VK_ERROR_FEATURE_NOT_PRESENT); }
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

    // Active mode consumes only completed marker semaphores on the private
    // queue. Callbacks receive the app queue whose current tail just retired
    // or was newly covered, respectively.
    template<class Retired,class Covered>
    VkResult pollActive(Retired&& retired,Covered&& covered) {
        if(error_!=VK_SUCCESS) return error_;
        if(!activeMarkers_) return checked(VK_ERROR_FEATURE_NOT_PRESENT);
        for(auto& q:queues_) {
            if(!q.pending || !q.activeCompletion) continue;
            auto result=fenceStatus_(device_,q.marker);
            if(result==VK_NOT_READY) continue;
            if(result!=VK_SUCCESS) return checked(result);
            VkSubmitInfo consume{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            consume.waitSemaphoreCount=1;
            consume.pWaitSemaphores=&q.activeCompletion;
            const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            consume.pWaitDstStageMask=&stage;
            result=submit_(copy_,1,&consume,VK_NULL_HANDLE);
            if(result!=VK_SUCCESS) return checked(result);
            result=waitIdle_(copy_);
            if(result!=VK_SUCCESS) return checked(result);
            const bool tail=q.markedGeneration==q.generation;
            retired(q.handle);
            result=resetFences_(device_,1,&q.marker);
            if(result!=VK_SUCCESS) return checked(result);
            q.pending=false;
            if(!tail) {
                result=marker(q);
                if(result!=VK_SUCCESS) return result;
                covered(q.handle);
            }
        }
        for(auto it=restoreWaits_.begin();it!=restoreWaits_.end();) {
            if(!it->submitted) { ++it; continue; }
            const auto result=fenceStatus_(device_,it->fence);
            if(result==VK_NOT_READY) { ++it; continue; }
            if(result!=VK_SUCCESS) return checked(result);
            destroyFence_(device_,it->fence,nullptr);
            destroySemaphore_(device_,it->semaphore,nullptr);
            it=restoreWaits_.erase(it);
        }
        return VK_SUCCESS;
    }

    // Called only after downstream vkQueueWaitIdle succeeded while queue entry
    // remains serialized. If pollActive covers a newer tail, the new marker is
    // the only new work on this already idle queue, so completing it cannot
    // wait for an unresolved application semaphore. Never wait on other queues.
    template<class Retired,class Covered>
    VkResult finishIdleActive(VkQueue handle,Retired&& retired,Covered&& covered) {
        auto result=pollActive(retired,covered);
        if(result!=VK_SUCCESS) return result;
        const auto q=std::find_if(queues_.begin(),queues_.end(),[&](const Queue& item){return item.handle==handle;});
        if(q==queues_.end()) return checked(VK_ERROR_FEATURE_NOT_PRESENT);
        if(q->pending) {
            result=waitIdle_(handle); if(result!=VK_SUCCESS) return checked(result);
            result=pollActive(retired,covered); if(result!=VK_SUCCESS) return result;
        }
        return q->pending?checked(VK_ERROR_UNKNOWN):VK_SUCCESS;
    }

    // Active restore visibility goes only to the queue that will use the
    // restored resource. Its temporary fence lets pollActive reclaim the
    // binary semaphore without ever CPU-waiting on an app queue.
    VkResult fromCopyQueueActive(VkQueue target) {
        if(error_!=VK_SUCCESS) return error_;
        if(!activeMarkers_) return checked(VK_ERROR_FEATURE_NOT_PRESENT);
        if(!target || target==copy_ || std::none_of(queues_.begin(),queues_.end(),
                [&](const Queue& q){return q.handle==target;}))
            return checked(VK_ERROR_FEATURE_NOT_PRESENT);
        RestoreWait item{};
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        auto result=createSemaphore_(device_,&semaphore,nullptr,&item.semaphore);
        if(result!=VK_SUCCESS) return checked(result);
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result=createFence_(device_,&fence,nullptr,&item.fence);
        if(result!=VK_SUCCESS) {
            destroySemaphore_(device_,item.semaphore,nullptr);
            return checked(result);
        }
        try { restoreWaits_.push_back(item); }
        catch(const std::bad_alloc&) {
            destroyFence_(device_,item.fence,nullptr);
            destroySemaphore_(device_,item.semaphore,nullptr);
            return checked(VK_ERROR_OUT_OF_HOST_MEMORY);
        }
        auto& stored=restoreWaits_.back();
        VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        signal.signalSemaphoreCount=1; signal.pSignalSemaphores=&stored.semaphore;
        result=submit_(copy_,1,&signal,VK_NULL_HANDLE);
        if(result!=VK_SUCCESS) return checked(result);
        VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        wait.waitSemaphoreCount=1; wait.pWaitSemaphores=&stored.semaphore;
        wait.pWaitDstStageMask=&stage;
        result=submit_(target,1,&wait,stored.fence);
        if(result!=VK_SUCCESS) return checked(result);
        stored.submitted=true;
        return VK_SUCCESS;
    }

    VkResult toCopyQueue() {
        if(activeMarkers_) return checked(VK_ERROR_FEATURE_NOT_PRESENT);
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
        if(activeMarkers_) return checked(VK_ERROR_FEATURE_NOT_PRESENT);
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
        return sparseBind(&buffer, 1);
    }

    VkResult sparseBind(const VkSparseBufferMemoryBindInfo* buffers, std::uint32_t count) {
        if(error_!=VK_SUCCESS) return error_;
        if(!count) return VK_SUCCESS;
        if(!buffers) return checked(VK_ERROR_INITIALIZATION_FAILED);
        VkSubmitInfo before{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        before.signalSemaphoreCount=1; before.pSignalSemaphores=&beforeSparse_;
        auto result=submit_(copy_,1,&before,VK_NULL_HANDLE); if(result!=VK_SUCCESS) return checked(result);
        VkBindSparseInfo bind{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        bind.waitSemaphoreCount=1; bind.pWaitSemaphores=&beforeSparse_;
        bind.signalSemaphoreCount=1; bind.pSignalSemaphores=&afterSparse_;
        bind.bufferBindCount=count; bind.pBufferBinds=buffers;
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
            if(q.activeCompletion && destroySemaphore_) destroySemaphore_(device_,q.activeCompletion,nullptr);
        }
        for(auto& wait:restoreWaits_) {
            if(wait.fence && destroyFence_) destroyFence_(device_,wait.fence,nullptr);
            if(wait.semaphore && destroySemaphore_) destroySemaphore_(device_,wait.semaphore,nullptr);
        }
        if(beforeSparse_ && destroySemaphore_) destroySemaphore_(device_,beforeSparse_,nullptr);
        if(afterSparse_ && destroySemaphore_) destroySemaphore_(device_,afterSparse_,nullptr);
        queues_.clear(); waits_.clear(); stages_.clear(); restoreWaits_.clear();
        beforeSparse_={}; afterSparse_={}; activeMarkers_=false;
    }
};
