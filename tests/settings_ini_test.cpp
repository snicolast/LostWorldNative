// save/settings.ini (src/ui/frontend_settings.cpp): what is written reads back
// the same, older files and looser spellings still read, and the [arcade]
// section keeps ARCADE ORIGINAL's settings apart from PC EDITION's
#include <cstdio>
#include <string>

#include "frontend_settings.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

static const char* FILE_NAME = "settings_ini_test.ini";

static void write_file(const std::string& text) {
    FILE* f = fopen(FILE_NAME, "w");
    if (!f) return;
    fputs(text.c_str(), f);
    fclose(f);
}

static std::string read_file() {
    std::string text;
    FILE* f = fopen(FILE_NAME, "r");
    if (!f) return text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    return text;
}

// A file of just these lines, read over the defaults
static FrontendSettings read_lines(const std::string& lines, const char* section = "") {
    write_file(lines);
    FrontendSettings s;
    s.load(FILE_NAME, section);
    return s;
}

int main() {
    // Every setting away from its default: written, read back, written again the same
    FrontendSettings pc;
    pc.display_mode = 2, pc.res_w = 2560, pc.res_h = 1440;
    pc.volume = 7, pc.speakers = 1, pc.rear_boost = 6, pc.weather_sound = 1, pc.cubic_samples = false;
    pc.aa = 3, pc.dlss_model = 2, pc.msaa = 8, pc.aniso = 16, pc.smooth_2d = false, pc.scale = 150;
    pc.max_fps = 144, pc.background_fps = false, pc.vsync = 2, pc.aspect = 4;
    pc.haze = pc.ao = pc.sky = pc.sun = pc.shadows = pc.shafts = pc.soft = false;
    pc.hdr = pc.rim = pc.sharpen = pc.torch = pc.lightning = false;
    pc.bloom = 3, pc.ray_tracing = 1, pc.rain = 1, pc.hd_text = false;
    pc.crosshair = false, pc.crosshair_style = 0, pc.crosshair_hide = 5, pc.free_play = true;
    pc.smooth = false, pc.second_mouse = false, pc.subtitles = 2, pc.saturation = 95, pc.gamma = 125;
    pc.pad_speed = 8, pc.gyro = false, pc.gyro_speed = 3, pc.vibration = 1, pc.trigger_rumble = 40, pc.aim_assist = 0;
    pc.menu_music = false, pc.splash = false, pc.run_weather = 4;
    pc.gun_p1 = "\\\\?\\HID#VID_1234&PID_5678#gun1", pc.gun_p2 = "";
    FrontendSettings::save_both(FILE_NAME, pc, nullptr, pc);
    const std::string written = read_file();
    FrontendSettings back;
    CHECK(back.load(FILE_NAME));
    CHECK(back.text() == pc.text());
    CHECK(back.aa_saved && back.display_mode_saved && back.max_fps_saved);
    CHECK(back.res_w == 2560 && back.res_h == 1440 && back.max_fps == 144);
    CHECK(back.gun_p1 == pc.gun_p1 && back.gun_p2.empty());
    CHECK(back.gamma == 125 && back.rear_boost == 6 && back.dlss_model == 2 && back.run_weather == 4);
    FrontendSettings::save_both(FILE_NAME, back, nullptr, back);
    CHECK(read_file() == written);   // the same file, byte for byte
    CHECK(written.find("[arcade]") == std::string::npos);

    // No file: false, nothing changed
    remove(FILE_NAME);
    FrontendSettings none;
    CHECK(!none.load(FILE_NAME));
    CHECK(none.text() == FrontendSettings().text() && !none.aa_saved);

    // A first start's file has no aa: the game chooses it then (app.cpp)
    CHECK(!read_lines("volume = 3\n").aa_saved);

    // An older version's file: full screen meant borderless, half_rate meant 60 at most
    FrontendSettings old = read_lines("fullscreen = 1\nhalf_rate = 1\n");
    CHECK(old.display_mode == 1 && old.max_fps == 60 && !old.display_mode_saved && !old.max_fps_saved);
    CHECK(read_lines("fullscreen = 0\n").display_mode == 0);
    CHECK(read_lines("fullscreen = 1\ndisplay_mode = 0\n").display_mode == 0);   // display_mode wins
    CHECK(read_lines("half_rate = 1\nmax_fps = monitor\n").max_fps == 0);       // and so does max_fps

    // max_fps and resolution, the keys with a format of their own
    CHECK(read_lines("max_fps = unlimited\n").max_fps == -1);
    CHECK(read_lines("max_fps = 5000\n").max_fps == 1000);
    CHECK(read_lines("resolution = 1920x1080\n").res_w == 1920);
    FrontendSettings bad = read_lines("resolution = big\n");
    CHECK(bad.res_w == 0 && bad.res_h == 0);
    CHECK(read_lines("resolution = -5x100\n").res_w == 0);

    // Looser spellings and numbers out of their steps
    CHECK(read_lines("bloom = on\n").bloom == 2);          // once a switch: on is MEDIUM
    CHECK(read_lines("rain = true\n").rain == 2);
    CHECK(read_lines("vsync = 0\n").vsync == 0);
    CHECK(read_lines("vsync = whatever\n").vsync == 1);    // an unknown word: the default
    CHECK(read_lines("rear_boost = 5\n").rear_boost == 3); // REAR LEVEL in 3 dB steps
    CHECK(read_lines("rear_boost = 99\n").rear_boost == 12);
    CHECK(read_lines("gamma = 1.234\n").gamma == 123);
    CHECK(read_lines("gun_p1 = none\n").gun_p1.empty());
    CHECK(read_lines("   volume   =   9   \n").volume == 9);
    CHECK(read_lines("no_such_key = 1\nvolume = 2\n").volume == 2);

    // ARCADE ORIGINAL's [arcade] section: its own settings, the whole game's keys only at the top
    FrontendSettings arcade = pc.arcade_defaults();
    arcade.volume = 2, arcade.speakers = 2;
    pc.experience = 1;
    FrontendSettings::save_both(FILE_NAME, pc, &arcade, pc);
    const std::string both = read_file();
    size_t at = both.find("[arcade]\n");
    CHECK(at != std::string::npos);
    CHECK(both.find("experience", at) == std::string::npos && both.find("gun_p1", at) == std::string::npos);
    FrontendSettings top, sect;
    CHECK(top.load(FILE_NAME));
    CHECK(top.text() == pc.text() && top.experience == 1);   // the section's lines are not read at the top
    sect = pc;   // (the whole game's keys come from the top)
    CHECK(sect.load(FILE_NAME, "arcade"));
    CHECK(sect.volume == 2 && sect.speakers == 2 && sect.aspect == 0 && sect.aa == 2);
    CHECK(sect.text(true) == arcade.text(true));
    write_file("experience = 1\n[arcade]\nexperience = 0\nvolume = 1\n");   // a hand-edited section
    FrontendSettings hand;
    hand.experience = 1;
    CHECK(hand.load(FILE_NAME, "arcade"));
    CHECK(hand.experience == 1 && hand.volume == 1);   // the whole game's keys are not read from a section
    write_file("volume = 4\n");
    FrontendSettings no_section;
    CHECK(!no_section.load(FILE_NAME, "arcade"));   // no such section
    CHECK(no_section.volume == FrontendSettings().volume);

    remove(FILE_NAME);
    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    printf("settings_ini: all checks passed\n");
    return 0;
}
