#version 460
// Ambient occlusion (not in the original), horizon based (HBAO): for each
// pixel of the game world, a few directions are walked across the depth
// buffer and the highest point seen above the surface in each tells how much
// of the sky the surrounding geometry hides. Output: 1 = open, 0 = enclosed.
// Drawn at half the resolution each way: each texel is the full-resolution
// pixel at twice its position (ao_apply.frag reads it back from there).
// Built with RAY_TRACING it can instead trace short rays through the world
// (raytrace.glsl), which also sees what is off screen or hidden behind.
#extension GL_GOOGLE_include_directive : require
#ifdef RAY_TRACING
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require
#endif
#include "ao_common.glsl"
#ifdef RAY_TRACING
// The 3D pass's descriptor set, as set 1: the texture RAM and the texture pack
// for the cut-out triangles, the acceleration structure and the cut-outs
layout(set = 1, binding = 0) uniform usampler2D texture_ram;
// (as many as the 3D pass's set has: r3d.frag's REPLACEMENT_SLOTS, set by the renderer)
layout(constant_id = 0) const uint REPLACEMENT_SLOTS = 4096u;
layout(set = 1, binding = 1) uniform sampler2D replacements[REPLACEMENT_SLOTS];
#define RT_SET 1
#include "raytrace.glsl"

// Short rays over the hemisphere above the surface (cosine weighted, turned
// per pixel): the nearer what they meet, the more it hides
float traced_open(vec3 p, vec3 n, float noise) {
    float radius = ao.params.x;
    vec3 origin = p + n * (0.02 + 0.002 * -p.z);
    vec3 t = normalize(cross(abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
    vec3 b = cross(n, t);
    int rays = int(ao.misc.z);
    float hidden = 0.0;
    for (int i = 0; i < rays; i++) {
        float u = (float(i) + 0.5) / float(rays);
        float a = (float(i) * 0.618034 + noise) * 6.2831853;
        float r = sqrt(u);
        vec3 dir = normalize(t * (r * cos(a)) + b * (r * sin(a)) + n * sqrt(1.0 - u));
        // Only what stands near the ground (ao_low): not the canopies above
        float hit = rt_trace(origin, dir, radius, RT_LOW, false);
        if (hit >= 0.0) hidden += (1.0 - hit / radius) * (1.0 - hit / radius);
    }
    return 1.0 - clamp(hidden / float(rays) * 2.0, 0.0, 1.0);
}
#endif

layout(location = 0) out vec4 out_ao;

void main() {
    vec2 frag = floor(gl_FragCoord.xy) * 2.0 + 0.5;   // the full-resolution pixel
    vec3 p = ao_view_pos(frag);
    // No 3D here (2D backdrop), or high above the ground
    float low = p.z < 0.0 ? ao_low(p) : 0.0;
    if (low <= 0.0) {
        out_ao = vec4(1.0);
        return;
    }
    // Surface normal from the neighbours on the side that continues the surface
    vec3 px0 = ao_view_pos(frag - vec2(1.0, 0.0)), px1 = ao_view_pos(frag + vec2(1.0, 0.0));
    vec3 py0 = ao_view_pos(frag - vec2(0.0, 1.0)), py1 = ao_view_pos(frag + vec2(0.0, 1.0));
    vec3 dx = abs(px1.z - p.z) < abs(p.z - px0.z) ? px1 - p : p - px0;
    vec3 dy = abs(py1.z - p.z) < abs(p.z - py0.z) ? py1 - p : p - py0;
    vec3 n = normalize(cross(dy, dx));
    if (dot(n, p) > 0.0) n = -n;   // towards the camera

    // Rotated per pixel (and per frame with DLAA, which averages it out)
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + ao.misc.x);
    // Fogged surfaces lose their shading, so they lose the occlusion too;
    // so do those fading out high up
    float fog = 1.0 - (1.0 - clamp(ao.params.w + (-p.z) * ao.params.z, 0.0, 1.0)) * low;
#ifdef RAY_TRACING
    // misc.w 1: traced on the left half only, for comparing (LW_RTSPLIT)
    if (ao.misc.z > 0.0 && (ao.misc.w == 0.0 || frag.x < ao.rect.x + ao.rect.z * 0.5)) {
        out_ao = vec4(mix(1.0, traced_open(p, n, noise), 1.0 - fog));
        return;
    }
#endif
    float radius = ao.params.x;
    // The radius on screen, limited so close-ups stay cheap and local
    float radius_px = min(radius * abs(ao.proj.y) * 0.5 * ao.rect.w / -p.z, ao.misc.y);
    if (radius_px < 1.5) {
        out_ao = vec4(1.0);
        return;
    }
    const int DIRECTIONS = 8, STEPS = 4;
    float occlusion = 0.0;
    for (int i = 0; i < DIRECTIONS; i++) {
        float angle = (float(i) + noise) * (6.2831853 / float(DIRECTIONS));
        vec2 dir = vec2(cos(angle), sin(angle));
        float horizon = 0.0;
        for (int s = 0; s < STEPS; s++) {
            float t = (float(s) + 0.5 + 0.5 * fract(noise * 3.7 + float(s) * 0.618)) / float(STEPS) * radius_px;
            vec3 q = ao_view_pos(frag + dir * t);
            if (q.z >= 0.0) continue;
            vec3 h = q - p;
            float d2 = dot(h, h);
            float elevation = dot(n, h) * inversesqrt(d2 + 1e-6);
            float falloff = clamp(1.0 - d2 / (radius * radius), 0.0, 1.0);
            horizon = max(horizon, (elevation - 0.3) * falloff * ao_low(q));
        }
        occlusion += horizon;
    }
    float open = 1.0 - clamp(occlusion / float(DIRECTIONS) * 2.4, 0.0, 1.0);
    out_ao = vec4(mix(1.0, open, 1.0 - fog));
}
