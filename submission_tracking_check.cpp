#include "submission_tracking.hpp"

#include <cstdlib>
#include <iostream>
#include <initializer_list>
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

static void emit(std::vector<std::uint32_t>& words, std::uint16_t opcode,
                 std::initializer_list<std::uint32_t> operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}

static void emit(std::vector<std::uint32_t>& words, std::uint16_t opcode,
                 const std::vector<std::uint32_t>& operands) {
    words.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}

static std::vector<std::uint32_t> storageShader(const std::vector<bool>& readOnlyMembers,
                                                bool variableReadOnly = false,
                                                bool arrayWrapped = false,
                                                bool decorationGroup = false,
                                                std::uint32_t storage = 12,
                                                bool runtimeArrayWrapped = false,
                                                bool duplicateWritableAlias = false,
                                                bool includeUniformBuffer = false) {
    std::vector<std::uint32_t> words{0x07230203u, 0x00010000u, 0u, 16u, 0u};
    emit(words, 14, {0u, 1u});
    if (variableReadOnly) emit(words, 71, {6u, 24u});
    emit(words, 71, {2u, storage == 12 ? 2u : 3u});
    if (includeUniformBuffer) {
        emit(words, 71, {11u, 2u});
        emit(words, 71, {13u, 33u, 0u});
        emit(words, 71, {13u, 34u, 1u});
    }
    emit(words, 71, {6u, 33u, 0u});
    emit(words, 71, {6u, 34u, 0u});
    if (duplicateWritableAlias) {
        emit(words, 71, {8u, 33u, 0u});
        emit(words, 71, {8u, 34u, 0u});
    }
    for (std::uint32_t i = 0; i < readOnlyMembers.size(); ++i)
        if (readOnlyMembers[i]) emit(words, 72, {2u, i, 24u});
    if (decorationGroup) emit(words, 73, {10u});
    emit(words, 21, {1u, 32u, 0u});
    emit(words, 43, {1u, 9u, 1u});
    std::vector<std::uint32_t> members{2u};
    for (std::size_t i = 0; i < readOnlyMembers.size(); ++i) members.push_back(1u);
    emit(words, 30, members);
    if (arrayWrapped) {
        if (runtimeArrayWrapped) emit(words, 29, {3u, 2u});
        else emit(words, 28, {3u, 2u, 9u});
    }
    emit(words, 32, {4u, storage, arrayWrapped ? 3u : 2u});
    emit(words, 59, {4u, 6u, storage});
    if (duplicateWritableAlias) emit(words, 59, {4u, 8u, storage});
    if (includeUniformBuffer) {
        emit(words, 30, {11u, 1u});
        emit(words, 32, {12u, 2u, 11u});
        emit(words, 59, {12u, 13u, 2u});
    }
    return words;
}

static VkPipeline readonlyPipeline(VkSubmissionTracker& tracker, VkShaderModule shader,
                                   const std::vector<std::uint32_t>& words,
                                   std::uintptr_t handle) {
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = words.size() * sizeof(std::uint32_t);
    shaderInfo.pCode = words.data();
    tracker.shader(shader, &shaderInfo);
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader;
    const auto pipeline = fakeHandle<VkPipeline>(handle);
    tracker.computePipeline(pipeline, pipelineInfo);
    return pipeline;
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
    const char* diagnostic = "stale";
    require(tracker.collectRanges(1, &primary, ranges, &diagnostic) && !diagnostic,
            "successful diagnostic collection clears the reason");

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

    tracker.boundedRobustness(true);
    auto checkRobustnessRange = [&](VkPipelineRobustnessBufferBehaviorEXT behavior,
                                    VkDeviceSize expectedOffset, VkDeviceSize expectedSize,
                                    const char* message, std::uintptr_t handle) {
        VkPipelineRobustnessCreateInfoEXT boundedInfo{};
        boundedInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT;
        boundedInfo.storageBuffers = behavior;
        VkComputePipelineCreateInfo boundedPipelineInfo = pipelineInfo;
        boundedPipelineInfo.pNext = &boundedInfo;
        const auto boundedPipeline = fakeHandle<VkPipeline>(handle);
        tracker.computePipeline(boundedPipeline, boundedPipelineInfo);
        tracker.beginCommand(primary);
        tracker.pipeline(primary, boundedPipeline);
        tracker.descriptors(primary, 1, &copiedSet);
        require(tracker.collectRanges(1, &primary, ranges) && ranges.size() == 3 &&
                hasRange(ranges, bufferD, expectedOffset, expectedSize), message);
        tracker.erasePipeline(boundedPipeline);
    };
    checkRobustnessRange(VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT, 44, 20,
                         "bounded default keeps explicitly disabled robustness narrow", 36);
    checkRobustnessRange(VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT, 44, 20,
                         "bounded default keeps robustness2 narrow", 37);
    checkRobustnessRange(VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT, 44, 20,
                         "bounded default keeps explicit device default narrow", 38);
    checkRobustnessRange(VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT, 0, VK_WHOLE_SIZE,
                         "explicit robustness1 widens descriptor range", 39);
    tracker.boundedRobustness(false);
#endif

    VkBaseInStructure unknownPipelineInfo{};
    unknownPipelineInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    pipelineInfo.pNext = &unknownPipelineInfo;
    const auto unknownPipeline = fakeHandle<VkPipeline>(35);
    tracker.computePipeline(unknownPipeline, pipelineInfo);
    tracker.beginCommand(primary);
    tracker.pipeline(primary, unknownPipeline);
    require(!tracker.collect(1, &primary, found), "unknown pipeline chain falls back");
    diagnostic = nullptr;
    require(!tracker.collectRanges(1, &primary, ranges, &diagnostic) &&
            diagnostic && std::string(diagnostic) == "pipeline-chain-unsupported",
            "unsafe pipeline diagnostic identifies unsupported pipeline chain");
    tracker.beginCommand(primary);
    tracker.pipeline(primary, fakeHandle<VkPipeline>(999));
    require(!tracker.collectRanges(1, &primary, ranges, &diagnostic) &&
            diagnostic && std::string(diagnostic) == "pipeline-not-tracked",
            "untracked pipeline diagnostic is stable");
    const auto missingCommand = fakeHandle<VkCommandBuffer>(998);
    require(!tracker.collectRanges(1, &missingCommand, ranges, &diagnostic) &&
            diagnostic && std::string(diagnostic) == "command-buffer-not-tracked",
            "missing command buffer diagnostic is stable");
    pipelineInfo.pNext = nullptr;

    write.dstSet = partialSet;
    write.descriptorCount = 2;
    tracker.updateSets(1, &write, 0, nullptr);
    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    tracker.descriptors(primary, 1, &partialSet);
    require(!tracker.collect(1, &primary, found), "partially written storage array falls back");
    require(!tracker.collectRanges(1, &primary, ranges, &diagnostic) &&
            diagnostic && std::string(diagnostic) == "descriptor-partial",
            "partial descriptor diagnostic is stable");
    write.dstSet = sourceSet;
    write.descriptorCount = 3;

    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    tracker.descriptors(primary, 1, &emptySet);
    require(!tracker.collect(1, &primary, found), "unwritten storage descriptor falls back");
    require(!tracker.collectRanges(1, &primary, ranges, &diagnostic) &&
            diagnostic && std::string(diagnostic) == "descriptor-uninitialized",
            "uninitialized descriptor diagnostic is stable");

    tracker.beginCommand(primary);
    tracker.pipeline(primary, pipeline);
    tracker.unknown(primary);
    require(!tracker.collect(1, &primary, found), "unknown command falls back");
    require(!tracker.collectRanges(1, &primary, ranges, &diagnostic) &&
            diagnostic && std::string(diagnostic) == "untracked-command",
            "untracked command diagnostic is stable");
    tracker.beginCommand(primary);
    tracker.unknown(primary,"vkCmdExampleUnsupported");
    require(!tracker.collect(1, &primary, found), "named unknown command still falls back");
    require(!tracker.collectRanges(1, &primary, ranges, &diagnostic) &&
            diagnostic && std::string(diagnostic) == "vkCmdExampleUnsupported",
            "unknown command keeps its stable command name");
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

    // Access metadata remains conservative by default; transfer callers can mark sources read-only.
    tracker.allocateCommands(&commandInfo, commands);
    tracker.beginCommand(primary);
    tracker.bufferRange(primary, bufferA, 0, 16, false);
    tracker.bufferRange(primary, bufferB, 0, 16, true);
    require(tracker.collectRanges(1, &primary, ranges) && ranges.size() == 2 &&
            !ranges[0].mayWrite && ranges[1].mayWrite,
            "explicit transfer source and destination access flags survive collection");

    VkSubmissionTracker proofTracker;
    const auto proofLayout = fakeHandle<VkDescriptorSetLayout>(50);
    const VkDescriptorSetLayoutBinding proofBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
    };
    VkDescriptorSetLayoutCreateInfo proofLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    proofLayoutInfo.bindingCount = 2;
    proofLayoutInfo.pBindings = proofBindings;
    proofTracker.layout(proofLayout, proofLayoutInfo);
    const auto proofPool = fakeHandle<VkDescriptorPool>(51);
    const auto proofSet = fakeHandle<VkDescriptorSet>(52);
    const auto reboundSet = fakeHandle<VkDescriptorSet>(65);
    VkDescriptorSetAllocateInfo proofSetInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    proofSetInfo.descriptorPool = proofPool;
    const VkDescriptorSet proofSets[] = {proofSet, reboundSet};
    const VkDescriptorSetLayout proofLayouts[] = {proofLayout, proofLayout};
    proofSetInfo.descriptorSetCount = 2;
    proofSetInfo.pSetLayouts = proofLayouts;
    proofTracker.allocateSets(&proofSetInfo, proofSets);
    const auto alias = fakeHandle<VkBuffer>(53);
    VkDescriptorBufferInfo proofBufferInfos[] = {{alias, 4, 8}, {alias, 16, 8}};
    VkWriteDescriptorSet proofWrites[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        proofWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        proofWrites[i].dstSet = proofSet;
        proofWrites[i].dstBinding = i;
        proofWrites[i].descriptorCount = 1;
        proofWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        proofWrites[i].pBufferInfo = &proofBufferInfos[i];
    }
    proofTracker.updateSets(2, proofWrites, 0, nullptr);
    const auto reboundBuffer = fakeHandle<VkBuffer>(66);
    VkDescriptorBufferInfo reboundInfo{reboundBuffer, 27, 6};
    proofWrites[0].dstSet = reboundSet;
    proofWrites[0].dstBinding = 0;
    proofWrites[0].pBufferInfo = &reboundInfo;
    proofTracker.updateSets(1, proofWrites, 0, nullptr);
    const auto proofCommand = fakeHandle<VkCommandBuffer>(54);
    const VkCommandBuffer proofCommands[] = {proofCommand};
    VkCommandBufferAllocateInfo proofCommandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    proofCommandInfo.commandPool = commandPool;
    proofCommandInfo.commandBufferCount = 1;
    proofTracker.allocateCommands(&proofCommandInfo, proofCommands);

    const auto allMembersShader = fakeHandle<VkShaderModule>(55);
    const auto allMembersWords = storageShader({true});
    const auto allMembersPipeline = readonlyPipeline(proofTracker, allMembersShader, allMembersWords, 56);
    proofTracker.eraseShader(allMembersShader);
    proofTracker.beginCommand(proofCommand);
    proofTracker.pipeline(proofCommand, allMembersPipeline);
    proofTracker.descriptors(proofCommand, 1, &proofSet);
    require(proofTracker.collectRanges(1, proofCommands, ranges) && ranges.size() == 2 &&
            !ranges[0].mayWrite && ranges[1].mayWrite,
            "all block members prove only their matching descriptor binding read-only");

    const auto partialShader = fakeHandle<VkShaderModule>(57);
    const auto partialWords = storageShader({true, false}, false, true);
    const auto partialPipeline = readonlyPipeline(proofTracker, partialShader, partialWords, 58);
    proofTracker.beginCommand(proofCommand);
    proofTracker.pipeline(proofCommand, partialPipeline);
    proofTracker.descriptors(proofCommand, 1, &proofSet);
    require(proofTracker.collectRanges(1, proofCommands, ranges) && ranges[0].mayWrite,
            "one writable block member prevents a read-only proof through array wrappers");

    const auto variableShader = fakeHandle<VkShaderModule>(59);
    const auto variableWords = storageShader({}, true, true, false, 12, true);
    const auto variablePipeline = readonlyPipeline(proofTracker, variableShader, variableWords, 60);
    proofTracker.beginCommand(proofCommand);
    proofTracker.pipeline(proofCommand, variablePipeline);
    proofTracker.descriptors(proofCommand, 1, &proofSet);
    require(proofTracker.collectRanges(1, proofCommands, ranges) && !ranges[0].mayWrite,
            "variable NonWritable proves a storage block read-only through array wrappers");

    const auto uniformShader = fakeHandle<VkShaderModule>(69);
    const auto uniformWords = storageShader({true}, false, false, false, 12, false, false, true);
    const auto uniformPipeline = readonlyPipeline(proofTracker, uniformShader, uniformWords, 70);
    proofTracker.beginCommand(proofCommand);
    proofTracker.pipeline(proofCommand, uniformPipeline);
    proofTracker.descriptors(proofCommand, 1, &proofSet);
    require(proofTracker.collectRanges(1, proofCommands, ranges) && !ranges[0].mayWrite,
            "ordinary Uniform Block descriptors do not erase SSBO read-only proofs");

    const auto legacyStorageShader = fakeHandle<VkShaderModule>(71);
    const auto legacyStorageWords = storageShader({true}, false, false, false, 2);
    const auto legacyStoragePipeline = readonlyPipeline(proofTracker, legacyStorageShader,
                                                        legacyStorageWords, 72);
    proofTracker.beginCommand(proofCommand);
    proofTracker.pipeline(proofCommand, legacyStoragePipeline);
    proofTracker.descriptors(proofCommand, 1, &proofSet);
    require(proofTracker.collectRanges(1, proofCommands, ranges) && !ranges[0].mayWrite,
            "legacy Uniform BufferBlock storage descriptor supports read-only proof");

    const auto aliasShader = fakeHandle<VkShaderModule>(67);
    const auto aliasWords = storageShader({false}, true, false, false, 12, false, true);
    const auto aliasPipeline = readonlyPipeline(proofTracker, aliasShader, aliasWords, 68);
    proofTracker.beginCommand(proofCommand);
    proofTracker.pipeline(proofCommand, aliasPipeline);
    proofTracker.descriptors(proofCommand, 1, &proofSet);
    require(proofTracker.collectRanges(1, proofCommands, ranges) && ranges[0].mayWrite,
            "duplicate variables at one descriptor binding require every alias read-only");

    const auto groupShader = fakeHandle<VkShaderModule>(61);
    const auto groupWords = storageShader({true}, false, false, true);
    const auto groupPipeline = readonlyPipeline(proofTracker, groupShader, groupWords, 62);
    proofTracker.beginCommand(proofCommand);
    proofTracker.pipeline(proofCommand, groupPipeline);
    proofTracker.descriptors(proofCommand, 1, &proofSet);
    require(proofTracker.collectRanges(1, proofCommands, ranges) && ranges[0].mayWrite,
            "unsupported decoration groups provide no read-only proof");

    const auto secondReadonlyShader = fakeHandle<VkShaderModule>(63);
    const auto secondReadonlyWords = storageShader({true});
    const auto secondReadonlyPipeline = readonlyPipeline(proofTracker, secondReadonlyShader,
                                                        secondReadonlyWords, 64);
    proofTracker.beginCommand(proofCommand);
    proofTracker.pipeline(proofCommand, allMembersPipeline);
    proofTracker.pipeline(proofCommand, secondReadonlyPipeline);
    proofTracker.descriptors(proofCommand, 1, &proofSet);
    require(proofTracker.collectRanges(1, proofCommands, ranges) && !ranges[0].mayWrite,
            "all bound pipeline proofs must agree for read-only classification");
    proofTracker.pipeline(proofCommand, partialPipeline);
    require(proofTracker.collectRanges(1, proofCommands, ranges) && ranges[0].mayWrite,
            "a writable alias pipeline makes the binding writable");

    proofTracker.beginCommand(proofCommand);
    proofTracker.pipeline(proofCommand, allMembersPipeline);
    proofTracker.descriptors(proofCommand, 1, &proofSet, 0);
    proofTracker.descriptors(proofCommand, 1, &reboundSet, 0);
    proofTracker.descriptors(proofCommand, 1, &proofSet, 1);
    require(proofTracker.collectRanges(1, proofCommands, ranges) && ranges.size() == 5 &&
            std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) {
                return range.buffer == reboundBuffer && range.offset == 27 && !range.mayWrite;
            }) && std::count_if(ranges.begin(), ranges.end(), [&](const auto& range) {
                return range.buffer == alias && range.offset == 4;
            }) == 2 && std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) {
                return range.buffer == alias && range.offset == 4 && !range.mayWrite;
            }) && std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) {
                return range.buffer == alias && range.offset == 4 && range.mayWrite;
            }), "descriptor set rebinding preserves both previously referenced sets");

    const auto physicalPipeline = fakeHandle<VkPipeline>(32);
    pipelineInfo.stage.module = physicalShader;
    tracker.computePipeline(physicalPipeline, pipelineInfo);
    tracker.allocateCommands(&commandInfo, commands);
    tracker.beginCommand(primary);
    tracker.pipeline(primary, physicalPipeline);
    require(!tracker.collect(1, &primary, found), "physical-storage pipeline falls back");
    require(!tracker.collectRanges(1, &primary, ranges, &diagnostic) &&
            diagnostic && std::string(diagnostic) == "pipeline-access-unknown",
            "unknown pipeline access diagnostic is stable");

    tracker.beginCommand(primary); tracker.beginCommand(secondary);
    tracker.secondary(primary, 1, &secondary);
    tracker.secondary(secondary, 1, &primary);
    require(!tracker.collect(1, &primary, found), "cyclic secondary command buffers fall back");
    require(!tracker.collectRanges(1, &primary, ranges, &diagnostic) &&
            diagnostic && std::string(diagnostic) == "cyclic-command-buffer-reference",
            "cyclic command diagnostic is stable");

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
