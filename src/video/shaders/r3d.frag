#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#ifdef RAY_TRACING
#extension GL_EXT_ray_query : require
#endif
// Real3D polygon shading: texture decode from the raw 16-bit texture RAM,
// sun + ambient lighting, fixed shading, specular and fog
// (docs/real3d_spec.md sections 6 and 7). Textures are filtered with the
// mip levels the game uploads (trilinear) plus anisotropic taps.
layout(location = 0) in vec3 view_pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec4 color;
layout(location = 4) flat in uint tex;
layout(location = 5) flat in uint flags;
layout(location = 6) in vec3 params;
layout(location = 7) flat in uint replace;
layout(location = 8) in vec4 clip_now;
layout(location = 9) in vec4 clip_before;

layout(set = 0, binding = 0) uniform usampler2D texture_ram;   // 2048x2048 R16_UINT
// Texture pack, RGBA8 with mips: as many slots as the GPU's descriptor limits
// allow (R3DRenderer::replacement_slots(), set when the pipeline is made)
layout(constant_id = 0) const uint REPLACEMENT_SLOTS = 4096u;
layout(set = 0, binding = 1) uniform sampler2D replacements[REPLACEMENT_SLOTS];

layout(push_constant) uniform Viewport {
    mat4 proj;
    vec4 sun;
    vec4 fog_color;
    vec4 params;
    vec4 quality;
} vp;

layout(set = 0, binding = 3) uniform sampler2D layer2d_back;   // 2D backdrop, for the distance haze
layout(set = 0, binding = 4) uniform sampler2D shadow_map;     // sun shadows (depth seen from the sun)
layout(set = 0, binding = 5) uniform sampler2D scene_depth;    // the opaque world's depth (soft particles)

#define ATMOSPHERE_BINDING 2
#define ATMOSPHERE_BACKDROP
#include "atmosphere.glsl"

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_motion;   // DLAA only: pixels from here to the previous image
layout(location = 2) out vec4 out_glow;     // bloom: the bright part of luminous world surfaces

#include "r3d_texture.glsl"
#ifdef RAY_TRACING
#define RT_SET 0
#include "raytrace.glsl"
#endif

// Sun shadows (not in the original): how much of the sun reaches this point
// of the game world, 0..1, from the shadow map drawn from the sun this frame
#ifdef RAY_TRACING
float traced_shadow(vec3 n);
#endif
float sun_shadow(vec3 n) {
    if (atm.shadow.x <= 0.0 || vp.sun.w <= 0.0 || (flags & WORLD) == 0u) return 1.0;
#ifdef RAY_TRACING
    // rt.x 2: traced on the left half only, for comparing (LW_RTSPLIT)
    if (atm.rt.x > 1.5 ? gl_FragCoord.x * atm.backdrop.x < 0.5 : atm.rt.x > 0.0) return traced_shadow(n);
#endif
    float nl = length(n);
    vec3 pos = view_pos + (nl > 0.0 ? n / nl : vec3(0.0)) * atm.shadow.z;
    vec4 lc = atm.shadow_matrix * vec4(pos, 1.0);
    vec2 uv = lc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || lc.z <= 0.0) return 1.0;
    // 4x4 texels around the point, compared one by one (soft edges)
    float lit = 0.0;
    vec2 t = vec2(atm.shadow.y);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            vec4 d = textureGather(shadow_map, uv + (vec2(x, y) - 0.5) * 2.0 * t, 0);
            lit += dot(vec4(greaterThan(d, vec4(lc.z + atm.shadow.w))), vec4(1.0));
        }
    lit = 1.0 - lit / 16.0;
    // Fades out towards the edge of the shadow map
    vec2 edge = min(uv, 1.0 - uv);
    float fade = smoothstep(0.0, 0.08, min(edge.x, edge.y));
    return mix(1.0, lit, atm.shadow.x * fade);
}

#ifdef RAY_TRACING
// Sun shadows traced through the world: a few rays spread over the sun's
// disc, so the shadow is sharp where the caster touches the ground and
// softens with distance from it
float traced_shadow(vec3 n) {
    float nl = length(n);
    vec3 N = nl > 0.0 ? n / nl : vec3(0.0);
    vec3 L = normalize(vp.sun.xyz);
    // Off the surface by a little more as it gets farther (depth precision)
    vec3 origin = view_pos + (N * 1.5 + L * 0.5) * (0.02 + 0.0015 * length(view_pos));
    vec3 X = normalize(cross(abs(L.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), L));
    vec3 Y = cross(L, X);
    int rays = int(atm.rt.z);
    float turn = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + atm.rt.w) * 6.2831853;
    float lit = 0.0;
    for (int i = 0; i < rays; i++) {
        // Points of a golden-angle spiral over the disc, turned per pixel
        float r = sqrt((float(i) + 0.5) / float(rays)) * atm.rt.y;
        float a = float(i) * 2.3999632 + turn;
        vec3 dir = normalize(L + (X * cos(a) + Y * sin(a)) * r);
        if (rt_trace(origin, dir, 2000.0, RT_LOW, true) < 0.0) lit += 1.0;
    }
    return mix(1.0, lit / float(rays), atm.shadow.x);
}
#endif

vec4 fetch(ivec2 base, ivec2 size, ivec2 c, uint fmt) {
    c.x = wrap(c.x, size.x, (flags & MIRROR_U) != 0u);
    c.y = wrap(c.y, size.y, (flags & MIRROR_V) != 0u);
    ivec2 p = ivec2((base.x + c.x) & 2047, (base.y & 1024) | ((base.y + c.y) & 1023));
    vec4 t = decode(texelFetch(texture_ram, p, 0).r, fmt);
    // Polygons without texture alpha show every texel, including the ones
    // marked transparent: their colour must survive the premultiplied filter
    if ((flags & (TEX_ALPHA | ALPHA_TEST)) == 0u) t.a = 1.0;
    return t;
}

// Mip level positions inside a texture page (spec 6.4)
const int MIP_X[12] = int[](0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047);
const int MIP_Y[11] = int[](0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023);

// Bilinear sample of one mip level; st in level-0 texels. Premultiplied
// result: the colour of transparent texels (often sky blue in the texture
// data) must not bleed into the edges of foliage.
vec4 bilinear(ivec2 base, ivec2 size, vec2 st, int level, uint fmt) {
    ivec2 b = ivec2(MIP_X[level] + (base.x >> level), (base.y & 1024) | (MIP_Y[level] + ((base.y & 1023) >> level)));
    ivec2 s = max(size >> level, ivec2(1));
    vec2 p = st / float(1 << level) - 0.5;
    ivec2 i0 = ivec2(floor(p));
    vec2 f = p - vec2(i0);
    ivec2 i1 = i0 + 1;
    // Axes without wrap smoothing (and all of an alpha-tested texture) repeat
    // without blending across the edge: both taps stay in the repetition the
    // sample falls in (spec 6.6)
    bvec2 smooth_edge = bvec2((flags & SMOOTH_U) != 0u, (flags & SMOOTH_V) != 0u);
    if ((flags & ALPHA_TEST) != 0u) smooth_edge = bvec2(false);
    ivec2 first = ivec2(floor((p + 0.5) / vec2(s))) * s;
    for (int k = 0; k < 2; k++)
        if (!smooth_edge[k]) {
            i0[k] = clamp(i0[k], first[k], first[k] + s[k] - 1);
            i1[k] = clamp(i1[k], first[k], first[k] + s[k] - 1);
        }
    vec4 a = fetch(b, s, i0, fmt);
    vec4 bb = fetch(b, s, ivec2(i1.x, i0.y), fmt);
    vec4 c = fetch(b, s, ivec2(i0.x, i1.y), fmt);
    vec4 d = fetch(b, s, i1, fmt);
    a.rgb *= a.a;
    bb.rgb *= bb.a;
    c.rgb *= c.a;
    d.rgb *= d.a;
    return mix(mix(a, bb, f.x), mix(c, d, f.x), f.y);
}

// duv_dx/duv_dy: screen-space derivatives of uv, taken in uniform control flow
vec4 sample_texture(vec2 duv_dx, vec2 duv_dy) {
    if (replace != 0u) {   // texture pack image: hardware trilinear + anisotropic filtering
        vec2 u = uv;
        if ((flags & MIRROR_U) != 0u) { float m = mod(u.x, 2.0); u.x = m > 1.0 ? 2.0 - m : m; }
        if ((flags & MIRROR_V) != 0u) { float m = mod(u.y, 2.0); u.y = m > 1.0 ? 2.0 - m : m; }
        return textureGrad(replacements[nonuniformEXT(replace)], u, duv_dx, duv_dy);
    }
    ivec2 base = ivec2(int(tex & 2047u), int((tex >> 11) & 2047u));
    ivec2 size = ivec2(32 << int((tex >> 22) & 7u), 32 << int((tex >> 25) & 7u));
    uint fmt = (tex >> 28) & 15u;
    vec2 st = uv * vec2(size);
    vec4 sum;
    float max_aniso = vp.quality.y;
    if (max_aniso < 1.0) {
        sum = bilinear(base, size, st, 0, fmt);   // original look: base level only
    } else {
        // Footprint of the pixel in level-0 texels: the long axis gets several
        // taps, the short one picks the mip level
        vec2 dx = duv_dx * vec2(size), dy = duv_dy * vec2(size);
        float lx = length(dx), ly = length(dy);
        float pmax = max(lx, ly), pmin = max(min(lx, ly), 1e-6);
        float taps = clamp(ceil(pmax / pmin), 1.0, max_aniso);
        // Levels are stored while both sides are >= 4 before halving
        int max_level = max(findMSB(min(size.x, size.y)) - 1, 0);
        float lod = clamp(log2(max(pmax / taps, 1e-6)), 0.0, float(max_level));
        int l0 = int(floor(lod));
        int l1 = min(l0 + 1, max_level);
        float lf = lod - float(l0);
        vec2 axis = lx > ly ? dx : dy;
        sum = vec4(0.0);
        int n = int(taps);
        for (int i = 0; i < n; i++) {
            vec2 p = st + axis * ((float(i) + 0.5) / taps - 0.5);
            vec4 s = bilinear(base, size, p, l0, fmt);
            if (lf > 0.0 && l1 != l0) s = mix(s, bilinear(base, size, p, l1, fmt), lf);
            sum += s;
        }
        sum /= taps;
    }
    sum.rgb = sum.a > 0.0 ? sum.rgb / sum.a : vec3(0.0);
    return sum;
}

// Surface detail (not in the original): the texture's own light and dark
// read as height, so the light, the shadows and the rim pick out the grain of
// stone, bark, sheet metal and skin. Brightness at an offset (uv units).
float relief_height(vec2 offset, vec2 duv_dx, vec2 duv_dy, int level) {
    vec4 s;
    if (replace != 0u) {
        s = textureGrad(replacements[nonuniformEXT(replace)], uv + offset, duv_dx, duv_dy);
    } else {
        ivec2 base = ivec2(int(tex & 2047u), int((tex >> 11) & 2047u));
        ivec2 size = ivec2(32 << int((tex >> 22) & 7u), 32 << int((tex >> 25) & 7u));
        s = bilinear(base, size, (uv + offset) * vec2(size), level, (tex >> 28) & 15u);
        s.rgb = s.a > 0.0 ? s.rgb / s.a : vec3(0.0);
    }
    return dot(s.rgb, vec3(0.2126, 0.7152, 0.0722));
}

// The normal tilted by the texture's height slopes, in the surface's own
// frame worked out from the screen derivatives (no tangents in the data)
vec3 relief_normal(vec3 n, float h0, vec3 dpx, vec3 dpy, vec2 duv_dx, vec2 duv_dy) {
    float strength = atm.relief.x * clamp(1.0 - (-view_pos.z) / atm.relief.y, 0.0, 1.0);
    float nl = length(n);
    if (strength <= 0.0 || nl <= 0.0) return n;
    vec3 N = n / nl;
    vec2 size = replace != 0u ? vec2(textureSize(replacements[nonuniformEXT(replace)], 0))
                              : vec2(32 << int((tex >> 22) & 7u), 32 << int((tex >> 25) & 7u));
    // One step = the pixel's footprint (at least a texel), at the matching mip level
    vec2 footprint = max(abs(duv_dx), abs(duv_dy)) * size;
    float texels = max(max(footprint.x, footprint.y), 1.0);
    int max_level = max(findMSB(int(min(size.x, size.y))) - 1, 0);
    int level = replace != 0u ? 0 : clamp(int(floor(log2(texels))), 0, max_level);
    float hu = relief_height(vec2(texels / size.x, 0.0), duv_dx, duv_dy, level);
    float hv = relief_height(vec2(0.0, texels / size.y), duv_dx, duv_dy, level);
    vec3 dp2perp = cross(dpy, N), dp1perp = cross(N, dpx);
    vec3 T = dp2perp * duv_dx.x + dp1perp * duv_dy.x;
    vec3 B = dp2perp * duv_dx.y + dp1perp * duv_dy.y;
    float inv = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-30));
    vec3 tilted = normalize(N - strength * ((hu - h0) * T * inv + (hv - h0) * B * inv));
    return tilted * nl;
}

// Where lightning lights the world from (view space): above the strike, or
// straight overhead
vec3 lightning_dir() {
    return dot(atm.bolt_light.xyz, atm.bolt_light.xyz) > 0.0 ? atm.bolt_light.xyz : atm.axes[1].xyz;
}

// Rain on the world (r3d_rain.h): soaked surfaces are darker and reflect the
// light of the fog around them (and the lightning) with a Fresnel sheen,
// strongest on the ground, where puddles gather and falling drops ring the
// water. lit: the lit colour; n: the normal (view space); p: world position;
// px: size of a pixel there (world units). Returns the wet colour.
vec3 wet_surface(vec3 lit, vec3 n, vec3 p, float px) {
    float wet = atm.rain.x;
    vec3 N = normalize(n);
    float up = dot(atm.axes[1].xyz, N);
    float ground = smoothstep(0.55, 0.9, up);
    // Puddles in the hollows of a slow noise, with ragged edges
    float hollow = 0.7 * atm_noise(vec3(p.xz * 0.06, 0.5)) + 0.3 * atm_noise(vec3(p.xz * 0.3, 2.5));
    float puddle = ground * smoothstep(0.6, 0.65, hollow);
    lit *= 1.0 - wet * (0.15 + 0.2 * ground + 0.25 * puddle);
    // Ripples: in each cell of the ground a drop lands now and then, splashes
    // and a ring spreads from it; they fade out where a ring gets thinner
    // than a pixel. Splashes also land on whatever faces up (the T-Rex's back).
    float ripple = 0.0, splash = 0.0;
    float detail = 1.0 - smoothstep(0.1, 0.35, px);
    if (up > 0.5) {
        const float CELL = 3.0;
        vec2 cell = floor(p.xz / CELL);
        for (int k = 0; k < 2; k++) {   // two drops per cell, out of step
            vec3 h = vec3(cell, float(k) * 7.31);
            float t = fract(atm.camera.w * 1.3 + atm_hash(h + 0.17));
            vec2 at = (cell + 0.25 + 0.5 * vec2(atm_hash(h + 3.1), atm_hash(h + 5.7))) * CELL;
            float r = length(p.xz - at);
            float d = r - t * 0.9;
            ripple += exp(-d * d / 0.012) * (1.0 - t) * (1.0 - t);
            splash += exp(-r * r / 0.06) * (1.0 - smoothstep(0.0, 0.12, t));
        }
        ripple *= detail * smoothstep(0.85, 0.97, up) * wet;
        splash *= smoothstep(0.5, 0.8, up) * wet;
    }
    vec3 V = normalize(view_pos);
    vec3 R = reflect(V, N);
    float facing = clamp(dot(-V, N), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
    float gloss = wet * mix(0.2, 1.6, max(0.25 * ground, puddle));
    vec3 sky = vp.fog_color.rgb * vp.fog_color.a;
    if (max(sky.r, max(sky.g, sky.b)) < 0.02) sky = atm.haze.rgb * 0.4;
    // Low reflections meet the trees around, dark; high ones the lit fog
    float rise = dot(atm.axes[1].xyz, R);
    vec3 env = sky * (min(mist_light(R), vec3(2.5)) * mix(0.5, 1.3, smoothstep(0.0, 0.4, rise)) + lightning_glow(R) * puddle);
    // Lightning glints on the wet ground, from overhead
    float glint = pow(max(dot(R, lightning_dir()), 0.0), 16.0) * atm.mist.w * 0.6;
    // Rings catch the light of the fog, most of all on the puddles
    vec3 rings = (ripple * (0.2 + 0.8 * puddle) + splash * 1.5) * sky * mist_light(N);
    return mix(lit, env, min(fresnel * gloss, 1.0)) + rings + glint * gloss * vec3(0.9, 0.95, 1.0);
}

// Spotlight of the original (spec 7.1), the torch of the night at the
// trailer: a lobe around a point of the view (spot_lobe), fading with
// distance past the extent. x: lobe on the light, y: lobe on the fog
vec2 spotlight() {
    float z = -view_pos.z;
    if (z <= 0.0 || atm.spot.z <= 0.0) return vec2(0.0);
    float ellipse = spot_lobe(view_pos.xy / z);
    float extent = atm.spot_fog.w, range = 0.0;
    if (extent > 0.0) {
        float d = min(0.0, atm.spot_color.w + extent - z);
        float k = d / (1.0 + extent) - 1.0;
        range = 1.0 / (k * k);
    }
    return vec2(range, z >= atm.spot_color.w ? 1.0 : 0.0) * ellipse;
}

void main() {
    vec2 duv_dx = dFdx(uv), duv_dy = dFdy(uv);
    vec3 dpx = dFdx(view_pos), dpy = dFdy(view_pos);   // the surface's frame, for the surface detail
    // World position (rain on the world), and its size of a pixel
    vec3 world_pos = atm.camera.xyz + vec3(dot(atm.axes[0].xyz, view_pos), dot(atm.axes[1].xyz, view_pos),
                                          dot(atm.axes[2].xyz, view_pos));
    float world_px = length(fwidth(world_pos));
    // Motion vector in pixels; NDC is linear across the screen, so its
    // derivatives give the viewport size without passing it in
    vec2 ndc = clip_now.xy / clip_now.w;
    vec2 px_per_ndc = 1.0 / vec2(dFdx(ndc.x), dFdy(ndc.y));
    out_motion = (clip_before.xy / clip_before.w - ndc) * px_per_ndc;
    gl_SampleMask[0] = -1;   // a statically written mask must be set on every path
    vec4 col = color;
    if ((flags & TRANSLATOR) != 0u) col.rgb *= 16.0;

    // Sun shadow pass (fog_color.w < 0): only what stands near the ground casts
    // shadows; canopies and roofs high above would put whole scenes in the dark
    if (vp.fog_color.w < 0.0 && dot(view_pos, vp.sun.xyz) + vp.fog_color.x > vp.fog_color.y) discard;

    vec4 t = vec4(1.0);
    if ((flags & TEXTURED) != 0u) {
        t = sample_texture(duv_dx, duv_dy);
        if ((flags & INVERT_RGB) != 0u) t.rgb = 1.0 - t.rgb;
        if ((flags & (TEX_ALPHA | ALPHA_TEST)) == 0u) t.a = 1.0;
        if ((flags & ALPHA_TEST) != 0u && t.a < 32.0 / 255.0) discard;
    }

    float ambient = vp.params.x;
    float fixed_shade = params.x;
    if ((flags & LUMINOUS) != 0u)
        col.rgb *= (flags & FIXED_SHADE) != 0u ? 1.0 + fixed_shade + ambient : 1.5;

    vec4 c = t * col;
    if (c.a < 1.0 / 32.0) discard;
    // Depth of the nearest see-through surface (spec 7.2); effects stay out of it
    if (vp.quality.x <= -1000.0) {
        if ((flags & (TEX_ALPHA | ALPHA_TEST | LUMINOUS)) != 0u) discard;
        return;
    }
    // Texture alpha (spec 7.2): solid texels belong to the opaque pass, the
    // see-through ones to the translucent pass
    if ((flags & TEX_ALPHA) != 0u && (vp.quality.x > 0.0 ? c.a < 1.0 : c.a >= 1.0)) discard;

    // Our rain (r3d_rain.cpp): a streak lit like the fog around it, brightest
    // along its middle and towards its head; lightning makes it flash
    if ((flags & RAIN) != 0u) {
        float across = 1.0 - uv.x * uv.x;
        float along = smoothstep(0.0, 0.12, uv.y) * (1.0 - 0.7 * uv.y);
        vec3 air = vp.fog_color.rgb * vp.fog_color.a;
        if (max(air.r, max(air.g, air.b)) < 0.02) air = atm.haze.rgb * 0.4;
        vec3 lit = air * mist_light(view_pos) * 1.5 + 0.05 + atm.mist.w * vec3(0.9, 0.95, 1.0);
        c = vec4(lit, c.a * across * along);
    }
    if ((flags & (LUMINOUS | RAIN)) == 0u) {
        vec3 n = normal;
        if (atm.relief.x > 0.0 && (flags & (WORLD | TEXTURED)) == (WORLD | TEXTURED) && (flags & FIXED_SHADE) == 0u)
            n = relief_normal(n, dot(t.rgb, vec3(0.2126, 0.7152, 0.0722)), dpx, dpy, duv_dx, duv_dy);
        float s = (flags & FIXED_SHADE) != 0u ? fixed_shade : dot(vp.sun.xyz, n);
        s = clamp(s, -1.0, 1.0);
        s = max(s, 0.0);
        if ((flags & FIXED_SHADE) == 0u && s > 0.0) s *= sun_shadow(n);
        vec3 light = vec3(s * vp.sun.w + ambient);
        // Clear outdoor scenes (SUN): a stronger, warmer sun and a softer,
        // slightly bluer ambient (light from the sky) for more contrast
        if (atm.sunlight.z > 0.0 && (flags & WORLD) != 0u) {
            float w = atm.sunlight.z;
            light = s * vp.sun.w * atm.sunlight.x * vec3(1.0, 1.0 - 0.05 * w, 1.0 - 0.14 * w) +
                    ambient * atm.sunlight.y * vec3(1.0 - 0.06 * w, 1.0, 1.0 + 0.08 * w);
        }
        // Rim light: dinosaurs, people and vehicles get a soft edge of light
        // along their silhouette, cool from the sky above and warm towards the
        // sun, which lifts them off the background as on a film set
        if (atm.sunlight.w > 0.0 && (flags & (ACTOR | WORLD)) == (ACTOR | WORLD) && length(n) > 0.0) {
            vec3 nn = normalize(n);
            float rim = pow(1.0 - clamp(dot(nn, normalize(-view_pos)), 0.0, 1.0), 4.0);
            float sky = 0.45 + 0.55 * max(dot(nn, atm.axes[1].xyz), 0.0);
            float sunward = max(dot(nn, vp.sun.xyz), 0.0) * clamp(vp.sun.w, 0.0, 1.0);
            light += rim * atm.sunlight.w * (vec3(0.62, 0.72, 0.9) * sky + vec3(1.0, 0.82, 0.58) * sunward);
        }
        // Lightning lights the world for a moment from where it strikes:
        // what faces it stands out, what faces away stays dark
        if (atm.mist.w > 0.0 && (flags & WORLD) != 0u && length(n) > 0.0)
            light += atm.mist.w * (0.12 + 0.75 * max(dot(normalize(n), lightning_dir()), 0.0));
        // The torch: in the original a flat lobe; the modern one comes from
        // the eye, so it shades the surfaces it meets (faces turned away get
        // less), is warm, and glints on what it hits head-on
        float glint = 0.0;
        if ((flags & WORLD) != 0u && atm.spot.z > 0.0) {
            float lobe = spotlight().x;
            if (atm.night.y > 0.5 && lobe > 0.0) {
                float ndl = length(n) > 0.0 ? max(dot(normalize(n), normalize(-view_pos)), 0.0) : 1.0;
                light += atm.spot_color.rgb * TORCH_TINT * lobe * (0.25 + 1.3 * ndl);
                glint = lobe * pow(ndl, 48.0) * (0.12 + ((flags & SPECULAR) != 0u ? 0.6 * params.z : 0.0));
            } else {
                light += atm.spot_color.rgb * lobe;
            }
        }
        c.rgb *= light;
        c.rgb += glint * atm.spot_color.rgb * TORCH_TINT;
        if ((flags & SPECULAR) != 0u) {
            uint shin = (flags >> 10) & 3u;
            float powers[4] = float[](8.0, 16.0, 32.0, 64.0);
            float scales[4] = float[](1.6, 1.6, 2.4, 3.2);
            float f = pow(max(s, 0.0), powers[shin]) * scales[shin] * params.z * vp.sun.w *
                      (atm.sunlight.z > 0.0 && (flags & WORLD) != 0u ? atm.sunlight.x : 1.0);
            c.rgb += f;
        }
        if (atm.rain.x > 0.0 && (flags & WORLD) != 0u && vp.quality.x > 0.0 && length(n) > 0.0)
            c.rgb = wet_surface(c.rgb, n, world_pos, world_px);
    }
    // The original clips the light at white; with HDR lighting the highlights
    // (sunlit surfaces, fire, specular) keep going and the composition's tone
    // curve rolls them off
    c.rgb = min(c.rgb, vec3(atm.soft.z > 0.5 ? 8.0 : 1.0));

    float fog = params.y * clamp(vp.params.z + (-view_pos.z) * vp.params.y, 0.0, 1.0);
    vec3 fog_color = vp.fog_color.rgb * vp.fog_color.a;
    // The lightning channel shows only where the fog hides almost everything
    if ((flags & WORLD) != 0u && fog > 0.0)
        fog_color = fog_color * (mist_light(view_pos) + lightning_glow(view_pos) * pow(fog, 6.0) * smoothstep(0.9, 1.0, fog)) +
                    atm.spot_fog.rgb * spotlight().y;
    c.rgb = mix(c.rgb, fog_color, fog);
    // The modern torch's beam in the air between the eye and this surface
    if ((flags & WORLD) != 0u && -view_pos.z > 0.0) c.rgb += torch_air(view_pos.xy / -view_pos.z, length(view_pos));
    // Optional distance haze
    if ((flags & WORLD) != 0u && atm.haze.a > 0.0)
        c.rgb = distance_haze(c.rgb, view_pos, float(flags >> 24u) / 255.0, fog);
    // Opaque pass: the colour written is always solid, since a filtered alpha
    // would let the 2D background show through. With MSAA, alpha-tested edges
    // become partial sample coverage instead (smooth foliage outlines).
    if (vp.quality.x > 0.0) {
        int samples = int(vp.quality.x);
        // DLAA: per-frame noise seed in the fractional part (0 = none)
        float seed = fract(vp.quality.x);
        if (samples > 1 && (flags & ALPHA_TEST) != 0u) {
            int covered = clamp(int(t.a * float(samples) + 0.5), 0, samples);
            if (covered == 0) discard;
            gl_SampleMask[0] = (1 << covered) - 1;
        } else if (seed > 0.0 && (flags & ALPHA_TEST) != 0u) {
            // The same partial coverage spread over frames: the pixel is kept
            // with probability alpha and DLAA averages the frames into a
            // smooth outline. Interleaved gradient noise, shifted every frame.
            float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
            if (t.a < fract(noise + seed)) discard;
        }
        c.a = 1.0;
    }
    // Soft particles: smoke, fire and other translucent surfaces of the world
    // fade out where they meet what stands behind them, instead of a hard line
    if (vp.quality.x < 0.0 && atm.soft.x > 0.0 && (flags & WORLD) != 0u) {
        float d = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
        if (d > 0.0) c.a *= clamp((view_pos.z + 1e-3 / d) / atm.soft.y, 0.0, 1.0);
    }
    out_color = c;
    // Bloom source: fire, explosions and lights are drawn unlit (luminous);
    // only their bright part glows. Everything else writes no glow, so it
    // hides the glow of what is behind it.
    float peak = max(c.r, max(c.g, c.b));
    bool glows = (flags & (LUMINOUS | WORLD)) == (LUMINOUS | WORLD);
    // ... and, more gently, whatever else of the world is very bright (sunlit
    // surfaces and highlights above white with HDR lighting)
    vec3 glow = glows ? c.rgb * smoothstep(0.6, 1.0, peak)
                      : (flags & WORLD) != 0u ? c.rgb * smoothstep(0.75, 1.6, peak) * 0.6 : vec3(0.0);
    out_glow = vec4(glow, c.a);
}
