// R3DStateReader (r3d_state.h) reads the same words as R3DFrameState with
// its page cache: culling RAM low and high, polygon RAM, pages the frame
// does not have, and a frame change
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "r3d_state.h"

static int g_failed = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAILED line %d: %s\n", __LINE__, #cond); \
            g_failed++;                                      \
        }                                                    \
    } while (0)

static uint32_t g_seed = 12345;
static uint32_t next_random() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed;
}

// A frame with `count` pages of noise spread over the three memories
static R3DFrameState make_frame(int count) {
    R3DFrameState f;
    const uint32_t bases[3] = {0x8C00, 0x8E00, 0x9800};
    for (int i = 0; i < count; i++) {
        uint32_t key = bases[i % 3] + (next_random() % (i % 3 == 1 ? 4 : 64));
        auto page = std::make_shared<R3DPage>();
        for (uint8_t& b : *page) b = uint8_t(next_random());
        f.pages[key] = page;
    }
    return f;
}

int main() {
    R3DFrameState a = make_frame(40), b = make_frame(25);
    R3DStateReader r;
    for (const R3DFrameState* f : {&a, &b, &a}) {
        r.attach(f);
        for (int i = 0; i < 200000; i++) {
            uint32_t addr;
            switch (next_random() % 4) {
            case 0: addr = next_random() % 0x100000; break;               // culling low
            case 1: addr = 0x800000 + next_random() % 0x40000; break;     // culling high
            case 2: addr = next_random() & 0xFFFFFF; break;               // anywhere (invalid ones read 0)
            default: addr = (next_random() % 0x100000) | 0x5000000; break;   // poly, high bits ignored
            }
            if (next_random() & 1) {
                CHECK(r.cull(addr) == f->cull(addr));
                CHECK(r.cull_valid(addr) == f->cull_valid(addr));
                float x = r.cullf(addr), y = f->cullf(addr);
                CHECK(memcmp(&x, &y, 4) == 0);
            } else {
                CHECK(r.poly(addr) == f->poly(addr));
            }
            if (g_failed > 10) break;
        }
        CHECK(r.state() == f);
    }
    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("r3d_state: the cached reader agrees with the frame\n");
    return 0;
}
