// The game's ROM at start (rom_load.h)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "options.h"
#include "rom_load.h"
#include "rom_zip.h"
#include "runtime.h"
#include "../common/paths.h"

static bool file_exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string full_path(const std::string& path) {
    char full[MAX_PATH];
    DWORD n = GetFullPathNameA(path.c_str(), MAX_PATH, full, nullptr);
    return n && n < MAX_PATH ? std::string(full, n) : path;
}

bool load_game_rom(const Options& opt, std::vector<uint8_t>& sound_program, std::vector<uint8_t>& sound_samples,
                   std::string& zip_used) {
    std::string zip = opt.rom_zip;
    if (zip.empty() && getenv("LW_ROM_ZIP")) zip = getenv("LW_ROM_ZIP");
    std::vector<std::string> looked;   // full paths, for the error
    auto missing = [&](const std::string& path) {
        std::string f = full_path(path);
        if (std::find(looked.begin(), looked.end(), f) == looked.end()) looked.push_back(f);
    };
    if (zip.empty()) {
        std::vector<std::string> dirs = {opt.data_dir};
        if (opt.data_dir == "data") dirs.push_back(exe_folder() + "data");   // --data not given
        for (const std::string& d : dirs) {
            if (file_exists(d + "/crom.bin")) {
                g_data_dir = d;
                if (!load_rom("crom.bin", g_crom, true)) return false;
                load_rom("banked_crom.bin", g_banked, false);
                load_rom("vrom.bin", g_vrom, false);
                load_rom("sound_program.bin", sound_program, false);
                load_rom("sound_samples.bin", sound_samples, false);
                if (g_banked.empty() || g_vrom.empty() || sound_program.empty() || sound_samples.empty()) {
                    // An older romtool.py stopped at the first missing chip and
                    // left crom.bin without the rest: say so, not a black screen
                    std::string msg =
                        "The ROM in " + full_path(d) + " is incomplete: it has crom.bin but not\n"
                        "banked_crom.bin, vrom.bin, sound_program.bin and sound_samples.bin.\n\n"
                        "Your lostwsga.zip probably lacks some chips. Get a complete lostwsga.zip,\n"
                        "put it next to build.bat and run build.bat again.";
                    fprintf(stderr, "%s\n", msg.c_str());
                    if (!opt.headless) MessageBoxA(nullptr, msg.c_str(), "Lost World Native", MB_OK | MB_ICONERROR);
                    return false;
                }
                printf("ROM: %s\n", d.c_str());
                return true;
            }
            missing(d + "\\crom.bin");
        }
        if (opt.data_dir == "data")
            for (const std::string& z : {exe_folder() + "lostwsga.zip", std::string("lostwsga.zip")}) {
                if (file_exists(z)) {
                    zip = z;
                    break;
                }
                missing(z);
            }
    }
    std::string error;
    if (!zip.empty()) {
        RomSet rom;
        if (load_rom_zip(zip, rom, error)) {
            g_crom = std::move(rom.crom);
            g_banked = std::move(rom.banked_crom);
            g_vrom = std::move(rom.vrom);
            sound_program = std::move(rom.sound_program);
            sound_samples = std::move(rom.sound_samples);
            zip_used = zip;
            printf("ROM: %s\n", zip.c_str());
            return true;
        }
        error = "Cannot load the ROM: " + error + ".\n";
    } else {
        error = "Cannot find The Lost World ROM (lostwsga.zip). Looked in:\n";
        for (const std::string& l : looked) error += "  " + l + "\n";
    }
    error += "\nPut lostwsga.zip next to LostWorld.exe, or give its path with\n"
             "  LostWorld.exe --rom D:\\roms\\lostwsga.zip\n"
             "or in the LW_ROM_ZIP environment variable.";
    fprintf(stderr, "%s\n", error.c_str());
    if (!opt.headless) MessageBoxA(nullptr, error.c_str(), "Lost World Native", MB_OK | MB_ICONERROR);
    return false;
}
