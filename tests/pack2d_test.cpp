// 2D replacement matching (video/pack2d): a replacement shows where its
// cells are on screen unchanged, at its own place or moved; changed cells
// keep the game's pixels; too few unchanged cells, a rival that matches
// better or a missing picture keep it off. Synthetic screens, and the PNG
// reading and the folder listing replaced by stand-ins (no Windows Imaging
// Component), so the matching itself is what is tested
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "pack2d.h"
#include "text_hd.h"
#include "tilegen.h"

namespace fs = std::filesystem;

// Stand-ins for image_io.cpp: every picture is 2x2 except "missing*"
bool png_load(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    if (fs::path(path).filename().string().rfind("missing", 0) == 0) return false;
    w = h = 2;
    rgba.assign(16, 255);
    return true;
}
bool png_save_bgra(const std::string&, int, int, const uint32_t*) { return true; }
std::vector<std::string> list_files(const std::string& dir, const char* pattern) {
    std::vector<std::string> out;
    std::string ext = std::string(pattern).substr(1);   // "*.txt" -> ".txt"
    for (const fs::directory_entry& e : fs::directory_iterator(dir))
        if (e.path().extension() == ext) out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

using Screen = std::vector<uint32_t>;
static Screen blank() { return Screen(SCREEN_W * SCREEN_H, 0); }

// A cell with a pattern of its own (seed): what a drawn letter looks like to the hash
static void draw_cell(Screen& s, int cx, int cy, uint32_t seed) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) s[(cy * 8 + y) * SCREEN_W + cx * 8 + x] = 0xff000000u | (seed * 2654435761u + y * 8 + x);
}
static bool cell_clear(const Screen& s, int cx, int cy) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (s[(cy * 8 + y) * SCREEN_W + cx * 8 + x]) return false;
    return true;
}

// The picture of a box of cells, drawn at (cx, cy)
static void draw_box(Screen& s, int cx, int cy, int cw, int ch, uint32_t seed) {
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) draw_cell(s, cx + x, cy + y, seed + y * cw + x);
}

// A replacement file as tools/make_2d_replacement.py writes it, from a screen
static void write_entry(const fs::path& dir, const char* name, const Screen& s, int cx, int cy, int cw, int ch,
                        const char* extra = "") {
    std::vector<uint64_t> cells;
    Pack2D::hash_cells(s.data(), cells);
    FILE* f = fopen((dir / (std::string(name) + ".txt")).string().c_str(), "w");
    fprintf(f, "# test\nrect %d %d %d %d\n%s", cx, cy, cw, ch, extra);
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) fprintf(f, "%016llx\n", (unsigned long long)cells[(cy + y) * Pack2D::CELLS_X + cx + x]);
    fclose(f);
}

static fs::path fresh_dir(const char* name) {
    fs::path d = fs::temp_directory_path() / ("lwn_pack2d_test_" + std::string(name));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

struct Result {
    int shown;
    Screen target;
    std::vector<uint32_t> cells;
    uint32_t list(int i) const { return cells[TextHD::CELLS_Y * TextHD::ROW_TEXELS + i]; }
};
static Result run(Pack2D& p, const Screen& raw, const Screen* raw_top = nullptr) {
    Result r;
    r.target = raw;
    r.cells.assign(TextHD::ROWS * TextHD::ROW_TEXELS, 0x11111111u);
    r.cells[TextHD::CELLS_Y * TextHD::ROW_TEXELS] = 0;   // the list, emptied each frame by TileGen::compose
    r.shown = p.apply(r.target.data(), SCREEN_W, raw.data(), (raw_top ? *raw_top : raw).data(), r.cells.data());
    return r;
}

// Found where it was captured: its cells cleared, listed with its box and slot
static void test_in_place() {
    fs::path dir = fresh_dir("place");
    Screen cap = blank();
    draw_box(cap, 10, 5, 6, 4, 100);
    write_entry(dir, "map", cap, 10, 5, 6, 4);
    Pack2D p;
    p.init(dir.string(), 3, 9);
    Screen screen = cap;
    draw_cell(screen, 30, 30, 7);   // something else elsewhere
    Result r = run(p, screen);
    CHECK(r.shown == 1);
    CHECK(cell_clear(r.target, 10, 5) && cell_clear(r.target, 15, 8));
    CHECK(!cell_clear(r.target, 30, 30));
    CHECK(r.list(0) >> 16 == 1);
    CHECK(r.list(1) == (uint32_t(5 + 4) << 24 | 10u << 16 | 5u << 8 | (10u + 6)));
    CHECK(r.list(2) >> 16 == 3);   // the first slot of the range
    CHECK(r.cells[(5 * Pack2D::CELLS_X + 10) * TextHD::CELL_TEXELS] == 0);   // the HD text gives way too
    std::vector<RgbaImage> imgs = p.take_new_images();
    CHECK(imgs.size() == 1 && imgs[0].slot == 3);
    CHECK(p.take_new_images().empty());
    // Not on screen: nothing
    CHECK(run(p, blank()).shown == 0);
}

// Cells that changed (the route drawn so far) keep the game's pixels; below
// the share of unchanged cells it is not shown at all
static void test_changed_cells() {
    fs::path dir = fresh_dir("changed");
    Screen cap = blank();
    draw_box(cap, 4, 4, 4, 4, 500);
    write_entry(dir, "box", cap, 4, 4, 4, 4);
    Pack2D p;
    p.init(dir.string());
    Screen screen = cap;
    draw_cell(screen, 5, 5, 9999);   // one of 16 changed
    Result r = run(p, screen);
    CHECK(r.shown == 1);
    CHECK(!cell_clear(r.target, 5, 5));
    CHECK(cell_clear(r.target, 4, 4));
    for (int i = 0; i < 9; i++) draw_cell(screen, 4 + i % 4, 4 + i / 4, 7000 + i);   // 9 of 16: under half
    CHECK(run(p, screen).shown == 0);
}

// Moved elsewhere on screen: found through a few of its cells
static void test_moved() {
    fs::path dir = fresh_dir("moved");
    Screen cap = blank();
    draw_box(cap, 2, 2, 5, 3, 900);
    write_entry(dir, "title", cap, 2, 2, 5, 3);
    Pack2D p;
    p.init(dir.string());
    Screen screen = blank();
    draw_box(screen, 20, 30, 5, 3, 900);
    Result r = run(p, screen);
    CHECK(r.shown == 1);
    CHECK(cell_clear(r.target, 20, 30) && cell_clear(r.target, 24, 32));
    CHECK(r.list(1) == (uint32_t(30 + 3) << 24 | 20u << 16 | 30u << 8 | 25u));
}

// Rival readings over the same cells (STAGE1 inside STAGE10): only the one
// with the larger share of its cells unchanged shows
static void test_rivals() {
    fs::path dir = fresh_dir("rivals");
    Screen cap = blank();
    draw_box(cap, 10, 10, 5, 1, 300);   // "STAGE1"
    write_entry(dir, "a_short", cap, 10, 10, 5, 1);
    draw_cell(cap, 15, 10, 42);   // "STAGE10": one more cell
    write_entry(dir, "b_long", cap, 10, 10, 6, 1);
    Pack2D p;
    p.init(dir.string(), 1, 10);
    Screen screen = blank();
    draw_box(screen, 10, 10, 5, 1, 300);
    Result r = run(p, screen);
    CHECK(r.shown == 1);
    CHECK(r.list(1) == (11u << 24 | 10u << 16 | 10u << 8 | 15u));   // the short one: 5 of 5 beats 5 of 6
    Result both = run(p, cap);
    CHECK(both.shown == 1);
    CHECK(both.list(1) == (11u << 24 | 10u << 16 | 10u << 8 | 16u));   // 6 of 6 beats 5 of 5 on how many
}

// "cover": the whole box gives way, changed cells too; "match": a share of its own
static void test_cover_and_match() {
    fs::path dir = fresh_dir("cover");
    Screen cap = blank();
    draw_box(cap, 0, 0, 4, 1, 1200);
    write_entry(dir, "text", cap, 0, 0, 4, 1, "cover\nmatch 75\n");
    Pack2D p;
    p.init(dir.string());
    Screen screen = cap;
    draw_cell(screen, 3, 0, 5);   // 3 of 4 unchanged: 75%
    Result r = run(p, screen);
    CHECK(r.shown == 1);
    CHECK(cell_clear(r.target, 3, 0));   // covered although changed
    draw_cell(screen, 2, 0, 6);   // 2 of 4: under 75%
    CHECK(run(p, screen).shown == 0);
}

// A reference captured on the top layer pair matches whatever is behind it
static void test_top_layer() {
    fs::path dir = fresh_dir("top");
    Screen cap = blank();
    draw_box(cap, 8, 8, 3, 2, 77);
    write_entry(dir, "hint", cap, 8, 8, 3, 2);
    Pack2D p;
    p.init(dir.string());
    Screen raw = blank();
    draw_box(raw, 8, 8, 3, 2, 5000);   // the layers below change what the full image shows
    CHECK(run(p, raw).shown == 0);
    Result r = run(p, raw, &cap);
    CHECK(r.shown == 1);
}

// A picture that cannot be read: not shown, not tried again, the next entry still is
static void test_missing_picture() {
    fs::path dir = fresh_dir("missing");
    Screen cap = blank();
    draw_box(cap, 1, 1, 2, 2, 10);
    write_entry(dir, "missing_one", cap, 1, 1, 2, 2);
    draw_box(cap, 30, 20, 2, 2, 20);
    write_entry(dir, "present", cap, 30, 20, 2, 2);
    Pack2D p;
    p.init(dir.string(), 5, 9);
    Result r = run(p, cap);
    CHECK(r.shown == 1);
    CHECK(!cell_clear(r.target, 1, 1));
    CHECK(cell_clear(r.target, 30, 20));
    CHECK(r.list(2) >> 16 == 5);   // the slot went to the picture that loaded
    CHECK(run(p, cap).shown == 1);
}

// Files with a box that does not fit the screen or the wrong number of cells are left out
static void test_invalid_files() {
    fs::path dir = fresh_dir("invalid");
    FILE* f = fopen((dir / "outside.txt").string().c_str(), "w");
    fprintf(f, "rect 60 0 4 1\n1\n2\n3\n4\n");
    fclose(f);
    f = fopen((dir / "short.txt").string().c_str(), "w");
    fprintf(f, "rect 0 0 2 2\n1\n2\n3\n");
    fclose(f);
    Pack2D p;
    p.init(dir.string());
    CHECK(p.empty());
}

int main() {
    test_in_place();
    test_changed_cells();
    test_moved();
    test_rivals();
    test_cover_and_match();
    test_top_layer();
    test_missing_picture();
    test_invalid_files();
    for (const char* d : {"place", "changed", "moved", "rivals", "cover", "top", "missing", "invalid"})
        fs::remove_all(fs::temp_directory_path() / ("lwn_pack2d_test_" + std::string(d)));
    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    printf("pack2d: all checks passed\n");
    return 0;
}
