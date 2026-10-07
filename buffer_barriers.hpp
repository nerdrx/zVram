#pragma once

#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>

namespace zvram {
namespace detail {

// The caller must exclude promoted buffers used for external/foreign ownership.

inline bool externalFamily(std::uint32_t family) {
    if(family==VK_QUEUE_FAMILY_EXTERNAL) return true;
#ifdef VK_EXT_queue_family_foreign
    if(family==VK_QUEUE_FAMILY_FOREIGN_EXT) return true;
#endif
    return false;
}

inline bool normalize(VkBufferMemoryBarrier& barrier) {
    if(barrier.srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED &&
       barrier.dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED) return false;
    if(barrier.srcQueueFamilyIndex!=barrier.dstQueueFamilyIndex &&
       !externalFamily(barrier.srcQueueFamilyIndex) &&
       !externalFamily(barrier.dstQueueFamilyIndex)) {
        barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        return true;
    }
    if(barrier.srcQueueFamilyIndex==barrier.dstQueueFamilyIndex) {
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    }
    return false;
}

inline bool normalize(VkBufferMemoryBarrier2& barrier) {
    if(barrier.srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED &&
       barrier.dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED) return false;
    if(barrier.srcQueueFamilyIndex!=barrier.dstQueueFamilyIndex &&
       !externalFamily(barrier.srcQueueFamilyIndex) &&
       !externalFamily(barrier.dstQueueFamilyIndex)) {
        barrier.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        return true;
    }
    if(barrier.srcQueueFamilyIndex==barrier.dstQueueFamilyIndex) {
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    }
    return false;
}

template<class IsForced>
inline bool anyForced(IsForced& isForced,std::uint32_t count,
                      const VkBufferMemoryBarrier* barriers) {
    for(std::uint32_t i=0;i<count;i++)
        if(isForced(barriers[i].buffer) &&
           (barriers[i].srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED ||
            barriers[i].dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED)) return true;
    return false;
}

template<class IsForced>
inline bool anyForced(IsForced& isForced,std::uint32_t count,
                      const VkBufferMemoryBarrier2* barriers) {
    for(std::uint32_t i=0;i<count;i++)
        if(isForced(barriers[i].buffer) &&
           (barriers[i].srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED ||
            barriers[i].dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED)) return true;
    return false;
}

template<class IsForced>
inline void copyChunk(std::array<VkBufferMemoryBarrier,32>& chunk,
                      std::uint32_t start,std::uint32_t count,
                      const VkBufferMemoryBarrier* src,IsForced& isForced,
                      bool& ownership) {
    for(std::uint32_t i=0;i<count;i++) {
        chunk[i]=src[start+i];
        if(isForced(chunk[i].buffer)) ownership|=normalize(chunk[i]);
    }
}

template<class IsForced>
inline void copyChunk(std::array<VkBufferMemoryBarrier2,32>& chunk,
                      std::uint32_t start,std::uint32_t count,
                      const VkBufferMemoryBarrier2* src,IsForced& isForced,
                      bool& ownership) {
    for(std::uint32_t i=0;i<count;i++) {
        chunk[i]=src[start+i];
        if(isForced(chunk[i].buffer)) ownership|=normalize(chunk[i]);
    }
}

} // namespace detail

template<class IsForced>
inline void cmdPipelineBarrier(PFN_vkCmdPipelineBarrier fn,IsForced isForced,
    VkCommandBuffer commandBuffer,VkPipelineStageFlags srcStage,
    VkPipelineStageFlags dstStage,VkDependencyFlags dependencyFlags,
    std::uint32_t memoryCount,const VkMemoryBarrier* memoryBarriers,
    std::uint32_t bufferCount,const VkBufferMemoryBarrier* bufferBarriers,
    std::uint32_t imageCount,const VkImageMemoryBarrier* imageBarriers) {
    if(!fn || !bufferCount || !bufferBarriers ||
       !detail::anyForced(isForced,bufferCount,bufferBarriers)) {
        if(fn) fn(commandBuffer,srcStage,dstStage,dependencyFlags,memoryCount,memoryBarriers,
                  bufferCount,bufferBarriers,imageCount,imageBarriers);
        return;
    }
    std::array<VkBufferMemoryBarrier,32> chunk{};
    bool first=true;
    for(std::uint32_t start=0;start<bufferCount;) {
        const auto count=std::min<std::uint32_t>(32,bufferCount-start);
        bool ownership=false;
        detail::copyChunk(chunk,start,count,bufferBarriers,isForced,ownership);
        fn(commandBuffer,ownership?VkPipelineStageFlags(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT):srcStage,
           ownership?VkPipelineStageFlags(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT):dstStage,dependencyFlags,
           first?memoryCount:0,first?memoryBarriers:nullptr,count,chunk.data(),
           first?imageCount:0,first?imageBarriers:nullptr);
        first=false; start+=count;
    }
}

template<class IsForced>
inline void cmdWaitEvents(PFN_vkCmdWaitEvents fn,IsForced isForced,
    VkCommandBuffer commandBuffer,std::uint32_t eventCount,const VkEvent* events,
    VkPipelineStageFlags srcStage,VkPipelineStageFlags dstStage,
    std::uint32_t memoryCount,const VkMemoryBarrier* memoryBarriers,
    std::uint32_t bufferCount,const VkBufferMemoryBarrier* bufferBarriers,
    std::uint32_t imageCount,const VkImageMemoryBarrier* imageBarriers) {
    if(!fn || !bufferCount || !bufferBarriers ||
       !detail::anyForced(isForced,bufferCount,bufferBarriers)) {
        if(fn) fn(commandBuffer,eventCount,events,srcStage,dstStage,memoryCount,memoryBarriers,
                  bufferCount,bufferBarriers,imageCount,imageBarriers);
        return;
    }
    std::array<VkBufferMemoryBarrier,32> chunk{};
    bool first=true;
    for(std::uint32_t start=0;start<bufferCount;) {
        const auto count=std::min<std::uint32_t>(32,bufferCount-start);
        bool ownership=false;
        detail::copyChunk(chunk,start,count,bufferBarriers,isForced,ownership);
        // Keep both stage masks unchanged: srcStageMask selects which earlier
        // vkCmdSetEvent signals this wait includes.
        (void)ownership;
        fn(commandBuffer,eventCount,events,srcStage,dstStage,
           first?memoryCount:0,first?memoryBarriers:nullptr,count,chunk.data(),
           first?imageCount:0,first?imageBarriers:nullptr);
        first=false; start+=count;
    }
}

template<class IsForced>
inline void cmdPipelineBarrier2(PFN_vkCmdPipelineBarrier2 fn,IsForced isForced,
    VkCommandBuffer commandBuffer,const VkDependencyInfo* dependency) {
    if(!fn || !dependency || !dependency->bufferMemoryBarrierCount ||
       !dependency->pBufferMemoryBarriers ||
       !detail::anyForced(isForced,dependency->bufferMemoryBarrierCount,
                          dependency->pBufferMemoryBarriers)) {
        if(fn) fn(commandBuffer,dependency);
        return;
    }
    std::array<VkBufferMemoryBarrier2,32> chunk{};
    bool first=true;
    for(std::uint32_t start=0;start<dependency->bufferMemoryBarrierCount;) {
        const auto count=std::min<std::uint32_t>(32,dependency->bufferMemoryBarrierCount-start);
        bool ownership=false;
        detail::copyChunk(chunk,start,count,dependency->pBufferMemoryBarriers,isForced,ownership);
        VkDependencyInfo part=*dependency;
        part.bufferMemoryBarrierCount=count; part.pBufferMemoryBarriers=chunk.data();
        if(!first) { part.memoryBarrierCount=0; part.pMemoryBarriers=nullptr;
                     part.imageMemoryBarrierCount=0; part.pImageMemoryBarriers=nullptr; }
        fn(commandBuffer,&part);
        first=false; start+=count;
    }
}

template<class IsForced>
inline void cmdWaitEvents2(PFN_vkCmdWaitEvents2 fn,IsForced isForced,
    VkCommandBuffer commandBuffer,std::uint32_t eventCount,const VkEvent* events,
    const VkDependencyInfo* dependencies) {
    (void)isForced;
    // Without asymmetric-event mode, wait dependencies must match the paired
    // vkCmdSetEvent2 dependency exactly. Concurrent buffers do not define
    // queue-family ownership transfers in sync2, so leave these unchanged.
    if(fn) fn(commandBuffer,eventCount,events,dependencies);
}

} // namespace zvram
