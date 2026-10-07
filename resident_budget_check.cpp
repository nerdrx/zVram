#include "resident_budget.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(std::uint64_t expected, std::uint64_t actual, const char* name) {
    if (actual != expected) throw std::runtime_error(name);
}
}

int main() try {
    using zvram::residentBudgetLimit;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();

    // Usage includes the tracked bytes; leave only budget-minus-other-use.
    check(18, residentBudgetLimit(20, 30, 18, 6, 0), "double-counted tracked usage");
    // Competing activity shrinks budget while this process usage is unchanged.
    check(11, residentBudgetLimit(20, 24, 18, 6, 1), "budget shrink ignored");
    // Untracked allocations in this process (including layer staging) stay reserved.
    check(13, residentBudgetLimit(20, 30, 18, 6, 5), "reserve or other usage ignored");
    // No negative other-use estimate when driver usage is below our tracked count.
    check(15, residentBudgetLimit(20, 15, 10, 12, 0), "usage estimate underflow");
    check(0, residentBudgetLimit(20, 5, 8, 2, 1), "budget underflow");
    check(0, residentBudgetLimit(20, 5, 4, 8, 5), "reserve underflow");
    check(4, residentBudgetLimit(4, 100, 10, 5, 1), "hard cap ignored");
    check(max, residentBudgetLimit(max, max, 0, 0, 0), "maximum boundary");
    check(0, residentBudgetLimit(max, max, max, 0, max), "maximum subtraction overflow");

    std::cout << "PASS: resident budget cap arithmetic and saturation\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
