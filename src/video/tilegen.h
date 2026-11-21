// Model 3 2D tile generator: 4 layers (A, A', B, B') of 64x64 tiles of 8x8
// pixels, drawn below or above the 3D image. See docs/real3d_spec.md 7.6:
//   0x000000  patterns (4 bpp: 32 bytes per tile, 8 bpp: 64 bytes)
//   0x0F6000  line scroll tables, 0x0F7000 A/A' and B/B' selection mask
//   0x0F8000  name tables, 0x2000 bytes per layer; each 32-bit
//             (little-endian) word holds two 16-bit entries, left in the high half
//   0x100000  palette: 32768 colours, R in the low bits, bit 15 = transparent
//   0x180000  registers
#pragma once
#include <cstdint>
#include <vector>

class SkyPack;
class TextHD;

constexpr int SCREEN_W = 496, SCREEN_H = 384;
// The layers below the 3D image (the sky) are also drawn beyond both sides of
// the screen for widescreen: the tilemaps wrap every 512 pixels, so the extra
// columns continue the background as the game scrolls it. 440 on each side
// covers a 32:9 view (384 * 32 / 9 = 1365 pixels wide).
constexpr int BACK_MARGIN = 440, BACK_W = SCREEN_W + 2 * BACK_MARGIN;
constexpr uint32_t TILEGEN_BASE = 0xF1000000, TILEGEN_SIZE = 0x190000;
constexpr uint32_t TILEGEN_PAGES = TILEGEN_SIZE >> 16;   // in 64 KB pages
static_assert(TILEGEN_SIZE % 0x10000 == 0);

class TileGen {
public:
    // Images compose() fills from the frame's layers (BGRA pixels, 0xAARRGGBB,
    // alpha 0 = empty). back and front are required; the rest are optional
    // (nullptr = not wanted) and cost time only when asked for.
    struct Output {
        uint32_t* back = nullptr;    // BACK_W x SCREEN_H: the layers below the 3D image
        uint32_t* front = nullptr;   // SCREEN_W x SCREEN_H: the layers above it
        // The foreground without the brightness offsets the game uses for
        // fades, so graphics can be recognised at any brightness; front_raw_top
        // the same with only the top layer pair (A/A'), where texts and the
        // route map are, without the pictures behind them
        uint32_t* front_raw = nullptr;
        uint32_t* front_raw_top = nullptr;
        // What `front` shows without the top layer pair (with the fade
        // offsets): the picture a replaced title leaves visible behind it
        uint32_t* front_under = nullptr;
        // The same for the layers below the 3D image, inside the 496x384
        // screen (title and tutorial screens); back_under what `back` shows
        // there without the top layer pair, with the fade offsets: what a
        // replaced text leaves visible
        uint32_t* back_raw = nullptr;
        uint32_t* back_raw_top = nullptr;
        uint32_t* back_under = nullptr;
        // With text: cells that are known text, described as TextHD::CELLS_X x
        // CELLS_Y x CELL_TEXELS texels and left out of `front`
        uint32_t* text_cells = nullptr;
        // With sky: which replacement picture and point each back pixel shows
        // (SKY_COORDS_W x SKY_COORDS_H; layout below)
        uint32_t* sky_coords = nullptr;
        // SCREEN_H x 3: on each row, the layer that shows most of the back
        // image on screen (-1: none) and where its picture is at screen
        // column 0 (x, y in its 512x512 picture), to follow its scroll
        int32_t* back_rows = nullptr;
    };
    // vram: copy of 0xF1000000..0xF118FFFF as written by the game.
    // text (optional): the high-resolution text to recognise (needs out.text_cells).
    // sky (optional): replacements for the layers below the 3D image; their
    // pixels in `back` come from the replacement (needs out.sky_coords).
    // Reentrant: the only state kept between calls is a per-thread palette cache.
    static void compose(const uint8_t* vram, const Output& out, const TextHD* text = nullptr, SkyPack* sky = nullptr);
    // Sky coordinates: per back pixel B = procedural sky << 7 | upper procedural sky << 6 | layer pair << 5 |
    // picture slot << 2 | y bit 8 << 1 | x bit 8,
    // R = x, G = y (low bits, in the 512x512 layer), A = 255 where a replacement
    // shows; the first extra row starts with the fade offset of each layer pair
    // (R, G, B = offset / 2 + 128), and from texel 8 on holds the colours of
    // each procedural sky (SkyPack::Picture::bands, 64 texels per slot). The
    // last two are not written here: the renderer puts there how the back
    // layers move on each row between game frames (x, then y; sky_motion.h)
    static constexpr int SKY_COORDS_W = BACK_W, SKY_COORDS_H = SCREEN_H + 3;
    // The whole 512x512 picture of a layer, without scroll or fade offsets
    static void layer_plane(const uint8_t* vram, int layer, bool four_bpp, std::vector<uint32_t>& out);
    // Hash of the data a layer is drawn from (names, patterns, palettes)
    static uint64_t layer_key(const uint8_t* vram, int layer, bool four_bpp);
    // Brightness offset of the foreground layer pair (A/A'), -255..255 per channel
    static void front_offset(const uint8_t* vram, int out[3]);
};
