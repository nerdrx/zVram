#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace zvram::clean_cache {

enum class Policy { First, Lru, Mru, Lfu };

inline Policy parsePolicy(const char* value, bool& valid) noexcept {
    valid = !value || std::strcmp(value, "first") == 0 || std::strcmp(value, "lru") == 0 ||
            std::strcmp(value, "mru") == 0 || std::strcmp(value, "lfu") == 0;
    if (value && std::strcmp(value, "lru") == 0) return Policy::Lru;
    if (value && std::strcmp(value, "mru") == 0) return Policy::Mru;
    if (value && std::strcmp(value, "lfu") == 0) return Policy::Lfu;
    return Policy::First;
}

struct Candidate {
    std::chrono::steady_clock::time_point lastUse;
    std::uint64_t identityGeneration;
    std::size_t childIndex;
    std::uint64_t successfulFreezes{};
};

inline bool older(const Candidate& a, const Candidate& b) noexcept {
    if (a.lastUse != b.lastUse) return a.lastUse < b.lastUse;
    if (a.identityGeneration != b.identityGeneration)
        return a.identityGeneration < b.identityGeneration;
    return a.childIndex < b.childIndex;
}

inline bool newer(const Candidate& a, const Candidate& b) noexcept {
    if (a.lastUse != b.lastUse) return a.lastUse > b.lastUse;
    if (a.identityGeneration != b.identityGeneration)
        return a.identityGeneration < b.identityGeneration;
    return a.childIndex < b.childIndex;
}

inline bool lessFrequent(const Candidate& a, const Candidate& b) noexcept {
    if (a.successfulFreezes != b.successfulFreezes)
        return a.successfulFreezes < b.successfulFreezes;
    return older(a, b);
}

inline std::uint64_t nextSuccessfulFreeze(std::uint64_t count) noexcept {
    return count == std::numeric_limits<std::uint64_t>::max() ? count : count + 1;
}

inline bool needsTrim(std::uint64_t coldBytes, std::uint64_t cacheBytes,
                      std::uint64_t budgetBytes, std::uint64_t requiredBytes) noexcept {
    if (requiredBytes > budgetBytes) return false;
    const auto available = budgetBytes - requiredBytes;
    return coldBytes > available || cacheBytes > available - coldBytes;
}

inline bool needsTrim(std::uint64_t coldBytes, std::uint64_t cacheBytes,
                      std::uint64_t budgetBytes, std::uint64_t requiredBytes,
                      std::uint64_t cacheLimitBytes) noexcept {
    return needsTrim(coldBytes, cacheBytes, budgetBytes, requiredBytes) ||
           cacheBytes > cacheLimitBytes;
}

} // namespace zvram::clean_cache
