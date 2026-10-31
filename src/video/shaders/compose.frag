#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// Final composition: background 2D layers, 3D image and foreground 2D layers,
// with black bars to preserve the Model 3 aspect ratio, plus an optional
// high-visibility crosshair drawn at the real mouse position.
// The 2D layers are upscaled with xBR (Hyllian's xBR-lv2), which rebuilds
// smooth edges from the pixel art instead of blurring or blocking it.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D scene3d;
layout(set = 0, binding = 1) uniform sampler2D layer2d_front;
layout(set = 0, binding = 2) uniform sampler2D layer2d_back;
layout(set = 0, binding = 3) uniform sampler2D overlays[64];   // 2D replacement pictures (pack2d.h)
#define ATMOSPHERE_BINDING 4
#include "atmosphere.glsl"
layout(set = 0, binding = 5) uniform sampler2D text_atlas;   // high-resolution text (text_hd.h)
layout(set = 0, binding = 6) uniform sampler2D text_cells;
layout(set = 0, binding = 8) uniform sampler2D bloom;   // blurred glow of luminous surfaces (bloom.frag)
layout(set = 0, binding = 7) uniform sampler2D scene3d_alpha;   // alpha of the 3D image, in red
layout(set = 0, binding = 9) uniform sampler2D sky_coords;   // backdrop replacements (sky_pack.h, tilegen.h)
layout(set = 0, binding = 10) uniform sampler2D light_shafts;   // rays.frag
layout(set = 0, binding = 12) uniform sampler2D diffusion;   // the picture behind the HUD, blurred (bloom.frag)
layout(set = 0, binding = 11) uniform sampler2D exposure_log;   // 1x1: average log brightness of the 3D image, eased
layout(set = 0, binding = 13) uniform sampler2D crosshair_pictures[2];   // crosshair.png, crosshair_p2.png

layout(push_constant) uniform Compose {
    vec2 scale;       // image size relative to the window
    vec2 offset;      // top-left corner
    vec2 crosshair;   // position in window pixels; negative = hidden
    vec2 window;      // window size in pixels
    vec2 layer2d;     // x offset and width of the 4:3 2D area inside the 3D view
    vec2 filter2d;    // x: 1 = xBR, 0 = plain pixels; y: window pixels per 2D pixel
    vec4 hud;         // x: shift of the top HUD band to the screen edges (2D pixels); y: 1 = HDR lighting;
                      // z: brightness offset of the foreground layers (fades), applied to the overlays;
                      // w: 1 = background layers continue beside the 4:3 area
    vec4 sky;         // x: seconds since launch (drift of the procedural clouds);
                      // y > 0: glow mode, for the bloom: only the bright part of the sky, times y;
                      // z: 1 = a sun in the procedural sky; w: 1 = mask mode, for the light shafts:
                      // the visible sky around the sun; 2 = measure mode: average log brightness of the 3D image;
                      // 3 = scene mode: the picture behind the HUD (backdrop and 3D), for the diffusion
    vec4 effects;     // x: bloom strength (0 = off); y: colour saturation (1 = original);
                      // z: gamma (1 = original, higher = brighter mid-tones); w: 1 = light shafts
    vec4 motion;      // x: how far the 3D has gone from the previous game frame, in vblanks:
                      // the back layers go on by their velocity (in sky_coords) times this;
                      // y: which players have their own crosshair picture (1 = player 1, 2 = player 2)
    vec4 crosshair2;  // xy: player 2's crosshair, like crosshair; z: sharpening (0 = off); w unused
} pc;

// Rows of the 2D screen holding the score (rows 9-30); in widescreen its left
// half moves to the left edge and its right half to the right edge. The points
// won ("200") appear from row 33 down and stay where the game puts them.
const float HUD_TOP_ROWS = 32.0;

const ivec2 SIZE_2D = ivec2(496, 384);
const int BACK_MARGIN = 200;   // tilegen.h: the back layers extend beyond both sides
const ivec2 SIZE_BACK = ivec2(496 + 2 * BACK_MARGIN, 384);

// Crosshair: ring and cross with a dark outline so it reads on any background;
// player 1's yellow, player 2's cyan
vec4 crosshair(vec2 frag, vec2 at, vec3 tint) {
    vec2 d = frag - at;
    float size = max(10.0, pc.window.y / 60.0);
    float r = length(d);
    float ring = abs(r - size);
    float cross = min(abs(d.x), abs(d.y));
    bool in_cross = max(abs(d.x), abs(d.y)) < size * 1.6 && r > size * 0.35;
    float core = min(ring, in_cross ? cross : 1e9);
    if (core < 1.2) return vec4(tint, 1.0);
    if (core < 3.0) return vec4(0.0, 0.0, 0.0, 0.85);   // outline
    if (r < 1.8) return vec4(tint, 1.0);                // centre dot
    return vec4(0.0);
}

// A player's own crosshair picture, centred on the gun: its pixels count as
// pixels of a 1080-line screen, so it keeps its size at any resolution
vec4 crosshair_picture(vec2 frag, vec2 at, int player) {
    vec2 size = vec2(textureSize(crosshair_pictures[player], 0)) * (pc.window.y / 1080.0);
    vec2 t = (frag - at) / size + 0.5;
    if (any(lessThan(t, vec2(0.0))) || any(greaterThan(t, vec2(1.0)))) return vec4(0.0);
    return texture(crosshair_pictures[player], t);
}

// Player 1 or 2's crosshair: their own picture if there is one, else the ring
vec4 player_crosshair(vec2 frag, vec2 at, int player, vec3 tint) {
    if ((int(pc.motion.y) & (1 << player)) != 0) return crosshair_picture(frag, at, player);
    return crosshair(frag, at, tint);
}

// ---- xBR-lv2 ---------------------------------------------------------------

const vec4 Ao = vec4(1.0, -1.0, -1.0, 1.0);
const vec4 Bo = vec4(1.0, 1.0, -1.0, -1.0);
const vec4 Co = vec4(1.5, 0.5, -0.5, 0.5);
const vec4 Ax = vec4(1.0, -1.0, -1.0, 1.0);
const vec4 Bx = vec4(0.5, 2.0, -0.5, -2.0);
const vec4 Cx = vec4(1.0, 1.0, -0.5, 0.0);
const vec4 Ay = vec4(1.0, -1.0, -1.0, 1.0);
const vec4 By = vec4(2.0, 0.5, -2.0, -0.5);
const vec4 Cy = vec4(2.0, 0.0, -1.0, 0.5);
const vec4 Ci = vec4(0.25);
const float EQ_THRESHOLD = 15.0;
const float LV2_COEFFICIENT = 2.0;

// Edge-detection value of a texel: luma, with transparency far from any colour
float val(vec4 c) {
    return c.a < 0.5 ? -128.0 : dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)) * 255.0;
}
vec4 df(vec4 a, vec4 b) { return abs(a - b); }
bvec4 eq(vec4 a, vec4 b) { return lessThan(df(a, b), vec4(EQ_THRESHOLD)); }
bvec4 neq(vec4 a, vec4 b) { return not(eq(a, b)); }
bvec4 band(bvec4 a, bvec4 b) { return bvec4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w); }
bvec4 bor(bvec4 a, bvec4 b) { return bvec4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w); }
vec4 wdist(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) {
    return df(a, b) + df(a, c) + df(d, e) + df(d, f) + 4.0 * df(g, h);
}
float cdist(vec4 a, vec4 b) { return dot(abs(a - b), vec4(1.0)); }

vec4 texel(sampler2D layer, ivec2 size, ivec2 p) { return texelFetch(layer, clamp(p, ivec2(0), size - 1), 0); }

vec4 xbr(sampler2D layer, vec2 q, ivec2 size) {
    vec2 pos = q * vec2(size);
    ivec2 e0 = ivec2(floor(pos));
    vec2 fp = pos - vec2(e0);
    //    A1 B1 C1
    // A0 A  B  C  C4
    // D0 D  E  F  F4
    // G0 G  H  I  I4
    //    G5 H5 I5
    vec4 A1 = texel(layer, size, e0 + ivec2(-1, -2)), B1 = texel(layer, size, e0 + ivec2(0, -2)), C1 = texel(layer, size, e0 + ivec2(1, -2));
    vec4 A0 = texel(layer, size, e0 + ivec2(-2, -1)), A = texel(layer, size, e0 + ivec2(-1, -1)), B = texel(layer, size, e0 + ivec2(0, -1));
    vec4 C = texel(layer, size, e0 + ivec2(1, -1)), C4 = texel(layer, size, e0 + ivec2(2, -1));
    vec4 D0 = texel(layer, size, e0 + ivec2(-2, 0)), D = texel(layer, size, e0 + ivec2(-1, 0)), E = texel(layer, size, e0);
    vec4 F = texel(layer, size, e0 + ivec2(1, 0)), F4 = texel(layer, size, e0 + ivec2(2, 0));
    vec4 G0 = texel(layer, size, e0 + ivec2(-2, 1)), G = texel(layer, size, e0 + ivec2(-1, 1)), H = texel(layer, size, e0 + ivec2(0, 1));
    vec4 I = texel(layer, size, e0 + ivec2(1, 1)), I4 = texel(layer, size, e0 + ivec2(2, 1));
    vec4 G5 = texel(layer, size, e0 + ivec2(-1, 2)), H5 = texel(layer, size, e0 + ivec2(0, 2)), I5 = texel(layer, size, e0 + ivec2(1, 2));

    // The four corner rotations are evaluated at once in the vector lanes
    vec4 b = vec4(val(B), val(D), val(H), val(F));
    vec4 c = vec4(val(C), val(A), val(G), val(I));
    vec4 e = vec4(val(E));
    vec4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;
    vec4 i4 = vec4(val(I4), val(C1), val(A0), val(G5));
    vec4 i5 = vec4(val(I5), val(C4), val(A1), val(G0));
    vec4 h5 = vec4(val(H5), val(F4), val(B1), val(D0));
    vec4 f4 = h5.yzwx;

    // Straight-line inequations for 45, 30 and 60 degree edges
    vec4 fx = Ao * fp.y + Bo * fp.x;
    vec4 fx_left = Ax * fp.y + Bx * fp.x;
    vec4 fx_up = Ay * fp.y + By * fp.x;

    bvec4 restriction_lv1 = band(neq(e, f), neq(e, h));
    bvec4 lv2_extra = bor(bor(band(neq(f, b), neq(f, c)), band(neq(h, d), neq(h, g))),
                          bor(band(eq(e, i), bor(band(neq(f, f4), neq(f, i4)), band(neq(h, h5), neq(h, i5)))),
                              bor(eq(e, g), eq(e, c))));
    restriction_lv1 = band(restriction_lv1, lv2_extra);
    bvec4 restriction_left = band(neq(e, g), neq(d, g));
    bvec4 restriction_up = band(neq(e, c), neq(b, c));

    // Edge smoothing about one window pixel wide
    vec4 delta = vec4(1.0 / max(pc.filter2d.y, 1.0));
    vec4 deltaL = vec4(0.5, 1.0, 0.5, 1.0) * delta;
    vec4 deltaU = deltaL.yxwz;
    vec4 fx45i = clamp((fx + delta - Co - Ci) / (2.0 * delta), 0.0, 1.0);
    vec4 fx45 = clamp((fx + delta - Co) / (2.0 * delta), 0.0, 1.0);
    vec4 fx30 = clamp((fx_left + deltaL - Cx) / (2.0 * deltaL), 0.0, 1.0);
    vec4 fx60 = clamp((fx_up + deltaU - Cy) / (2.0 * deltaU), 0.0, 1.0);

    vec4 wd1 = wdist(e, c, g, i, h5, f4, h, f);
    vec4 wd2 = wdist(h, d, i5, f, i4, b, e, i);
    bvec4 edri = band(lessThanEqual(wd1, wd2), restriction_lv1);
    bvec4 edr = band(lessThan(wd1, wd2), restriction_lv1);
    bvec4 edr_left = band(band(lessThanEqual(LV2_COEFFICIENT * df(f, g), df(h, c)), restriction_left), edr);
    bvec4 edr_up = band(band(greaterThanEqual(df(f, g), LV2_COEFFICIENT * df(h, c)), restriction_up), edr);

    fx45 = vec4(edr) * fx45;
    fx30 = vec4(edr_left) * fx30;
    fx60 = vec4(edr_up) * fx60;
    fx45i = vec4(edri) * fx45i;

    bvec4 px = lessThanEqual(df(e, f), df(e, h));
    vec4 maximos = max(max(fx30, fx60), max(fx45, fx45i));

    vec4 res1 = E;
    res1 = mix(res1, px.x ? F : H, maximos.x);
    res1 = mix(res1, px.z ? D : B, maximos.z);
    vec4 res2 = E;
    res2 = mix(res2, px.y ? B : F, maximos.y);
    res2 = mix(res2, px.w ? H : D, maximos.w);
    return cdist(E, res2) >= cdist(E, res1) ? res2 : res1;
}

vec4 layer(sampler2D tex, vec2 q, ivec2 size) {
    return pc.filter2d.x > 0.5 ? xbr(tex, q, size) : texture(tex, q);
}

// ---- Backdrop replacements ---------------------------------------------------
// The back layer pixels that show a replaced backdrop say which picture and
// which point of its 512x512 layer they show; the picture is sampled in
// between at full resolution, following the game's scroll.

const uint SKY_FIRST_SLOT = 56u;        // SkyPack::FIRST_SLOT

#include "sky_procedural.glsl"

// Sun in the procedural sky (SUN), where the game's own light comes from:
// a small bright disc with a warm halo around it. p: position in the 3D image
vec3 sky_sun(vec3 c, vec2 p) {
    if (atm.sun.w <= 0.0) return c;
    vec2 ndc = p * 2.0 - 1.0;
    vec3 dir = normalize(vec3((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y, -1.0));
    float mu = dot(dir, atm.sky_sun.xyz);
    if (mu <= 0.0) return c;
    // Clouds (grey, where the clear sky is blue) hide the disc and dim the halo
    float clear = smoothstep(0.05, 0.22, c.b - c.r);
    float disc = smoothstep(0.99990, 0.99994, mu) * clear;   // about half a degree across
    float halo = (0.55 * pow(mu, 900.0) + 0.18 * pow(mu, 60.0)) * mix(0.45, 1.0, clear);
    return min(c + disc * vec3(1.0, 0.97, 0.88) + halo * vec3(1.0, 0.86, 0.62) * atm.sun.w, vec3(1.0));
}

// Night sky (SKY: PROCEDURAL) where the game leaves a black backdrop behind
// a dark world (the T-Rex at the trailer): the procedural clouds, faintly
// moonlit on a deep blue, laid around the camera by the world's own
// directions since there is no layer scroll to follow. p: position in the 3D image
vec3 sky_night(vec2 p) {
    vec2 ndc = p * 2.0 - 1.0;
    vec3 dir = normalize(vec3((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y, -1.0));
    vec3 d = normalize(vec3(dot(atm.axes[0].xyz, dir), dot(atm.axes[1].xyz, dir), dot(atm.axes[2].xyz, dir)));
    float el = asin(clamp(d.y, -1.0, 1.0));
    float t = clamp((el - 0.03) / 1.5, 0.0, 1.0);   // as in sky_procedural
    vec2 at = vec2(fract(atan(d.z, d.x) / 6.2831853) * 512.0, 500.0 - 1012.0 * t);
    // The day sky's clouds in dark colours: made at a third of the
    // brightness, from colours three times as bright
    const float DIM = 0.33;
    vec3 sky = mix(vec3(0.050, 0.070, 0.130), vec3(0.012, 0.020, 0.050), smoothstep(0.0, 0.6, t)) / DIM;
    vec3 lit = vec3(0.17, 0.19, 0.25) / DIM;
    return min(sky_procedural(at, pc.sky.x * 0.002, sky, lit) * DIM, vec3(1.0));
}

// Which picture and point each back layer pixel shows: sampled in between.
// dx, dy: screen derivatives of qb * SIZE_BACK / 512 (explicit, as the
// position wraps every 512 pixels). True where the sky is procedural.
bool sky_layer(vec2 qb, vec2 dx, vec2 dy, inout vec4 back) {
    vec2 pos = qb * vec2(SIZE_BACK);
    ivec2 t = clamp(ivec2(floor(pos)), ivec2(0), SIZE_BACK - 1);
    vec4 v = texelFetch(sky_coords, t, 0);
    if (v.a < 0.5) return false;
    uvec4 b = uvec4(round(v * 255.0));
    vec2 at = vec2(b.r | ((b.b & 1u) << 8), b.g | (((b.b >> 1) & 1u) << 8)) + clamp(pos - vec2(t), 0.0, 1.0);
    uint slot = SKY_FIRST_SLOT + ((b.b >> 2) & 7u);
    // Procedural: its picture is the original, for the trees on the horizon
    bool procedural = (b.b & 128u) != 0u;
    vec4 s;
    if (!procedural) {
        s = textureGrad(overlays[nonuniformEXT(slot)], fract(at / 512.0), dx, dy);
    } else {
        // Colours of this sky's band of rows, taken from its original (last row)
        float y = mod(at.y, 512.0);
        float fy = clamp(y / 16.0 - 0.5, 0.0, 31.0);
        int band = int(fy), table = 8 + int((b.b >> 2) & 7u) * 64;
        int next = min(band + 1, 31);
        vec3 sky = mix(texelFetch(sky_coords, ivec2(table + band * 2, SIZE_BACK.y), 0).rgb,
                       texelFetch(sky_coords, ivec2(table + next * 2, SIZE_BACK.y), 0).rgb, fy - float(band));
        vec3 lit = mix(texelFetch(sky_coords, ivec2(table + band * 2 + 1, SIZE_BACK.y), 0).rgb,
                       texelFetch(sky_coords, ivec2(table + next * 2 + 1, SIZE_BACK.y), 0).rgb, fy - float(band));
        if ((b.b & 64u) != 0u) {   // the sky above
            s = vec4(sky_procedural(vec2(at.x, y - 512.0), pc.sky.x * 0.005, sky, lit), 1.0);
        } else {
            // Trees on the horizon: the dark parts of the original's bottom rows
            vec4 o = textureGrad(overlays[nonuniformEXT(slot)], fract(at / 512.0), dx, dy);
            float tree = y > 455.0 ? 1.0 - smoothstep(0.10, 0.20, dot(o.rgb, vec3(0.2126, 0.7152, 0.0722))) : 0.0;
            s = vec4(mix(sky_procedural(vec2(at.x, y), pc.sky.x * 0.005, sky, lit), o.rgb * 0.9, tree), 1.0);
        }
    }
    // Fade offset of the layer pair, in the last row
    vec3 offset = (texelFetch(sky_coords, ivec2((b.b >> 5) & 1u, SIZE_BACK.y), 0).rgb * 255.0 - 128.0) * (2.0 / 255.0);
    back = vec4(clamp(s.rgb + offset, 0.0, 1.0) * s.a, s.a);
    return procedural;
}

// ---- High-resolution text ----------------------------------------------------
// Cells that hold known letters were left out of the foreground layer; the
// letter is drawn here from the vector-font atlas with the game's colours.

const int TEXT_CELL_TEXELS = 4;
const int OVERLAY_ROW = 48;          // after the cells: the replacement pictures on screen

// The 2D replacement pictures (pack2d.h) of the foreground or of the layers
// below the 3D image over `under` (premultiplied), at `pos` in 2D pixels.
// Listed in the last row of the text cells (text_hd.h): rectangle in cells,
// then slot in R and 1 in G for the layers below the 3D image.
vec4 replacement_pictures(vec4 under, vec2 pos, bool below_3d) {
    int pictures = int(texelFetch(text_cells, ivec2(0, OVERLAY_ROW), 0).r * 255.0 + 0.5);
    for (int i = 0; i < pictures; i++) {
        vec4 info = texelFetch(text_cells, ivec2(2 + 2 * i, OVERLAY_ROW), 0);
        if ((info.g > 0.5) != below_3d) continue;
        vec4 rect = round(texelFetch(text_cells, ivec2(1 + 2 * i, OVERLAY_ROW), 0) * 255.0) * 8.0;
        vec2 r = (pos - rect.xy) / (rect.zw - rect.xy);
        if (all(greaterThanEqual(r, vec2(0.0))) && all(lessThan(r, vec2(1.0)))) {
            vec4 o = texture(overlays[nonuniformEXT(uint(info.r * 255.0 + 0.5))], r);
            under = under * (1.0 - o.a) + vec4(clamp(o.rgb + pc.hud.z, 0.0, 1.0) * o.a, o.a);
        }
    }
    return under;
}
const int ATLAS_COLS = 16;           // tools/make_text_atlas.py
const float ATLAS_SLOT = 80.0;       // (8 + 2 * PAD) * SCALE
const float ATLAS_PAD = 8.0;         // PAD * SCALE
const float ATLAS_CELL = 64.0;       // 8 * SCALE

vec4 front_layer(vec2 q) {
    vec4 f = layer(layer2d_front, q, SIZE_2D);
    vec2 pos = q * vec2(SIZE_2D) / 8.0;
    ivec2 cell = clamp(ivec2(pos), ivec2(0), SIZE_2D / 8 - 1);
    ivec2 t = ivec2(cell.x * TEXT_CELL_TEXELS, cell.y);
    vec4 info = texelFetch(text_cells, t, 0);
    if (info.a < 0.5) return f;
    int slot = int(info.r * 255.0 + 0.5) + 256 * int(info.g * 255.0 + 0.5);
    vec2 local = clamp(pos - vec2(cell), 0.0, 1.0);
    vec2 corner = vec2(slot % ATLAS_COLS, slot / ATLAS_COLS) * ATLAS_SLOT + ATLAS_PAD;
    // Explicit mip level: screen derivatives jump at the cell edges, where the
    // atlas coordinates change slot
    float lod = max(log2(ATLAS_CELL / 8.0 / max(pc.filter2d.y, 1e-3)), 0.0);
    vec4 g = textureLod(text_atlas, (corner + local * ATLAS_CELL) / vec2(textureSize(text_atlas, 0)), lod);
    vec4 top = texelFetch(text_cells, t + ivec2(1, 0), 0);
    vec4 bottom = texelFetch(text_cells, t + ivec2(2, 0), 0);
    vec4 edge = texelFetch(text_cells, t + ivec2(3, 0), 0);
    float ea = g.g * edge.a, fa = g.r * top.a;
    f = f * (1.0 - ea) + vec4(edge.rgb, 1.0) * ea;   // premultiplied, like the layer
    f = f * (1.0 - fa) + vec4(mix(top.rgb, bottom.rgb, g.b), 1.0) * fa;
    return f;
}

// HDR lighting: below the knee the image is left as the original drew it;
// above it the highlights roll off smoothly towards white instead of clipping
vec3 tone_curve(vec3 x) {
    const float knee = 0.8;
    vec3 over = max(x - knee, 0.0);
    return min(x, vec3(knee)) + (1.0 - knee) * (1.0 - exp(-over / (1.0 - knee)));
}

// How much the eased average brightness opens or closes the exposure: gently,
// within a narrow range, so dark scenes stay dark and bright ones bright
float exposure() {
    float average = exp(texelFetch(exposure_log, ivec2(0), 0).r);
    return clamp(pow(0.17 / max(average, 1e-3), 0.35), 0.9, 1.15);
}

void main() {
    if (pc.sky.w > 1.5 && pc.sky.w < 2.5) {   // measure mode (one pixel): log-average brightness of the 3D image
        float sum = 0.0, n = 0.0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                vec2 q = (vec2(x, y) + 0.5) / 16.0;
                vec4 s = texture(scene3d, q);
                if (texture(scene3d_alpha, q).r < 0.5) continue;
                sum += log(max(dot(s.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-3));
                n += 1.0;
            }
        color = vec4(n > 0.0 ? sum / n : log(0.17));
        return;
    }
    vec2 p = (uv - pc.offset) / pc.scale;
    vec3 c = vec3(0.0);
    if (p.x >= 0.0 && p.y >= 0.0 && p.x <= 1.0 && p.y <= 1.0) {
        vec4 s = texture(scene3d, p);
        // With DLAA the alpha comes from its own anti-aliasing pass, steady
        // from frame to frame like the colour
        s.a = texture(scene3d_alpha, p).r;
        // Sharpening (SHARPEN), contrast adaptive like AMD's CAS: detail comes
        // back after the anti-aliasing and the diffusion, flat areas and
        // strong edges are left alone (no halos)
        if (pc.crosshair2.z > 0.0 && s.a > 0.0) {
            vec2 t = 1.0 / vec2(textureSize(scene3d, 0));
            vec3 a = texture(scene3d, p - vec2(0.0, t.y)).rgb, b = texture(scene3d, p - vec2(t.x, 0.0)).rgb;
            vec3 d = texture(scene3d, p + vec2(t.x, 0.0)).rgb, e = texture(scene3d, p + vec2(0.0, t.y)).rgb;
            vec3 lo = min(s.rgb, min(min(a, b), min(d, e))), hi = max(s.rgb, max(max(a, b), max(d, e)));
            vec3 amp = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, vec3(1e-4)), 0.0, 1.0));
            vec3 w = -amp * mix(0.125, 0.2, pc.crosshair2.z);
            s.rgb = max((a + b + d + e) * w + s.rgb, 0.0) / (1.0 + 4.0 * w);
        }
        if (pc.hud.y > 0.5 && s.a > 0.0) {   // HDR lighting: exposure and tone curve (premultiplied)
            vec3 lit = s.rgb / max(s.a, 1e-4) * exposure();
            s.rgb = tone_curve(lit) * s.a;
        }
        vec2 q = vec2((p.x - pc.layer2d.x) / pc.layer2d.y, p.y);
        // The back layers (sky) were drawn wider than the screen; beyond that
        // and for the front layers the outermost column is repeated
        vec2 qb = vec2((q.x * float(SIZE_2D.x) + float(BACK_MARGIN)) / float(SIZE_BACK.x), q.y);
        // The back layers (sky) at their sub-pixel position, going on between
        // game frames like the 3D image in front of them (sky_motion.h): per
        // row and axis the offset from the game's scroll and the velocity
        {
            int row = clamp(int(q.y * 384.0), 0, 383);
            vec4 mx = texelFetch(sky_coords, ivec2(row, SIZE_BACK.y + 1), 0);
            vec4 my = texelFetch(sky_coords, ivec2(row, SIZE_BACK.y + 2), 0);
            if (mx.r > 0.0) qb.x += ((mx.g - 0.5) * 2.0 + (round(mx.b * 255.0) * 256.0 + round(mx.a * 255.0) - 32768.0) / 256.0 * pc.motion.x) / float(SIZE_BACK.x);
            if (my.r > 0.0) qb.y += ((my.g - 0.5) * 2.0 + (round(my.b * 255.0) * 256.0 + round(my.a * 255.0) - 32768.0) / 256.0 * pc.motion.x) / float(SIZE_BACK.y);
        }
        qb.x = clamp(qb.x, 0.5 / float(SIZE_BACK.x), 1.0 - 0.5 / float(SIZE_BACK.x));
        q.x = clamp(q.x, 0.5 / 496.0, 1.0 - 0.5 / 496.0);
        bool inside_43 = p.x >= pc.layer2d.x && p.x <= pc.layer2d.x + pc.layer2d.y;
        vec2 sky_dx = dFdx(qb * vec2(SIZE_BACK)) / 512.0, sky_dy = dFdy(qb * vec2(SIZE_BACK)) / 512.0;
        vec4 back = vec4(0.0);
        if (pc.hud.w > 0.5 || inside_43) {
            back = layer(layer2d_back, qb, SIZE_BACK);
            if (sky_layer(qb, sky_dx, sky_dy, back) && pc.sky.z > 0.5) back.rgb = sky_sun(back.rgb, p);
            else if (atm.night.x > 0.01 && atm.view.x != 0.0 && max(back.r, max(back.g, back.b)) < 0.002)
                back = vec4(sky_night(p) * atm.night.x, 1.0);
            // The weather's cloud grey over the sky (r3d_weather.h), lit by its lightning
            if (atm.weather.x > 0.0 && atm.view.x != 0.0 && atm.view.y != 0.0) {
                vec2 ndc = p * 2.0 - 1.0;
                vec3 dir = vec3((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y, -1.0);
                float l = dot(back.rgb, vec3(0.2126, 0.7152, 0.0722));
                vec3 grey = atm.weather.yzw * (0.4 * back.a + 0.6 * l) * mist_light(dir);
                back.rgb = mix(back.rgb, grey, atm.weather.x) + lightning_glow(dir) * 0.5 * atm.weather.x * back.a;
            }
            // The modern torch's beam in the air in front of the sky (rays as long as the scenery's)
            if (atm.view.x != 0.0 && atm.view.y != 0.0) {
                vec2 ndc = p * 2.0 - 1.0;
                vec2 slope = vec2((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y);
                back.rgb += torch_air(slope, atm.range.y) * back.a;
            }
        }
        // 2D replacement pictures of the layers below the 3D image (title and
        // tutorial screens), inside the 4:3 area
        if (inside_43) back = replacement_pictures(back, q * vec2(SIZE_2D), true);
        vec4 front;
        float shift = pc.hud.x;
        if (shift > 0.0 && q.y * 384.0 < HUD_TOP_ROWS) {
            float x = (p.x - pc.layer2d.x) / pc.layer2d.y * 496.0;
            float from_left = x + shift, from_right = x - shift;
            if (from_left < 248.0)
                front = front_layer(vec2(max(from_left, 0.5) / 496.0, q.y));
            else if (from_right >= 248.0)
                front = front_layer(vec2(min(from_right, 495.5) / 496.0, q.y));
            else
                front = vec4(0.0);
        } else {
            front = front_layer(q);
        }
        // The original's scroll fog veils the 2D layers behind the 3D image
        vec2 ndc_view = p * 2.0 - 1.0;
        vec3 dir_view = vec3((ndc_view.x + atm.view.z) / atm.view.x, (ndc_view.y + atm.view.w) / atm.view.y, -1.0);
        back.rgb = scroll_fog(back.rgb, p, atm.view.x != 0.0 && atm.view.y != 0.0 ? mist_light(dir_view) + lightning_glow(dir_view)
                                                                               : vec3(1.0));
        // The sky behind a game world gets the same exposure as the 3D image
        // in front of it, or fully fogged scenery (the same fog colour as the
        // fogged sky) would stand out as a slightly darker silhouette
        if (pc.hud.y > 0.5 && atm.view.x != 0.0 && back.a > 0.0)
            back.rgb = tone_curve(back.rgb / back.a * exposure()) * back.a;
        // Glow mode (r3d_renderer.cpp, bloom): the sky's bright part where the 3D leaves it visible
        if (pc.sky.w > 0.5 && pc.sky.w < 1.5) {   // light shaft mask: visible sky, more of it near the sun
            vec3 b = back.rgb * (1.0 - s.a);
            vec2 ndc = p * 2.0 - 1.0;
            vec3 dir = normalize(vec3((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y, -1.0));
            float mu = max(dot(dir, atm.sky_sun.xyz), 0.0);
            color = vec4(vec3(dot(b, vec3(0.2126, 0.7152, 0.0722)) * pow(mu, 20.0)), 0.0);
            return;
        }
        if (pc.sky.y > 0.0) {
            // A sky hidden by the scroll fog is just fog: it glows no more than
            // the fogged scenery in front of it (lightning would outline it)
            vec3 b = back.rgb * (1.0 - s.a) * (1.0 - scroll_fog_amount(p));
            float peak = max(b.r, max(b.g, b.b));
            color = vec4(b * smoothstep(0.55, 1.0, peak) * pc.sky.y, 0.0);
            return;
        }
        // Transparent 2D texels are all zero, so filtered 2D colours are
        // premultiplied by their alpha
        c = back.rgb;
        // The 3D image is premultiplied too: it starts cleared to zero, MSAA
        // averages partly covered edges with that zero, and the translucent
        // pass blends over it
        c = c * (1.0 - s.a) + s.rgb;
        if (pc.sky.w > 2.5) {   // scene mode
            color = vec4(c, 0.0);
            return;
        }
        // Diffusion (part of BLOOM): the picture mixed with a blurred copy of
        // itself, light spilling into the dark around it more than the other
        // way round, like a soft-focus filter: the hard contrasts soften while
        // the overall brightness stays where it was
        if (pc.effects.x > 0.0) {
            vec3 soft = texture(diffusion, p).rgb;
            float k = 0.3 * pc.effects.x * (0.45 + 0.55 * dot(soft, vec3(0.2126, 0.7152, 0.0722)));
            c = mix(c, soft, clamp(k, 0.0, 0.5));
        }
        // Glow of fire, explosions and lights over the 3D image and the backdrop
        if (pc.effects.x > 0.0) c += texture(bloom, p).rgb * (0.5 * pc.effects.x);
        // Shafts of sunlight between trees and silhouettes, warm like the sun
        if (pc.effects.w > 0.0) c += texture(light_shafts, p).r * vec3(1.0, 0.9, 0.72);
        // 2D replacement pictures of the foreground: above the 3D image, below
        // what changed in the 2D layer
        vec2 pos = vec2((p.x - pc.layer2d.x) / pc.layer2d.y * 496.0, p.y * 384.0);
        vec4 pictures = replacement_pictures(vec4(c, 1.0), pos, false);
        c = pictures.rgb;
        c = c * (1.0 - front.a) + front.rgb;
    }
    // Picture settings from the menu: saturation, then gamma
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(mix(vec3(luma), c, pc.effects.y), 0.0, 1.0);
    c = pow(c, vec3(1.0 / pc.effects.z));
    if (pc.crosshair2.x >= 0.0) {
        vec4 x = player_crosshair(uv * pc.window, pc.crosshair2.xy, 1, vec3(0.2, 0.9, 1.0));
        c = mix(c, x.rgb, x.a);
    }
    if (pc.crosshair.x >= 0.0) {
        vec4 x = player_crosshair(uv * pc.window, pc.crosshair, 0, vec3(1.0, 0.95, 0.2));
        c = mix(c, x.rgb, x.a);
    }
    color = vec4(c, 1.0);
}
