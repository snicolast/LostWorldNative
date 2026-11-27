// The Lost World as NicoEngine's Model 3 adapter sees it: what its scene
// builder has that another game's has not, and the other way round. The
// engine's model3/ code calls each name (its model3/games/sr2/game_scene.h
// says what each is for, until the API's own header does). Compiled by the
// engine only (docs/NICOENGINE.md), never by the game's build.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nico_frame.h"
#include "r3d_scene.h"
#include "sky_pack.h"
#include "texture_pack.h"

#include "view_backs.h"

namespace game {

constexpr const char* NAME = "The Lost World";
constexpr const char* ID = "lw";
constexpr const char* REPO = "LostWorldNative";
constexpr const char* EXE = "LostWorld.exe";

// This game's builder does not keep the polygons its back-face test leaves
// out: the adapter finds them itself, with a second build of the frame
// (the engine's model3/view_backs.h)
constexpr bool KEEPS_BACKS = false;

// Whether its world viewport hangs from a node whose matrix is the camera
// (the engine's model3/game_space.h): not known
constexpr bool CAMERA_ROOT = false;
inline void kept_backs(const R3DScene&, const std::vector<std::vector<R3DInstance>>&, ViewBacks&) {}

// What the builder marks that only this game has (scenery still dissolving
// into the distance) is its own renderer's to draw: the engine's has no such
// thing yet. Its rain is not the builder's at all
inline uint32_t traits(uint32_t) { return 0; }

// The builder as the adapter wants it: the frame as the game's own program
// has it. The black bars of the game's cinematics are taken out, as there:
// they are polygons as wide as the game's screen and no wider, drawn over
// the scene (the builder folds each into a point: every model keeps its
// place in the lists, and its count of vertices). And the builder marks the
// HUD's 3D by the side of the screen it is at (the ammunition): `side`
inline void setup_builder(R3DSceneBuilder& b) {
    b.set_hud_edges(true);
    b.set_remove_letterbox(true);
}

// The side of a picture wider than the game's that a vertex goes out with,
// in a viewport that takes the whole screen: -1 the left, 1 the right, 0 it
// stays where it is. The builder's marks (its r3d_anchor_hud), which the
// game's own renderer moves the same way
inline int side(const R3DVertex& x) { return (x.flags & R3D_HUD_LEFT) ? -1 : (x.flags & R3D_HUD_RIGHT) ? 1 : 0; }

// The common rule is this game's
inline int follows(const R3DViewport&) { return 0; }

// Nothing the game draws is known to be a shadow's patch
inline bool drawn_shadow(const R3DVertex&) { return false; }

// The engine renderer's extras (its render/extras.h) were made for another game and
// never tried on this one: off until a player turns them on
constexpr bool EXTRAS = false;

// (no viewport's models are moved once their faces are culled)
inline float set_down(const R3DScene&, const R3DViewport&, float[3]) { return 0; }

// A picture wider than the game's: nothing is made wider (the engine's
// model3/games/sr2/game_scene.h has what these are)
inline void set_wide(R3DSceneBuilder&, float) {}

// What of the screen goes out to its sides then: the top rows, each half to
// its side (the players' scores), while they look like a HUD: nearly nothing
// in their middle. Not when they are part of a picture, or a boss's name
// plate is up between the scores. The rule of the game's own program
// (src/runtime/frame_prep.cpp, hud_band), which looks at the picture before
// a pack's letters and pictures leave it, as `over` is here
constexpr int HUD_ROWS = 32, HUD_SPLIT = 248;   // (src/video/shaders/compose.frag: HUD_TOP_ROWS, and the screen's middle)
inline void bands(const R3DScene&, const nico::Picture* over, std::vector<nico::Band>& out) {
    out.clear();
    constexpr int W = 496, H = 384;
    if (!over || over->w != uint32_t(W) || over->h != uint32_t(H) || over->pixels.size() < size_t(W) * H) return;
    int middle = 0;
    for (int y = 0; y < 36; y++)
        for (int x = 160; x < 336; x++) middle += (over->pixels[size_t(y) * W + x] >> 24) != 0;
    if (middle * 20 < 36 * 176) out.push_back({0.0f, float(HUD_ROWS), float(HUD_SPLIT), float(HUD_SPLIT)});
}

// The game's skies drawn anew, as its own program has them (its "procedural
// sky"): its finder of skies is told to (sky_pack.h, sky_procedural.h). The
// sun its own renderer puts in them and its night sky are not here
constexpr bool DRAWN_SKIES = true;
inline void setup_skies(SkyPack& pack, bool on) { pack.set_procedural(on); }

// A frame the game draws that is better not shown, told by its sharp letters: none
inline bool nothing_to_show(const R3DScene&, const std::vector<nico::Letter>&) { return false; }

// The game's texture pack in `dir`, for a renderer that keeps `slots` pictures
inline void setup_pack(TexturePack& pack, const std::string& dir, uint32_t slots) {
    pack.init(dir, "");
    pack.set_max_slots(slots);
}

// The pack as a frame is made with it: its pictures in the place of the
// game's textures (`textures`). It has no shadows painted into its textures.
// nullptr: nothing of it
inline TexturePack* pack_for(TexturePack& pack, bool textures, bool) { return textures && pack.active() ? &pack : nullptr; }

}   // namespace game
