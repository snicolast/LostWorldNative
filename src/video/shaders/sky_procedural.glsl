// Procedural sky (SKY: PROCEDURAL): a gradient with soft clouds, on the
// layers as a panorama that wraps around the camera (the lower one with the
// horizon, the upper one above it). Included by compose.frag.
//
// sky_procedural.cpp has the same sky for the CPU, which makes the picture
// that stands in for the layer's own pixels (keep both in step).
// tests/sky_procedural_test.cpp compiles this file as C++ and checks that the
// two give the same colours.

float sky_hash(ivec2 p) {
    uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFFFu) / 16777216.0;
}

// Value noise that repeats every `period` cells along x (the layer wraps around)
float sky_noise(vec2 p, int period) {
    ivec2 i = ivec2(floor(p));
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    int x0 = ((i.x % period) + period) % period, x1 = (x0 + 1) % period;
    return mix(mix(sky_hash(ivec2(x0, i.y)), sky_hash(ivec2(x1, i.y)), f.x),
               mix(sky_hash(ivec2(x0, i.y + 1)), sky_hash(ivec2(x1, i.y + 1)), f.x), f.y);
}

float sky_fbm(vec2 p, int period, int octaves) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < octaves; i++) {
        s += a * sky_noise(p, period);
        p = p * 2.0 + vec2(5.3, 9.7);
        period *= 2;
        a *= 0.5;
    }
    return s;
}

float sky_density(vec2 p) { return max(sky_fbm(p, 8, 6) - 0.45, 0.0) * 10.0; }

// at: x and y in the 512x512 lower layer (the upper one has y from -512 to 0)
// sky, lit: colours of the clear sky and of the lit clouds there
vec3 sky_procedural(vec2 at, float drift, vec3 sky, vec3 lit) {
    const float HORIZON = 500.0, HEIGHT = 1012.0;
    float t = clamp((HORIZON - at.y) / HEIGHT, 0.0, 1.0);   // 0 at the horizon, 1 at the top
    // More depth than the original: richer colour, darker towards the top,
    // a pale glow at the horizon, and whiter cloud tops
    float l = dot(sky, vec3(0.2126, 0.7152, 0.0722));
    sky = max(vec3(l) + (sky - vec3(l)) * 1.3, 0.0) * (1.0 - 0.3 * smoothstep(0.1, 0.8, t))
          + 0.06 * (1.0 - smoothstep(0.0, 0.15, t));
    lit = mix(lit, vec3(1.0), 0.3);
    // The layer is a cylinder around the camera: u around it (8 cells), v up
    // it, squeezed towards the horizon by perspective
    float el = 0.03 + 1.5 * t;   // elevation (radians)
    vec2 p = vec2(at.x * (8.0 / 512.0) + drift, 6.0 * tanh(2.5 / tan(el) / 6.0) - 6.0 * t);
    p += 0.5 * vec2(sky_fbm(p * 0.5 + vec2(3.1, 0.0), 4, 5), sky_fbm(p * 0.5 + vec2(0.0, 7.7), 4, 5));
    // Cloud density, then the light that reaches each point from above
    // through the cloud: lit tops, shaded bases, bright thin edges
    float density = sky_density(p);
    float above = 0.0;
    for (int i = 1; i <= 4; i++) above += sky_density(p - vec2(0.0, 0.06 * float(i)));
    float light = exp(-above * 0.9);
    float cover = (1.0 - exp(-density * 3.0)) * smoothstep(0.0, 0.12, t);
    vec3 cloud = mix((sky * 0.6 + lit * 0.3) * 0.75, min(lit * 1.05, vec3(1.0)), light) + (1.0 - cover) * 0.15;
    cloud *= 0.8 + 0.4 * sky_fbm(p * 4.0 + vec2(1.7, 2.9), 32, 3);   // finer billows
    cloud = mix(cloud, sky, smoothstep(0.17, 0.0, t) * 0.6);
    return mix(sky, cloud, cover);
}
