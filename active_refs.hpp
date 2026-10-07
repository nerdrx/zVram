#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <utility>
#include <vector>

// Externally serialized queue epochs for resources referenced by submissions.
class ActiveRefs {
public:
    struct Status {
        bool hasTail = false;
        bool hasCovered = false;
        bool blocksAll = false;
    };

    void record(VkQueue queue, const std::vector<VkDeviceMemory>& memories, bool known) {
        auto& state = get(queue);
        state.hasTail = true;
        if (!known) state.tailUnknown = true;
        for (VkDeviceMemory memory : memories) {
            if (std::find(state.tail.begin(), state.tail.end(), memory) == state.tail.end())
                state.tail.push_back(memory);
        }
    }

    // Promotes the accumulated tail once its previous covered epoch retired.
    bool cover(VkQueue queue) {
        auto* state = find(queue);
        if (!state || !state->hasTail || state->hasCovered) return false;
        state->covered.swap(state->tail);
        state->coveredUnknown = state->tailUnknown;
        state->hasCovered = true;
        state->hasTail = false;
        state->tailUnknown = false;
        return true;
    }

    void retire(VkQueue queue) {
        if (auto* state = find(queue)) {
            state->covered.clear();
            state->hasCovered = false;
            state->coveredUnknown = false;
        }
    }

    bool busy(VkDeviceMemory memory) const {
        for (const auto& state : queues_) {
            if (state.coveredUnknown || state.tailUnknown ||
                std::find(state.covered.begin(), state.covered.end(), memory) != state.covered.end() ||
                std::find(state.tail.begin(), state.tail.end(), memory) != state.tail.end())
                return true;
        }
        return false;
    }

    Status status(VkQueue queue) const {
        if (const auto* state = find(queue))
            return {state->hasTail, state->hasCovered, state->coveredUnknown || state->tailUnknown};
        return {};
    }

private:
    struct QueueState {
        VkQueue queue{};
        bool hasTail = false;
        bool tailUnknown = false;
        std::vector<VkDeviceMemory> tail;
        bool hasCovered = false;
        bool coveredUnknown = false;
        std::vector<VkDeviceMemory> covered;
    };

    QueueState* find(VkQueue queue) {
        for (auto& state : queues_)
            if (state.queue == queue) return &state;
        return nullptr;
    }
    const QueueState* find(VkQueue queue) const {
        for (const auto& state : queues_)
            if (state.queue == queue) return &state;
        return nullptr;
    }
    QueueState& get(VkQueue queue) {
        if (auto* state = find(queue)) return *state;
        QueueState state;
        state.queue = queue;
        queues_.push_back(std::move(state));
        return queues_.back();
    }

    std::vector<QueueState> queues_;
};
