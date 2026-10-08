#include "clean_cache_policy.hpp"

#include <array>

int main() {
    using namespace zvram::clean_cache;
    bool valid = false;
    if (parsePolicy(nullptr, valid) != Policy::First || !valid) return 1;
    if (parsePolicy("first", valid) != Policy::First || !valid) return 2;
    if (parsePolicy("lru", valid) != Policy::Lru || !valid) return 3;
    if (parsePolicy("mru", valid) != Policy::Mru || !valid) return 4;
    if (parsePolicy("LRU", valid) != Policy::First || valid) return 5;

    const auto epoch = std::chrono::steady_clock::time_point{};
    const std::array<Candidate, 7> candidates{{
        {epoch + std::chrono::seconds(2), 1, 0},
        {epoch + std::chrono::seconds(1), 9, 0},
        {epoch + std::chrono::seconds(1), 3, 2},
        {epoch + std::chrono::seconds(1), 3, 1},
        {epoch + std::chrono::seconds(2), 2, 8},
        {epoch + std::chrono::seconds(2), 1, 9},
        {epoch + std::chrono::seconds(2), 1, 3},
    }};
    Candidate best = candidates[0];
    for (const auto& candidate : candidates)
        if (older(candidate, best)) best = candidate;
    if (best.lastUse != epoch + std::chrono::seconds(1) ||
        best.identityGeneration != 3 || best.childIndex != 1) return 6;
    Candidate newest = candidates[0];
    for (const auto& candidate : candidates)
        if (newer(candidate, newest)) newest = candidate;
    if (newest.lastUse != epoch + std::chrono::seconds(2) || newest.identityGeneration != 1 ||
        newest.childIndex != 0) return 7;
    if (!older(candidates[1], candidates[0]) || older(candidates[0], candidates[1])) return 8;
    if (needsTrim(40, 50, 100, 10) || !needsTrim(41, 50, 100, 10) ||
        needsTrim(90, 9, 100, 1) || !needsTrim(101, 0, 100, 0) ||
        needsTrim(100, 0, 99, 100)) return 9;
    return 0;
}
