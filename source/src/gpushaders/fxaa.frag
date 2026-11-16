#version 450
// FXAA (gfx->postpass(0)): the same as mslpost's ffxaa in gfx_sdlgpu.cpp and shaders.cpp's GL version.
layout(location = 0) in vec2 intc;
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform PostU { vec4 params; }; // texel width, height
layout(location = 0) out vec4 ocolor;
#define S(p) texture(tex, vec2((p).x, 1.0 - (p).y)).rgb
void main()
{
    vec2 tc = vec2(intc.x, 1.0 - intc.y), texel = params.xy;
    const vec3 luma = vec3(0.299, 0.587, 0.114);
    vec3 rgbNW = S(tc + vec2(-1.0, -1.0) * texel), rgbNE = S(tc + vec2(1.0, -1.0) * texel);
    vec3 rgbSW = S(tc + vec2(-1.0, 1.0) * texel), rgbSE = S(tc + vec2(1.0, 1.0) * texel), rgbM = S(tc);
    float lNW = dot(rgbNW, luma), lNE = dot(rgbNE, luma), lSW = dot(rgbSW, luma), lSE = dot(rgbSE, luma), lM = dot(rgbM, luma);
    float lmin = min(lM, min(min(lNW, lNE), min(lSW, lSE))), lmax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);
    float rcpmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpmin, vec2(-8.0), vec2(8.0)) * texel;
    vec3 rgbA = 0.5 * (S(tc + dir * (1.0 / 3.0 - 0.5)) + S(tc + dir * (2.0 / 3.0 - 0.5)));
    vec3 rgbB = rgbA * 0.5 + 0.25 * (S(tc - dir * 0.5) + S(tc + dir * 0.5));
    float lB = dot(rgbB, luma);
    ocolor = vec4((lB < lmin || lB > lmax) ? rgbA : rgbB, 1.0);
}
