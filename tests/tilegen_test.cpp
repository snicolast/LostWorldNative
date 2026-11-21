// Golden test of the 2D composition (TileGen::compose). 40 pseudo-random
// VRAM images (fixed seed, so identical on every machine and compiler) are
// composed and every output buffer is hashed; the hashes must match
// tests/tilegen_expected.txt, which was produced by the original, slow
// implementation before the compose fast paths were added. Anything that
// changes a single pixel of any output shows up here.
//
//   tilegen_test <expected.txt>            compares
//   tilegen_test <expected.txt> --write    regenerates the file (new reference)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "fnv1a.h"
#include "tilegen.h"

namespace {

constexpr int REFERENCE_MARGIN = 200;   // BACK_MARGIN when tilegen_expected.txt was made
static_assert(BACK_MARGIN >= REFERENCE_MARGIN);

struct Trial {
    uint32_t format;
    bool raws;
    uint64_t hash;
};

// The images: all four layers random, every third one sparse (so the
// transparent paths run), random format and scroll registers, every other
// one with the raw/top/under buffers requested as well.
std::vector<Trial> run() {
    std::mt19937 rng(12345);
    std::vector<uint8_t> vram(TILEGEN_SIZE);
    std::vector<Trial> out;
    for (int trial = 0; trial < 40; trial++) {
        for (auto& b : vram) b = uint8_t(rng());
        if (trial % 3 == 0)
            for (size_t i = 0; i < 0xF6000; i++)
                if (rng() % 4) vram[i] = 0;
        auto put = [&](uint32_t off, uint32_t v) { memcpy(&vram[off], &v, 4); };
        uint32_t format = rng() & 0xFF00;  // bits 8-11 layer above 3D, 12-15 four bpp
        put(0x180000 + 0x20, format);
        for (int l = 0; l < 4; l++) {
            uint32_t sc = (rng() & 0x01FF03FF) | ((rng() & 1) ? 0x80000000u : 0) | ((rng() & 3) == 0 ? 0x8000 : 0);
            put(0x180000 + 0x60 + 4 * l, sc);
        }
        bool raws = trial & 1;
        std::vector<uint32_t> back(BACK_W * SCREEN_H), front(SCREEN_W * SCREEN_H), front_raw(SCREEN_W * SCREEN_H),
            front_top(SCREEN_W * SCREEN_H), back_raw(SCREEN_W * SCREEN_H), back_top(SCREEN_W * SCREEN_H),
            back_under(SCREEN_W * SCREEN_H);
        TileGen::compose(vram.data(), {.back = back.data(),
                                       .front = front.data(),
                                       .front_raw = raws ? front_raw.data() : nullptr,
                                       .front_raw_top = raws ? front_top.data() : nullptr,
                                       .back_raw = raws ? back_raw.data() : nullptr,
                                       .back_raw_top = raws ? back_top.data() : nullptr,
                                       .back_under = raws ? back_under.data() : nullptr});
        // The back image over the 200 columns beyond each side it had when the
        // reference was made (BACK_MARGIN grew to 440 for 32:9: the columns
        // added further out come from the same loop)
        uint64_t h = FNV1A_BASIS;
        for (int y = 0; y < SCREEN_H; y++)
            h = fnv1a_words(&back[y * BACK_W + BACK_MARGIN - REFERENCE_MARGIN], SCREEN_W + 2 * REFERENCE_MARGIN, h);
        for (auto* v : {&front, &front_raw, &front_top, &back_raw, &back_top, &back_under})
            h = fnv1a_words(v->data(), v->size(), h);
        out.push_back({format, raws, h});
    }
    return out;
}

// The rows of the back image follow the scroll (Output::back_rows, used to
// interpolate the sky): the same layers composed again with every layer
// scrolled 7 pixels right and 3 down must report that move on every row
// where the same layer shows the most both times (with random layers that
// can change from one to the other on a row, but not on most). Returns the
// rows wrong.
int back_rows_follow_scroll() {
    std::mt19937 rng(777);
    std::vector<uint8_t> vram(TILEGEN_SIZE);
    for (auto& b : vram) b = uint8_t(rng());
    auto put = [&](uint32_t off, uint32_t v) { memcpy(&vram[off], &v, 4); };
    put(0x180000 + 0x20, 0);   // every layer below the 3D image, 8 bpp
    std::vector<uint32_t> back(BACK_W * SCREEN_H), front(SCREEN_W * SCREEN_H);
    std::vector<int32_t> rows[2] = {std::vector<int32_t>(SCREEN_H * 3), std::vector<int32_t>(SCREEN_H * 3)};
    for (int pass = 0; pass < 2; pass++) {
        for (int l = 0; l < 4; l++)
            put(0x180000 + 0x60 + 4 * l, 0x80000000u | uint32_t(40 * l + 3 * pass) << 16 | uint32_t(100 * l + 7 * pass));
        TileGen::compose(vram.data(), {.back = back.data(), .front = front.data(), .back_rows = rows[pass].data()});
    }
    int wrong = 0, same = 0;
    for (int y = 0; y < SCREEN_H; y++) {
        const int32_t *a = &rows[0][3 * y], *b = &rows[1][3 * y];
        if (a[0] < 0 || b[0] != a[0]) continue;
        same++;
        wrong += ((b[1] - a[1]) & 511) != 7 || ((b[2] - a[2]) & 511) != 3;
    }
    if (same < SCREEN_H / 2) return SCREEN_H - same;   // too few rows to prove anything
    return wrong;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tilegen_test <expected.txt> [--write]\n");
        return 2;
    }
    const char* path = argv[1];
    bool write = argc > 2 && strcmp(argv[2], "--write") == 0;
    std::vector<Trial> got = run();

    if (write) {
        FILE* f = fopen(path, "w");
        if (!f) {
            perror(path);
            return 2;
        }
        fprintf(f, "# TileGen::compose golden hashes: trial format raws fnv1a64 (see tests/tilegen_test.cpp)\n");
        for (size_t i = 0; i < got.size(); i++)
            fprintf(f, "%zu %04X %d %016llX\n", i, got[i].format, int(got[i].raws), (unsigned long long)got[i].hash);
        fclose(f);
        printf("wrote %s (%zu cases)\n", path, got.size());
        return 0;
    }

    FILE* f = fopen(path, "r");
    if (!f) {
        perror(path);
        return 2;
    }
    int fails = 0, seen = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned idx, format;
        int raws;
        unsigned long long hash;
        if (sscanf(line, "%u %x %d %llx", &idx, &format, &raws, &hash) != 4 || idx >= got.size()) {
            fprintf(stderr, "invalid line: %s", line);
            return 2;
        }
        seen++;
        const Trial& t = got[idx];
        bool ok = t.format == format && int(t.raws) == raws && t.hash == hash;
        if (!ok)
            printf("case %u: expected %04X %d %016llX, got %04X %d %016llX\n", idx, format, raws, hash, t.format,
                   int(t.raws), (unsigned long long)t.hash);
        fails += !ok;
    }
    fclose(f);
    if (seen != int(got.size())) {
        printf("%s has %d cases, the test generates %zu\n", path, seen, got.size());
        return 1;
    }
    printf(fails ? "tilegen: %d of %d cases differ\n" : "tilegen: %d cases match\n", fails ? fails : seen, seen);
    int rows_wrong = back_rows_follow_scroll();
    printf(rows_wrong ? "tilegen: %d back rows do not follow the scroll\n" : "tilegen: the back rows follow the scroll\n",
           rows_wrong);
    return fails != 0 || rows_wrong != 0;
}
