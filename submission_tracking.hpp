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
    struct BufferRange {
        VkBuffer buffer;
        VkDeviceSize offset{};
        VkDeviceSize size{VK_WHOLE_SIZE};
        bool mayWrite{true};
    };

    void boundedRobustness(bool bounded) { boundedDefault_ = bounded; }

    void shader(VkShaderModule module, const VkShaderModuleCreateInfo* info) {
        shaders_[module] = parseShader(info);
    }
    void eraseShader(VkShaderModule module) { shaders_.erase(module); }
    ShaderKind shaderKind(VkShaderModule module) const {
        const auto i = shaders_.find(module);
        return i == shaders_.end() ? ShaderKind::Unknown : i->second.kind;
    }

    void computePipeline(VkPipeline pipeline, const VkComputePipelineCreateInfo& info) {
        const bool safe = info.stage.stage == VK_SHADER_STAGE_COMPUTE_BIT &&
                               safePipelineChain(info.pNext) &&
                               safeStageChain(info.stage.pNext) &&
                               shaderKind(info.stage.module) == ShaderKind::Logical;
        pipelines_[pipeline] = safe;
        pipelineReadOnly_.erase(pipeline);
        if (safe) pipelineReadOnly_[pipeline] = shaders_.at(info.stage.module).readOnly;
        bool wide=false;
        for(auto* p=static_cast<const VkBaseInStructure*>(info.pNext);p;p=p->pNext) {
#ifdef VK_EXT_pipeline_robustness
            if(p->sType==VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT) {
                const auto* robustness=reinterpret_cast<const VkPipelineRobustnessCreateInfoEXT*>(p);
                switch(robustness->storageBuffers) {
                case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT:
                    wide=!boundedDefault_; break;
                case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT:
                    wide=false; break;
                case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT:
                    wide=true; break;
                case VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT:
                    wide=!boundedDefault_; break;
                default:
                    wide=true; break;
                }
            }
#endif
        }
        widePipelines_[pipeline]=wide;
    }
    void erasePipeline(VkPipeline pipeline) {
        pipelines_.erase(pipeline);
        widePipelines_.erase(pipeline);
        pipelineReadOnly_.erase(pipeline);
    }

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
                    state.buffers[entry.first].resize(entry.second.count);
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
                set->second.buffers[slot.binding][slot.element] = write.pBufferInfo[j];
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
        bufferRange(command, handle, 0, VK_WHOLE_SIZE);
    }
    void bufferRange(VkCommandBuffer command, VkBuffer handle, VkDeviceSize offset,
                     VkDeviceSize size, bool mayWrite = true) {
        auto i = commands_.find(command);
        if (i == commands_.end()) return;
        if (!handle) { i->second.safe = false; return; }
        i->second.buffers.push_back({handle, offset, size, mayWrite});
    }
    void descriptors(VkCommandBuffer command, std::uint32_t count, const VkDescriptorSet* sets,
                     std::uint32_t firstSet = 0) {
        auto i = commands_.find(command);
        if (i == commands_.end()) return;
        if (count && (!sets || count > UINT32_MAX - firstSet)) { i->second.safe = false; return; }
        for (std::uint32_t j = 0; j < count; ++j)
            i->second.sets.emplace_back(firstSet + j, sets[j]);
    }
    void pipeline(VkCommandBuffer command, VkPipeline pipelineHandle) {
        auto c = commands_.find(command);
        if (c == commands_.end()) return;
        const auto p = pipelines_.find(pipelineHandle);
        if (p == pipelines_.end() || !p->second) c->second.safe = false;
        else if (std::find(c->second.pipelines.begin(), c->second.pipelines.end(), pipelineHandle) ==
                 c->second.pipelines.end()) c->second.pipelines.push_back(pipelineHandle);
        const auto wide=widePipelines_.find(pipelineHandle);
        if(wide!=widePipelines_.end() && wide->second) c->second.wideBuffers=true;
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
        std::vector<BufferRange> ranges;
        if (!collectRanges(count, commands, ranges)) return false;
        std::unordered_set<VkBuffer> uniqueBuffers;
        for (const auto& range : ranges)
            if (uniqueBuffers.insert(range.buffer).second) out.push_back(range.buffer);
        return true;
    }

    bool collectRanges(std::uint32_t count, const VkCommandBuffer* commands,
                       std::vector<BufferRange>& out) const {
        out.clear();
        if (count && !commands) return false;
        std::vector<BufferRange> found;
        std::unordered_set<VkCommandBuffer> visited, visiting;
        for (std::uint32_t i = 0; i < count; ++i)
            if (!collectCommand(commands[i], found, visited, visiting)) return false;
        out = std::move(found);
        return true;
    }

private:
    using SetBinding = std::pair<std::uint32_t, std::uint32_t>;
    struct ShaderData {
        ShaderKind kind{ShaderKind::Unknown};
        std::map<SetBinding, bool> readOnly;
    };
    struct Type {
        enum class Kind { Other, Pointer, Array, RuntimeArray, Struct } kind{Kind::Other};
        std::uint32_t element{};
        std::uint32_t storage{};
        std::vector<std::uint32_t> members;
    };
    struct Decoration {
        bool nonWritable{};
        bool block{};
        bool bufferBlock{};
        bool hasSet{};
        bool hasBinding{};
        bool conflict{};
        std::uint32_t set{};
        std::uint32_t binding{};
    };
    struct Binding { VkDescriptorType type; std::uint32_t count; bool storage; };
    struct Layout { bool safe{true}; std::map<std::uint32_t, Binding> bindings; };
    struct Set {
        VkDescriptorPool pool{};
        bool safe{true};
        std::map<std::uint32_t, Binding> bindings;
        std::map<std::uint32_t, std::vector<VkDescriptorBufferInfo>> buffers;
        std::map<std::uint32_t, std::vector<bool>> written;
    };
    struct Command {
        explicit Command(VkCommandPool owner = VK_NULL_HANDLE) : pool(owner) {}
        VkCommandPool pool{};
        bool safe{true};
        std::vector<BufferRange> buffers;
        bool wideBuffers{};
        std::vector<std::pair<std::uint32_t, VkDescriptorSet>> sets;
        std::vector<VkPipeline> pipelines;
        std::vector<VkCommandBuffer> secondaries;
    };
    struct Slot { std::uint32_t binding, element; };

    static ShaderData parseShader(const VkShaderModuleCreateInfo* info) {
        ShaderData result;
        if (!info || info->pNext || info->flags || !info->pCode || info->codeSize < 5 * sizeof(std::uint32_t) ||
            info->codeSize % sizeof(std::uint32_t)) return result;
        const auto* words = info->pCode;
        const auto count = info->codeSize / sizeof(std::uint32_t);
        if (words[0] != 0x07230203u || words[1] < 0x00010000u || words[1] > 0x00010600u ||
            words[3] == 0 || words[4] != 0) return result;
        bool hasMemoryModel = false, physicalStorage = false;
        bool reflectionSupported = true;
        std::uint32_t addressingModel = UINT32_MAX;
        std::unordered_map<std::uint32_t, Type> types;
        std::unordered_map<std::uint32_t, Decoration> decorations;
        std::map<std::pair<std::uint32_t, std::uint32_t>, bool> nonWritableMembers;
        struct Variable { std::uint32_t pointer{}, id{}, storage{}; };
        std::vector<Variable> variables;
        std::unordered_set<std::uint32_t> variableIds;
        for (std::size_t at = 5; at < count;) {
            const auto instruction = words[at];
            const auto wordCount = instruction >> 16;
            const auto opcode = instruction & 0xffffu;
            if (!wordCount || wordCount > count - at) return result;
            if (opcode == 17) {
                if (wordCount < 2) return result;
                if (words[at + 1] == 5347u) physicalStorage = true;
            } else if (opcode == 14) {
                if (wordCount != 3 || hasMemoryModel) return result;
                hasMemoryModel = true;
                addressingModel = words[at + 1];
                if (addressingModel == 5348u) physicalStorage = true;
            } else if (opcode == 28 || opcode == 29 || opcode == 30 || opcode == 32) {
                if ((opcode == 28 && wordCount != 4) || (opcode == 29 && wordCount != 3) ||
                    (opcode == 30 && wordCount < 2) || (opcode == 32 && wordCount != 4)) {
                    reflectionSupported = false;
                } else {
                    const auto id = words[at + 1];
                    if (!id || id >= words[3] || types.count(id)) reflectionSupported = false;
                    else {
                        Type type;
                        if (opcode == 28 || opcode == 29) {
                            type.kind = opcode == 28 ? Type::Kind::Array : Type::Kind::RuntimeArray;
                            type.element = words[at + 2];
                        } else if (opcode == 30) {
                            type.kind = Type::Kind::Struct;
                            type.members.assign(words + at + 2, words + at + wordCount);
                        } else {
                            type.kind = Type::Kind::Pointer;
                            type.storage = words[at + 2];
                            type.element = words[at + 3];
                        }
                        types.emplace(id, std::move(type));
                    }
                }
            } else if (opcode == 59) {
                if (wordCount < 4 || !words[at + 1] || !words[at + 2] || words[at + 2] >= words[3] ||
                    !variableIds.insert(words[at + 2]).second)
                    reflectionSupported = false;
                else variables.push_back({words[at + 1], words[at + 2], words[at + 3]});
            } else if (opcode == 71) {
                if (wordCount < 3 || !words[at + 1] || words[at + 1] >= words[3]) {
                    reflectionSupported = false;
                } else {
                    auto& d = decorations[words[at + 1]];
                    const auto decoration = words[at + 2];
                    if (decoration == 24 || decoration == 2 || decoration == 3) {
                        if (wordCount != 3) reflectionSupported = false;
                        if (decoration == 24) d.nonWritable = true;
                        if (decoration == 2) d.block = true;
                        if (decoration == 3) d.bufferBlock = true;
                    } else if (decoration == 33 || decoration == 34) {
                        if (wordCount != 4) reflectionSupported = false;
                        else {
                            const auto value = words[at + 3];
                            auto& has = decoration == 33 ? d.hasBinding : d.hasSet;
                            auto& stored = decoration == 33 ? d.binding : d.set;
                            if (has && stored != value) d.conflict = true;
                            has = true;
                            stored = value;
                        }
                    }
                }
            } else if (opcode == 72) {
                if (wordCount < 4 || !words[at + 1] || words[at + 1] >= words[3]) {
                    reflectionSupported = false;
                } else if (words[at + 3] == 24) {
                    if (wordCount != 4) reflectionSupported = false;
                    nonWritableMembers[{words[at + 1], words[at + 2]}] = true;
                }
            } else if (opcode == 73 || opcode == 74 || opcode == 75) {
                reflectionSupported = false;
            }
            at += wordCount;
        }
        if (!hasMemoryModel) return result;
        if (physicalStorage) result.kind = ShaderKind::PhysicalStorageBuffer;
        else if (addressingModel == 0) result.kind = ShaderKind::Logical;
        if (result.kind != ShaderKind::Logical || !reflectionSupported) return result;

        for (const auto& variable : variables) {
            if (variable.storage != 12 && variable.storage != 2) continue;
            const auto pointer = types.find(variable.pointer);
            if (pointer == types.end() || pointer->second.kind != Type::Kind::Pointer ||
                pointer->second.storage != variable.storage) {
                reflectionSupported = false;
                break;
            }
            auto typeId = pointer->second.element;
            std::unordered_set<std::uint32_t> visited;
            while (true) {
                if (!visited.insert(typeId).second) { reflectionSupported = false; break; }
                const auto type = types.find(typeId);
                if (type == types.end()) { reflectionSupported = false; break; }
                if (type->second.kind == Type::Kind::Array || type->second.kind == Type::Kind::RuntimeArray) {
                    typeId = type->second.element;
                    continue;
                }
                if (type->second.kind != Type::Kind::Struct) {
                    reflectionSupported = false;
                    break;
                }
                const auto block = decorations.find(typeId);
                const bool uniformBlock = block != decorations.end() && block->second.block &&
                                          !block->second.bufferBlock;
                if (variable.storage == 2 && uniformBlock) break;
                const bool storageBlock = block != decorations.end() &&
                    (variable.storage == 12 ? block->second.block && !block->second.bufferBlock :
                                               block->second.bufferBlock && !block->second.block);
                const auto object = decorations.find(variable.id);
                const bool nonWritable = object != decorations.end() && object->second.nonWritable;
                const auto variableDecorations = object == decorations.end() ? Decoration{} : object->second;
                if (!storageBlock || variableDecorations.conflict ||
                    !variableDecorations.hasSet || !variableDecorations.hasBinding) {
                    reflectionSupported = false;
                    break;
                }
                for (const auto& member : nonWritableMembers)
                    if (member.first.first == typeId && member.first.second >= type->second.members.size())
                        reflectionSupported = false;
                if (reflectionSupported) {
                    bool allMembersReadOnly = !type->second.members.empty();
                    for (std::uint32_t member = 0; member < type->second.members.size(); ++member)
                        allMembersReadOnly &= nonWritableMembers.count({typeId, member}) != 0;
                    const bool readOnly = nonWritable || allMembersReadOnly;
                    const SetBinding key{variableDecorations.set, variableDecorations.binding};
                    const auto existing = result.readOnly.find(key);
                    if (existing == result.readOnly.end()) result.readOnly.emplace(key, readOnly);
                    else existing->second = existing->second && readOnly;
                }
                break;
            }
            if (!reflectionSupported) break;
        }
        if (!reflectionSupported) result.readOnly.clear();
        return result;
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
        std::vector<std::pair<VkDescriptorBufferInfo, bool>> values;
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
    bool collectCommand(VkCommandBuffer command, std::vector<BufferRange>& out,
                        std::unordered_set<VkCommandBuffer>& visited,
                        std::unordered_set<VkCommandBuffer>& visiting) const {
        if (visited.count(command)) return true;
        if (!visiting.insert(command).second) return false;
        const auto c = commands_.find(command);
        if (c == commands_.end() || !c->second.safe) return false;
        out.insert(out.end(), c->second.buffers.begin(), c->second.buffers.end());
        for (const auto& setEntry : c->second.sets) {
            const auto setIndex = setEntry.first;
            const auto setHandle = setEntry.second;
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
                    const auto& descriptor = buffers->second[i];
                    const auto buffer = descriptor.buffer;
                    if (!written->second[i] || !buffer) return false;
                    bool mayWrite = c->second.pipelines.empty();
                    for (const auto pipeline : c->second.pipelines) {
                        const auto proof = pipelineReadOnly_.find(pipeline);
                        if (proof == pipelineReadOnly_.end()) { mayWrite = true; break; }
                        const auto access = proof->second.find({setIndex, binding.first});
                        if (access == proof->second.end() || !access->second) {
                            mayWrite = true;
                            break;
                        }
                    }
                    if (binding.second.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC || c->second.wideBuffers)
                        out.push_back({buffer, 0, VK_WHOLE_SIZE, mayWrite});
                    else
                        out.push_back({buffer, descriptor.offset, descriptor.range, mayWrite});
                }
            }
            if (!hasInitializedStorage) return false;
        }
        for (auto secondary : c->second.secondaries)
            if (!collectCommand(secondary, out, visited, visiting)) return false;
        visiting.erase(command);
        visited.insert(command);
        return true;
    }
    void poisonSets() { for (auto& set : sets_) set.second.safe = false; }
    void poisonCommands() { for (auto& command : commands_) command.second.safe = false; }

    std::unordered_map<VkShaderModule, ShaderData> shaders_;
    std::unordered_map<VkPipeline, bool> pipelines_;
    std::unordered_map<VkPipeline, bool> widePipelines_;
    std::unordered_map<VkPipeline, std::map<SetBinding, bool>> pipelineReadOnly_;
    bool boundedDefault_{};
    std::unordered_map<VkDescriptorSetLayout, Layout> layouts_;
    std::unordered_map<VkDescriptorSet, Set> sets_;
    std::unordered_map<VkCommandBuffer, Command> commands_;
};
