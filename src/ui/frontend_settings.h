// The options menu's settings (save/settings.ini), without the menu itself
// (frontend.h), for the settings bridge and its test
#pragma once
#include <string>

struct FrontendSettings {
    int display_mode = 0;      // MODE: 0 window, 1 borderless (the desktop's resolution), 2 exclusive full screen
    bool fullscreen = false;   // older versions' key (written as display_mode != window): when display_mode is missing, borderless
    bool display_mode_saved = false;   // load() found display_mode
    int res_w = 0, res_h = 0;  // RESOLUTION: the window's inside, or the full-screen mode (0: the window as it opens / the desktop's)
    int volume = 5;    // 0..10 (output gain)
    int speakers = 0;  // 0 stereo, 1 surround 4.0, 2 headphones (AudioStream::Speakers)
    int rear_boost = 0;  // dB on the rear pair: -6, -3, 0, 3, 6, 9, 12
    int weather_sound = 2;  // rain and thunder of our weather: 0 off, 1 low, 2 medium, 3 high
    bool cubic_samples = true;   // smooth (cubic) sample interpolation instead of the original linear one
    // 0 MSAA, 1 DLAA, 2 off, 3 DLSS QUALITY, 4 BALANCED, 5 PERFORMANCE, 6 ULTRA PERFORMANCE
    // (DLAA and DLSS on, off or changed take effect on the next launch)
    int aa = 0;
    bool aa_saved = false;   // load() found it in the file (not there on the first start: chosen then for the GPU, app.cpp)
    // settings.ini only (dlss_model = auto / k / m / l), for advanced users: the
    // model DLAA and DLSS use. 0 AUTO (dlss::preset_for), 1 K, 2 M, 3 L
    int dlss_model = 0;
    int msaa = 4;      // MSAA samples per pixel with aa = 0: 2, 4 or 8
    int aniso = 8;     // anisotropic taps of the texture filtering: 0 (no mipmaps, original look), 2, 4, 8, 16
    bool smooth_2d = true;   // HUD and 2D layers upscaled with xBR (false: square pixels, cheaper)
    int scale = 100;   // RENDER SCALE: the 3D's resolution in % of the window's (SCALES)
    // MAX FPS (MOTION SMOOTH): 0 the monitor's refresh rate, -1 UNLIMITED (without vsync), else images a
    // second at most (with vsync in whole refreshes). settings.ini: max_fps = monitor / unlimited / N
    int max_fps = 0;
    bool max_fps_saved = false;   // load() found it (else an older file's half_rate = 1: 60)
    bool half_rate = false;       // older versions' key (above 100 Hz every other refresh), read only
    int vsync = 1;            // 0 off (images go out without waiting for the refresh), 1 on, 2 adaptive (on unless late)
    int aspect = 1;    // widest 3D view: 0 4:3 (original), 1 16:9, 2 21:9 (ultrawide, up to 2.4:1)
    bool haze = true;  // distance haze
    int bloom = 2;     // glow around bright things: 0 off, 1 low, 2 medium, 3 high
    bool ao = true;    // ambient occlusion
    bool sky = true;   // procedural sky instead of the original picture
    bool sun = true;   // a sun in the procedural sky
    bool shadows = true;   // sun shadows
    int ray_tracing = 2;   // traced through the world (GPUs with ray tracing): 0 off, 1 sun shadows, 2 also ambient occlusion
    bool shafts = true;    // light shafts from the sun
    bool soft = true;      // soft particles
    bool hdr = true;       // HDR lighting
    bool rim = true;       // rim light on dinosaurs and people
    bool relief = true;    // surface detail from the textures
    bool sharpen = true;   // contrast adaptive sharpening
    bool torch = true;     // modern flashlight at night (false: the original spotlight)
    bool lightning = true; // lightning in dark grey fog
    int rain = 2;          // rain where the game has its own: 0 off, 1 light, 2 heavy
    bool hd_text = true;
    bool crosshair = true;
    int crosshair_style = 1; // 0 the drawn rings, 1 the crosshair.png pictures when there are any (ui/crosshair_file.h)
    int crosshair_hide = 0; // seconds without use before a crosshair hides: 0 never, 3, 5 or 10
    bool free_play = false; // the game's own free play: start and continue with no credit
    bool smooth = true;     // the 3D drawn between game frames at the monitor's refresh rate
    bool second_mouse = true;   // a second mouse is player 2's gun
    int subtitles = 0;      // voice-line subtitles: 0 off, 1 English, 2 Japanese
    int saturation = 80;    // colour saturation in % (100 = original; 80 by default)
    int gamma = 100;        // gamma x 100 (100 = original, higher = brighter)
    int pad_speed = 5;      // gamepad sight speed with the sticks, 1..10
    bool gyro = true;       // the gamepad's gyroscope moves the sight
    int gyro_speed = 5;     // 1..10
    int vibration = 2;      // gamepad vibration: 0 off, 1 low, 2 medium, 3 high
    int trigger_rumble = 100;   // the triggers' own motors (Xbox One / Series pads), % of VIBRATION's, steps of 10
    int aim_assist = 2;     // gamepad aim assist: 0 off, 1 low, 2 medium, 3 high
    bool menu_music = true; // ui/menu_music.h, when there is a music file
    bool splash = true;     // the snicolast logo before the main menu (only in the file: splash_screens)
    int run_weather = 0;    // RANDOM SPAWNS and survival: the WEATHER last picked before a run (frontend.cpp)
    std::string gun_p1, gun_p2; // the guns picked in GUN TEST: device paths ("" = the first and second to fire)
    bool check_updates = true;  // at launch, news.txt on GitHub: a window with what is new and UPDATE NOW
    int news_seen = 0;          // the newest news.txt entry the player closed with LATER

    // save/settings.ini; missing keys keep their current value
    void load(const char* path);
    void save(const char* path) const;

    static constexpr int SCALES[5] = {50, 67, 75, 85, 100};   // RENDER SCALE's steps
};
