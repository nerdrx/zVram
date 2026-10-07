#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class VkSubmissionTracker {
public:
    enum class ShaderKind { Unknown, Logical, PhysicalStorageBuffer };

    void shader(VkShaderModule module, const VkShaderModuleCreateInfo* info) {
        shaders_[module] = classify(info);
    }
    void eraseShader(VkShaderModule module) { shaders_.erase(module); }
    ShaderKind shaderKind(VkShaderModule module) const {
        const auto i = shaders_.find(module);
        return i == shaders_.end() ? ShaderKind::Unknown : i->second;
    }

    void computePipeline(VkPipeline pipeline, const VkComputePipelineCreateInfo& info) {
        pipelines_[pipeline] = info.stage.stage == VK_SHADER_STAGE_COMPUTE_BIT &&
                               safePipelineChain(info.pNext) &&
                               safeStageChain(info.stage.pNext) &&
                               shaderKind(info.stage.module) == ShaderKind::Logical;
    }
    void erasePipeline(VkPipeline pipeline) { pipelines_.erase(pipeline); }

    void layout(VkDescriptorSetLayout handle, const VkDescriptorSetLayoutCreateInfo& info) {
        Layout state;
        state.safe = info.flags == 0 && (!info.bindingCount || info.pBindings != nullptr);
        bool sawBindingFlags = false;
        for (auto* p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext) {
            if (p->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO || sawBindingFlags) {
                state.safe = false;
                continue;
            }
            sawBindingFlags = true;
            const auto* flags = reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(p);
            if (flags->bindingCount != info.bindingCount || (flags->bindingCount && !flags->pBindingFlags)) {
                state.safe = false;
                continue;
            }
            for (std::uint32_t i = 0; i < flags->bindingCount; ++i)
                if (flags->pBindingFlags[i] != 0) state.safe = false;
        }
        if (info.bindingCount && info.pBindings) {
            for (std::uint32_t i = 0; i < info.bindingCount; ++i) {
                const auto& binding = info.pBindings[i];
                const bool storage = binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                                     binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
                if (!knownDescriptorType(binding.descriptorType) || !binding.descriptorCount ||
                    !state.bindings.emplace(binding.binding,
                        Binding{binding.descriptorType, binding.descriptorCount, storage}).second)
                    state.safe = false;
            }
        }
        layouts_[handle] = std::move(state);
    }
    void eraseLayout(VkDescriptorSetLayout layoutHandle) { layouts_.erase(layoutHandle); }
    bool layoutSafe(VkDescriptorSetLayout handle) const {
        const auto i = layouts_.find(handle);
        return i != layouts_.end() && i->second.safe;
    }

    void allocateSets(const VkDescriptorSetAllocateInfo* info, const VkDescriptorSet* sets) {
        if (!info || (info->descriptorSetCount && (!sets || !info->pSetLayouts))) return;
        for (std::uint32_t i = 0; i < info->descriptorSetCount; ++i) {
            Set state;
            state.pool = info->descriptorPool;
            const auto layoutIt = layouts_.find(info->pSetLayouts[i]);
            if (layoutIt == layouts_.end()) state.safe = false;
            else {
                state.safe = layoutIt->second.safe;
                state.bindings = layoutIt->second.bindings;
                for (const auto& entry : state.bindings) if (entry.second.storage) {
                    state.buffers[entry.first].resize(entry.second.count, VK_NULL_HANDLE);
                    state.written[entry.first].resize(entry.second.count, false);
                }
            }
            sets_[sets[i]] = std::move(state);
        }
    }
    void freeSets(std::uint32_t count, const VkDescriptorSet* sets) {
        if (count && !sets) { poisonSets(); return; }
        for (std::uint32_t i = 0; i < count; ++i) sets_.erase(sets[i]);
    }
    void resetDescriptorPool(VkDescriptorPool pool) {
        for (auto i = sets_.begin(); i != sets_.end();) {
            if (i->second.pool == pool) i = sets_.erase(i);
            else ++i;
        }
    }
    void updateSets(std::uint32_t writeCount, const VkWriteDescriptorSet* writes,
                    std::uint32_t copyCount, const VkCopyDescriptorSet* copies) {
        if ((writeCount && !writes) || (copyCount && !copies)) { poisonSets(); return; }
        for (std::uint32_t i = 0; i < writeCount; ++i) {
            const auto& write = writes[i];
            auto set = sets_.find(write.dstSet);
            if (set == sets_.end()) continue;
            if (!set->second.safe || write.pNext) { set->second.safe = false; continue; }
            std::vector<Slot> slots;
            if (!resolve(set->second.bindings, write.dstBinding, write.dstArrayElement,
                         write.descriptorCount, write.descriptorType, slots)) {
                set->second.safe = false;
                continue;
            }
            if (!isStorage(write.descriptorType)) continue;
            if (!write.pBufferInfo) { set->second.safe = false; continue; }
            for (std::uint32_t j = 0; j < write.descriptorCount; ++j) {
                const auto& slot = slots[j];
                set->second.buffers[slot.binding][slot.element] = write.pBufferInfo[j].buffer;
                set->second.written[slot.binding][slot.element] = write.pBufferInfo[j].buffer != VK_NULL_HANDLE;
            }
        }
        for (std::uint32_t i = 0; i < copyCount; ++i) copyDescriptor(copies[i]);
    }

    void allocateCommands(const VkCommandBufferAllocateInfo* info, const VkCommandBuffer* commands) {
        if (!info || (info->commandBufferCount && !commands)) return;
        for (std::uint32_t i = 0; i < info->commandBufferCount; ++i)
            commands_[commands[i]] = Command{info->commandPool};
    }
    void freeCommands(std::uint32_t count, const VkCommandBuffer* commands) {
        if (count && !commands) { poisonCommands(); return; }
        for (std::uint32_t i = 0; i < count; ++i) commands_.erase(commands[i]);
    }
    void resetCommandPool(VkCommandPool pool) {
        for (auto& entry : commands_)
            if (entry.second.pool == pool) {
                entry.second = Command{pool};
                entry.second.safe = false;
            }
    }
    void resetCommand(VkCommandBuffer command) {
        auto i = commands_.find(command);
        if (i == commands_.end()) return;
        const auto pool = i->second.pool;
        i->second = Command{pool};
        i->second.safe = false;
    }
    void eraseCommandPool(VkCommandPool pool) {
        for (auto i = commands_.begin(); i != commands_.end();) {
            if (i->second.pool == pool) i = commands_.erase(i);
            else ++i;
        }
    }
    void beginCommand(VkCommandBuffer command) {
        auto i = commands_.find(command);
        if (i == commands_.end()) { commands_[command].safe = false; return; }
        const auto pool = i->second.pool;
        i->second = Command{pool};
    }
    void buffer(VkCommandBuffer command, VkBuffer handle) {
        auto i = commands_.find(command);
        if (i == commands_.end()) return;
        if (!handle) { i->second.safe = false; return; }
        i->second.buffers.push_back(handle);
    }
    void descriptors(VkCommandBuffer command, std::uint32_t count, const VkDescriptorSet* sets) {
        auto i = commands_.find(command);
        if (i == commands_.end()) return;
        if (count && !sets) { i->second.safe = false; return; }
        if (count) i->second.sets.insert(i->second.sets.end(), sets, sets + count);
    }
    void pipeline(VkCommandBuffer command, VkPipeline pipelineHandle) {
        auto c = commands_.find(command);
        if (c == commands_.end()) return;
        const auto p = pipelines_.find(pipelineHandle);
        if (p == pipelines_.end() || !p->second) c->second.safe = false;
    }
    void secondary(VkCommandBuffer command, std::uint32_t count, const VkCommandBuffer* secondaryCommands) {
        auto i = commands_.find(command);
        if (i == commands_.end()) return;
        if (count && !secondaryCommands) { i->second.safe = false; return; }
        if (count) i->second.secondaries.insert(i->second.secondaries.end(), secondaryCommands,
                                               secondaryCommands + count);
    }
    void unknown(VkCommandBuffer command) {
        auto i = commands_.find(command);
        if (i != commands_.end()) i->second.safe = false;
    }

    bool collect(std::uint32_t count, const VkCommandBuffer* commands, std::vector<VkBuffer>& out) const {
        out.clear();
        if (count && !commands) return false;
        std::vector<VkBuffer> found;
        std::unordered_set<VkBuffer> uniqueBuffers;
        std::unordered_set<VkCommandBuffer> visited, visiting;
        for (std::uint32_t i = 0; i < count; ++i)
            if (!collectCommand(commands[i], found, uniqueBuffers, visited, visiting)) return false;
        out = std::move(found);
        return true;
    }

private:
    struct Binding { VkDescriptorType type; std::uint32_t count; bool storage; };
    struct Layout { bool safe{true}; std::map<std::uint32_t, Binding> bindings; };
    struct Set {
        VkDescriptorPool pool{};
        bool safe{true};
        std::map<std::uint32_t, Binding> bindings;
        std::map<std::uint32_t, std::vector<VkBuffer>> buffers;
        std::map<std::uint32_t, std::vector<bool>> written;
    };
    struct Command {
        explicit Command(VkCommandPool owner = VK_NULL_HANDLE) : pool(owner) {}
        VkCommandPool pool{};
        bool safe{true};
        std::vector<VkBuffer> buffers;
        std::vector<VkDescriptorSet> sets;
        std::vector<VkCommandBuffer> secondaries;
    };
    struct Slot { std::uint32_t binding, element; };

    static ShaderKind classify(const VkShaderModuleCreateInfo* info) {
        if (!info || info->pNext || info->flags || !info->pCode || info->codeSize < 5 * sizeof(std::uint32_t) ||
            info->codeSize % sizeof(std::uint32_t)) return ShaderKind::Unknown;
        const auto* words = info->pCode;
        const auto count = info->codeSize / sizeof(std::uint32_t);
        if (words[0] != 0x07230203u || words[1] < 0x00010000u || words[1] > 0x00010600u ||
            words[3] == 0 || words[4] != 0) return ShaderKind::Unknown;
        bool hasMemoryModel = false, physicalStorage = false;
        std::uint32_t addressingModel = UINT32_MAX;
        for (std::size_t at = 5; at < count;) {
            const auto instruction = words[at];
            const auto wordCount = instruction >> 16;
            const auto opcode = instruction & 0xffffu;
            if (!wordCount || wordCount > count - at) return ShaderKind::Unknown;
            if (opcode == 17) {
                if (wordCount < 2) return ShaderKind::Unknown;
                if (words[at + 1] == 5347u) physicalStorage = true;
            } else if (opcode == 14) {
                if (wordCount != 3 || hasMemoryModel) return ShaderKind::Unknown;
                hasMemoryModel = true;
                addressingModel = words[at + 1];
                if (addressingModel == 5348u) physicalStorage = true;
            }
            at += wordCount;
        }
        if (!hasMemoryModel) return ShaderKind::Unknown;
        if (physicalStorage) return ShaderKind::PhysicalStorageBuffer;
        return addressingModel == 0 ? ShaderKind::Logical : ShaderKind::Unknown;
    }
    static bool safeStageChain(const void* chain) {
        bool subgroupSize = false;
        for (auto* p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext) {
#if defined(VK_VERSION_1_3) || defined(VK_EXT_subgroup_size_control)
            if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO) {
                if (subgroupSize) return false;
                subgroupSize = true;
                continue;
            }
#endif
            return false;
        }
        return true;
    }
    static bool safePipelineChain(const void* chain) {
        bool robustness = false, flags2 = false;
        for (auto* p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext) {
#ifdef VK_EXT_pipeline_robustness
            if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT) {
                if (robustness) return false;
                robustness = true;
                continue;
            }
#endif
#ifdef VK_KHR_maintenance5
            if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR) {
                if (flags2) return false;
                flags2 = true;
                continue;
            }
#endif
            return false;
        }
        return true;
    }
    static bool isStorage(VkDescriptorType type) {
        return type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
               type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    }
    static bool knownDescriptorType(VkDescriptorType type) {
        switch (type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return true;
        default:
            return false;
        }
    }
    static bool resolve(const std::map<std::uint32_t, Binding>& bindings,
                        std::uint32_t binding, std::uint32_t element, std::uint32_t count,
                        VkDescriptorType type, std::vector<Slot>& out) {
        out.clear();
        if (!count) return true;
        out.reserve(count);
        auto current = bindings.find(binding);
        if (current == bindings.end() || element >= current->second.count || current->second.type != type)
            return false;
        while (count) {
            const auto amount = std::min(count, current->second.count - element);
            for (std::uint32_t i = 0; i < amount; ++i) out.push_back({current->first, element + i});
            count -= amount;
            if (!count) break;
            if (current->first == UINT32_MAX) return false;
            const auto nextNumber = current->first + 1;
            current = bindings.find(nextNumber);
            element = 0;
            if (current == bindings.end() || current->second.type != type) return false;
        }
        return true;
    }
    void copyDescriptor(const VkCopyDescriptorSet& copy) {
        auto destination = sets_.find(copy.dstSet);
        if (destination == sets_.end()) return;
        auto source = sets_.find(copy.srcSet);
        if (!destination->second.safe || source == sets_.end() || !source->second.safe || copy.pNext) {
            destination->second.safe = false;
            return;
        }
        const auto srcBinding = source->second.bindings.find(copy.srcBinding);
        const auto dstBinding = destination->second.bindings.find(copy.dstBinding);
        if (srcBinding == source->second.bindings.end() || dstBinding == destination->second.bindings.end() ||
            srcBinding->second.type != dstBinding->second.type) {
            destination->second.safe = false;
            return;
        }
        std::vector<Slot> src, dst;
        if (!resolve(source->second.bindings, copy.srcBinding, copy.srcArrayElement,
                     copy.descriptorCount, srcBinding->second.type, src) ||
            !resolve(destination->second.bindings, copy.dstBinding, copy.dstArrayElement,
                     copy.descriptorCount, dstBinding->second.type, dst)) {
            destination->second.safe = false;
            return;
        }
        if (!srcBinding->second.storage) return;
        std::vector<std::pair<VkBuffer, bool>> values;
        values.reserve(copy.descriptorCount);
        for (const auto& slot : src) {
            const auto& written = source->second.written[slot.binding];
            values.emplace_back(source->second.buffers[slot.binding][slot.element], written[slot.element]);
        }
        for (std::size_t i = 0; i < dst.size(); ++i) {
            destination->second.buffers[dst[i].binding][dst[i].element] = values[i].first;
            destination->second.written[dst[i].binding][dst[i].element] = values[i].second;
        }
    }
    bool collectCommand(VkCommandBuffer command, std::vector<VkBuffer>& out,
                        std::unordered_set<VkBuffer>& uniqueBuffers,
                        std::unordered_set<VkCommandBuffer>& visited,
                        std::unordered_set<VkCommandBuffer>& visiting) const {
        if (visited.count(command)) return true;
        if (!visiting.insert(command).second) return false;
        const auto c = commands_.find(command);
        if (c == commands_.end() || !c->second.safe) return false;
        for (auto buffer : c->second.buffers)
            if (uniqueBuffers.insert(buffer).second) out.push_back(buffer);
        for (auto setHandle : c->second.sets) {
            const auto set = sets_.find(setHandle);
            if (set == sets_.end() || !set->second.safe) return false;
            bool hasInitializedStorage = false;
            for (const auto& binding : set->second.bindings) if (binding.second.storage) {
                const auto written = set->second.written.find(binding.first);
                const auto buffers = set->second.buffers.find(binding.first);
                if (written == set->second.written.end() || buffers == set->second.buffers.end() ||
                    written->second.size() != binding.second.count || buffers->second.size() != binding.second.count)
                    return false;
                const bool anyWritten = std::any_of(written->second.begin(), written->second.end(),
                                                    [](bool value) { return value; });
                if (!anyWritten) continue;
                hasInitializedStorage = true;
                for (std::size_t i = 0; i < written->second.size(); ++i) {
                    const auto buffer = buffers->second[i];
                    if (!written->second[i] || !buffer) return false;
                    if (uniqueBuffers.insert(buffer).second) out.push_back(buffer);
                }
            }
            if (!hasInitializedStorage) return false;
        }
        for (auto secondary : c->second.secondaries)
            if (!collectCommand(secondary, out, uniqueBuffers, visited, visiting)) return false;
        visiting.erase(command);
        visited.insert(command);
        return true;
    }
    void poisonSets() { for (auto& set : sets_) set.second.safe = false; }
    void poisonCommands() { for (auto& command : commands_) command.second.safe = false; }

    std::unordered_map<VkShaderModule, ShaderKind> shaders_;
    std::unordered_map<VkPipeline, bool> pipelines_;
    std::unordered_map<VkDescriptorSetLayout, Layout> layouts_;
    std::unordered_map<VkDescriptorSet, Set> sets_;
    std::unordered_map<VkCommandBuffer, Command> commands_;
};
