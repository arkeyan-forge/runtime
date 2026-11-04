// Checks the remembered "scale auto" step-down's file line (gpu/auto_scale.h). usage: SSAAutoScaleTest
#include "gpu/auto_scale.h"
#include <cstdio>

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok)
        {
            fprintf(stderr, "FAIL %s\n", what);
            failures++;
        }
    };
    using namespace gpu::autoscale;
    check(Parse(Format(1, "Apple M1 Pro"), "Apple M1 Pro") == 1, "round trip");
    check(Parse("1 Apple M1 Pro\r\n", "Apple M1 Pro") == 1, "CRLF");
    check(Parse(Format(1, "Apple M1 Pro"), "Apple M2") == 0, "another adapter");
    check(Parse("x Apple M1 Pro", "Apple M1 Pro") == 0, "not a number");
    check(Parse("0 Apple M1 Pro", "Apple M1 Pro") == 0 && Parse("11 Apple M1 Pro", "Apple M1 Pro") == 0, "range");
    check(Parse("", "Apple M1 Pro") == 0 && Parse("2", "Apple M1 Pro") == 0, "empty, no adapter");
    check(Parse("2 NVIDIA GeForce GTX 1060 6GB", "NVIDIA GeForce GTX 1060 6GB") == 2, "spaces in the name");
    if (failures == 0)
        printf("auto scale test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
