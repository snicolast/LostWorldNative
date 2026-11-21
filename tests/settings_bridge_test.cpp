// The options menu and the command-line options (runtime/settings_bridge):
// every menu value survives the trip to Options and back
#include <cmath>
#include <cstdio>

#include "settings_bridge.h"
#include "../src/ui/frontend_settings.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

// The settings the bridge carries (the others, such as the window, the
// gamepad and the menu music, stay in FrontendSettings)
#define BRIDGED(X)                                                                                            \
    X(volume) X(speakers) X(rear_boost) X(weather_sound) X(cubic_samples) X(aa) X(dlss_model) X(msaa) X(aniso) X(smooth_2d)  \
    X(scale) X(max_fps) X(vsync) X(aspect) X(haze) X(bloom) X(ao) X(sky) X(sun) X(shadows) X(ray_tracing)  \
    X(shafts) X(soft) X(hdr) X(rim) X(relief) X(sharpen) X(torch) X(lightning) X(rain) X(hd_text)           \
    X(crosshair) X(crosshair_style) X(crosshair_hide) X(free_play) X(smooth) X(second_mouse) X(subtitles)    \
    X(saturation) X(gamma)

static bool same(const FrontendSettings& a, const FrontendSettings& b, const char* what) {
    bool ok = true;
#define COMPARE(f)                                                                     \
    if (a.f != b.f) {                                                                  \
        printf("  %s: %s %d -> %d\n", what, #f, int(a.f), int(b.f));                    \
        ok = false;                                                                    \
    }
    BRIDGED(COMPARE)
#undef COMPARE
    return ok;
}

static FrontendSettings round_trip(const FrontendSettings& s) {
    Options o;
    options_from_settings(s, o);
    FrontendSettings back = s;
    settings_from_options(o, back);
    return back;
}

// Each menu value of each row, one at a time over the defaults
static void test_round_trip() {
    struct Row {
        const char* name;
        int FrontendSettings::*field;
        std::initializer_list<int> values;
    };
    static const Row ints[] = {
        {"volume", &FrontendSettings::volume, {0, 1, 5, 9, 10}},
        {"speakers", &FrontendSettings::speakers, {0, 1, 2}},
        {"rear_boost", &FrontendSettings::rear_boost, {-6, -3, 0, 3, 6, 9, 12}},
        {"weather_sound", &FrontendSettings::weather_sound, {0, 1, 2, 3}},
        {"aa", &FrontendSettings::aa, {0, 1, 2, 3, 4, 5, 6, 7}},
        {"dlss_model", &FrontendSettings::dlss_model, {0, 1, 2, 3}},
        {"msaa", &FrontendSettings::msaa, {2, 4, 8}},
        {"aniso", &FrontendSettings::aniso, {0, 2, 4, 8, 16}},
        {"scale", &FrontendSettings::scale, {50, 67, 75, 85, 100, 125, 150, 175, 200}},
        {"max_fps", &FrontendSettings::max_fps, {-1, 0, 30, 60, 117, 144, 240}},
        {"vsync", &FrontendSettings::vsync, {0, 1, 2}},
        {"aspect", &FrontendSettings::aspect, {0, 1, 2, 3, 4}},
        {"bloom", &FrontendSettings::bloom, {0, 1, 2, 3}},
        {"ray_tracing", &FrontendSettings::ray_tracing, {0, 1, 2}},
        {"rain", &FrontendSettings::rain, {0, 1, 2}},
        {"crosshair_style", &FrontendSettings::crosshair_style, {0, 1}},
        {"crosshair_hide", &FrontendSettings::crosshair_hide, {0, 3, 5, 10}},
        {"subtitles", &FrontendSettings::subtitles, {0, 1, 2}},
        {"saturation", &FrontendSettings::saturation, {0, 50, 80, 100, 135, 200}},
        {"gamma", &FrontendSettings::gamma, {50, 75, 100, 125, 150}},
    };
    for (const Row& r : ints)
        for (int v : r.values) {
            FrontendSettings s;
            s.*r.field = v;
            if (r.field == &FrontendSettings::msaa) s.aa = 0;   // the samples only show with MSAA on
            char what[64];
            snprintf(what, sizeof(what), "%s = %d", r.name, v);
            CHECK(same(s, round_trip(s), what));
        }
    struct Flag {
        const char* name;
        bool FrontendSettings::*field;
    };
    static const Flag flags[] = {
        {"cubic_samples", &FrontendSettings::cubic_samples}, {"smooth_2d", &FrontendSettings::smooth_2d},
        {"haze", &FrontendSettings::haze},                   {"ao", &FrontendSettings::ao},
        {"sky", &FrontendSettings::sky},                     {"sun", &FrontendSettings::sun},
        {"shadows", &FrontendSettings::shadows},             {"shafts", &FrontendSettings::shafts},
        {"soft", &FrontendSettings::soft},                   {"hdr", &FrontendSettings::hdr},
        {"rim", &FrontendSettings::rim},                     {"relief", &FrontendSettings::relief},
        {"sharpen", &FrontendSettings::sharpen},             {"torch", &FrontendSettings::torch},
        {"lightning", &FrontendSettings::lightning},         {"hd_text", &FrontendSettings::hd_text},
        {"crosshair", &FrontendSettings::crosshair},         {"free_play", &FrontendSettings::free_play},
        {"smooth", &FrontendSettings::smooth},               {"second_mouse", &FrontendSettings::second_mouse},
    };
    for (const Flag& f : flags)
        for (bool v : {false, true}) {
            FrontendSettings s;
            s.*f.field = v;
            char what[64];
            snprintf(what, sizeof(what), "%s = %d", f.name, int(v));
            CHECK(same(s, round_trip(s), what));
        }
}

// A command-line aspect wider than 21:9 shows as 21:9 and is kept until the
// menu row changes
static void test_aspect_kept() {
    Options o;
    o.max_aspect = 2.6f;
    FrontendSettings s;
    settings_from_options(o, s);
    CHECK(s.aspect == 2);
    options_from_settings(s, o);
    CHECK(std::fabs(o.max_aspect - 2.6f) < 1e-6f);
    s.aspect = 1;
    options_from_settings(s, o);
    CHECK(std::fabs(o.max_aspect - 16.0f / 9.0f) < 1e-6f);
    // AUTO and 32:9 allow the same width; each stays as it was chosen
    for (int a : {3, 4}) {
        s.aspect = a;
        options_from_settings(s, o);
        CHECK(std::fabs(o.max_aspect - 32.0f / 9.0f) < 1e-6f);
        settings_from_options(o, s);
        CHECK(s.aspect == a);
    }
}

int main() {
    test_round_trip();
    test_aspect_kept();
    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    printf("settings bridge: all checks passed\n");
    return 0;
}
