// The window's 2D chain and its preparation thread (frame_prep.h)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include "frame_prep.h"
#include "options.h"
#include "runtime.h"
#include "survival.h"
#include "../common/clock.h"

FILE* g_sky_log = getenv("LW_SKYLOG") ? fopen(getenv("LW_SKYLOG"), "w") : nullptr;
std::mutex g_sky_log_mutex;

void FramePrep::poll_snapshots() {
    std::lock_guard<std::mutex> lock(g_video_mutex);
    if (g_video_frame != snaps[newest_snap].frame && !g_boot_skip) {
        newest_snap = (newest_snap + 1) % snaps.size();
        Snapshot2D& s = snaps[newest_snap];
        s.frame = g_video_frame;
        for (uint32_t p = 0; p < TILEGEN_PAGES; p++)   // (the pages copied again since this one was filled)
            if (s.serial[p] != g_video_page_serial[p]) {
                memcpy(&s.vram[p << 16], &g_video_vram[p << 16], 0x10000);
                s.serial[p] = g_video_page_serial[p];
            }
    }
}

void FramePrep::take_snapshot(uint64_t up_to, Layers2D& out) {
    std::lock_guard<std::mutex> lock(snaps_mutex);
    poll_snapshots();
    const Snapshot2D* pick = nullptr;
    for (const Snapshot2D& s : snaps)
        if (s.frame != ~0ull && s.frame <= up_to && (!pick || s.frame > pick->frame)) pick = &s;
    out.frame = pick ? pick->frame : ~0ull;
    if (pick) memcpy(out.vram.data(), pick->vram.data(), TILEGEN_SIZE);
}

void FramePrep::compose_2d(Layers2D& out) {
    const TextHD* hd_text = opt->classic_text ? nullptr : &text;
    if (!hd_text) std::fill(out.text_cells.begin(), out.text_cells.end(), 0u);
    bool capture = capture_2d.exchange(false);
    if (capture) sky.capture();   // F12 also saves the backdrops, for textures/sky
    bool sky_on = sky.active();
    // Raw layers for recognising the 2D replacements (and for F12)
    bool raw_front = !pack2d.empty() || capture, raw_back = !pack2d_back.empty() || capture;
    TileGen::compose(out.vram.data(),
                     {.back = out.back.data(),
                      .front = out.front.data(),
                      .front_raw = raw_front ? front_raw.data() : nullptr,
                      .front_raw_top = raw_front ? front_raw_top.data() : nullptr,
                      .front_under = raw_front ? front_under.data() : nullptr,
                      .back_raw = raw_back ? back_raw.data() : nullptr,
                      .back_raw_top = raw_back ? back_raw_top.data() : nullptr,
                      .back_under = raw_back ? back_under.data() : nullptr,
                      .text_cells = out.text_cells.data(),
                      .sky_coords = sky_on ? out.sky_coords.data() : nullptr,
                      .back_rows = back_rows.data()},
                     hd_text, sky_on ? &sky : nullptr);
    // The layer and scroll of each row of the back layers, for drawing
    // them in between game frames (renderer.set_sky_rows)
    {
        out.back_rows = back_rows;
        // Debugging aid: LW_SKYLOG=path writes the back layers' scroll of each 2D
        // frame and the world camera of each 3D frame (tools/sky_log.py)
        if (g_sky_log) {
            std::lock_guard<std::mutex> lock(g_sky_log_mutex);
            fprintf(g_sky_log, "2d %llu", (unsigned long long)out.frame);
            for (int y = 0; y < SCREEN_H; y += 8)
                fprintf(g_sky_log, " %d,%d,%d", back_rows[3 * y], back_rows[3 * y + 1], back_rows[3 * y + 2]);
            fprintf(g_sky_log, "\n");
        }
        out.shift_from = back_rows_frame;
        out.shift_to = back_rows_frame = out.frame;
    }
    out.sky_pictures = sky.take_new_images();
    if (capture) {   // F12 or --shot: capture for tools/make_2d_replacement.py
        CreateDirectoryA("out/2d", nullptr);
        std::string path = "out/2d/capture_" + std::to_string(out.frame);
        Pack2D::save_capture(front_raw.data(), path);
        Pack2D::save_capture(front_raw_top.data(), path + "_layer_a");   // only the texts' layers
        Pack2D::save_capture(back_raw.data(), path + "_back");          // the layers below the 3D image
        Pack2D::save_capture(back_raw_top.data(), path + "_back_layer_a");
        printf("2D layer saved to %s.png\n", path.c_str());
    }
    // The top band only moves to the screen edges when it looks like a
    // HUD (empty in the middle), not when it is part of a picture.
    // Measured before the 2D replacements clear what they cover (the
    // boss name plate in the middle stays a reason to keep it)
    int middle = 0;
    for (int y = 0; y < 36; y++)
        for (int x = 160; x < 336; x++)
            middle += (out.front[y * SCREEN_W + x] >> 24) != 0 ||
                      out.text_cells[((y / 8) * TextHD::CELLS_X + x / 8) * TextHD::CELL_TEXELS] != 0;
    out.hud_band = middle * 20 < 36 * 176;
    int offset[3];
    TileGen::front_offset(out.vram.data(), offset);
    // Survival's start screen: the game stands still on its first frame
    // in the arena, where the foreground can still hold the screen the
    // home state was kept at (from the menu, the boot's FBI warning)
    SurvivalMode surv_mode = survival_shown ? survival_view().mode : SurvivalMode::Off;
    if (surv_mode == SurvivalMode::Loading || surv_mode == SurvivalMode::Ready) {
        std::fill(out.front.begin(), out.front.end(), 0u);
        std::fill(out.text_cells.begin(), out.text_cells.end(), 0u);
    } else if (!pack2d.empty())
        pack2d.apply(out.front.data(), SCREEN_W, front_raw.data(), front_raw_top.data(), out.text_cells.data(),
                     false, front_under.data(), out.back.data() + BACK_MARGIN, BACK_W);
    if (!pack2d_back.empty())
        pack2d_back.apply(out.back.data() + BACK_MARGIN, BACK_W, back_raw.data(), back_raw_top.data(),
                          out.text_cells.data(), true, back_under.data());
    out.overlay_brightness = float(offset[0] + offset[1] + offset[2]) / (3 * 255.0f);
    out.overlays = pack2d.take_new_images();
    for (auto& img : pack2d_back.take_new_images()) out.overlays.push_back(std::move(img));
}

// The preparation thread (see ready_frames)
void FramePrep::prepare_frames() {
    std::vector<std::shared_ptr<Prepared>> pool;   // reused once nobody else holds them
    std::shared_ptr<const R3DFrameState> done;
    uint64_t serial = 0;
    while (!prep_stop) {
        std::shared_ptr<const R3DFrameState> state;
        {
            std::unique_lock<std::mutex> lock(g_r3d_mutex);
            g_r3d_cv.wait_for(lock, std::chrono::milliseconds(20),
                              [&] { return prep_stop || (prep_on && g_r3d_latest != done); });
            state = g_r3d_latest;
        }
        if (!prep_on) done.reset();
        if (prep_stop || !prep_on || !state || state == done) continue;
        double before = done ? done->time : 0;
        done = state;
        std::shared_ptr<Prepared> p;
        for (auto& q : pool)
            if (q.use_count() == 1) {
                p = q;
                break;
            }
        if (!p) pool.push_back(p = std::make_shared<Prepared>());
        p->state = state;
        p->serial = ++serial;
        p->period = before > 0 ? std::clamp(state->time - before, 1.0 / 240, 0.1) : 1.0 / GAME_HZ;
        NewPictures pictures;
        pictures.serial = p->serial;
        {
            std::lock_guard<std::mutex> lock(prep_mutex);
            take_snapshot(state->vblank, p->layers);   // the 2D layers of the frame's vblank
            bool layers = p->layers.frame != ~0ull;
            if (layers) layers_worker.run([&] { compose_2d(p->layers); });
            double from = now_seconds();
            builder.build(*state, p->scene);
            double built_at = now_seconds();
            pictures.replacements = pack.take_new_images();
            if (layers) layers_worker.wait();
            double took = now_seconds() - from;
            prep_us += uint64_t(took * 1e6);
            prep_frames++;
            frame_log("R %llu %.6f %.3f %.3f\n", (unsigned long long)state->frame, now_seconds(),
                      (built_at - from) * 1000, took * 1000);
            pictures.overlays.swap(p->layers.overlays);
            pictures.sky.swap(p->layers.sky_pictures);
            p->layers.overlays.clear();
            p->layers.sky_pictures.clear();
        }
        std::lock_guard<std::mutex> lock(ready_mutex);
        ready_pictures.push_back(std::move(pictures));
        p->ready_at = now_seconds();
        if (prep_on) {
            ready_frames.push_back(p);
            if (ready_frames.size() > 8) ready_frames.pop_front();
        }
    }
}

void FramePrep::start(const Options* options) {
    opt = options;
    prep_thread = std::thread([this] { prepare_frames(); });
}

void FramePrep::stop() {
    prep_stop = true;
    g_r3d_cv.notify_all();
    if (prep_thread.joinable()) prep_thread.join();
}
