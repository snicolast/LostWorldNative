#include "r3d_shelter.h"

#include <algorithm>
#include <cmath>

int R3DShelter::hits(const R3DVertex* v, size_t count, const float axes[9]) {
    // The ray origins across the ground plane: the camera, then the ring
    float ox[RAYS], oz[RAYS];
    ox[0] = oz[0] = 0;
    for (int i = 1; i < RAYS; i++) {
        float a = float(i - 1) * (6.2831853f / float(RAYS - 1));
        ox[i] = RING * std::cos(a);
        oz[i] = RING * std::sin(a);
    }
    const float* X = axes;
    const float* Y = axes + 3;
    const float* Z = axes + 6;
    bool hit[RAYS] = {};
    int found = 0;
    for (size_t t = 0; t + 2 < count && found < RAYS; t += 3) {
        const R3DVertex* tri = v + t;
        uint32_t flags = tri[0].flags;
        if (flags & (R3D_ACTOR | R3D_RAIN)) continue;
        if ((flags & R3D_TEXTURED) && (flags & (R3D_ALPHA_TEST | R3D_TEX_ALPHA))) continue;
        // In the world's frame around the camera: across the ground (x, z), up (h)
        float x[3], z[3], h[3];
        for (int k = 0; k < 3; k++) {
            const float* p = tri[k].pos;
            x[k] = X[0] * p[0] + X[1] * p[1] + X[2] * p[2];
            h[k] = Y[0] * p[0] + Y[1] * p[1] + Y[2] * p[2];
            z[k] = Z[0] * p[0] + Z[1] * p[1] + Z[2] * p[2];
        }
        if (std::max({h[0], h[1], h[2]}) < MIN_UP || std::min({h[0], h[1], h[2]}) > MAX_UP) continue;
        if (std::min({x[0], x[1], x[2]}) > RING || std::max({x[0], x[1], x[2]}) < -RING) continue;
        if (std::min({z[0], z[1], z[2]}) > RING || std::max({z[0], z[1], z[2]}) < -RING) continue;
        // Seen from above: walls have no area
        float area = (x[1] - x[0]) * (z[2] - z[0]) - (x[2] - x[0]) * (z[1] - z[0]);
        if (std::fabs(area) < 1e-4f) continue;
        for (int r = 0; r < RAYS; r++) {
            if (hit[r]) continue;
            float b1 = ((ox[r] - x[0]) * (z[2] - z[0]) - (x[2] - x[0]) * (oz[r] - z[0])) / area;
            float b2 = ((x[1] - x[0]) * (oz[r] - z[0]) - (ox[r] - x[0]) * (z[1] - z[0])) / area;
            float b0 = 1.0f - b1 - b2;
            if (b0 < 0 || b1 < 0 || b2 < 0) continue;
            float up = b0 * h[0] + b1 * h[1] + b2 * h[2];
            if (up < MIN_UP || up > MAX_UP) continue;
            hit[r] = true;
            found++;
        }
    }
    return found;
}

void R3DShelter::step(const R3DVertex* v, size_t count, const float axes[9]) {
    if (frame_++ % 4 != 0) return;
    last_hits_ = hits(v, count, axes);
    bool covered = last_hits_ >= COVERED;
    if (covered == sheltered_) {
        pending_ = 0;
        return;
    }
    if (++pending_ >= 2) {
        sheltered_ = covered;
        pending_ = 0;
    }
}
