// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// https://en.wikipedia.org/wiki/Lanczos_resampling

#version 460 core

layout (location = 0) in vec2 frag_tex_coord;
layout (location = 0) out vec4 color;
layout (binding = 0) uniform sampler2D color_texture;

#define PI 3.1415926535897932384626433
float sinc(float x) {
    return x == 0.0f ? 1.0f : sin(PI * x) / (PI * x);
}
// Separable Lanczos-3 weight for a distance in source texels.
float lanczos(float x) {
    return abs(x) < 3.0f ? sinc(x) * sinc(x / 3.0f) : 0.0f;
}
vec4 textureLanczos(sampler2D textureSampler, vec2 p) {
    vec2 res = vec2(textureSize(textureSampler, 0));
    vec2 pos = p * res - 0.5f;
    vec2 base = floor(pos);
    vec2 f = pos - base;
    vec3 c_sum = vec3(0.0f);
    float w_sum = 0.0f;
    for (int y = -2; y <= 3; ++y) {
        float wy = lanczos(float(y) - f.y);
        for (int x = -2; x <= 3; ++x) {
            float w = wy * lanczos(float(x) - f.x);
            c_sum += w * texture(textureSampler, (base + vec2(x, y) + 0.5f) / res).rgb;
            w_sum += w;
        }
    }
    return vec4(max(c_sum / w_sum, 0.0f), 1.0f);
}

void main() {
    color = textureLanczos(color_texture, frag_tex_coord);
}
