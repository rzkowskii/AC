#version 450
// The GPU backend's scene fragment shader for Vulkan, in GLSL 450: the same as mslfragment, and mslpick with PICK 1, in
// gfx_sdlgpu.cpp (keep them in step). build.sh compiles it to SPIR-V for each variant (TEXMAT, LINEAA, PICK).
struct Unit { ivec4 rgb; ivec4 rgbop; ivec4 alpha; ivec4 alphaop; vec4 k; };
#if TEXMAT
layout(location = 0) in vec4 tc;
layout(location = 1) in vec4 tc1;
#define SAMPLE(t, c) textureProj(t, c)
#else
layout(location = 0) in vec2 tc;
layout(location = 1) in vec2 tc1;
#define SAMPLE(t, c) texture(t, c)
#endif
layout(location = 2) in vec4 color;
layout(location = 3) in float fogdist;
#if LINEAA
layout(location = 4) noperspective in vec4 lineaa;
#endif
layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 3, binding = 0) uniform FSU { vec4 fogcolor; vec4 fog; vec4 params; ivec4 units; Unit unit[2]; };
layout(location = 0) out vec4 ocolor;
vec4 source(int s, vec4 t, vec4 p, vec4 k, vec4 c) { return s == 0 ? t : (s == 1 ? p : (s == 2 ? k : c)); }
vec3 argrgb(int s, int op, vec4 t, vec4 p, vec4 k, vec4 c) { vec4 v = source(s, t, p, k, c); return op == 0 ? v.rgb : (op == 1 ? 1.0 - v.rgb : (op == 2 ? vec3(v.a) : vec3(1.0 - v.a))); }
float arga(int s, int op, vec4 t, vec4 p, vec4 k, vec4 c) { vec4 v = source(s, t, p, k, c); return op == 3 ? 1.0 - v.a : v.a; }
vec3 combine3(int f, vec3 a, vec3 b, vec3 c) { return f == 1 ? a : (f == 3 ? a + b : (f == 4 ? a + b - 0.5 : (f == 5 ? a*c + b*(1.0 - c) : (f == 6 ? a - b : (f == 7 ? vec3(4.0*dot(a - 0.5, b - 0.5)) : a*b))))); }
float combine1(int f, float a, float b, float c) { return f == 1 ? a : (f == 3 ? a + b : (f == 4 ? a + b - 0.5 : (f == 5 ? a*c + b*(1.0 - c) : (f == 6 ? a - b : a*b)))); }
vec4 texunit(Unit u, vec4 t, vec4 p, vec4 c)
{
    vec3 rgb = combine3(u.rgb.x, argrgb(u.rgb.y, u.rgbop.x, t, p, u.k, c), argrgb(u.rgb.z, u.rgbop.y, t, p, u.k, c), argrgb(u.rgb.w, u.rgbop.z, t, p, u.k, c)) * float(u.rgbop.w);
    float a = combine1(u.alpha.x, arga(u.alpha.y, u.alphaop.x, t, p, u.k, c), arga(u.alpha.z, u.alphaop.y, t, p, u.k, c), arga(u.alpha.w, u.alphaop.z, t, p, u.k, c)) * float(u.alphaop.w);
    return clamp(vec4(rgb, a), 0.0, 1.0);
}
void main()
{
    vec4 c = color;
    if(units.x != 0) c = texunit(unit[0], SAMPLE(tex0, tc), c, color);
    if(units.y != 0) c = texunit(unit[1], SAMPLE(tex1, tc1), c, color);
#if PICK
    if(params.y >= 0.0 && c.a <= params.y) discard;
    ocolor = vec4(gl_FragCoord.z);
#else
#if LINEAA
    vec4 l = lineaa;
    c.a *= l.y >= 0.0 ? clamp(min(l.x + 0.5, l.y) - max(l.x - 0.5, -l.y), 0.0, 1.0) :
                        clamp(-l.y - abs(l.x), 0.0, 1.0) * clamp(0.5 + min(l.z, l.w - l.z), 0.0, 1.0);
#endif
    if(params.y >= 0.0 && c.a <= params.y) discard;
    if(fog.z > 0.5) c.rgb = mix(fogcolor.rgb, c.rgb, clamp((fog.x - fogdist) * fog.y, 0.0, 1.0));
    ocolor = c;
#endif
}
