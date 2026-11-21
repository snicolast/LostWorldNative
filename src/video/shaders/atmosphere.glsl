// Optional atmosphere, shared by the 3D pass (in front of the polygons) and
// the composition (in front of the 2D sky). Not part of the original hardware.
//
// Distance haze (--haze): aerial perspective that tints and desaturates the
// scenery towards the colour of the horizon as it gets farther, and over the
// last stretch of the draw distance dissolves it into the game's own 2D
// backdrop, so models appearing at the horizon fade in instead of popping.

layout(set = 0, binding = ATMOSPHERE_BINDING) uniform Atmosphere {
    vec4 axes[3];    // world x, y (up), z axes in view space
    vec4 camera;     // camera position in the tracked world frame; w = time in seconds
    vec4 sun;        // direction towards the sun in view space; w = sun intensity
    vec4 view;       // projection of the world viewport: x and y scale, x and y offset
    vec4 haze;      // horizon colour; a = haze strength (0 = off)
    vec4 range;      // haze: distance where the tint starts, distance of the farthest scenery, desaturation,
                     // 1 = the back layer continues beside the 4:3 area
    vec4 backdrop;   // 3D target: 1 / width, 1 / height; 4:3 area: x offset and width (fractions)
    vec4 scroll_fog; // the original's scroll fog: colour, a = amount (0 = none)
    vec4 scroll_rect;// its rectangle in the 3D image: x0, y0, x1, y1 (fractions)
    mat4 shadow_matrix;  // sun shadows: view space -> shadow map (xy: -1..1, z: 0 far .. 1 near the sun)
    vec4 shadow;     // strength (0 = off), texel size, normal offset (view units), depth bias
    vec4 soft;       // soft particles: x 1 = on, y: distance over which they fade into what is behind (view units);
                     // z: 1 = HDR lighting (no clipping at white in the 3D image)
    vec4 mist;       // light in the game's fog: x strength (0 = off), y brighter upwards, z drifting banks,
                     // w lightning flash (0 = none)
    vec4 sunlight;   // clear outdoor scenes (SUN): x sun gain, y ambient gain, z warmth (0 = the game's own light);
                     // w: rim light on the world's actors (0 = off)
    vec4 spot;       // spotlight of the world viewport (spec 7.1): centre as view slopes (x / -z, y / -z), 1 / radii
    vec4 spot_color; // its light colour (0 = none); w: distance where it starts
    vec4 spot_fog;   // its colour on the fog (spot-on-fog colour * fog attenuation * fog colour); w: extent
    vec4 night;      // x: how much of the procedural night sky shows behind the world, 0..1 (SKY: PROCEDURAL, black backdrop in the dark);
                     // y: 1 = modern flashlight (the spotlight shaded, soft, glowing in the air);
                     // z: texture detail scale, the 3D image's size over the picture shown (1 / DLSS upscaling)
    vec4 rain;       // x: how much it rains, 0..1 (r3d_rain.h): drops in the air, the world wet
    vec4 wind;       // R3DRain::wind: direction across the ground (world x, z), gust 0..1, distance carried
    vec4 bolt;       // lightning channel: x brightness (0 = none), y segments, z its azimuth in the world frame
                     // (radians), w segments of the main channel (the rest are branches)
    vec4 bolt_light; // xyz: direction the lightning lights the world from (view space; 0 = none)
    vec4 bolt_seg[24];   // segments: azimuth (from bolt.z) and elevation of both ends, radians
    vec4 relief;     // surface detail: x strength (0 = off), y distance where it has faded out (view units)
    vec4 sky_sun;    // the sun drawn in the sky and its light shafts: direction in view space (R3DRenderer::step_sky_sun)
    vec4 rt;         // ray traced sun shadows: x 1 = on (2: left half only), y size of the sun (tangent of its radius),
                     // z rays per point, w noise seed (changes every image with DLAA)
    vec4 weather;    // the weather (r3d_weather.h): x how much the backdrop turns to a cloud grey, yzw that grey
} atm;

// Spotlight lobe across the view (spec 7.1) for a view direction given as
// slopes (x / -z, y / -z): the original's ellipse 1 - r^2, or for the modern
// flashlight a bright core fading softly into a dim spill a little past the
// original rim, with the faint rings of a reflector
float spot_lobe(vec2 slope) {
    vec2 e = (slope - atm.spot.xy) * atm.spot.zw;
    float r2 = dot(e, e);
    if (atm.night.y < 0.5) return max(1.0 - r2, 0.0);
    float r = sqrt(r2);
    float beam = pow(1.0 - smoothstep(0.0, 1.5, r), 1.3) + 0.35 * exp(-r2 * 5.0);
    return beam * (1.0 + 0.05 * sin(r * 19.0) * (1.0 - smoothstep(0.6, 1.2, r)));
}

// Modern flashlight: its warm colour, and the light it leaves in the night
// air along a view ray of length d (in-scattering), so the beam itself shows
const vec3 TORCH_TINT = vec3(1.0, 0.93, 0.8);
vec3 torch_air(vec2 slope, float d) {
    if (atm.night.y < 0.5 || atm.spot.z <= 0.0) return vec3(0.0);
    return atm.spot_color.rgb * TORCH_TINT * spot_lobe(slope) * (1.0 - exp(-d / 250.0)) * 0.11;
}

// Scroll fog of the original hardware over a colour of the 2D layers behind
// the 3D image, at a position in the 3D image (fractions); light: mist_light()
// in that direction
float scroll_fog_amount(vec2 p) {
    bool inside = all(greaterThanEqual(p, atm.scroll_rect.xy)) && all(lessThan(p, atm.scroll_rect.zw));
    return inside ? atm.scroll_fog.a : 0.0;
}

vec3 scroll_fog(vec3 c, vec2 p, vec3 light) {
    float a = scroll_fog_amount(p);
    if (a > 0.0) c = mix(c, atm.scroll_fog.rgb * light, a);
    return c;
}

float atm_hash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float atm_noise(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(atm_hash(i + vec3(0, 0, 0)), atm_hash(i + vec3(1, 0, 0)), f.x),
                   mix(atm_hash(i + vec3(0, 1, 0)), atm_hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(atm_hash(i + vec3(0, 0, 1)), atm_hash(i + vec3(1, 0, 1)), f.x),
                   mix(atm_hash(i + vec3(0, 1, 1)), atm_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// The lightning channel (r3d_renderer.cpp) seen in a world direction d: a
// thin white core with a glow around it, the branches fainter. It shows in
// the light of the fog (lightning_glow), so scenery in front hides it.
float lightning_channel(vec3 d) {
    if (atm.bolt.x <= 0.0) return 0.0;
    float el = asin(clamp(d.y, -1.0, 1.0));
    float a = atan(d.z, d.x) - atm.bolt.z;
    a = mod(a + 3.14159265, 6.2831853) - 3.14159265;
    float squeeze = cos(el);   // azimuth spans less of the view higher up
    vec2 p = vec2(a * squeeze, el);
    float core = 0.0, glow = 0.0;
    int count = int(atm.bolt.y), main = int(atm.bolt.w);
    for (int i = 0; i < count; i++) {
        vec4 s = atm.bolt_seg[i];
        vec2 p0 = vec2(s.x * squeeze, s.y), p1 = vec2(s.z * squeeze, s.w);
        vec2 e = p1 - p0;
        float t = clamp(dot(p - p0, e) / max(dot(e, e), 1e-8), 0.0, 1.0);
        float dist = length(p - p0 - e * t);
        float weight = i < main ? 1.0 : 0.45;
        core = max(core, weight * exp(-dist * dist / 0.0000008));
        glow += weight * exp(-dist / 0.006);
    }
    // Brighter towards the clouds it comes from
    float above = smoothstep(-0.02, 0.2, el);
    return atm.bolt.x * above * (30.0 * core + 1.2 * min(glow, 2.0));
}

// Light in the game's own fog (not in the original): instead of one flat
// colour it gets brighter upwards, where the light comes in between the
// treetops, with big banks drifting slowly. It depends on the view direction
// only, so a fogged surface and the fogged sky behind it always match and
// scenery the fog hides stays hidden. Multiplies the fog colour.
vec3 mist_light(vec3 dir_view) {
    if (atm.mist.x <= 0.0 && atm.mist.w <= 0.0) return vec3(1.0);
    vec3 d = vec3(dot(atm.axes[0].xyz, dir_view), dot(atm.axes[1].xyz, dir_view), dot(atm.axes[2].xyz, dir_view));
    d /= max(length(d), 1e-6);
    float up = smoothstep(-0.2, 0.75, d.y);
    // The banks drift, faster with the wind when it rains
    vec3 drift = vec3(1.0, 0.15, 0.6) * atm.camera.w * 0.012 + vec3(atm.wind.x, 0.0, atm.wind.y) * atm.wind.w * 0.004 * atm.rain.x;
    float n = 0.65 * atm_noise(d * 2.5 + drift) + 0.35 * atm_noise(d * 6.0 - drift * 1.7 + vec3(3.1, 7.7, 1.3));
    float m = 1.0 + atm.mist.y * (up - 0.2) + atm.mist.z * (n - 0.5) * 2.0;
    // Rain: tall curtains, lighter and darker, sweep across the far fog
    if (atm.rain.x > 0.0) {
        float c = atm_noise(vec3(atan(d.z, d.x) * 7.0 - atm.wind.w * 0.012, d.y * 1.2, atm.wind.w * 0.004));
        m *= 1.0 + atm.rain.x * 0.35 * (c - 0.5) * (1.0 - smoothstep(0.05, 0.6, d.y));
    }
    vec3 light = vec3(mix(1.0, max(m, 0.0), atm.mist.x));
    // Lightning: the whole fog lights up, most of all overhead and towards
    // the strike, cold white; its channel shows through the fog
    float toward = 0.0;
    if (atm.bolt.y > 0.0) {
        float a = atan(d.z, d.x) - atm.bolt.z;
        a = mod(a + 3.14159265, 6.2831853) - 3.14159265;
        toward = exp(-a * a / 0.35) * smoothstep(-0.1, 0.4, d.y);
    }
    return light + atm.mist.w * (1.5 + 2.0 * up + 1.5 * toward) * (0.8 + 0.4 * n) * vec3(0.9, 0.96, 1.1);
}

// The lightning channel in a view direction, as light added to mist_light()
// where nothing but the fog stands in front of it
vec3 lightning_glow(vec3 dir_view) {
    if (atm.bolt.x <= 0.0) return vec3(0.0);
    vec3 d = vec3(dot(atm.axes[0].xyz, dir_view), dot(atm.axes[1].xyz, dir_view), dot(atm.axes[2].xyz, dir_view));
    return lightning_channel(d / max(length(d), 1e-6)) * vec3(0.85, 0.9, 1.15);
}

#ifdef ATMOSPHERE_BACKDROP
// Aerial perspective for a world surface at a view-space point, with its
// colour; appear: 1 for scenery that has just appeared in the distance,
// falling to 0 as it fades in; game_fog: how much of it the game's own fog
// already covers
vec3 distance_haze(vec3 c, vec3 view_point, float appear, float game_fog) {
    float d = length(view_point);
    float far = atm.range.y;
    // Tint and desaturation grow steadily with distance, past the near scenery
    float k = atm.haze.a * 0.4 * (1.0 - exp(-2.0 * max(d - atm.range.x, 0.0) / far));
    // Where the game's fog already hides the scenery the haze adds nothing:
    // a fully fogged surface keeps the fog colour, like the fogged sky behind
    // it, so far scenery the game moves around (the T-Rex chase loops the
    // forest) cannot show through as a lighter silhouette
    k *= 1.0 - game_fog;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(c, vec3(luma), atm.range.z * k);
    c = mix(c, atm.haze.rgb, k);
    // Scenery that has just appeared fades in from what the game drew behind
    // it: the 2D backdrop where there is one, the horizon colour elsewhere
    float fade = min(atm.haze.a, 1.0) * appear * appear;
    if (fade > 0.0) {
        vec2 p = gl_FragCoord.xy * atm.backdrop.xy;
        float x = (p.x - atm.backdrop.z) / atm.backdrop.w;
        bool inside = atm.range.w > 0.5 || (x >= 0.0 && x <= 1.0);
        // The back layer is drawn 440 pixels wider on each side (tilegen.h BACK_MARGIN)
        vec2 q = vec2((x * 496.0 + 440.0) / 1376.0, p.y);
        vec4 b = inside && q.x > 0.0 && q.x < 1.0 ? texture(layer2d_back, q) : vec4(0.0);
        vec3 behind = b.a > 0.5 ? scroll_fog(b.rgb / b.a, p, mist_light(view_point) + lightning_glow(view_point)) : atm.haze.rgb;
        c = mix(c, behind, fade);
    }
    return c;
}
#endif
