// Minimal self-contained test harness.
//
// Deliberately not Catch2: the core has zero dependencies and the test suite
// should be able to build and run anywhere with nothing but a compiler. That
// matters when you need to prove the safety layer works on a machine you have
// not set up yet.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& registry();
void check(bool ok, const char* expr, const char* file, int line);
void note(const std::string& message);

int  failures();
int  checksRun();
void beginCase(const char* name);

struct Reg {
    Reg(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

} // namespace test

#define FORGE_CONCAT_(a, b) a##b
#define FORGE_CONCAT(a, b)  FORGE_CONCAT_(a, b)

#define TEST_CASE(name)                                                        \
    static void name();                                                        \
    static ::test::Reg FORGE_CONCAT(reg_, name)(#name, name);                  \
    static void name()

#define CHECK(expr)   ::test::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define REQUIRE(expr)                                                          \
    do {                                                                       \
        const bool ok_ = static_cast<bool>(expr);                              \
        ::test::check(ok_, #expr, __FILE__, __LINE__);                         \
        if (!ok_) return;                                                      \
    } while (false)
