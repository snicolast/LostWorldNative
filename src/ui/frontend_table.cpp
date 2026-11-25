// The settings table (frontend_rows.h): every option of the menu and
// every key of save/settings.ini, in the order the rows are shown.
#include "frontend_rows.h"

using namespace setting_text;
using FS = FrontendSettings;

// In each tab the rows are shown in this order
const std::vector<Frontend::Setting>& Frontend::setting_table() {
    static const std::vector<Setting> table = {
        // DISPLAY
        {.id = OPT_DISPLAY, .tab = DISPLAY_TAB, .label = L"MODE", .key = "display_mode", .number = &FS::display_mode,
         .words = {"window", "borderless", "fullscreen"}, .kind = CHOICE, .shown = {L"WINDOW", L"BORDERLESS", L"FULLSCREEN"},
         .hints = {L"A window you can move and resize (F11 or Alt+Enter: full screen)",
                   L"Full screen at the desktop's resolution, the monitor left as it is: quick to switch to other windows",
                   L"Exclusive full screen: the monitor switches to the RESOLUTION below while the game runs"}},
        // (settings.ini: "resolution = 1920x1080", read and written apart, below; the row's value: option_items)
        {.id = OPT_RESOLUTION, .tab = DISPLAY_TAB, .label = L"RESOLUTION"},
        {.id = OPT_ASPECT, .tab = DISPLAY_TAB, .label = L"WIDESCREEN", .key = "aspect", .number = &FS::aspect,
         .words = {"4:3", "16:9", "21:9", "32:9", "auto"}, .other = 1, .kind = CHOICE,
         .shown = {L"4:3 (ORIGINAL)", L"16:9", L"21:9", L"32:9", L"AUTO"},
         .hints = {L"The arcade's own 4:3 picture: black bars at the sides of a wider window",
                   L"The 3D view at most 16:9 wide: black bars at the sides of a wider window",
                   L"The 3D view at most 21:9 wide: fills ultrawide monitors (2560x1080, 3440x1440)",
                   L"The 3D view at most 32:9 wide: fills super ultrawide monitors (5120x1440, 3840x1080)",
                   L"Fills the window, whatever its shape (up to 32:9)"}},
        {.id = OPT_SCALE, .tab = DISPLAY_TAB, .label = L"RENDER SCALE", .key = "resolution_3d", .number = &FS::scale,
         .lo = FS::SCALES[0], .hi = 200, .kind = SLIDER, .steps = {std::begin(FS::SCALES), std::end(FS::SCALES)},
         .text = percent, .hint = L"Resolution of the 3D picture: lower is faster; above 100% it is drawn bigger and scaled down, "
                                  L"sharper and much slower.  The HUD and the text stay sharp"},
        {.id = OPT_MOTION, .tab = DISPLAY_TAB, .label = L"MOTION", .key = "motion", .flag = &FS::smooth,
         .words = {"original", "smooth"}, .other = 1, .kind = CHOICE, .shown = {L"ORIGINAL", L"SMOOTH"},
         .hint = L"Smooth: the 3D is also drawn between the game's 57.5 frames, at your monitor's refresh rate"},
        {.id = OPT_VSYNC, .tab = DISPLAY_TAB, .label = L"VSYNC", .key = "vsync", .number = &FS::vsync,
         .words = {"off", "on", "adaptive"}, .other = 1, .also = {{"0", 0}, {"1", 1}, {"false", 0}, {"true", 1}},
         .kind = CHOICE, .shown = {L"OFF", L"ON", L"ADAPTIVE"},
         .hints = {L"Each image goes out as soon as it is drawn, without waiting for the monitor: less latency, it may tear without G-SYNC or FreeSync",
                   L"Images in step with the monitor's refreshes: no tearing",
                   L"In step with the refreshes while the game keeps up; a late image goes out at once (less stutter, it may tear then)"}},
        // (settings.ini: "max_fps = monitor / unlimited / N", read and written apart, below; the row: option_items)
        {.id = OPT_MAX_FPS, .tab = DISPLAY_TAB, .label = L"MAX FPS"},
        {.id = OPT_BGFPS, .tab = DISPLAY_TAB, .label = L"BACKGROUND FPS", .key = "background_fps", .flag = &FS::background_fps,
         .kind = TOGGLE, .hint = L"On: 30 images a second at most while another window has the focus (less work for the GPU)"},
        // (settings.ini keeps "dlss" as DLSS QUALITY, as in Sega Rally 2; shown and hints in the order of steps)
        {.id = OPT_AA, .tab = DISPLAY_TAB, .label = L"ANTI-ALIASING", .key = "aa", .number = &FS::aa,
         .words = {"msaa", "dlaa", "none", "dlss", "dlss_balanced", "dlss_performance", "dlss_ultra", "fxaa"},
         .also = {{"off", 2}, {"dlss_quality", 3}}, .kind = CHOICE,
         .shown = {L"OFF", L"FXAA", L"MSAA", L"DLAA", L"DLSS QUALITY", L"DLSS BALANCED", L"DLSS PERFORMANCE",
                   L"DLSS ULTRA PERFORMANCE"},
         .steps = {2, 7, 0, 1, 3, 4, 5, 6},
         .hints = {L"No edge smoothing: the fastest, with jagged edges",
                   L"Edge smoothing over the finished picture: nearly free, for older cards and integrated graphics; a little softer",
                   L"Classic edge smoothing, with the samples below",
                   L"NVIDIA DLAA: the cleanest edges, at full size",
                   L"NVIDIA DLSS: draws the 3D smaller and sharpens it back; much faster, nearly as sharp",
                   L"NVIDIA DLSS: draws the 3D smaller still and sharpens it back; faster, slightly softer",
                   L"NVIDIA DLSS: draws the 3D at half size and sharpens it back; for 4K or slower cards",
                   L"NVIDIA DLSS: draws the 3D at a third of the size; the fastest, the softest"}},
        {.id = OPT_MSAA, .tab = DISPLAY_TAB, .label = L"MSAA SAMPLES", .key = "msaa", .number = &FS::msaa, .lo = 2, .hi = 8,
         .kind = CHOICE, .shown = {L"2X", L"4X", L"8X"}, .steps = {2, 4, 8}, .levels = true,
         .hint = L"Samples per pixel of the edge smoothing: more is smoother and costs more, above all on integrated graphics"},
        {.id = OPT_ANISO, .tab = DISPLAY_TAB, .label = L"ANISOTROPIC FILTER", .key = "anisotropic", .number = &FS::aniso,
         .lo = 0, .hi = 16, .kind = CHOICE, .shown = {L"OFF", L"2X", L"4X", L"8X", L"16X"}, .steps = {0, 2, 4, 8, 16},
         .levels = true,
         .hint = L"Texture detail on the ground and at grazing angles, with the game's mipmaps.  OFF: the original look, no mipmaps"},
        {.id = OPT_FILTER_2D, .tab = DISPLAY_TAB, .label = L"2D FILTER", .key = "filter_2d", .flag = &FS::smooth_2d,
         .words = {"pixels", "smooth"}, .other = 1, .kind = CHOICE, .shown = {L"PIXELS", L"SMOOTH"},
         .hint = L"Smooth: the HUD and the 2D layers upscaled with xBR (edges redrawn); Pixels: square pixels, cheaper"},
        {.id = OPT_TEXT, .tab = DISPLAY_TAB, .label = L"HD TEXT", .key = "hd_text", .flag = &FS::hd_text, .kind = TOGGLE,
         .hint = L"Sharp fonts for menus, captions and score instead of the original pixels"},
        {.id = OPT_SAT, .tab = DISPLAY_TAB, .label = L"COLOR SATURATION", .key = "saturation", .number = &FS::saturation,
         .lo = 0, .hi = 200, .kind = SLIDER, .step = 10, .text = percent,
         .hint = L"How vivid the colours are (100% = original, 80% by default)"},
        {.id = OPT_GAMMA, .tab = DISPLAY_TAB, .label = L"GAMMA", .key = "gamma", .number = &FS::gamma, .lo = 50, .hi = 150,
         .hundredths = true, .kind = SLIDER, .step = 5, .text = decimal,
         .hint = L"Brightness of the mid-tones (1.00 = original)"},
        {.id = OPT_SHARPEN, .tab = DISPLAY_TAB, .label = L"SHARPEN", .key = "sharpen", .flag = &FS::sharpen, .kind = TOGGLE,
         .hint = L"Brings back texture detail after the edge smoothing, without halos"},
        // GRAPHICS: the lighting
        {.id = OPT_SUN, .tab = LIGHTING_TAB, .label = L"SUN", .key = "sun", .flag = &FS::sun, .kind = TOGGLE,
         .hint = L"A sun in the sky and stronger, warmer sunlight outdoors"},
        {.id = OPT_SHADOWS, .tab = LIGHTING_TAB, .label = L"SUN SHADOWS", .key = "shadows", .flag = &FS::shadows,
         .kind = TOGGLE, .hint = L"Dinosaurs, trees and buildings cast shadows away from the sun"},
        {.id = OPT_HDR, .tab = LIGHTING_TAB, .label = L"HDR LIGHTING", .key = "hdr", .flag = &FS::hdr, .kind = TOGGLE,
         .hint = L"Bright light keeps its detail instead of clipping to white; exposure adapts gently"},
        {.id = OPT_AO, .tab = LIGHTING_TAB, .label = L"AMBIENT OCCLUSION", .key = "ao", .flag = &FS::ao, .kind = TOGGLE,
         .hint = L"Soft shadows in corners, creases and on the ground around things"},
        {.id = OPT_BLOOM, .tab = LIGHTING_TAB, .label = L"BLOOM", .key = "bloom", .number = &FS::bloom,
         .words = LEVEL_WORDS, .also = ON_IS_2, .kind = CHOICE, .shown = LEVELS, .levels = true,
         .hint = L"Soft glow around fire, lights, the sky and sunlit surfaces"},
        {.id = OPT_RIM, .tab = LIGHTING_TAB, .label = L"RIM LIGHT", .key = "rim_light", .flag = &FS::rim, .kind = TOGGLE,
         .hint = L"A soft edge of light around dinosaurs and people, lifting them off the background"},
        {.id = OPT_TORCH, .tab = LIGHTING_TAB, .label = L"FLASHLIGHT", .key = "flashlight", .flag = &FS::torch,
         .words = {"original", "modern"}, .other = 1, .kind = CHOICE, .shown = {L"ORIGINAL", L"MODERN"},
         .hint = L"At night: a soft beam with real shading and a glow in the air, or the original spot"},
        {.id = OPT_RT, .tab = LIGHTING_TAB, .label = L"RAY TRACING", .key = "ray_tracing", .number = &FS::ray_tracing,
         .words = {"off", "shadows", "ao"}, .other = 2, .also = {{"0", 0}}, .kind = CHOICE,
         .shown = {L"OFF", L"SHADOWS", L"SHADOWS + AO"}, .levels = true,
         .hints = {L"Shadows from a shadow map and occlusion from the depth, as without ray tracing",
                   L"Sun shadows traced through the world: every tree and dinosaur, soft with distance",
                   L"Also the ambient occlusion: corners and the ground under things, even off screen"}},
        // GRAPHICS (after the lighting): the atmosphere
        {.id = OPT_SKY, .tab = LIGHTING_TAB, .label = L"SKY", .key = "sky", .flag = &FS::sky,
         .words = {"original", "procedural"}, .kind = CHOICE, .shown = {L"ORIGINAL", L"PROCEDURAL"},
         .hint = L"Smooth drifting clouds drawn at full resolution, or the original picture"},
        {.id = OPT_SHAFTS, .tab = LIGHTING_TAB, .label = L"LIGHT SHAFTS", .key = "light_shafts", .flag = &FS::shafts,
         .kind = TOGGLE, .hint = L"Rays of sunlight streaming between trees and silhouettes"},
        {.id = OPT_HAZE, .tab = LIGHTING_TAB, .label = L"DISTANCE HAZE", .key = "haze", .flag = &FS::haze, .kind = TOGGLE,
         .hint = L"Far scenery fades into the sky instead of appearing suddenly"},
        {.id = OPT_LIGHTNING, .tab = LIGHTING_TAB, .label = L"LIGHTNING", .key = "lightning", .flag = &FS::lightning,
         .kind = TOGGLE, .hint = L"Flashes of lightning in stormy fog, with the trees standing out against it"},
        {.id = OPT_RAIN, .tab = LIGHTING_TAB, .label = L"RAIN", .key = "rain", .number = &FS::rain,
         .words = {"off", "light", "heavy"}, .also = ON_IS_2, .kind = CHOICE, .shown = {L"OFF", L"LIGHT", L"HEAVY"},
         .levels = true, .hint = L"Where it rains in the game: a dense fall of drops, wet ground, puddles and ripples"},
        {.id = OPT_SOFT, .tab = LIGHTING_TAB, .label = L"SOFT PARTICLES", .key = "soft_particles", .flag = &FS::soft,
         .kind = TOGGLE, .hint = L"Smoke, fire and explosions blend softly where they touch the ground and walls"},
        // AUDIO
        {.id = OPT_VOLUME, .tab = AUDIO_TAB, .label = L"VOLUME", .key = "volume", .number = &FS::volume, .lo = 0, .hi = 10,
         .kind = SLIDER, .hint = L"Sound volume"},
        {.id = OPT_MUSIC, .tab = AUDIO_TAB, .label = L"MENU MUSIC", .key = "menu_music", .flag = &FS::menu_music,
         .kind = TOGGLE, .hint = L"Music in the menus (res/menu/music.mp3, or your own in third_party/menu/), at the sound volume"},
        {.id = OPT_SPEAKERS, .tab = AUDIO_TAB, .label = L"SPEAKERS", .key = "speakers", .number = &FS::speakers,
         .words = {"stereo", "surround", "headphones"}, .kind = CHOICE,
         .shown = {L"STEREO", L"SURROUND 4.0", L"HEADPHONES"},
         .hints = {L"Front and rear sound mixed on two speakers",
                   L"Rear sound on your rear speakers, as on the cabinet (4.0, 5.1 or 7.1)",
                   L"Two ears: gentle crossfeed, and the rear sound placed behind you"}},
        {.id = OPT_REAR, .tab = AUDIO_TAB, .label = L"REAR LEVEL", .key = "rear_boost", .number = &FS::rear_boost, .lo = -6,
         .hi = 12, .grain = 3, .kind = CHOICE, .steps = {-6, -3, 0, 3, 6, 9, 12}, .text = decibels,
         .hint = L"Rear speakers quieter or louder, as the cabinet's seat speakers: T-Rex roars and many effects"},
        {.id = OPT_WEATHER_SOUND, .tab = AUDIO_TAB, .label = L"WEATHER SOUND", .key = "weather_sound",
         .number = &FS::weather_sound, .words = LEVEL_WORDS, .also = ON_IS_2, .kind = CHOICE, .shown = LEVELS,
         .levels = true, .hint = L"Rain and thunder when the weather brings them, and thunder after every lightning flash"},
        {.id = OPT_SAMPLES, .tab = AUDIO_TAB, .label = L"SAMPLE QUALITY", .key = "sample_interpolation",
         .flag = &FS::cubic_samples, .words = {"linear", "cubic"}, .other = 1, .kind = CHOICE,
         .shown = {L"ORIGINAL", L"SMOOTH"},
         .hint = L"Smooth interpolation of the instrument and voice samples, or the original chip's"},
        // CONTROLS
        {.id = OPT_PAD_SPEED, .tab = CONTROLS_TAB, .label = L"STICK SPEED", .key = "pad_speed", .number = &FS::pad_speed,
         .lo = 1, .hi = 10, .kind = SLIDER, .hint = L"How fast the gamepad's sticks move the sight.  "},   // + the pads
        {.id = OPT_GYRO, .tab = CONTROLS_TAB, .label = L"GYRO AIM", .key = "gyro", .flag = &FS::gyro, .kind = TOGGLE,
         .hint = L"Point the gamepad to aim, together with the sticks"},   // option_items: without a gyroscope
        {.id = OPT_GYRO_SPEED, .tab = CONTROLS_TAB, .label = L"GYRO SPEED", .key = "gyro_speed", .number = &FS::gyro_speed,
         .lo = 1, .hi = 10, .kind = SLIDER, .hint = L"How far the sight moves when you turn the gamepad"},
        {.id = OPT_VIBRATION, .tab = CONTROLS_TAB, .label = L"VIBRATION", .key = "vibration", .number = &FS::vibration,
         .words = LEVEL_WORDS, .kind = CHOICE, .shown = LEVELS, .levels = true,
         .hint = L"The gamepad rumbles when you shoot and when you are hit"},
        {.id = OPT_TRIGGERS, .tab = CONTROLS_TAB, .label = L"TRIGGER RUMBLE", .key = "trigger_rumble", .number = &FS::trigger_rumble,
         .lo = 0, .hi = 100, .grain = 10, .kind = SLIDER, .step = 10, .text = percent_or_off,
         .hint = L"Xbox One / Series pads: the trigger kicks when you fire, the other when you reload, both when you are hit (a share of VIBRATION)"},
        {.id = OPT_ASSIST, .tab = CONTROLS_TAB, .label = L"AIM ASSIST", .key = "aim_assist", .number = &FS::aim_assist,
         .words = LEVEL_WORDS, .kind = CHOICE, .shown = LEVELS, .levels = true,
         .hint = L"With a gamepad the sight slows down over the dinosaurs and follows them a little"},
        {.id = OPT_CROSS, .tab = CONTROLS_TAB, .label = L"CROSSHAIR", .key = "crosshair", .flag = &FS::crosshair,
         .kind = TOGGLE, .hint = L"Extra crosshair where the mouse points"},
        {.id = OPT_CROSS_STYLE, .tab = CONTROLS_TAB, .label = L"CROSSHAIR STYLE", .key = "crosshair_style",
         .number = &FS::crosshair_style, .words = {"default", "custom"}, .other = 1, .kind = CHOICE,
         .shown = {L"DEFAULT", L"CUSTOM"},
         .hint = L"The drawn rings, or your own pictures (crosshair.png and crosshair_p2.png beside the game)"},
        {.id = OPT_CROSS_P1, .tab = CONTROLS_TAB, .label = L"PLAYER 1 PICTURE", .kind = BUTTON, .shown = {L"CHOOSE PNG..."},
         .hint = L"Pick a PNG for player 1's crosshair.  It is copied beside the game as crosshair.png, at its own size in pixels at 1080p"},
        {.id = OPT_CROSS_P2, .tab = CONTROLS_TAB, .label = L"PLAYER 2 PICTURE", .kind = BUTTON, .shown = {L"CHOOSE PNG..."},
         .hint = L"Pick a PNG for player 2's crosshair (crosshair_p2.png).  Without one, player 2 uses player 1's picture"},
        {.id = OPT_CROSS_HIDE, .tab = CONTROLS_TAB, .label = L"HIDE IDLE CROSSHAIR", .key = "crosshair_hide",
         .number = &FS::crosshair_hide, .lo = 0, .hi = 10, .kind = CHOICE, .steps = {0, 3, 5, 10}, .text = seconds,
         .hint = L"A crosshair whose gun has not moved or fired for a while disappears until it moves again"},
        {.id = OPT_MICE, .tab = CONTROLS_TAB, .label = L"SECOND MOUSE", .key = "second_mouse", .flag = &FS::second_mouse,
         .kind = CHOICE, .shown = {L"OFF", L"PLAYER 2"},
         .hint = L"A second mouse joins as player 2 with a click while player 1 plays"},
        {.id = OPT_GUN_TEST, .tab = CONTROLS_TAB, .label = L"GUN TEST", .kind = BUTTON, .shown = {L"OPEN..."},
         .hint = L"Every mouse and light gun Windows sees, what each one is doing and which player it is.  "
                 L"Fire each gun to make it player 1's or player 2's for good"},
        // EXPERIENCE (the first section, as in Sega Rally 2 and Star Wars Trilogy)
        // (settings.ini's first section only: the keys of the whole game, FrontendSettings::copy_whole_game)
        {.id = OPT_EXPERIENCE, .tab = EXPERIENCE_TAB, .label = L"EXPERIENCE", .key = "experience", .number = &FS::experience,
         .words = {"pc", "arcade"}, .kind = CHOICE, .shown = {L"PC EDITION", L"ARCADE ORIGINAL"},
         .hints = {L"The PC version: the main menu, every mode, the achievements on screen and the new graphics.  "
                   L"Each experience keeps its own settings",
                   L"The arcade game as it was: no menu, straight to the attract, START (1 or a gamepad) to play, the "
                   L"original 4:3 look.  Esc pauses.  Each experience keeps its own settings"}},
        // GAME (each experience has its own)
        {.id = OPT_FREE_PLAY, .tab = GAME_TAB, .label = L"FREE PLAY", .key = "free_play", .flag = &FS::free_play,
         .kind = TOGGLE, .hint = L"The arcade's own free play: start and continue with 1 / 2 or a light gun's start button, no coins"},
        {.id = OPT_SUBS, .tab = GAME_TAB, .label = L"SUBTITLES", .key = "subtitles", .number = &FS::subtitles,
         .words = {"off", "english", "japanese"}, .kind = CHOICE, .shown = {L"OFF", L"ENGLISH", L"JAPANESE"},
         .hint = L"Captions of what the characters say"},
        // Only in the file: the WEATHER last picked before a run (in WEATHER_CHOICES' order)
        {.id = NO_ROW, .key = "run_weather", .number = &FS::run_weather,
         .words = {"random", "sunny", "cloudy", "fog", "rain", "storm", "original"}},
        // and the guns picked in GUN TEST
        {.id = NO_ROW, .key = "gun_p1", .path = &FS::gun_p1},
        {.id = NO_ROW, .key = "gun_p2", .path = &FS::gun_p2},
        // and the logos at launch (splash_screens = 0: straight to the menu)
        {.id = NO_ROW, .key = "splash_screens", .flag = &FS::splash},
        {.id = NO_ROW, .key = "fullscreen", .flag = &FS::fullscreen},   // (older versions: display_mode then)
        // and the model DLAA and DLSS use, for advanced users (auto: dlss::preset_for)
        {.id = NO_ROW, .key = "dlss_model", .number = &FS::dlss_model, .words = {"auto", "k", "m", "l"}},
    };
    return table;
}

const Frontend::Setting* Frontend::setting_of(Id id) {
    for (const Setting& d : setting_table())
        if (d.id == id) return &d;
    return nullptr;
}
