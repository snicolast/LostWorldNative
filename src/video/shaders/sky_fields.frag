#version 450
#extension GL_GOOGLE_include_directive : require
// The procedural sky's clouds (SKY: PROCEDURAL) over the whole pair of sky
// layers, as a panorama the composition shades and samples instead of working
// the clouds out for every pixel of the screen: x around the camera (the
// layer's 512 pixels, wrapping), y from the horizon (top) to the zenith
// (bottom), see sky_procedural.glsl. r3d_renderer.cpp redraws a strip of it
// every frame with the clouds' drift of that moment.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fields;

layout(push_constant) uniform Sky {
    vec4 p;   // x: drift of the clouds (seconds since launch * 0.005)
} pc;

#include "sky_procedural.glsl"

void main() {
    // uv (compose.vert) runs over the whole panorama, 0..1 each way
    vec2 at = vec2(uv.x * 512.0, 500.0 - uv.y * 1012.0);
    fields = vec4(sky_fields(at, pc.p.x), 1.0);
}
