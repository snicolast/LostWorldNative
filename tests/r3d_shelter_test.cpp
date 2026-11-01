// The roof over the camera (r3d_shelter.h): a ceiling, a room with walls
// only, foliage, an actor, a tilted camera and the two-test hysteresis
#include <cmath>
#include <cstdio>
#include <vector>

#include "r3d_shelter.h"

static int g_failed = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAILED line %d: %s\n", __LINE__, #cond); \
            g_failed++;                                      \
        }                                                    \
    } while (0)

static const float LEVEL[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

// A horizontal square (two triangles) at height h, half size s, around (cx, cz)
static void quad(std::vector<R3DVertex>& out, float h, float s, uint32_t flags = 0, float cx = 0, float cz = 0) {
    const float c[4][2] = {{-s, -s}, {s, -s}, {s, s}, {-s, s}};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i : idx) {
        R3DVertex v{};
        v.pos[0] = cx + c[i][0];
        v.pos[1] = h;
        v.pos[2] = cz + c[i][1];
        v.flags = flags | R3D_WORLD;
        out.push_back(v);
    }
}

// A vertical wall at x = d
static void wall(std::vector<R3DVertex>& out, float d) {
    const float c[4][2] = {{-20, -5}, {20, -5}, {20, 30}, {-20, 30}};   // (z, y)
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i : idx) {
        R3DVertex v{};
        v.pos[0] = d;
        v.pos[1] = c[i][1];
        v.pos[2] = c[i][0];
        v.flags = R3D_WORLD;
        out.push_back(v);
    }
}

int main() {
    std::vector<R3DVertex> s;
    // Open sky, a floor below
    quad(s, -5, 100);
    CHECK(R3DShelter::hits(s.data(), s.size(), LEVEL) == 0);
    // A ceiling 8 units up covers every ray
    std::vector<R3DVertex> room = s;
    quad(room, 8, 20);
    CHECK(R3DShelter::hits(room.data(), room.size(), LEVEL) == R3DShelter::RAYS);
    // Walls alone are no roof
    std::vector<R3DVertex> walls = s;
    wall(walls, 6), wall(walls, -6);
    CHECK(R3DShelter::hits(walls.data(), walls.size(), LEVEL) == 0);
    // Too high: a sky dome or a cliff top
    std::vector<R3DVertex> dome = s;
    quad(dome, 200, 300);
    CHECK(R3DShelter::hits(dome.data(), dome.size(), LEVEL) == 0);
    // Foliage and actors let the rain through
    std::vector<R3DVertex> leaves = s;
    quad(leaves, 10, 20, R3D_TEXTURED | R3D_ALPHA_TEST);
    quad(leaves, 4, 20, R3D_ACTOR);
    CHECK(R3DShelter::hits(leaves.data(), leaves.size(), LEVEL) == 0);
    // An eave over one side only: the camera at its edge stays outdoors
    std::vector<R3DVertex> eave = s;
    quad(eave, 6, 5, 0, 5.5f, 0);   // x from 0.5 to 10.5
    int h = R3DShelter::hits(eave.data(), eave.size(), LEVEL);
    CHECK(h > 0 && h < R3DShelter::COVERED);
    // A camera pitched down 30 degrees and rolled: the ceiling is still found
    {
        float p = 0.5236f, r = 0.3f;
        // world -> view rotation: pitch about x then roll about z; axes = view coordinates of world x, y, z
        float cp = std::cos(p), sp = std::sin(p), cr = std::cos(r), sr = std::sin(r);
        float Rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};   // row-major
        float Rz[9] = {cr, -sr, 0, sr, cr, 0, 0, 0, 1};
        float M[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                M[i * 3 + j] = 0;
                for (int k = 0; k < 3; k++) M[i * 3 + j] += Rz[i * 3 + k] * Rx[k * 3 + j];
            }
        float axes[9];
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 3; i++) axes[k * 3 + i] = M[i * 3 + k];   // column k
        std::vector<R3DVertex> turned = room;
        for (R3DVertex& v : turned) {
            float w[3] = {v.pos[0], v.pos[1], v.pos[2]};
            for (int i = 0; i < 3; i++) v.pos[i] = M[i * 3] * w[0] + M[i * 3 + 1] * w[1] + M[i * 3 + 2] * w[2];
        }
        CHECK(R3DShelter::hits(turned.data(), turned.size(), axes) == R3DShelter::RAYS);
    }
    // Hysteresis: one test under a roof does not flip it, two do; out again likewise
    R3DShelter sh;
    int frames = 0;
    auto run = [&](const std::vector<R3DVertex>& v, int n) {
        for (int i = 0; i < n; i++, frames++) sh.step(v.data(), v.size(), LEVEL);
    };
    run(s, 8);
    CHECK(!sh.sheltered());
    run(room, 1);   // a test
    run(s, 3);
    CHECK(!sh.sheltered());
    run(s, 1);      // the next test disagrees: back to zero
    run(room, 1);   // ... not a test (frame 13)
    run(room, 7);   // tests at 16 and 20
    CHECK(sh.sheltered());
    run(s, 8);
    CHECK(!sh.sheltered());
    sh.clear();
    CHECK(!sh.sheltered());

    if (g_failed) {
        printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    printf("r3d_shelter: all checks passed\n");
    return 0;
}
