// The options menu's settings (save/settings.ini) and its QUALITY levels,
// without the menu itself (frontend.h), for the settings bridge and its test
#pragma once
#include <string>

struct FrontendSettings {
    bool fullscreen = false;
    int volume = 5;    // 0..10 (output gain)
    int speakers = 0;  // 0 stereo, 1 surround 4.0, 2 headphones (AudioStream::Speakers)
    int rear_boost = 0;  // dB on the rear pair: -6, -3, 0, 3, 6, 9, 12
    int weather_sound = 2;  // rain and thunder of our weather: 0 off, 1 low, 2 medium, 3 high
    bool cubic_samples = true;   // smooth (cubic) sample interpolation instead of the original linear one
    int aa = 0;        // 0 MSAA, 1 DLAA, 2 off (DLAA on or off takes effect on the next launch)
    int msaa = 4;      // MSAA samples per pixel with aa = 0: 2, 4 or 8
    int aniso = 8;     // anisotropic taps of the texture filtering: 0 (no mipmaps, original look), 2, 4, 8, 16
    bool smooth_2d = true;   // HUD and 2D layers upscaled with xBR (false: square pixels, cheaper)
    int scale = 100;   // 3D resolution in % of the window's (SCALES)
    bool half_rate = false;   // above 100 Hz the 3D is drawn every other refresh
    bool vsync = true;        // off: images go out without waiting for the monitor's refresh
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
    bool free_play = false; // 1 / 2 (or a gun's start button) insert the credit themselves
    bool smooth = true;     // the 3D drawn between game frames at the monitor's refresh rate
    bool second_mouse = true;   // a second mouse is player 2's gun
    int subtitles = 0;      // voice-line subtitles: 0 off, 1 English, 2 Japanese
    int saturation = 80;    // colour saturation in % (100 = original; 80 by default)
    int gamma = 100;        // gamma x 100 (100 = original, higher = brighter)
    int pad_speed = 5;      // gamepad sight speed with the sticks, 1..10
    bool gyro = true;       // the gamepad's gyroscope moves the sight
    int gyro_speed = 5;     // 1..10
    int vibration = 2;      // gamepad vibration: 0 off, 1 low, 2 medium, 3 high
    int aim_assist = 2;     // gamepad aim assist: 0 off, 1 low, 2 medium, 3 high
    bool menu_music = true; // ui/menu_music.h, when there is a music file
    int run_weather = 0;    // RANDOM SPAWNS and survival: the WEATHER last picked before a run (frontend.cpp)
    std::string gun_p1, gun_p2; // the guns picked in GUN TEST: device paths ("" = the first and second to fire)

    // save/settings.ini; missing keys keep their current value
    void load(const char* path);
    void save(const char* path) const;

    // QUALITY: the graphics options that cost the most, set together. quality()
    // is the level they match, CUSTOM if none
    enum Quality { LOW, MEDIUM, HIGH, ULTRA, CUSTOM };
    static constexpr int SCALES[5] = {50, 67, 75, 85, 100};
    void set_quality(int q);
    int quality() const;
};
