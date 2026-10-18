#include <cstring>
#include <exception>

#include "Test.h"

int main(int argc, char** argv) {
    int passed = 0, failed = 0, skipped = 0;
    for (const auto& c : bfarb::test::Cases()) {
        bool wanted = argc < 2;
        for (int i = 1; i < argc; ++i) wanted = wanted || std::strstr(c.name, argv[i]) != nullptr;
        if (!wanted) continue;
        try {
            c.fn();
            ++passed;
            std::printf("ok      %s\n", c.name);
        } catch (const bfarb::test::Skip& s) {
            ++skipped;
            std::printf("skipped %s: %s\n", c.name, s.what());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAILED  %s: %s\n", c.name, e.what());
        }
        std::fflush(stdout);
    }
    std::printf("%d passed, %d failed, %d skipped, %d checks\n", passed, failed, skipped, bfarb::test::Checks());
    return failed ? 1 : 0;
}
