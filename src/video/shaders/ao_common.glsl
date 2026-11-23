// Shared by ao.frag and ao_apply.frag: the depth of the game world (taken
// before the HUD viewports clear it) and the view-space position of a pixel.

layout(set = 0, binding = 0) uniform sampler2D depth_tex;   // reverse Z: near / -z_view, 0 = nothing
layout(set = 0, binding = 1) uniform sampler2D ao_tex;      // raw occlusion (ao.frag)

layout(push_constant) uniform AmbientOcclusion {
    vec4 proj;     // world viewport projection: x scale, y scale, x offset, y offset (as drawn)
    vec4 rect;     // world viewport in target pixels: x, y, width, height
    vec4 params;   // radius (view units), strength, fog density, fog start
    vec4 misc;     // x: noise seed, y: largest radius in pixels, z: rays per pixel when traced (0 = from the depth),
                   // w: 1 = traced on the left half only (LW_RTSPLIT)
    vec4 up;       // world up in view space, camera height above the ground (0, 0, 0, 0: unknown)
    vec4 drawn;    // xy: the part of the targets drawn this image, from their top left corner, in pixels
                   // (smaller than them with the dynamic resolution: what lies beyond is an older image's)
} ao;

const float AO_NEAR = 1e-3;   // r3d_scene.cpp projection

// High above the ground the scenery is a stage set: a flat ceiling of foliage
// over the forest, the tops of painted backdrop walls. Occlusion there (and
// from there) traces the corners where those planes meet, so it fades out
// from AO_LOW to AO_HIGH above the ground (AO_HIGH: the height the sun
// shadows and the traced rays split at, r3d_renderer.cpp)
const float AO_LOW = 25.0, AO_HIGH = 35.0;

// 1 where a point of the world (view space) may take part in the occlusion,
// fading to 0 high up
float ao_low(vec3 p) {
    if (dot(ao.up.xyz, ao.up.xyz) < 0.5) return 1.0;
    return 1.0 - smoothstep(AO_LOW, AO_HIGH, dot(p, ao.up.xyz) + ao.up.w);
}

// View-space position of the surface at a target pixel; z >= 0: no surface
vec3 ao_view_pos(vec2 frag) {
    if (any(lessThan(frag, vec2(0.0))) || any(greaterThanEqual(frag, ao.drawn.xy))) return vec3(0.0, 0.0, 1.0);
    float d = texelFetch(depth_tex, ivec2(frag), 0).r;
    if (d <= 0.0) return vec3(0.0, 0.0, 1.0);
    float z = -AO_NEAR / d;
    vec2 ndc = (floor(frag) + 0.5 - ao.rect.xy) / ao.rect.zw * 2.0 - 1.0;
    return vec3(-z * (ndc.x + ao.proj.z) / ao.proj.x, -z * (ndc.y + ao.proj.w) / ao.proj.y, z);
}
