// Debugging aids for one scene: snapshots (F9, --replay, LW_FILM) and the
// scene's text dump (debug_snapshot.h)
#define WIN32_LEAN_AND_MEAN   // runtime.h brings windows.h: no min/max macros
#define NOMINMAX
#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>

#include "debug_snapshot.h"
#include "runtime.h"
#include "../video/r3d_scene.h"
#include "../video/r3d_state.h"
#include "../video/tilegen.h"

static const uint32_t SNAPSHOT_MAGIC = 0x3153574C;   // "LWS1"

void save_snapshot(const char* path, const R3DFrameState& state, const std::vector<uint8_t>& vram) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t count = uint32_t(state.pages.size());
    fwrite(&SNAPSHOT_MAGIC, 4, 1, f);
    fwrite(&state.frame, 8, 1, f);
    fwrite(&count, 4, 1, f);
    for (auto& [key, page] : state.pages) {
        fwrite(&key, 4, 1, f);
        fwrite(page->data(), 1, page->size(), f);
    }
    std::vector<uint16_t> texels;
    g_texture_ram.copy_all(texels);
    fwrite(texels.data(), 2, texels.size(), f);
    fwrite(vram.data(), 1, vram.size(), f);
    fclose(f);
}

bool load_snapshot(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint32_t magic = 0, count = 0;
    auto state = std::make_shared<R3DFrameState>();
    bool ok = fread(&magic, 4, 1, f) == 1 && magic == SNAPSHOT_MAGIC && fread(&state->frame, 8, 1, f) == 1 &&
              fread(&count, 4, 1, f) == 1;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint32_t key;
        auto page = std::make_shared<R3DPage>();
        ok = fread(&key, 4, 1, f) == 1 && fread(page->data(), 1, page->size(), f) == page->size();
        state->pages[key] = page;
    }
    std::vector<uint16_t> texels(2048 * 2048);
    ok = ok && fread(texels.data(), 2, texels.size(), f) == texels.size();
    std::vector<uint8_t> vram(TILEGEN_SIZE);
    ok = ok && fread(vram.data(), 1, vram.size(), f) == vram.size();
    fclose(f);
    if (!ok) return false;
    g_texture_ram.load_all(texels);
    {
        std::lock_guard<std::mutex> lock(g_video_mutex);
        g_video_vram = vram;
        g_video_frame = state->frame;
        for (uint64_t& serial : g_video_page_serial) serial++;
    }
    std::lock_guard<std::mutex> lock(g_r3d_mutex);
    g_r3d_latest = std::move(state);
    return true;
}

// Debugging aid for --shot: every triangle with its screen bounding box
// (native pixels), plus the 2D layers' alpha along two columns
void dump_scene(const R3DScene& scene, const uint32_t* back, const uint32_t* front, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (size_t vi = 0; vi < scene.viewports.size(); vi++) {
        const R3DViewport& v = scene.viewports[vi];
        fprintf(f, "viewport %zu prio %d rect %.0f %.0f %.0f %.0f opaque %u trans %u\n", vi, v.priority, v.x, v.y,
                v.w, v.h, v.opaque_count / 3, v.trans_count / 3);
        fprintf(f, "  fog density %.5f start %.3f colour %.3f %.3f %.3f ambient %.3f scroll %.3f far %.1f world %d\n",
                v.fog_density, v.fog_start, v.fog_color[0], v.fog_color[1], v.fog_color[2], v.fog_ambient,
                v.scroll_fog, v.scenery_far, int(v.world));
        for (int pass = 0; pass < 2; pass++) {
            uint32_t first = pass ? v.trans_first : v.opaque_first, count = pass ? v.trans_count : v.opaque_count;
            for (uint32_t t = 0; t + 2 < count; t += 3) {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, zmin = 1e9f;
                const R3DVertex* tri = &scene.vertices[first + t];
                for (int k = 0; k < 3; k++) {
                    const float* p = tri[k].pos;
                    float cx = v.proj[0] * p[0] + v.proj[4] * p[1] + v.proj[8] * p[2] + v.proj[12];
                    float cy = v.proj[1] * p[0] + v.proj[5] * p[1] + v.proj[9] * p[2] + v.proj[13];
                    float cw = v.proj[3] * p[0] + v.proj[7] * p[1] + v.proj[11] * p[2] + v.proj[15];
                    if (cw <= 1e-6f) cw = 1e-6f;
                    float sx = v.x + (cx / cw + 1) / 2 * v.w, sy = v.y + (cy / cw + 1) / 2 * v.h;
                    x0 = std::min(x0, sx), x1 = std::max(x1, sx), y0 = std::min(y0, sy), y1 = std::max(y1, sy);
                    zmin = std::min(zmin, -p[2]);
                }
                fprintf(f, "  tri v%zu %s bbox %.1f %.1f %.1f %.1f z %.3f color %08x flags %x tex %08x\n", vi,
                        pass ? "T" : "O", x0, y0, x1, y1, zmin, tri[0].color, tri[0].flags, tri[0].tex);
            }
        }
    }
    for (int y = 0; y < SCREEN_H; y += 4)
        fprintf(f, "2d row %d: back[0] %08x back[248] %08x front[0] %08x front[248] %08x\n", y,
                back[y * BACK_W + BACK_MARGIN], back[y * BACK_W + BACK_MARGIN + 248], front[y * SCREEN_W],
                front[y * SCREEN_W + 248]);
    fclose(f);
}
