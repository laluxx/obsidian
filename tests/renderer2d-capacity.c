/// renderer2d-capacity — tests subsystem
#include "renderer2d_capacity.h"
#include "renderer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/// Implementation

int main(void) {
    assert(sizeof(Vertex2D) == 176 && _Alignof(Vertex2D) == 16);
    assert(renderer2d_capacity_for(0, 1, 1u << 21) == 4096);
    assert(renderer2d_capacity_for(4096, 4096, 1u << 21) == 4096);
    assert(renderer2d_capacity_for(4096, 4097, 1u << 21) == 8192);
    assert(renderer2d_capacity_for(8192, 8193, 1u << 21) == 16384);
    assert(renderer2d_capacity_for(8192, 9000, 10000) == 10000);
    assert(renderer2d_capacity_for(8192, 10001, 10000) == 0);
    assert(renderer2d_capacity_for(0, 1, 0) == 0);
    assert(renderer2d_capacity_for(SIZE_MAX / 2 + 1, SIZE_MAX, SIZE_MAX) == SIZE_MAX);
    puts("ok: renderer 2D capacity grows geometrically within its bound");
    return 0;
}
