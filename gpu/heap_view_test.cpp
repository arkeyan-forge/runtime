// Checks which textures the bindless heaps bind through their own view: a cube map and a stacked array always (the
// default view is a 2D array the shaders do not declare), a swizzled texture, and never a plain 2D or 3D texture.
// usage: ArkeyanHeapViewTest
#include "gpu/heap_view.h"
#include <cstdio>

int main()
{
    int failures = 0;
    auto check = [&](bool condition, const char* what) {
        if (!condition)
        {
            fprintf(stderr, "FAIL: %s\n", what);
            failures++;
        }
    };
    check(gpu::NeedsOwnView(true, 6, 0), "a cube map without a swizzle");
    check(gpu::NeedsOwnView(true, 6, 0x05040302), "a swizzled cube map");
    check(gpu::NeedsOwnView(false, 4, 0), "a stacked 2D array");
    check(gpu::NeedsOwnView(false, 1, 0x05040302), "a swizzled 2D texture");
    check(!gpu::NeedsOwnView(false, 1, 0), "a plain 2D or 3D texture keeps the default view");
    if (failures == 0)
        printf("ArkeyanHeapViewTest: all passed\n");
    return failures == 0 ? 0 : 1;
}
