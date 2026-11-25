// Sub-pixel motion of the 2D layers below the 3D image (the sky) between
// game frames.
//
// The game scrolls the sky in whole pixels as the camera turns, so its scroll
// is a rounded sample of a smooth motion: 10, 10, 11, 11, 11, 12... on a slow
// turn. Drawn between frames by its last step (SkyPack / compose.frag before
// this), the sky went on by a whole pixel after each step and came back when
// the next frame had none: a tremor at 4 pixels of a 1440p screen, worst when
// the camera moved slowly or came to rest. Here each row's scroll is
// reconstructed at sub-pixel precision from the samples seen so far:
//
// - A row's recent positions (up to 16 frames) are taken as a corridor: the
//   true position at each frame lies within half a pixel of the integer the
//   game wrote. The longest run of newest samples that one constant velocity
//   can pass through gives the plausible velocities (an interval); a step
//   change that no velocity explains (a cut) starts the history again.
// - The velocity drawn in between is the world camera's turn in this frame
//   (R3DRenderer's pose, converted at 4096 / 2pi pixels per radian: the
//   game's own scale, measured) when it lies within the corridor's range plus
//   a tolerance; otherwise the middle of the range. The camera moves the
//   scenery, so the sky follows exactly what it will be next to; its single
//   frame blips (the world tracker's) fall outside the corridor and are ignored.
// - The position at the frame goes on from where the sky was drawn at the end
//   of the previous one when the scroll seen allows it (no jump at all), else
//   the nearest point allowed: within a pixel of the game's own scroll with
//   the camera leading, half a pixel without it. The sky never drifts away
//   from where the game put it, and never steps back against its motion.
//
// Fed once per 2D frame with TileGen's back_rows (the layer and scroll of
// each screen row); solved once per 2D frame when the renderer draws it, with
// the camera's turn when the 3D frame of the same vblank is on screen. Its
// output is two rows of texels for compose.frag (x, then y): per screen row
// R = the integer step since the previous frame + 128 (a 0 texel: nothing
// known for this row), G = the sub-pixel offset of the position at the frame
// from the game's scroll ((G / 255 - 0.5) * 2 pixels), B:A = the velocity in
// pixels per vblank as a 16-bit number of 1/256 pixel biased by 32768. The
// shader draws the row's picture at scroll + offset + velocity * (how far the
// 3D has gone from the previous frame to this one, in vblanks).
#pragma once
#include <cstdint>

#include "tilegen.h"

class SkyMotion {
public:
    static constexpr int ROWS = SCREEN_H;
    static constexpr int HISTORY = 16;   // frames of scroll kept per row
    static constexpr float PX_PER_RADIAN = 4096.0f / 6.2831853f;   // the game's sky scroll per radian of camera turn

    // The back layers of a 2D frame (its vblank): per row the layer that
    // shows most of it (-1: none), its scroll x (0..1023) and picture row y
    // (0..511), as TileGen::Output::back_rows
    void feed(uint64_t frame, const int32_t* back_rows);
    // Solves the motion of the frame fed last. camera: whether the world
    // camera's turn from the previous 3D frame to the one of this vblank is
    // known, as cam_dx, cam_dy sky pixels (x right, y down)
    void solve(bool camera, float cam_dx, float cam_dy);
    bool pending() const { return pending_; }

    // The texels for the shader: ROWS for x, then ROWS for y (layout above)
    const uint32_t* texels() const { return texels_; }
    struct Row {
        bool known = false;   // the row's layer was seen in the previous frame too
        int step = 0;         // the game's scroll step since the previous frame (pixels)
        float move = 0;       // how far the drawn position moved since then (sub-pixel)
        float offset = 0;     // drawn position at the frame minus the game's scroll
        float velocity = 0;   // pixels per vblank drawn in between
    };
    Row row(int y, int axis) const;

private:
    struct Track {
        int layer = -1;
        int last[2] = {0, 0};           // raw scroll of the previous frame
        int n = 0;                      // samples kept
        uint64_t frames[HISTORY];       // oldest first
        int32_t pos[HISTORY][2];        // unwrapped positions
        bool drawn = false;             // a, v hold the last motion solved
        double a[2] = {0, 0}, v[2] = {0, 0};
        uint64_t k = 0;                 // the frame a, v were solved for
        Row out[2];
    };
    void reset(Track& t);
    void solve_axis(Track& t, int axis, bool camera, float cam_v);
    Track rows_[ROWS];
    uint64_t frame_ = ~0ull;
    bool pending_ = false;
    uint32_t texels_[2 * ROWS] = {};
};
