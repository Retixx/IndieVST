#include "TestUtil.h"

#include <cstdio>

namespace test {
namespace {
int  gFailures = 0;
int  gChecks   = 0;
const char* gCurrent = "";
int  gCaseFailures = 0;
} // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void beginCase(const char* name) {
    gCurrent = name;
    gCaseFailures = 0;
}

void check(bool ok, const char* expr, const char* file, int line) {
    ++gChecks;
    if (ok) return;
    ++gFailures;
    ++gCaseFailures;
    std::printf("    FAIL  %s:%d\n          %s\n", file, line, expr);
}

void note(const std::string& message) {
    std::printf("    note: %s\n", message.c_str());
}

int failures()  { return gFailures; }
int checksRun() { return gChecks; }

} // namespace test

int main() {
    auto& cases = test::registry();
    std::printf("Forge core test suite - %zu cases\n\n", cases.size());

    int failedCases = 0;
    for (const auto& c : cases) {
        const int before = test::failures();
        test::beginCase(c.name);
        std::printf("  %-40s", c.name);
        std::fflush(stdout);
        std::printf("\n");
        c.fn();
        const int added = test::failures() - before;
        if (added > 0) {
            ++failedCases;
            std::printf("  %-40s FAILED (%d)\n", c.name, added);
        } else {
            std::printf("  %-40s ok\n", c.name);
        }
    }

    std::printf("\n%d checks, %d failures across %d/%zu failing cases\n",
                test::checksRun(), test::failures(), failedCases, cases.size());
    return test::failures() == 0 ? 0 : 1;
}
