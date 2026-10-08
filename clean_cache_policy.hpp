#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace zvram::clean_cache {

enum class Policy { First, Lru, Mru };

inline Policy parsePolicy(const char* value, bool& valid) noexcept {
    valid = !value || std::strcmp(value, "first") == 0 || std::strcmp(value, "lru") == 0 ||
            std::strcmp(value, "mru") == 0;
    if (value && std::strcmp(value, "lru") == 0) return Policy::Lru;
    if (value && std::strcmp(value, "mru") == 0) return Policy::Mru;
    return Policy::First;
}

struct Candidate {
    std::chrono::steady_clock::time_point lastUse;
    std::uint64_t identityGeneration;
    std::size_t childIndex;
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

inline bool needsTrim(std::uint64_t coldBytes, std::uint64_t cacheBytes,
                      std::uint64_t budgetBytes, std::uint64_t requiredBytes) noexcept {
    if (requiredBytes > budgetBytes) return false;
    const auto available = budgetBytes - requiredBytes;
    return coldBytes > available || cacheBytes > available - coldBytes;
}

} // namespace zvram::clean_cache
