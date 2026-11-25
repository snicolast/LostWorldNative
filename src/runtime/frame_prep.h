// The window's 2D chain and its preparation thread (app.cpp). The 2D layers
// of each vblank are copied from the game (snapshots), composed into the
// pictures the renderer takes (TileGen, HD text, 2D replacements, backdrops)
// and, while interpolating, made ready together with the 3D scene of each
// game frame as soon as the game publishes it.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../common/game_hz.h"
#include "../common/worker.h"
#include "../video/image_io.h"
#include "../video/pack2d.h"
#include "../video/r3d_scene.h"
#include "../video/r3d_state.h"
#include "../video/sky_pack.h"
#include "../video/text_hd.h"
#include "../video/texture_pack.h"
#include "../video/tilegen.h"

struct Options;

// Debugging aid: LW_SKYLOG=path, the sky's scroll and the world camera,
// frame by frame (tools/sky_log.py); 2D frames are logged on the preparation thread
extern FILE* g_sky_log;
extern std::mutex g_sky_log_mutex;

// The 2D layers of one vblank as the renderer takes them (FramePrep::compose_2d)
struct Layers2D {
    uint64_t frame = ~0ull;   // the vblank
    std::vector<uint8_t> vram = std::vector<uint8_t>(TILEGEN_SIZE);   // its layer memory
    std::vector<uint32_t> back = std::vector<uint32_t>(BACK_W * SCREEN_H),
                          front = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          text_cells = std::vector<uint32_t>(TextHD::ROW_TEXELS * TextHD::ROWS),
                          sky_coords = std::vector<uint32_t>(TileGen::SKY_COORDS_W * TileGen::SKY_COORDS_H);
    std::vector<RgbaImage> overlays, sky_pictures;   // new pictures for the GPU slots
    bool hud_band = false;
    float overlay_brightness = 1;
    std::vector<int32_t> back_rows = std::vector<int32_t>(SCREEN_H * 3);   // layer and scroll per row (TileGen)
    uint64_t shift_from = ~0ull, shift_to = ~0ull;   // this 2D frame and the one composed before it
};
// A game frame made ready for the screen ahead of time: its 3D scene and
// the 2D layers of its vblank (FramePrep)
struct Prepared {
    std::shared_ptr<const R3DFrameState> state;
    R3DScene scene;
    Layers2D layers;
    uint64_t serial = 0;
    double ready_at = 0;   // when it was ready
    double period = 1.0 / GAME_HZ;   // since the frame built before it (where its vertices come from)
};
// The 2D layers of a vblank as the game left them (FramePrep::snaps)
struct Snapshot2D {
    uint64_t frame = ~0ull;
    std::vector<uint8_t> vram = std::vector<uint8_t>(TILEGEN_SIZE);
    std::array<uint64_t, TILEGEN_PAGES> serial;   // g_video_page_serial of each page copied
    Snapshot2D() { serial.fill(~0ull); }
};
// New pictures for the GPU slots of a prepared frame (FramePrep::ready_pictures)
struct NewPictures {
    uint64_t serial = 0;
    std::vector<RgbaImage> replacements, overlays, sky;
};

struct FramePrep {
    R3DSceneBuilder builder;
    TexturePack pack;
    // 2D replacements: the foreground (GPU slots 1-27) and the layers below the
    // 3D image (28-55; the backdrops of SkyPack use 56-63)
    Pack2D pack2d, pack2d_back;
    SkyPack sky;   // sky and other backdrops below the 3D image
    TextHD text;   // menus, captions and score drawn from vector fonts
    std::atomic<bool> capture_2d{false};   // F12 / --shot: the next 2D composition is saved
    bool survival_shown = false;   // the survival screens are on (their start screen hides the foreground)

    std::array<Snapshot2D, 4> snaps;   // the 2D layers of the last vblanks (snaps_mutex)
    size_t newest_snap = 0;
    std::mutex snaps_mutex;

    // Preparation thread: while interpolating, each new game frame is made
    // ready as soon as the game finishes it, its 3D scene and its 2D layers
    // side by side, so this thread only draws and keeps up with every
    // refresh (the two took ~14 ms here, more than a refresh at 120 Hz, and
    // the images went out unevenly). The interpolation delay follows when
    // the frames are ready (lateness), so it includes this work
    std::mutex prep_mutex;    // one composition and one scene build at a time
    std::mutex ready_mutex;   // ready_frames and ready_pictures
    // The last frames made ready, oldest first: the picture runs behind the
    // game by the frames' lateness, so after a slow frame several can be
    // waiting, and each is taken in turn (its vertices move from the frame
    // before it, so skipping one would stretch that motion over two)
    std::deque<std::shared_ptr<Prepared>> ready_frames;
    // New pictures for the GPU slots, in order: handed over when their frame
    // (or a later one) is taken, also those of frames never taken
    std::deque<NewPictures> ready_pictures;
    std::atomic<bool> prep_on{false};
    std::atomic<uint64_t> prep_us{0}, prep_frames{0};   // its work, read once a second

    // Copies the game's newest 2D layers into the ring if they are new; the
    // caller holds snaps_mutex
    void poll_snapshots();
    // Copies the newest 2D layers at or before that vblank into `out` (its
    // frame stays ~0 if there are none); takes snaps_mutex
    void take_snapshot(uint64_t up_to, Layers2D& out);
    // Composes the 2D layers of a vblank (out.frame, its layer memory in
    // out.vram) for the renderer. One composition at a time (prep_mutex): the
    // 2D packs and the back layers' scroll keep state between them
    void compose_2d(Layers2D& out);

    void start(const Options* options);   // the preparation thread, with the settings it reads
    void stop();                          // ...stopped (also with the FramePrep, however it ends)
    ~FramePrep() { stop(); }

private:
    const Options* opt = nullptr;
    // Interpolation of the back layers (the sky, renderer's SkyMotion): the
    // layer and scroll of each row of the 2D frame composed, and its vblank
    std::vector<int32_t> back_rows = std::vector<int32_t>(SCREEN_H * 3);
    uint64_t back_rows_frame = ~0ull;
    // Scratch layers of the 2D composition (compose_2d)
    std::vector<uint32_t> front_raw = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          front_raw_top = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          back_raw = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          back_raw_top = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          back_under = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                          front_under = std::vector<uint32_t>(SCREEN_W * SCREEN_H);
    std::atomic<bool> prep_stop{false};
    std::thread prep_thread;
    Worker layers_worker;   // composes the 2D layers while prep_thread builds the 3D scene
    void prepare_frames();   // the preparation thread
};
