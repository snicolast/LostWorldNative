// Debugging aids driven by environment variables (debug_aids.h)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "debug_aids.h"
#include "debug_snapshot.h"
#include "../video/image_io.h"
#include "../video/r3d_scene.h"
#include "../video/r3d_state.h"

namespace debug_aids {

MenuStep menu_keys_step() {
    static const char* menu_keys = getenv("LW_MENU_KEYS");
    static int menu_presents = 0;
    MenuStep step;
    if (menu_keys && ++menu_presents % 10 == 0) {
        if (*menu_keys == 'c') {
            int cx = 0, cy = 0, len = 0;
            sscanf(menu_keys, "c%d/%d%n", &cx, &cy, &len);
            menu_keys += len;
            if (*menu_keys == ',') menu_keys++;
            step.kind = MenuStep::CLICK, step.x = cx, step.y = cy;
        } else if (*menu_keys) {
            char* end = nullptr;
            int vk = int(strtol(menu_keys, &end, 10));
            menu_keys = *end == ',' ? end + 1 : end;
            step.kind = MenuStep::KEY, step.vk = vk;
        } else if (menu_presents % 30 == 0) {
            menu_keys = nullptr;
            step.kind = MenuStep::DONE;
        }
    }
    return step;
}

static int& film_left() {   // pictures still to take
    static int left = getenv("LW_MENU_FILM") ? atoi(getenv("LW_MENU_FILM")) : 0;
    return left;
}

bool menu_film_on() { return film_left() > 0; }

bool menu_picture(const uint32_t* pixels, int w, int h, const std::function<void(int vk)>& key) {
    static const char* menu_shot = getenv("LW_MENU_SHOT");
    static int film_index = 0;
    static const char* film_keys = getenv("LW_MENU_FILM_KEYS");
    if (menu_shot) bmp_save(menu_shot, w, h, pixels);
    if (film_left() <= 0) return false;
    int fw = w / 2, fh = h / 2;
    std::vector<uint32_t> half(size_t(fw) * fh);
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++) {
            const uint32_t* q = &pixels[size_t(2 * y) * w + 2 * x];
            uint32_t px = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                uint32_t c = ((q[0] >> sh) & 0xFF) + ((q[1] >> sh) & 0xFF) + ((q[w] >> sh) & 0xFF) +
                             ((q[w + 1] >> sh) & 0xFF);
                px |= ((c + 2) / 4) << sh;
            }
            half[size_t(y) * fw + x] = px;
        }
    CreateDirectoryA("out", nullptr);
    CreateDirectoryA("out/menu_film", nullptr);
    char path[64];
    snprintf(path, sizeof(path), "out/menu_film/%03d.bmp", film_index);
    bmp_save(path, fw, fh, half.data());
    for (const char* k = film_keys; k && *k;) {
        int at = 0, vk = 0, len = 0;
        if (sscanf(k, "%d:%d%n", &at, &vk, &len) < 2) break;
        if (at == film_index) key(vk);
        k += len;
        if (*k == ',') k++;
    }
    film_index++;
    return --film_left() == 0;
}

void spot_log(const R3DScene& scene, uint64_t frame, bool back_black, float night_level) {
    static FILE* spot_log = getenv("LW_SPOTLOG") ? fopen(getenv("LW_SPOTLOG"), "w") : nullptr;
    if (!spot_log) return;
    int worlds = 0;
    for (const R3DViewport& v : scene.viewports)
        if (v.world) {
            worlds++;
            fprintf(spot_log,
                    "%llu vp%d col %.0f%.0f%.0f c %.4f %.4f r %.4f %.4f start %.3f ext %.2f sun %.3f amb %.3f "
                    "fog %.5f/%.3f scroll %.3f far %.1f black %d night %.2f "
                    "light %.3f %.3f %.3f world %.3f %.3f %.3f\n",
                    (unsigned long long)frame, worlds, v.spot_color[0], v.spot_color[1], v.spot_color[2],
                    v.spot_center[0], v.spot_center[1], v.spot_radius[0], v.spot_radius[1], v.spot_start,
                    v.spot_extent, v.sun_intensity, v.ambient, v.fog_density, v.fog_ambient, v.scroll_fog,
                    v.scenery_far, int(back_black), night_level, v.sun[0], v.sun[1], v.sun[2],
                    v.world_axes[0] * v.sun[0] + v.world_axes[1] * v.sun[1] + v.world_axes[2] * v.sun[2],
                    v.world_axes[3] * v.sun[0] + v.world_axes[4] * v.sun[1] + v.world_axes[5] * v.sun[2],
                    v.world_axes[6] * v.sun[0] + v.world_axes[7] * v.sun[1] + v.world_axes[8] * v.sun[2]);
        }
    if (!worlds) fprintf(spot_log, "%llu no world\n", (unsigned long long)frame);
    fflush(spot_log);
}

void film_record(const R3DFrameState& built, const std::vector<uint8_t>& vram) {
    static const char* film_rec = getenv("LW_FILM");
    static int film_saved = 0;
    if (!film_rec) return;
    unsigned long long first = 0;
    int count = 0, every = 1;
    sscanf(film_rec, "%llu,%d,%d", &first, &count, &every);
    static unsigned long long film_last = 0;
    if (built.frame >= first && film_saved < count && (!film_saved || built.frame >= film_last + every)) {
        film_last = built.frame;
        CreateDirectoryA("out/film", nullptr);
        char path[64];
        snprintf(path, sizeof(path), "out/film/f_%06llu.bin", (unsigned long long)built.frame);
        save_snapshot(path, built, vram);
        film_saved++;
    }
}

std::string shot_path(uint64_t shown) {
    static int shots_every = 0;
    static unsigned long long shots_from = 0, shots_to = ~0ull;
    static bool shots_parsed = false;
    if (!shots_parsed) {
        shots_parsed = true;
        if (const char* e = getenv("LW_SHOTS")) sscanf(e, "%d,%llu,%llu", &shots_every, &shots_from, &shots_to);
    }
    static uint64_t last_shot = 0;
    if (shots_every > 0 && shown != ~0ull && shown >= shots_from && shown <= shots_to &&
        shown >= last_shot + shots_every) {
        last_shot = shown;
        CreateDirectoryA("out/shots", nullptr);
        char path[64];
        snprintf(path, sizeof(path), "out/shots/%06llu.bmp", (unsigned long long)shown);
        return path;
    }
    return {};
}

}  // namespace debug_aids
