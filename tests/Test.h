#pragma once
// A minimal test runner: TEST(name) { CHECK(...); } registers a test; tests/TestMain.cpp runs them all, or those
// whose names contain an argument. SKIP("why") ends a test as skipped.

#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bfarb::test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& Cases() {
    static std::vector<Case> cases;
    return cases;
}

struct Register {
    Register(const char* name, void (*fn)()) { Cases().push_back({name, fn}); }
};

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Skip : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline int& Checks() {
    static int n = 0;
    return n;
}

}  // namespace bfarb::test

#define BFARB_CAT2(a, b) a##b
#define BFARB_CAT(a, b) BFARB_CAT2(a, b)
#define TEST(name)                                                                        \
    static void name();                                                                   \
    static ::bfarb::test::Register BFARB_CAT(register_, name)(#name, &name);              \
    static void name()

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        ++::bfarb::test::Checks();                                                                  \
        if (!(cond)) {                                                                              \
            std::ostringstream os_;                                                                 \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed";                      \
            throw ::bfarb::test::Failure(os_.str());                                                \
        }                                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        ++::bfarb::test::Checks();                                                                  \
        const auto& a_ = (a);                                                                       \
        const auto& b_ = (b);                                                                       \
        if (!(a_ == b_)) {                                                                          \
            std::ostringstream os_;                                                                 \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b ") failed: " << a_       \
                << " != " << b_;                                                                    \
            throw ::bfarb::test::Failure(os_.str());                                                \
        }                                                                                           \
    } while (0)

#define SKIP(why) throw ::bfarb::test::Skip(why)
