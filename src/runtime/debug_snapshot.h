// Debugging aids for one scene (app.cpp)
#pragma once
#include <cstdint>
#include <vector>

struct R3DFrameState;
struct R3DScene;

// Debug snapshot (F9): the Real3D memory of the frame on screen, texture RAM
// and 2D layer memory in one file, to look at a scene again with --replay.
// save_snapshot writes the frame given with the texture RAM as it is now;
// load_snapshot makes the file's frame the latest one (false: no such file,
// or not a snapshot)
void save_snapshot(const char* path, const R3DFrameState& state, const std::vector<uint8_t>& vram);
bool load_snapshot(const char* path);

// For --shot and F9: every triangle with its screen bounding box (native
// pixels), plus the 2D layers' alpha along two columns
void dump_scene(const R3DScene& scene, const uint32_t* back, const uint32_t* front, const char* path);
