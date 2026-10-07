#include "submission_tracking.hpp"

#include <cstdlib>
#include <iostream>
#include <type_traits>

template <typename Handle>
Handle fakeHandle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<Handle>)
        return reinterpret_cast<Handle>(value);
    else
        return static_cast<Handle>(value);
}

static void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

static bool hasRange(const std::vector<VkSubmissionTracker::BufferRange>& ranges,
                     VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
    for (const auto& range : ranges)
        if (range.buffer == buffer && range.offset == offset && range.size == size) return true;
    return false;
}

int main() {
    const std::uint32_t logicalWords[] = {
        0x07230203u, 0x00010000u, 0u, 8u, 0u,
        (3u << 16) | 14u, 0u, 1u
    };
    const std::uint32_t physicalWords[] = {
        0x07230203u, 0x00010000u, 0u, 8u, 0u,
        (2u << 16) | 17u, 5347u,
        (3u << 16) | 14u, 5348u, 1u
    };
    const std::uint32_t malformedWords[] = {
        0x07230203u, 0x00010000u, 0u, 8u, 0u,
        (5u << 16) | 14u, 0u, 1u
    };
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = sizeof(logicalWords);
    shaderInfo.pCode = logicalWords;

    VkSubmissionTracker tracker;
    const auto logicalShader = fakeHandle<VkShaderModule>(1);
    const auto physicalShader = fakeHandle<VkShaderModule>(2);
    const auto malformedShader = fakeHandle<VkShaderModule>(3);
    tracker.shader(logicalShader, &shaderInfo);
    shaderInfo.codeSize = sizeof(physicalWords);
    shaderInfo.pCode = physicalWords;
    tracker.shader(physicalShader, &shaderInfo);
    shaderInfo.codeSize = sizeof(malformedWords);
    shaderInfo.pCode = malformedWords;
    tracker.shader(malformedShader, &shaderInfo);
    require(tracker.shaderKind(logicalShader) == VkSubmissionTracker::ShaderKind::Logical,
            "logical SPIR-V classification");
    require(tracker.shaderKind(physicalShader) == VkSubmissionTracker::ShaderKind::PhysicalStorageBuffer,
            "physical-storage SPIR-V classification");
    require(tracker.shaderKind(malformedShader) == VkSubmissionTracker::ShaderKind::Unknown,
            "malformed SPIR-V rejected");

    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    const auto layout = fakeHandle<VkDescriptorSetLayout>(10);
    tracker.layout(layout, layoutInfo);
    require(tracker.layoutSafe(layout), "ordinary fixed descriptor layout accepted");

    const auto pool = fakeHandle<VkDescriptorPool>(11);
    const auto sourceSet = fakeHandle<VkDescriptorSet>(12);
    const auto copiedSet = fakeHandle<VkDescriptorSet>(13);
    const auto emptySet = fakeHandle<VkDescriptorSet>(14);
    const auto partialSet = fakeHandle<VkDescriptorSet>(15);
    const VkDescriptorSetLayout setLayouts[] = {layout, layout, layout, layout};
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = pool;
    setInfo.descriptorSetCount = 4;
    setInfo.pSetLayouts = setLayouts;
    const VkDescriptorSet allocated[] = {sourceSet, copiedSet, emptySet, partialSet};
    tracker.allocateSets(&setInfo, allocated);

    const auto bufferA = fakeHandle<VkBuffer>(21);
    const auto bufferB = fakeHandle<VkBuffer>(22);
    const auto bufferC = fakeHandle<VkBuffer>(23);
    const auto bufferD = fakeHandle<VkBuffer>(24);
    VkDescriptorBufferInfo bufferInfos[] = {{bufferA, 11, 17}, {bufferB, 22, 18}, {bufferC, 33, 19}};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sourceSet;
    write.dstBinding = 0;
    write.descriptorCount = 3;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = bufferInfos;
    tracker.updateSets(1, &write, 0, nullptr);

    VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
    copy.srcSet = sourceSet;
    copy.dstSet = copiedSet;
    copy.descriptorCount = 3;
    tracker.updateSets(0, nullptr, 1, &copy);

    const auto pipeline = fakeHandle<VkPipeline>(31);
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = logicalShader;
    tracker.computePipeline(pipeline, pipelineInfo);

    const auto commandPool = fakeHandle<VkCommandPool>(41);
    const auto primary = fakeHandle<VkCommandBuffer>(42);
    const auto secondary = fakeHandle<VkCommandBuffer>(43);
    const VkCommandBuffer commands[] = {primary, secondary};
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool;
    commandInfo.commandBufferCount = 2;
    tracker.allocateCommands(&commandInfo, commands);
    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    tracker.descriptors(primary, 1, &copiedSet);
    tracker.bufferRange(secondary, bufferD, 7, 9);
    tracker.buffer(primary, bufferD);
    tracker.secondary(primary, 1, &secondary);

    std::vector<VkBuffer> found;
    require(tracker.collect(1, &primary, found), "tracked descriptors and secondary collect");
    require(found.size() == 4, "collection includes copied buffers and secondary buffer");
    std::vector<VkSubmissionTracker::BufferRange> ranges;
    require(tracker.collectRanges(1, &primary, ranges), "tracked descriptor ranges and secondary collect");
    require(ranges.size() == 5, "ranges preserve descriptor, secondary, and direct references");
    require(hasRange(ranges, bufferA, 11, 17) && hasRange(ranges, bufferB, 22, 18) &&
            hasRange(ranges, bufferC, 33, 19), "descriptor offsets and ranges survive descriptor copy");
    require(hasRange(ranges, bufferD, 7, 9) && hasRange(ranges, bufferD, 0, VK_WHOLE_SIZE),
            "secondary range and direct whole-buffer reference both survive");

    // Updates after recording resolve at submit; copying again preserves source's latest value.
    bufferInfos[2] = {bufferD, 44, 20};
    tracker.updateSets(1, &write, 0, nullptr);
    require(tracker.collectRanges(1, &primary, ranges) && hasRange(ranges, bufferC, 33, 19),
            "descriptor copy retains prior range until recopied");
    tracker.updateSets(0, nullptr, 1, &copy);
    require(tracker.collect(1, &primary, found), "live descriptor update remains trackable");
    require(found.size() == 3 && std::find(found.begin(), found.end(), bufferD) != found.end(),
            "live descriptor copy replaces prior buffer reference");
    require(tracker.collectRanges(1, &primary, ranges) && hasRange(ranges, bufferD, 44, 20),
            "latest copied descriptor offset and range are collected");
    require(ranges.size() == 5, "overlapping duplicate buffer references may remain separate");

    const VkDescriptorSetLayoutBinding dynamicBinding{
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dynamicLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dynamicLayoutInfo.bindingCount = 1;
    dynamicLayoutInfo.pBindings = &dynamicBinding;
    const auto dynamicLayout = fakeHandle<VkDescriptorSetLayout>(16);
    tracker.layout(dynamicLayout, dynamicLayoutInfo);
    const auto dynamicSet = fakeHandle<VkDescriptorSet>(17);
    VkDescriptorSetAllocateInfo dynamicSetInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dynamicSetInfo.descriptorPool = pool;
    dynamicSetInfo.descriptorSetCount = 1;
    dynamicSetInfo.pSetLayouts = &dynamicLayout;
    tracker.allocateSets(&dynamicSetInfo, &dynamicSet);
    VkDescriptorBufferInfo dynamicBufferInfo{bufferA, 13, 23};
    VkWriteDescriptorSet dynamicWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    dynamicWrite.dstSet = dynamicSet;
    dynamicWrite.descriptorCount = 1;
    dynamicWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    dynamicWrite.pBufferInfo = &dynamicBufferInfo;
    tracker.updateSets(1, &dynamicWrite, 0, nullptr);
    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    tracker.descriptors(primary, 1, &dynamicSet);
    require(tracker.collectRanges(1, &primary, ranges) && ranges.size() == 1 &&
            hasRange(ranges, bufferA, 0, VK_WHOLE_SIZE),
            "dynamic storage descriptor conservatively covers its whole buffer");

    VkComputePipelineCreateInfo allowedInfo = pipelineInfo;
#ifdef VK_EXT_subgroup_size_control
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroupInfo{};
    subgroupInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
    subgroupInfo.requiredSubgroupSize = 32;
    allowedInfo.stage.pNext = &subgroupInfo;
#endif
#ifdef VK_EXT_pipeline_robustness
    VkPipelineRobustnessCreateInfoEXT robustnessInfo{};
    robustnessInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT;
    allowedInfo.pNext = &robustnessInfo;
#endif
#ifdef VK_KHR_maintenance5
    VkPipelineCreateFlags2CreateInfoKHR flags2Info{};
    flags2Info.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR;
    flags2Info.pNext = allowedInfo.pNext;
    allowedInfo.pNext = &flags2Info;
#endif
    const auto allowedPipeline = fakeHandle<VkPipeline>(34);
    tracker.computePipeline(allowedPipeline, allowedInfo);
    tracker.beginCommand(primary);
    tracker.pipeline(primary, allowedPipeline);
    tracker.descriptors(primary, 1, &copiedSet);
    require(tracker.collect(1, &primary, found), "known execution-only pipeline chains accepted");
#ifdef VK_EXT_pipeline_robustness
    require(tracker.collectRanges(1,&primary,ranges) && hasRange(ranges,bufferA,0,VK_WHOLE_SIZE),
            "explicit pipeline robustness conservatively widens descriptor ranges");
#endif

    VkBaseInStructure unknownPipelineInfo{};
    unknownPipelineInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    pipelineInfo.pNext = &unknownPipelineInfo;
    const auto unknownPipeline = fakeHandle<VkPipeline>(35);
    tracker.computePipeline(unknownPipeline, pipelineInfo);
    tracker.beginCommand(primary);
    tracker.pipeline(primary, unknownPipeline);
    require(!tracker.collect(1, &primary, found), "unknown pipeline chain falls back");
    pipelineInfo.pNext = nullptr;

    write.dstSet = partialSet;
    write.descriptorCount = 2;
    tracker.updateSets(1, &write, 0, nullptr);
    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    tracker.descriptors(primary, 1, &partialSet);
    require(!tracker.collect(1, &primary, found), "partially written storage array falls back");
    write.dstSet = sourceSet;
    write.descriptorCount = 3;

    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    tracker.descriptors(primary, 1, &emptySet);
    require(!tracker.collect(1, &primary, found), "unwritten storage descriptor falls back");

    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    tracker.unknown(primary);
    require(!tracker.collect(1, &primary, found), "unknown command falls back");
    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    tracker.descriptors(primary, 1, &copiedSet);
    require(tracker.collect(1, &primary, found), "begin resets prior unknown command state");

    tracker.resetDescriptorPool(pool);
    require(!tracker.collect(1, &primary, found), "descriptor pool reset invalidates set tracking");
    tracker.resetCommandPool(commandPool);
    require(!tracker.collect(1, &primary, found), "command pool reset invalidates command tracking");
    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    require(tracker.collect(1, &primary, found), "begin restores a command after pool reset");
    tracker.resetCommand(primary);
    require(!tracker.collect(1, &primary, found), "command reset invalidates recorded state");
    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    require(tracker.collect(1, &primary, found), "begin restores a reset command buffer");
    tracker.eraseCommandPool(commandPool);
    require(!tracker.collect(1, &primary, found), "command pool destruction erases handles");

    const auto physicalPipeline = fakeHandle<VkPipeline>(32);
    pipelineInfo.stage.module = physicalShader;
    tracker.computePipeline(physicalPipeline, pipelineInfo);
    tracker.allocateCommands(&commandInfo, commands);
    tracker.beginCommand(primary);
    tracker.pipeline(primary, physicalPipeline);
    require(!tracker.collect(1, &primary, found), "physical-storage pipeline falls back");

    const auto malformedPipeline = fakeHandle<VkPipeline>(33);
    pipelineInfo.stage.module = malformedShader;
    tracker.computePipeline(malformedPipeline, pipelineInfo);
    tracker.beginCommand(primary);
    tracker.pipeline(primary, malformedPipeline);
    require(!tracker.collect(1, &primary, found), "malformed shader pipeline falls back");

    const VkDescriptorBindingFlags flags[] = {VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = 3;
    flagsInfo.pBindingFlags = flags;
    layoutInfo.pNext = &flagsInfo;
    const auto unsafeLayout = fakeHandle<VkDescriptorSetLayout>(15);
    tracker.layout(unsafeLayout, layoutInfo);
    require(!tracker.layoutSafe(unsafeLayout), "update-after-bind layout rejected");

    std::cout << "submission tracking checks passed\n";
}
