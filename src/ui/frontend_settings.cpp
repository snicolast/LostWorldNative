// FrontendSettings' QUALITY levels, apart from the menu's drawing so that
// the tests can link them (tests/settings_bridge_test.cpp)
#include <algorithm>

#include "frontend_settings.h"

namespace {
// What each QUALITY level sets (the other options are left as they are)
struct QualityLevel {
    int scale;
    bool half_rate;
    int ray_tracing;   // 0 off, 1 sun shadows, 2 also ambient occlusion
    bool ao, shadows, shafts, soft, sky, relief;
    int bloom;
    int msaa, aniso;   // samples per pixel (1 = off), anisotropic taps
    bool smooth_2d;    // xBR on the 2D layers
};
const QualityLevel QUALITY_LEVELS[4] = {
    // LOW: laptops, integrated graphics and older cards. Measured on an RTX
    // 5070 Ti at 1440p, the costly ones are the ambient occlusion, the 3D
    // resolution, the procedural sky (its composition) and the sun shadows;
    // on an integrated GPU, limited by memory bandwidth and fill rate, also
    // the MSAA samples, the anisotropic taps (read by hand from the texture
    // RAM), the surface detail and the xBR of the two 2D layers in every pixel
    {75, true, 0, false, false, false, true, false, false, 1, 1, 2, false},
    // MEDIUM: mid-range cards (RTX 3060, RX 6600)
    {100, true, 0, true, true, true, true, true, true, 1, 2, 4, true},
    // HIGH
    {100, false, 1, true, true, true, true, true, true, 2, 4, 8, true},
    // ULTRA: everything (the defaults)
    {100, false, 2, true, true, true, true, true, true, 2, 4, 8, true},
};
}  // namespace

void FrontendSettings::set_quality(int q) {
    if (q < LOW || q > ULTRA) return;
    const QualityLevel& l = QUALITY_LEVELS[q];
    scale = l.scale;
    half_rate = l.half_rate;
    ray_tracing = l.ray_tracing;
    ao = l.ao;
    shadows = l.shadows;
    shafts = l.shafts;
    soft = l.soft;
    sky = l.sky;
    relief = l.relief;
    bloom = l.bloom;
    // MSAA off is the ANTI-ALIASING row; DLAA, picked by hand, is left alone
    if (l.msaa == 1) aa = aa == 1 ? 1 : 2;
    else if (aa == 2) aa = 0;
    msaa = std::max(l.msaa, 2);
    aniso = l.aniso;
    smooth_2d = l.smooth_2d;
}

int FrontendSettings::quality() const {
    for (int q = LOW; q <= ULTRA; q++) {
        const QualityLevel& l = QUALITY_LEVELS[q];
        bool msaa_same = aa == 1 || (l.msaa == 1 ? aa == 2 : aa == 0 && msaa == l.msaa);
        if (scale == l.scale && half_rate == l.half_rate && ray_tracing == l.ray_tracing && ao == l.ao &&
            shadows == l.shadows && shafts == l.shafts && soft == l.soft && sky == l.sky && relief == l.relief &&
            bloom == l.bloom && msaa_same && aniso == l.aniso && smooth_2d == l.smooth_2d)
            return q;
    }
    return CUSTOM;
}
