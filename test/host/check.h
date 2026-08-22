// Minimal check harness.  Deliberately no gtest/catch dependency - CLAUDE.md
// says no new dependencies without a reason, and these tests need three macros.
#pragma once

#include <cstdio>
#include <cstring>

inline int g_failures = 0;
inline const char* g_argv1 = nullptr;  // optional data-file path from add_test

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const long long va_ = static_cast<long long>(a);                                     \
        const long long vb_ = static_cast<long long>(b);                                     \
        if (va_ != vb_) {                                                                    \
            std::printf("FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                        va_, vb_);                                                           \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_STREQ(a, b)                                                                   \
    do {                                                                                    \
        if (std::strcmp((a), (b)) != 0) {                                                   \
            std::printf("FAIL %s:%d  %s == %s  (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, \
                        #b, (a), (b));                                                      \
            ++g_failures;                                                                   \
        }                                                                                   \
    } while (0)

void run_tests();

int main(int argc, char** argv) {
    g_argv1 = argc > 1 ? argv[1] : nullptr;
    run_tests();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
