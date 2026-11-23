#version 450
// Bloom (not in the original): the glow image written by the 3D pass (the
// bright parts of luminous surfaces: fire, explosions, lights) is reduced to
// a quarter of the size, the sky as the composition shows it is added
// (compose.frag, glow mode), and the result is blurred; the composition adds
// it on top.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Bloom {
    vec4 p;   // xy: texel size of the source; z: 0 = reduce 4x4, 1 = horizontal blur, 2 = vertical blur;
              // w: spread of the blur taps; reducing: the share of the source drawn, from
              // its top left corner (the 3D's dynamic resolution), 0 = all of it
} pc;

void main() {
    vec2 t = pc.p.xy;
    if (pc.p.z < 0.5) {   // four bilinear taps cover a 4x4 block
        vec2 at = uv * (pc.p.w > 0.0 ? pc.p.w : 1.0);
        color = 0.25 * (texture(src, at + t * vec2(-1.0, -1.0)) + texture(src, at + t * vec2(1.0, -1.0)) +
                        texture(src, at + t * vec2(-1.0, 1.0)) + texture(src, at + t * vec2(1.0, 1.0)));
        return;
    }
    // Gaussian over 17 texels in 9 bilinear taps, spread a little wider
    vec2 d = (pc.p.z < 1.5 ? vec2(t.x, 0.0) : vec2(0.0, t.y)) * pc.p.w;   // w: spread
    const float offset[5] = float[](0.0, 1.4117647, 3.2941176, 5.1764706, 7.0588235);
    const float weight[5] = float[](0.1964825, 0.2969069, 0.0944703, 0.0103813, 0.0003740);
    vec4 sum = texture(src, uv) * weight[0];
    for (int i = 1; i < 5; i++)
        sum += (texture(src, uv + d * offset[i]) + texture(src, uv - d * offset[i])) * weight[i];
    color = sum;
}
