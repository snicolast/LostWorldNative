// The game's ROM at start (app.cpp): from lostwsga.zip, or from the data
// folder build.bat extracts
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Options;

// Loads the ROM from the first of: --rom or LW_ROM_ZIP (lostwsga.zip read
// directly), --data, data\ in the current folder or next to the exe (what
// build.bat extracts), lostwsga.zip next to the exe or in the current folder.
// If none is there, says where it looked and how to give the path
// (a message box too, unless headless). zip_used: the zip it came from, empty
// when it came from a data folder
bool load_game_rom(const Options& opt, std::vector<uint8_t>& sound_program, std::vector<uint8_t>& sound_samples,
                   std::string& zip_used);
