// Minimal test helpers (no external test framework needed for Milestone 1).
// GoogleTest is introduced in a later milestone.
#pragma once
#include <iostream>
#include <sstream>
#include <string>

inline int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n"; \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        auto va_ = (a);                                                               \
        auto vb_ = (b);                                                               \
        if (!(va_ == vb_)) {                                                          \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #a << " == " << #b \
                      << "   (got " << va_ << " vs " << vb_ << ")\n";                 \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

#define CHECK_THROWS(expr)                                                            \
    do {                                                                              \
        bool threw_ = false;                                                          \
        try {                                                                         \
            expr;                                                                     \
        } catch (const std::exception&) {                                             \
            threw_ = true;                                                            \
        }                                                                             \
        if (!threw_) {                                                                \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  expected an exception from: " << #expr << "\n"; \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

inline int finish(const char* name) {
    if (g_failures == 0) {
        std::cout << name << ": all checks passed\n";
        return 0;
    }
    std::cerr << name << ": " << g_failures << " check(s) FAILED\n";
    return 1;
}
