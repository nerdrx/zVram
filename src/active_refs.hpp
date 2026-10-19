#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Externally serialized queue epochs for resources referenced by submissions.
class ActiveRefs {
public:
    struct Use {
        VkDeviceMemory memory{};
        std::size_t child = SIZE_MAX;

        bool operator==(const Use& other) const {
            return memory == other.memory && child == other.child;
        }
    };

    struct Status {
        bool hasTail = false;
        bool hasCovered = false;
        bool blocksAll = false;
    };

    void record(VkQueue queue, const std::vector<VkDeviceMemory>& memories, bool known) {
        std::vector<Use> uses;
        uses.reserve(memories.size());
        for (VkDeviceMemory memory : memories) uses.push_back({memory});
        recordRanges(queue, uses, known);
    }

    void recordRanges(VkQueue queue, const std::vector<Use>& uses, bool known) {
        auto& state = get(queue);
        state.hasTail = true;
        if (!known) state.tailUnknown = true;
        for (const Use& use : uses)
            if (std::find(state.tail.begin(), state.tail.end(), use) == state.tail.end())
                state.tail.push_back(use);
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
        return busy(memory, SIZE_MAX);
    }

    bool busy(VkDeviceMemory memory, std::size_t child) const {
        for (const auto& state : queues_) {
            if (state.coveredUnknown || state.tailUnknown) return true;
            const auto matches = [memory, child](const Use& use) {
                return use.memory == memory &&
                       (use.child == SIZE_MAX || child == SIZE_MAX || use.child == child);
            };
            if (std::any_of(state.covered.begin(), state.covered.end(), matches) ||
                std::any_of(state.tail.begin(), state.tail.end(), matches)) return true;
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
        std::vector<Use> tail;
        bool hasCovered = false;
        bool coveredUnknown = false;
        std::vector<Use> covered;
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
