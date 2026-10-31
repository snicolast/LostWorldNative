// Command-line options of LostWorld.exe. One table in options.cpp drives the
// parser and --help; the menu (frontend.h) reads and writes the same struct
// while the game runs.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

struct Options {
    // Run
    std::string rom_zip;             // --rom: lostwsga.zip, read directly (LW_ROM_ZIP when not given)
    std::string data_dir = "data";   // --data: the ROM files tools/romtool.py extracts
    int frame_limit = INT32_MAX;     // --frames; with --headless, 600 by default
    bool headless = false;           // --headless: no window, full speed, report at the end
    bool menu = true;                // the main menu and save/settings.ini; off in test runs
    bool show_boot = false;          // --boot: the power-on screens before the SEGA logo
    int boss_maker = -1;             // --make-boss-state N: boss index 0..3, -1 when not making one
    // Picture
    float supersample = 1.0f;        // --scale: internal 3D resolution factor
    int msaa = 4;                    // --msaa: samples per pixel (1 = off)
    bool dlaa = false;               // --aa dlaa: NVIDIA DLAA instead of MSAA
    int aniso = 8;                   // --aniso: anisotropic taps (0 = no mipmaps, original look)
    float max_aspect = 16.0f / 9.0f; // --aspect: widest 3D view (4:3 = original)
    bool pixel_2d = false;           // --pixel2d: 2D layers without xBR upscaling
    bool hud_centre = false;         // --hud-centre: keep the HUD in the 4:3 area
    bool letterbox = false;          // --letterbox: keep the black cutscene bars
    bool classic_text = false;       // --classic-text: original pixel-art letters instead of vector fonts
    bool interp = true;              // --no-interp: show game frames as they are
    bool half_rate = false;          // --half-rate: above 100 Hz the 3D is drawn every other refresh
    bool vsync = true;               // --no-vsync: images go out without waiting for the refresh (tearing)
    bool dump_textures = false;      // --dump-textures: write new textures to out/textures
    float light_gain = 1.0f;         // --light-gain: light multiplier on the sun and the ambient
    float bloom = 1;                 // --bloom [N] / --no-bloom: glow around bright things
    float ao = 1;                    // --ao [N] / --no-ao: ambient occlusion (up to 1)
    float haze = 1;                  // --haze [N] / --no-haze: distance haze that hides the draw distance
    float saturation = 0.8f;         // --saturation: colour saturation (1 = original)
    float gamma = 1;                 // --gamma: 1 = original, higher = brighter
    bool sky_procedural = true;      // --sky original|procedural: drawn sky instead of the original picture
    bool hdr = true;                 // --no-hdr: the original's clipped lighting
    bool rim = true;                 // --no-rim: no rim light on dinosaurs and people
    bool relief = true;              // --no-relief: flat textures, as in the original
    bool sharpen = true;             // --no-sharpen: no sharpening of the 3D image
    bool modern_torch = true;        // --torch original: the spotlight exactly as the Real3D draws it
    bool shafts = true;              // --no-shafts: no light shafts from the sun
    bool soft = true;                // --no-soft: hard edges where translucent effects meet geometry
    bool lightning = true;           // --no-lightning: no lightning in stormy fog
    int rain = 2;                    // --rain off|light|heavy: rain where the game has its own
    int weather = -1;                // --weather NAME: this weather in any game (Weather; -1 = none)
    bool shadows = true;             // --no-shadows: no sun shadows
    int ray_tracing = 2;             // --rt off|shadows|ao (--no-rt): traced sun shadows (1) and ambient occlusion (2)
    bool sun = true;                 // --no-sun: no sun in the procedural sky
    bool show_crosshair = true;      // menu option; the C key toggles it
    bool custom_crosshair = true;    // menu option: crosshair.png pictures (ui/crosshair_file.h) instead of the rings
    int crosshair_hide = 0;          // --crosshair-hide N: hide a crosshair after N seconds unused (0 = never)
    int subtitles = 0;               // --subtitles off|en|jp: 0, 1, 2
    // Sound
    float volume = 5.0f;             // --volume: output gain
    int speakers = 0;                // --speakers stereo|surround|headphones: 0, 1, 2 (AudioStream::Speakers)
    int rear_boost = 0;              // --rear-boost: dB on the rear pair (-6, -3, 0, 3, 6, 9, 12)
    int weather_sound = 2;           // --weather-sound off|low|medium|high: rain and thunder
    bool cubic_samples = true;       // --linear-samples: the SCSP's own linear interpolation instead of cubic
    std::string wav_path;            // --wav: record the sound output
    bool several_mice = true;        // a second mouse is player 2's gun (--one-mouse: all move player 1's)
    bool free_play = false;          // --free-play: 1 / 2 insert the credit themselves (no coin key needed)
    // Debugging and tests
    uint64_t shot_frame = 0;         // --shot N: screenshot of frame N, then exit
    std::string replay;              // --replay: a debug snapshot (or a folder of them) instead of the game
    std::vector<std::string> auto_scripts;   // --auto coin@F,start@F (each occurrence)
    bool fixed_aim = false;          // --aim X,Y: fixed raw gun ADC values
    int aim_x = 128, aim_y = 128;
    std::string diff;                // --diff FN@FRAME
    std::vector<std::string> traces; // --trace A,B,... (each occurrence)
};

// Handler for options the table does not know (the survival mode's live in
// survival.cpp): returns how many arguments it consumed, 0 if the option is
// not its. `next` is the following argument or nullptr.
using ExtraOption = std::function<int(Options&, const std::string& name, const char* next)>;

// Fills `o` from the command line. Returns true to run the game; otherwise
// (--help, an unknown option or a bad value) the usage or the error has been
// printed and exit_code says how to end
bool parse_options(Options& o, int argc, char** argv, const ExtraOption& extra, int& exit_code);

// Whether an argument asks for a test run, which leaves the menu out so tests
// always start from the same defaults: the options marked so in the table,
// plus the names in `more`
bool options_disable_menu(int argc, char** argv, std::initializer_list<const char*> more = {});

void print_usage(FILE* out);
