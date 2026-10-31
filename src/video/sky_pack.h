// Replacements for the 2D layers below the 3D image: the sky and the other
// backdrops behind the scenery.
//
// Each of those layers is a 512x512 picture that wraps around as the game
// scrolls it. It is identified by a hash of that picture (without the
// brightness offsets of fades), so the same sky is recognised in every stage.
// F12 in the game (and --dump-textures, for every new one) saves the
// backdrops on screen as out/sky/<hash>.png; a file with the same name in
// textures/sky/ replaces it, at any square resolution (2048x2048 for 4x
// detail) and with its own alpha. The picture is drawn at full resolution
// following the game's scroll; a 512x512 reduction of it stands in for the
// layer everywhere else (the distance haze, the horizon colour).
//
// A <hash>.txt next to a replacement can hold "keep X Y W H" lines: that box
// of the 512x512 picture is left out of the hash and the game's own pixels
// show there, for texts that change on a screen that is otherwise the same
// (CREDITS N and INSERT COIN under the chosen difficulty). The PNG is then
// named after the hash of the picture with that box cleared (transparent).
//
// With the procedural sky on, the stages' skies (any backdrop below the 3D
// image that wraps around as a panorama) are drawn instead with soft drifting
// clouds, at any resolution (shaders/sky_procedural.glsl), in the colours of
// the original; the trees on the horizon still come from the original picture.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "image_io.h"

class SkyPack {
public:
    // Slots of the 2D replacement pictures (compose.frag overlays[]) used for
    // backdrops, counting down from the last one; Pack2D fills them from 1 up
    // (replacement files from the first one up; procedural skies from the
    // last one down, holding the original for the trees on the horizon)
    static constexpr uint32_t FIRST_SLOT = 56, LAST_SLOT = 63;
    static constexpr int SIZE = 512;
    // Colours of a procedural sky, from its original: per band of 16 rows the
    // clear sky and the lit clouds (BGRA)
    static constexpr int BANDS = 32;

    // dir: folder with replacement PNGs; dump_dir: where every new backdrop
    // is written (empty = only on capture())
    void init(const std::string& dir, const std::string& dump_dir);
    bool active() const { return !available_.empty() || !dump_dir_.empty() || capture_ || procedural_on_; }
    // Procedural sky instead of the stages' own (or its replacement file)
    void set_procedural(bool on) { procedural_on_ = on; }
    // Saves the backdrops of the next frame composed to out/sky
    void capture() { capture_ = true; }

    struct Picture {
        uint32_t slot;
        std::vector<uint32_t> low;   // SIZE x SIZE BGRA (alpha 0 or 255), for the layer's own pixels
        bool upper = false;          // procedural sky above the one with the horizon
        bool procedural = false;
        uint32_t bands[BANDS * 2] = {};   // procedural: sky and cloud colour per band
        uint64_t hash = 0;                // replacement file it was loaded from
        uint64_t used = 0;                // frame it was last shown in (frame_done counts)
        int keep_x = 0, keep_y = 0, keep_w = 0, keep_h = 0;   // box where the game's own pixels show
        bool kept(int px, int py) const {
            return px >= keep_x && px < keep_x + keep_w && py >= keep_y && py < keep_y + keep_h;
        }
    };
    // Replacement for a layer below the 3D image (TileGen::compose), nullptr = none
    const Picture* find(const uint8_t* vram, int layer, bool four_bpp);
    // End of a composed frame (ends a capture)
    void frame_done();

    // Pictures loaded since the last call, to be uploaded by the renderer
    std::vector<RgbaImage> take_new_images();

private:
    const Picture* load(uint64_t hash);
    const Picture* procedural(uint64_t hash, bool upper);
    uint64_t with_keep(uint64_t hash, const uint8_t* vram, int layer, bool four_bpp);

    struct Keep { int x, y, w, h; };

    std::string dir_, dump_dir_, capture_dir_;
    bool capture_ = false, procedural_on_ = false;
    std::unordered_map<uint64_t, Picture> procedural_;   // sky hash -> procedural picture
    std::unordered_map<uint64_t, bool> panorama_;        // picture hash -> wraps around as a sky
    std::unordered_map<uint64_t, uint64_t> hashes_;      // layer contents key -> picture hash
    std::unordered_set<uint64_t> available_;             // hashes with a replacement file
    std::unordered_set<uint64_t> dumped_;
    std::vector<Keep> keeps_;                            // the different "keep" boxes of the .txt files
    std::unordered_map<uint64_t, int> keep_of_;          // replacement hash -> its box in keeps_
    std::unordered_map<uint64_t, uint64_t> kept_;        // picture hash -> replacement with a box (0 = none)
    std::unordered_map<uint64_t, int> loaded_;           // hash -> picture
    std::vector<Picture> pictures_;
    std::vector<RgbaImage> pending_;
    std::vector<uint32_t> plane_;
    uint64_t frame_ = 1;   // frames composed, to give the slot of the picture unused longest to a new one
    const Picture* last_lower_ = nullptr;   // procedural sky with the horizon seen last
};
