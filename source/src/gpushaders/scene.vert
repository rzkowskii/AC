#version 450
// The GPU backend's scene vertex shader for Vulkan, in GLSL 450: the same as mslcommon + mslvertex in gfx_sdlgpu.cpp
// (keep them in step). build.sh compiles it to SPIR-V for each variant (HAS_TC, HAS_COLOR, HAS_TC1, TEXMAT, LINEAA).
// SDL GPU's SPIR-V resource sets: vertex uniforms in set 1, fragment samplers in set 2, fragment uniforms in set 3.
#if TEXMAT
#define TCV vec4
#define TCNONE vec4(0.0, 0.0, 0.0, 1.0)
#define TCOUT(m, t) ((m) * (t))
#else
#define TCV vec2
#define TCNONE vec2(0.0)
#define TCOUT(m, t) (t)
#endif
layout(location = 0) in vec3 pos;
#if HAS_TC
layout(location = 1) in TCV tc;
#endif
#if HAS_COLOR
layout(location = 2) in vec4 color;
#endif
#if HAS_TC1
layout(location = 3) in TCV tc1;
#endif
#if LINEAA
layout(location = 3) in vec4 lineaa;
layout(location = 4) noperspective out vec4 olineaa;
#endif
#if TEXMAT
layout(set = 1, binding = 0) uniform VSU { mat4 mvp; mat4 mv; vec4 ucolor; mat4 tex0; mat4 tex1; };
#else
layout(set = 1, binding = 0) uniform VSU { mat4 mvp; mat4 mv; vec4 ucolor; };
#endif
layout(location = 0) out TCV otc;
layout(location = 1) out TCV otc1;
layout(location = 2) out vec4 ocolor;
layout(location = 3) out float ofogdist;
void main()
{
    vec4 p = vec4(pos, 1.0);
    gl_Position = mvp * p;
#if HAS_TC
    TCV t0 = tc;
#else
    TCV t0 = TCNONE;
#endif
#if HAS_TC1
    TCV t1 = tc1;
#else
    TCV t1 = t0;
#endif
    otc = TCOUT(tex0, t0);
    otc1 = TCOUT(tex1, t1);
#if HAS_COLOR
    ocolor = color;
#else
    ocolor = ucolor;
#endif
    ofogdist = length((mv * p).xyz);
#if LINEAA
    olineaa = lineaa;
#endif
}
