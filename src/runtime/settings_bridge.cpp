#include "settings_bridge.h"

#include <algorithm>

#include "../ui/frontend_settings.h"

// Ultrawide monitors are 2.37 (2560x1080) to 2.39 (3440x1440), wider than
// 21/9 = 2.33, so 21:9 allows up to 2.4
const float MAX_ASPECT[3] = {4.0f / 3.0f, 16.0f / 9.0f, 2.4f};
const float BLOOM_STRENGTH[4] = {0.0f, 0.6f, 1.0f, 1.6f};

int aspect_level(float max_aspect) { return max_aspect < 1.5f ? 0 : max_aspect < 2.0f ? 1 : 2; }

void quality_options(const FrontendSettings& s, Options& o) {
    o.supersample = s.scale / 100.0f;
    o.half_rate = s.half_rate;
    o.ray_tracing = s.ray_tracing;
    o.ao = s.ao ? 1.0f : 0.0f;
    o.shadows = s.shadows;
    o.shafts = s.shafts;
    o.soft = s.soft;
    o.sky_procedural = s.sky;
    o.relief = s.relief;
    o.bloom = BLOOM_STRENGTH[std::clamp(s.bloom, 0, 3)];
    o.msaa = s.aa == 2 ? 1 : s.msaa;
    o.aniso = s.aniso;
    o.pixel_2d = !s.smooth_2d;
}

void options_from_settings(const FrontendSettings& s, Options& o) {
    quality_options(s, o);
    o.volume = float(s.volume);
    o.speakers = s.speakers;
    o.cubic_samples = s.cubic_samples;
    o.rear_boost = s.rear_boost;
    o.weather_sound = s.weather_sound;
    o.dlaa = s.aa == 1;
    o.vsync = s.vsync;
    int aspect = std::clamp(s.aspect, 0, 2);
    if (aspect != aspect_level(o.max_aspect)) o.max_aspect = MAX_ASPECT[aspect];   // --aspect 2.6 stays as 21:9
    o.haze = s.haze ? 1.0f : 0.0f;
    o.sun = s.sun;
    o.lightning = s.lightning;
    o.rain = s.rain;
    o.hdr = s.hdr;
    o.rim = s.rim;
    o.sharpen = s.sharpen;
    o.modern_torch = s.torch;
    o.classic_text = !s.hd_text;
    o.subtitles = s.subtitles;
    o.show_crosshair = s.crosshair;
    o.custom_crosshair = s.crosshair_style == 1;
    o.crosshair_hide = s.crosshair_hide;
    o.free_play = s.free_play;
    o.interp = s.smooth;
    o.several_mice = s.second_mouse;
    o.saturation = s.saturation / 100.0f;
    o.gamma = s.gamma / 100.0f;
}

void settings_from_options(const Options& o, FrontendSettings& s) {
    s.volume = std::clamp(int(o.volume + 0.5f), 0, 10);
    s.speakers = std::clamp(o.speakers, 0, 2);
    s.cubic_samples = o.cubic_samples;
    s.rear_boost = std::clamp(o.rear_boost / 3 * 3, -6, 12);
    s.weather_sound = std::clamp(o.weather_sound, 0, 3);
    s.aa = o.dlaa ? 1 : o.msaa <= 1 ? 2 : 0;
    if (o.msaa > 1) s.msaa = o.msaa >= 8 ? 8 : o.msaa >= 4 ? 4 : 2;
    s.aniso = o.aniso <= 0 ? 0 : o.aniso < 4 ? 2 : o.aniso < 8 ? 4 : o.aniso < 16 ? 8 : 16;
    s.smooth_2d = !o.pixel_2d;
    s.scale = FrontendSettings::SCALES[0];   // the nearest step at or below
    for (int sc : FrontendSettings::SCALES)
        if (sc <= int(o.supersample * 100 + 0.5f)) s.scale = sc;
    s.half_rate = o.half_rate;
    s.vsync = o.vsync;
    s.aspect = aspect_level(o.max_aspect);   // --aspect 2.6 shows as 21:9 and is kept until changed
    s.haze = o.haze > 0;
    s.bloom = o.bloom <= 0 ? 0 : o.bloom < 0.8f ? 1 : o.bloom < 1.3f ? 2 : 3;
    s.ao = o.ao > 0;
    s.sky = o.sky_procedural;
    s.sun = o.sun;
    s.shadows = o.shadows;
    s.ray_tracing = o.ray_tracing;
    s.shafts = o.shafts;
    s.soft = o.soft;
    s.lightning = o.lightning;
    s.rain = o.rain;
    s.hdr = o.hdr;
    s.rim = o.rim;
    s.relief = o.relief;
    s.sharpen = o.sharpen;
    s.torch = o.modern_torch;
    s.hd_text = !o.classic_text;
    s.subtitles = o.subtitles;
    s.crosshair = o.show_crosshair;
    s.crosshair_style = o.custom_crosshair ? 1 : 0;
    s.crosshair_hide = o.crosshair_hide;
    s.free_play = o.free_play;
    s.smooth = o.interp;
    s.second_mouse = o.several_mice;
    s.saturation = std::clamp(int(o.saturation * 100 + 0.5f), 0, 200);
    s.gamma = std::clamp(int(o.gamma * 100 + 0.5f), 50, 150);
}
