// gfx_sdlgpu.cpp: the SDL3 GPU backend of the renderer interface (gfx.h): Metal on macOS, Vulkan on Linux. Selected
// with --gpu.
//
// It implements the fixed-function GL state the game draws with: the matrix stacks are kept on the CPU, the state that
// a GPU pipeline fixes (blending, depth test, culling, colour mask, vertex layout, primitive type) selects a cached
// pipeline, and the rest (matrices, colour, fog, alpha test, texture combiner) goes into per-draw uniforms of one small
// shader pair, which is the scene shader of shaders.cpp ("worldshader") written for SDL GPU.
//
// Frames are recorded, not drawn immediately: every draw stores its pipeline, texture, uniforms and the location of its
// vertices and indices (vertex arrays in client memory and immediate-mode vertices are copied into a per-frame arena).
// present() then uploads everything in one copy pass, runs the crosshair pick pass in its own command buffer (fenced, so
// the next frame can read the target without waiting for the rest of the frame), and draws the main pass, which is a
// single render pass into an offscreen target that is finally copied to the swapchain.
//
// Copies of the frame into textures (water reflection and refraction, minimap) end the current pass and are blitted
// after it; FXAA and gamma are passes of their own (postpass()). Stencil (stencil shadows) needs stencilbits 8 at start.
//
// Lines are drawn as quads, as Apple's GL rasterizes them (smooth lines with their coverage, any width: widelines()),
// and polygons in GL_LINE mode as their edges (the editor's cursor). In edit mode the crosshair pick pass is read in the
// same frame (the edit cursor needs it), submitting the frame so far and continuing it afterwards.
//
// Left to the GL backend (gfx_legacygl()): display lists, the GLSL programs of shaders.cpp (the GPU backend has its
// own), framebuffer objects for the water reflection (it renders and copies) and asynchronous depth reads.

#include "cube.h"

VAR(gpudebug, 0, 0, 2); // 1: log every pipeline the GPU backend creates; 2: and what every 500th frame contains
extern int benchnopresent;
static void setframesinflight();
VARF(gpuframes, 1, 2, 3, setframesinflight()); // frames the GPU backend lets the CPU queue ahead of the GPU
static void setpresentmode();
VARF(gpupresent, -1, -1, 2, setpresentmode()); // (testing) -1: from "vsync" (see displaychanged()), 0: immediate, 1: vsync, 2: mailbox

// --- column-major 4x4 matrices, as GL computes them

struct gmat
{
    float v[16];
    void identity() { loopi(16) v[i] = (i%5) == 0 ? 1 : 0; }
    void mul(const gmat &a, const gmat &b) // this = a*b
    {
        float r[16];
        loopi(4) loopj(4) r[i*4+j] = a.v[j]*b.v[i*4] + a.v[4+j]*b.v[i*4+1] + a.v[8+j]*b.v[i*4+2] + a.v[12+j]*b.v[i*4+3];
        memcpy(v, r, sizeof(r));
    }
    void mulright(const float *m) { gmat b; memcpy(b.v, m, sizeof(b.v)); gmat a = *this; mul(a, b); } // this = this*m
};

// --- shaders. Metal compiles MSL source at run time. Vulkan needs SPIR-V: the GLSL 450 equivalents below have to be
// compiled offline (glslangValidator -V), which isn't set up yet, so the Vulkan path can't run until then.

static const char *mslcommon =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    // texture coordinates: with TEXMAT (a texture matrix, which only the water's projective reflection lookup uses) all
    // four go through the matrices, are interpolated and divided per fragment; otherwise two, sampled directly (no
    // dependent texture read, and smaller uniforms without the two texture matrices)
    "#if TEXMAT\n"
    "struct VSU { float4x4 mvp; float4x4 mv; float4 color; float4x4 tex0; float4x4 tex1; };\n"
    "#define TCV float4\n"
    "#define TCNONE float4(0.0, 0.0, 0.0, 1.0)\n"
    "#define TCIN(t) ((t).xy / (t).w)\n"
    "#else\n"
    "struct VSU { float4x4 mvp; float4x4 mv; float4 color; };\n"
    "#define TCV float2\n"
    "#define TCNONE float2(0.0)\n"
    "#define TCIN(t) (t)\n"
    "#endif\n"
    // a texture unit's combiner (texture.cpp, encodetmu()): rgb/alpha = (function, source 0, 1, 2), rgbop/alphaop = (operand 0, 1, 2, scale)
    "struct Unit { int4 rgb; int4 rgbop; int4 alpha; int4 alphaop; float4 k; };\n"
    // fog: end, 1/(end-start), on; params: -, alpha test reference (<0: off); units: which texture units are on
    "struct FSU { float4 fogcolor; float4 fog; float4 params; int4 units; Unit unit[2]; };\n"
    "struct VIn\n"
    "{\n"
    "    float3 pos [[attribute(0)]];\n"
    "#if HAS_TC\n"
    "    TCV tc [[attribute(1)]];\n"   // (TEXMAT, 2 or 3 components in the vertex: z = 0, w = 1 are filled in)
    "#endif\n"
    "#if HAS_COLOR\n"
    "    float4 color [[attribute(2)]];\n"
    "#endif\n"
    "#if HAS_TC1\n"
    "    TCV tc1 [[attribute(3)]];\n"
    "#endif\n"
    "#if LINEAA\n"
    "    float2 lineaa [[attribute(3)]];\n" // (smooth lines have no second texture unit's coordinates)
    "#endif\n"
    "};\n"
    // LINEAA: a smooth line drawn as a quad (widelines()): lineaa = this pixel's distance from the line's centre along
    // the minor axis, and half the line's width, both in pixels
    "#if LINEAA\n"
    "#define LINEAAOUT float2 lineaa [[center_no_perspective]];\n"
    "#else\n"
    "#define LINEAAOUT\n"
    "#endif\n"
    "struct VOut { float4 pos [[position]]; TCV tc; TCV tc1; float4 color; float fogdist; LINEAAOUT };\n"
    // GL_ARB_texture_env_combine
    "static float4 source(int s, float4 t, float4 p, float4 k, float4 c) { return s == 0 ? t : (s == 1 ? p : (s == 2 ? k : c)); }\n"
    "static float3 argrgb(int s, int op, float4 t, float4 p, float4 k, float4 c)\n"
    "{\n"
    "    float4 v = source(s, t, p, k, c);\n"
    "    return op == 0 ? v.rgb : (op == 1 ? 1.0 - v.rgb : (op == 2 ? float3(v.a) : float3(1.0 - v.a)));\n"
    "}\n"
    "static float arga(int s, int op, float4 t, float4 p, float4 k, float4 c) { float4 v = source(s, t, p, k, c); return op == 3 ? 1.0 - v.a : v.a; }\n"
    "static float3 combine3(int f, float3 a, float3 b, float3 c)\n"
    "{\n"
    "    switch(f)\n"
    "    {\n"
    "        case 1: return a;\n"
    "        case 3: return a + b;\n"
    "        case 4: return a + b - 0.5;\n"
    "        case 5: return a*c + b*(1.0 - c);\n"
    "        case 6: return a - b;\n"
    "        case 7: return float3(4.0*dot(a - 0.5, b - 0.5));\n"
    "        default: return a*b;\n"
    "    }\n"
    "}\n"
    "static float combine1(int f, float a, float b, float c)\n"
    "{\n"
    "    switch(f)\n"
    "    {\n"
    "        case 1: return a;\n"
    "        case 3: return a + b;\n"
    "        case 4: return a + b - 0.5;\n"
    "        case 5: return a*c + b*(1.0 - c);\n"
    "        case 6: return a - b;\n"
    "        default: return a*b;\n"
    "    }\n"
    "}\n"
    "static float4 texunit(constant Unit &u, float4 t, float4 p, float4 c)\n"
    "{\n"
    "    float3 rgb = combine3(u.rgb.x, argrgb(u.rgb.y, u.rgbop.x, t, p, u.k, c), argrgb(u.rgb.z, u.rgbop.y, t, p, u.k, c), argrgb(u.rgb.w, u.rgbop.z, t, p, u.k, c)) * float(u.rgbop.w);\n"
    "    float a = combine1(u.alpha.x, arga(u.alpha.y, u.alphaop.x, t, p, u.k, c), arga(u.alpha.z, u.alphaop.y, t, p, u.k, c), arga(u.alpha.w, u.alphaop.z, t, p, u.k, c)) * float(u.alphaop.w);\n"
    "    return clamp(float4(rgb, a), 0.0, 1.0);\n"
    "}\n"
    "static float4 shade(VOut in, constant FSU &u, texture2d<float> tex0, sampler s0, texture2d<float> tex1, sampler s1)\n"
    "{\n"
    "    float4 c = in.color;\n"
    "    if(u.units.x != 0) c = texunit(u.unit[0], tex0.sample(s0, TCIN(in.tc)), c, in.color);\n"
    "    if(u.units.y != 0) c = texunit(u.unit[1], tex1.sample(s1, TCIN(in.tc1)), c, in.color);\n"
    "    return c;\n"
    "}\n";

static const char *mslvertex =
    "vertex VOut vmain(VIn in [[stage_in]], constant VSU &u [[buffer(0)]])\n"
    "{\n"
    "    VOut o;\n"
    "    float4 p = float4(in.pos, 1.0);\n"
    "    o.pos = u.mvp * p;\n"
    "#if HAS_TC\n"
    "    TCV tc = in.tc;\n"
    "#else\n"
    "    TCV tc = TCNONE;\n"
    "#endif\n"
    "#if HAS_TC1\n"
    "    TCV tc1 = in.tc1;\n"
    "#else\n"
    "    TCV tc1 = tc;\n"
    "#endif\n"
    "#if TEXMAT\n"
    "    o.tc = u.tex0 * tc;\n"     // the texture matrices (GL_TEXTURE)
    "    o.tc1 = u.tex1 * tc1;\n"
    "#else\n"
    "    o.tc = tc;\n"
    "    o.tc1 = tc1;\n"
    "#endif\n"
    "#if HAS_COLOR\n"
    "    o.color = in.color;\n"
    "#else\n"
    "    o.color = u.color;\n"
    "#endif\n"
    "    o.fogdist = length((u.mv * p).xyz);\n" // radial distance, like shaders.cpp's worldshader
    "#if LINEAA\n"
    "    o.lineaa = in.lineaa;\n"
    "#endif\n"
    "    return o;\n"
    "}\n";

// the texture units' combiners, alpha test, linear fog
static const char *mslfragment =
    "fragment float4 fmain(VOut in [[stage_in]], constant FSU &u [[buffer(0)]], texture2d<float> tex0 [[texture(0)]], texture2d<float> tex1 [[texture(1)]],\n"
    "                      sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]])\n"
    "{\n"
    "    float4 c = shade(in, u, tex0, s0, tex1, s1);\n"
    "#if LINEAA\n" // GL_LINE_SMOOTH: alpha times the pixel row's coverage by the line (before the alpha test, as in GL)
    "    c.a *= clamp(min(in.lineaa.x + 0.5, in.lineaa.y) - max(in.lineaa.x - 0.5, -in.lineaa.y), 0.0, 1.0);\n"
    "#endif\n"
    "    if(u.params.y >= 0.0 && c.a <= u.params.y) discard_fragment();\n"
    "    if(u.fog.z > 0.5) c.rgb = mix(u.fogcolor.rgb, c.rgb, clamp((u.fog.x - in.fogdist) * u.fog.y, 0.0, 1.0));\n"
    "    return c;\n"
    "}\n";

// crosshair pick pass: the depth of the nearest fragment, written to an R32F target (alpha-tested skins still cut out)
static const char *mslpick =
    "fragment float fpick(VOut in [[stage_in]], constant FSU &u [[buffer(0)]], texture2d<float> tex0 [[texture(0)]], texture2d<float> tex1 [[texture(1)]],\n"
    "                     sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]])\n"
    "{\n"
    "    if(u.params.y >= 0.0 && shade(in, u, tex0, s0, tex1, s1).a <= u.params.y) discard_fragment();\n"
    "    return in.pos.z;\n"
    "}\n";

// post-processing (shaders.cpp's gamma and FXAA): a full-screen triangle sampling the scene. "GL" coordinates have their
// origin at the bottom left like the GL versions, so FXAA's sample pattern is the same
static const char *mslpost =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct PostOut { float4 pos [[position]]; float2 tc; };\n"
    "struct PostU { float4 params; };\n" // gamma: exponent; FXAA: texel width, height
    "vertex PostOut vpost(uint vid [[vertex_id]])\n"
    "{\n"
    "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "    PostOut o;\n"
    "    o.pos = float4(p*2.0 - 1.0, 0.0, 1.0);\n"
    "    o.tc = float2(p.x, 1.0 - p.y);\n"
    "    return o;\n"
    "}\n"
    "fragment float4 fgamma(PostOut in [[stage_in]], constant PostU &u [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]])\n"
    "{\n"
    "    return float4(pow(tex.sample(s, in.tc).rgb, float3(u.params.x)), 1.0);\n"
    "}\n"
    "fragment float4 ffxaa(PostOut in [[stage_in]], constant PostU &u [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]])\n"
    "{\n"
    "    float2 tc = float2(in.tc.x, 1.0 - in.tc.y), texel = u.params.xy;\n"
    "    #define S(p) tex.sample(s, float2((p).x, 1.0 - (p).y)).rgb\n"
    "    const float3 luma = float3(0.299, 0.587, 0.114);\n"
    "    float3 rgbNW = S(tc + float2(-1.0, -1.0) * texel), rgbNE = S(tc + float2(1.0, -1.0) * texel);\n"
    "    float3 rgbSW = S(tc + float2(-1.0, 1.0) * texel), rgbSE = S(tc + float2(1.0, 1.0) * texel), rgbM = S(tc);\n"
    "    float lNW = dot(rgbNW, luma), lNE = dot(rgbNE, luma), lSW = dot(rgbSW, luma), lSE = dot(rgbSE, luma), lM = dot(rgbM, luma);\n"
    "    float lmin = min(lM, min(min(lNW, lNE), min(lSW, lSE))), lmax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "    float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
    "    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "    float rcpmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
    "    dir = clamp(dir * rcpmin, float2(-8.0), float2(8.0)) * texel;\n"
    "    float3 rgbA = 0.5 * (S(tc + dir * (1.0 / 3.0 - 0.5)) + S(tc + dir * (2.0 / 3.0 - 0.5)));\n"
    "    float3 rgbB = rgbA * 0.5 + 0.25 * (S(tc - dir * 0.5) + S(tc + dir * 0.5));\n"
    "    float lB = dot(rgbB, luma);\n"
    "    return float4((lB < lmin || lB > lmax) ? rgbA : rgbB, 1.0);\n"
    "}\n";

// The same shaders in GLSL 450 for Vulkan (SDL GPU's SPIR-V resource sets: vertex uniforms in set 1, fragment samplers
// in set 2, fragment uniforms in set 3). Not compiled yet (no SPIR-V compiler in the build); kept in step with the MSL.
const char *gpuglslsources[] =
{
    "#version 450\n"
    "#if TEXMAT\n"
    "#define TCV vec4\n"
    "#define TCNONE vec4(0.0, 0.0, 0.0, 1.0)\n"
    "#define TCOUT(m, t) ((m) * (t))\n"
    "#else\n"
    "#define TCV vec2\n"
    "#define TCNONE vec2(0.0)\n"
    "#define TCOUT(m, t) (t)\n"
    "#endif\n"
    "layout(location = 0) in vec3 pos;\n"
    "#if HAS_TC\n"
    "layout(location = 1) in TCV tc;\n"
    "#endif\n"
    "#if HAS_COLOR\n"
    "layout(location = 2) in vec4 color;\n"
    "#endif\n"
    "#if HAS_TC1\n"
    "layout(location = 3) in TCV tc1;\n"
    "#endif\n"
    "#if LINEAA\n"
    "layout(location = 3) in vec2 lineaa;\n"
    "layout(location = 4) noperspective out vec2 olineaa;\n"
    "#endif\n"
    "#if TEXMAT\n"
    "layout(set = 1, binding = 0) uniform VSU { mat4 mvp; mat4 mv; vec4 ucolor; mat4 tex0; mat4 tex1; };\n"
    "#else\n"
    "layout(set = 1, binding = 0) uniform VSU { mat4 mvp; mat4 mv; vec4 ucolor; };\n"
    "#endif\n"
    "layout(location = 0) out TCV otc;\n"
    "layout(location = 1) out TCV otc1;\n"
    "layout(location = 2) out vec4 ocolor;\n"
    "layout(location = 3) out float ofogdist;\n"
    "void main()\n"
    "{\n"
    "    vec4 p = vec4(pos, 1.0);\n"
    "    gl_Position = mvp * p;\n"
    "#if HAS_TC\n"
    "    TCV t0 = tc;\n"
    "#else\n"
    "    TCV t0 = TCNONE;\n"
    "#endif\n"
    "#if HAS_TC1\n"
    "    TCV t1 = tc1;\n"
    "#else\n"
    "    TCV t1 = t0;\n"
    "#endif\n"
    "    otc = TCOUT(tex0, t0);\n"
    "    otc1 = TCOUT(tex1, t1);\n"
    "#if HAS_COLOR\n"
    "    ocolor = color;\n"
    "#else\n"
    "    ocolor = ucolor;\n"
    "#endif\n"
    "    ofogdist = length((mv * p).xyz);\n"
    "#if LINEAA\n"
    "    olineaa = lineaa;\n"
    "#endif\n"
    "}\n",

    "#version 450\n"
    "struct Unit { ivec4 rgb; ivec4 rgbop; ivec4 alpha; ivec4 alphaop; vec4 k; };\n"
    "#if TEXMAT\n"
    "layout(location = 0) in vec4 tc;\n"
    "layout(location = 1) in vec4 tc1;\n"
    "#define SAMPLE(t, c) textureProj(t, c)\n"
    "#else\n"
    "layout(location = 0) in vec2 tc;\n"
    "layout(location = 1) in vec2 tc1;\n"
    "#define SAMPLE(t, c) texture(t, c)\n"
    "#endif\n"
    "layout(location = 2) in vec4 color;\n"
    "layout(location = 3) in float fogdist;\n"
    "#if LINEAA\n"
    "layout(location = 4) noperspective in vec2 lineaa;\n"
    "#endif\n"
    "layout(set = 2, binding = 0) uniform sampler2D tex0;\n"
    "layout(set = 2, binding = 1) uniform sampler2D tex1;\n"
    "layout(set = 3, binding = 0) uniform FSU { vec4 fogcolor; vec4 fog; vec4 params; ivec4 units; Unit unit[2]; };\n"
    "layout(location = 0) out vec4 ocolor;\n"
    "vec4 source(int s, vec4 t, vec4 p, vec4 k, vec4 c) { return s == 0 ? t : (s == 1 ? p : (s == 2 ? k : c)); }\n"
    "vec3 argrgb(int s, int op, vec4 t, vec4 p, vec4 k, vec4 c) { vec4 v = source(s, t, p, k, c); return op == 0 ? v.rgb : (op == 1 ? 1.0 - v.rgb : (op == 2 ? vec3(v.a) : vec3(1.0 - v.a))); }\n"
    "float arga(int s, int op, vec4 t, vec4 p, vec4 k, vec4 c) { vec4 v = source(s, t, p, k, c); return op == 3 ? 1.0 - v.a : v.a; }\n"
    "vec3 combine3(int f, vec3 a, vec3 b, vec3 c) { return f == 1 ? a : (f == 3 ? a + b : (f == 4 ? a + b - 0.5 : (f == 5 ? a*c + b*(1.0 - c) : (f == 6 ? a - b : (f == 7 ? vec3(4.0*dot(a - 0.5, b - 0.5)) : a*b))))); }\n"
    "float combine1(int f, float a, float b, float c) { return f == 1 ? a : (f == 3 ? a + b : (f == 4 ? a + b - 0.5 : (f == 5 ? a*c + b*(1.0 - c) : (f == 6 ? a - b : a*b)))); }\n"
    "vec4 texunit(Unit u, vec4 t, vec4 p, vec4 c)\n"
    "{\n"
    "    vec3 rgb = combine3(u.rgb.x, argrgb(u.rgb.y, u.rgbop.x, t, p, u.k, c), argrgb(u.rgb.z, u.rgbop.y, t, p, u.k, c), argrgb(u.rgb.w, u.rgbop.z, t, p, u.k, c)) * float(u.rgbop.w);\n"
    "    float a = combine1(u.alpha.x, arga(u.alpha.y, u.alphaop.x, t, p, u.k, c), arga(u.alpha.z, u.alphaop.y, t, p, u.k, c), arga(u.alpha.w, u.alphaop.z, t, p, u.k, c)) * float(u.alphaop.w);\n"
    "    return clamp(vec4(rgb, a), 0.0, 1.0);\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec4 c = color;\n"
    "    if(units.x != 0) c = texunit(unit[0], SAMPLE(tex0, tc), c, color);\n"
    "    if(units.y != 0) c = texunit(unit[1], SAMPLE(tex1, tc1), c, color);\n"
    "#if PICK\n"
    "    if(params.y >= 0.0 && c.a <= params.y) discard;\n"
    "    ocolor = vec4(gl_FragCoord.z);\n"
    "#else\n"
    "#if LINEAA\n"
    "    c.a *= clamp(min(lineaa.x + 0.5, lineaa.y) - max(lineaa.x - 0.5, -lineaa.y), 0.0, 1.0);\n"
    "#endif\n"
    "    if(params.y >= 0.0 && c.a <= params.y) discard;\n"
    "    if(fog.z > 0.5) c.rgb = mix(fogcolor.rgb, c.rgb, clamp((fog.x - fogdist) * fog.y, 0.0, 1.0));\n"
    "    ocolor = c;\n"
    "#endif\n"
    "}\n",
};

struct vsuniforms { float mvp[16], mv[16], color[4], tex0[16], tex1[16]; };
struct gpuunit { int rgb[4], rgbop[4], alpha[4], alphaop[4]; float k[4]; };
struct fsuniforms { float fogcolor[4], fog[4], params[4]; int units[4]; gpuunit unit[2]; };

// --- resources

struct gputexture
{
    SDL_GPUTexture *tex;
    int w, h, levels;
    GLint wraps, wrapt, magfilter, minfilter, aniso;
    bool used, rendertarget;
};

struct gpubuffer
{
    SDL_GPUBuffer *buf;
    uint size;                  // of buf
    vector<uchar> data;         // CPU copy (uploaded when dirty)
    bool dirty, used;
    // streamed buffers (GL_STREAM_DRAW, the world's vertices) can be refilled several times in a frame (minimap,
    // reflection, main view): each version goes into the frame's vertex arena, and a draw uses the one current when
    // it was recorded, as with GL
    bool streamed;
    uint arenaframe, arenaoffset;
};

enum { ATTR_POS = 0, ATTR_TC, ATTR_COLOR, ATTR_TC1, NUMATTRS }; // (ATTR_TC1: texture unit 1's coordinates)
// render targets: the scene (multisampled if antialiased), the crosshair pick target, FXAA (the scene filtered into a
// second texture, gfx->postpass(0)) and what's drawn after FXAA (the HUD, into that second texture, single-sampled)
enum { TARGET_MAIN = 0, TARGET_PICK, TARGET_FXAA, TARGET_POST };

struct gpuarray { bool enabled; GLint size; GLenum type; GLsizei stride; const void *ptr; GLuint buffer; };

// everything a pipeline fixes
struct pipelinekey
{
    uchar target, topology, samples, alphacoverage;
    uchar attrs[NUMATTRS];      // SDL_GPUVertexElementFormat, 0 = absent
    ushort pitch[NUMATTRS];
    uchar blend, srcblend, dstblend, colormask;
    uchar depthtest, depthfunc, depthwrite, cull;
    uchar depthbias, stenciltest, stencilmask, texmatrix;
    uchar sfront[4], sback[4];  // stencil function and fail, depth-fail and pass operations (stencilcode())
    uchar lineaa, pad[3];       // a smooth line's quads (the LINEAA shader variant)
    float biasfactor, biasunits;
};

static inline bool htcmp(const pipelinekey &a, const pipelinekey &b) { return !memcmp(&a, &b, sizeof(pipelinekey)); }
static inline uint hthash(const pipelinekey &k)
{
    const uchar *p = (const uchar *)&k;
    uint h = 5381;
    loopi(sizeof(pipelinekey)) h = ((h<<5)+h)^p[i];
    return h;
}

enum { SRC_NONE = 0, SRC_BUFFER, SRC_ARENA };

struct gpudraw
{
    int pipeline, stencilref;
    bool texmatrix;             // the vertex uniforms include the texture matrices
    uchar vbsrc[NUMATTRS];
    GLuint vbbuffer[NUMATTRS];
    uint vboffset[NUMATTRS];
    uchar ibsrc;                // SRC_BUFFER (16-bit, a GL element buffer) or SRC_ARENA (32-bit)
    GLuint ibbuffer;
    uint firstindex, numindices;
    GLuint tex[2];              // texture units 0 and 1 (0: white)
    int samplerkey[2];
    float viewport[4];          // x, y (from the top), w, h
    int scissor[4];             // x, y (from the top), w, h
    vsuniforms vs;
    fsuniforms fs;
};

// a copy of part of the frame into a texture (glCopyTexSubImage2D), after the pass it's recorded in
struct gpucopy { GLuint tex; int dx, dy, sx, sy, w, h; };

struct gpupass
{
    int target;
    bool clearcolor, cleardepth, clearstencil;
    float color[4];
    int stencilclear;
    int firstdraw, numdraws;
    float depthclear;
    int firstcopy, numcopies;   // copies of the frame into textures after this pass
};

struct samplerkey { GLint wraps, wrapt, magfilter, minfilter, aniso; };

struct sdlgpubackend : gfxdevice
{
    SDL_GPUDevice *dev;
    SDL_Window *window;
    SDL_GPUShaderFormat format;
    int samples, depthbits;
    SDL_GPUTextureFormat depthformat;
    SDL_GPUSampleCount samplecount;

    // GL state
    GLenum matrixmodeidx;
    gmat stacks[6][32];         // modelview, projection, texture units 0-3
    int stackpos[6];
    float curcolor[4], clearcol[4], fogstart, fogend, fogcolor[4];
    bool caps[16], texenabled[4];
    GLenum blendsrc, blenddst, depthfn, cullmode, alphafn;
    float alpharef, offsetfactor, offsetunits;
    bool depthwrite, colormaskbits[4];
    GLenum stencilstate[2][4];  // front and back faces: function, fail, depth-fail and pass operations
    GLint stencilrefv, clearstencilv;
    GLuint stencilmaskv;
    GLenum stencilface;         // GL_EXT_stencil_two_side's active face
    GLenum polymode;            // glPolygonMode: GL_FILL, or GL_LINE (polygons drawn as their edges: the editor's cursor)
    float linewidthv;           // glLineWidth: lines this wide (rounded) or wider are drawn as quads (widelines())
    int stencilbitsv;           // 8 if the depth target has a stencil buffer (stencilbits, for stencil shadows)
    int viewportv[4];
    int activetex, clientunit;
    GLuint boundtex[4];
    GLuint arraybuffer, elementbuffer;
    gpuarray arrays[NUMATTRS];

    // immediate mode
    struct immvert { float pos[3], tc[2], color[4]; };
    vector<immvert> immverts;
    // lines drawn as quads (widelines()): the vertices plus, for smooth lines, the distance from the line's centre
    struct quadvert { float pos[3], tc[2], color[4], aa[2]; };
    vector<quadvert> quadverts;
    GLenum immmode;
    float immtc[2];

    // resources
    vector<gputexture> textures;
    vector<int> freetextures;
    vector<gpubuffer> buffers;
    vector<int> freebuffers;
    SDL_GPUTexture *emptytex;
    vector<SDL_GPUSampler *> samplers;
    vector<samplerkey> samplerkeys;
    hashtable<pipelinekey, int> pipelineindex;
    vector<SDL_GPUGraphicsPipeline *> pipelines;
    vector<pipelinekey> pipelinekeys;
    // shader variants, made when first needed. Vertex: 1, 2, 4 = texture coordinates, colours, unit 1's coordinates in
    // the vertices, 8 = texture matrices, 16 = smooth line; fragment and pick: 1 = texture matrices, 2 = smooth line
    SDL_GPUShader *vshaders[32], *fshaders[4], *pickshaders[4];

    // the frame being recorded
    bool recording;
    bool midframe;              // the frame so far was submitted to read the edit cursor's target: the main target continues
    int widedraw;               // record(): lines drawn as quads from quadverts: 1 = line primitives, 2 = polygon edges
    bool widesmooth;            // ...smooth lines (with their coverage)
    vector<gpupass> passes;
    vector<gpudraw> draws;
    vector<gpucopy> copies;
    int curpass;
    vector<uchar> vertexarena;
    vector<uint> indexarena;
    struct texupload { GLuint tex; int level, w, h; vector<uchar> rgba; };
    vector<texupload *> texuploads;
    uint texuploadbytes;

    // targets
    SDL_GPUTexture *colortex, *msaatex, *depthtex, *pickcolor, *pickdepth, *posttex, *postdepth;
    SDL_GPUTexture *lastfinal;  // the last submitted frame's image (screenshots)
    // post-processing of the frame being recorded: FXAA done (later passes draw into posttex), gamma exponent (1: none)
    bool postfxactive;
    float gammaexponent;
    SDL_GPUShader *postvs, *gammafs, *fxaafs;
    SDL_GPUGraphicsPipeline *fxaapipe, *gammapipe;
    SDL_GPUTextureFormat gammapipeformat;
    SDL_GPUSampler *postsampler;
    int targetw, targeth;
    SDL_GPUBuffer *arenavb, *arenaib;
    uint arenavbsize, arenaibsize;
    uint lastuploadsize, smallframes; // shrinking the arena and upload buffers again (shrinkbuffers())
    SDL_GPUTransferBuffer *pickdownload;
    SDL_GPUFence *pickfence;
    bool pickpending;

    sdlgpubackend() : dev(NULL), window(NULL), format(SDL_GPU_SHADERFORMAT_INVALID), samples(1), depthbits(32),
        depthformat(SDL_GPU_TEXTUREFORMAT_D32_FLOAT), samplecount(SDL_GPU_SAMPLECOUNT_1), matrixmodeidx(0), activetex(0),
        arraybuffer(0), elementbuffer(0), immmode(0), emptytex(NULL), recording(false),
        curpass(-1), texuploadbytes(0), colortex(NULL), msaatex(NULL), depthtex(NULL), pickcolor(NULL), pickdepth(NULL),
        targetw(0), targeth(0), arenavb(NULL), arenaib(NULL), arenavbsize(0), arenaibsize(0), pickdownload(NULL),
        pickfence(NULL), pickpending(false), frameupload(NULL), frameuploadsize(0)
    {
        framecount = 0;
        midframe = false;
        widedraw = 0;
        widesmooth = false;
        lastuploadsize = smallframes = 0;
        stencilbitsv = 0;
        posttex = postdepth = lastfinal = NULL;
        postfxactive = false;
        gammaexponent = 1;
        postvs = gammafs = fxaafs = NULL;
        fxaapipe = gammapipe = NULL;
        gammapipeformat = SDL_GPU_TEXTUREFORMAT_INVALID;
        postsampler = NULL;
        wantvsync = -1;
        presentmode = SDL_GPUPresentMode(-1);
        loopi(32) vshaders[i] = NULL;
        loopi(4) fshaders[i] = pickshaders[i] = NULL;
        resetstate();
        textures.add(); // handle 0: no texture
        memset(&textures[0], 0, sizeof(gputexture));
        textures[0].used = true;
        buffers.add();
        buffers[0].buf = NULL; buffers[0].size = 0; buffers[0].dirty = false; buffers[0].used = true;
        buffers[0].streamed = false; buffers[0].arenaframe = buffers[0].arenaoffset = 0;
    }

    void resetstate()
    {
        loopi(6) { stackpos[i] = 0; stacks[i][0].identity(); }
        matrixmodeidx = 0;
        loopi(4) curcolor[i] = 1;
        loopi(4) clearcol[i] = 0;
        fogstart = 0; fogend = 1;
        loopi(4) fogcolor[i] = 0;
        loopi(16) caps[i] = false;
        loopi(4) texenabled[i] = false;
        caps[capindex(GL_DITHER)] = true;
        blendsrc = GL_ONE; blenddst = GL_ZERO; depthfn = GL_LESS; cullmode = GL_BACK; alphafn = GL_ALWAYS;
        alpharef = 0; offsetfactor = offsetunits = 0;
        depthwrite = true;
        loopi(4) colormaskbits[i] = true;
        loopi(4) viewportv[i] = scissorv[i] = 0;
        cleardepthv = 1;
        loopi(2) { stencilstate[i][0] = GL_ALWAYS; loopj(3) stencilstate[i][1+j] = GL_KEEP; }
        stencilrefv = clearstencilv = 0;
        stencilmaskv = ~0U;
        stencilface = GL_FRONT;
        polymode = GL_FILL;
        linewidthv = 1;
        activetex = clientunit = 0;
        loopi(4) boundtex[i] = 0;
        loopi(NUMATTRS) { memset(&arrays[i], 0, sizeof(gpuarray)); }
    }

    int api() const { return GFX_SDLGPU; }
    const char *name() const { return "SDL GPU"; }

    // --- device and targets

    bool init(SDL_Window *w, int wantsamples, int vsync)
    {
        window = w;
        dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
        if(!dev) { conoutf("\f3SDL GPU: no device (%s)", SDL_GetError()); return false; }
        format = SDL_GetGPUShaderFormats(dev);
        if(!(format & SDL_GPU_SHADERFORMAT_MSL))
        {
            conoutf("\f3SDL GPU: the %s driver needs SPIR-V shaders, which this build doesn't have yet", SDL_GetGPUDeviceDriver(dev));
            SDL_DestroyGPUDevice(dev);
            dev = NULL;
            return false;
        }
        if(!SDL_ClaimWindowForGPUDevice(dev, window))
        {
            conoutf("\f3SDL GPU: can't use the window (%s)", SDL_GetError());
            SDL_DestroyGPUDevice(dev);
            dev = NULL;
            return false;
        }
        wantvsync = vsync;
        displaychanged();
        SDL_SetGPUAllowedFramesInFlight(dev, gpuframes);

        // depth: 32-bit float (Apple's GL gives 32 bits too), else 24-bit; with an 8-bit stencil buffer if stencilbits
        // asks for one (stencil shadows, shadow.cpp)
        static const struct { SDL_GPUTextureFormat format; int depth, stencil; } depthformats[] =
        {
            { SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, 32, 8 }, { SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, 24, 8 },
            { SDL_GPU_TEXTUREFORMAT_D32_FLOAT, 32, 0 }, { SDL_GPU_TEXTUREFORMAT_D24_UNORM, 24, 0 }
        };
        depthformat = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
        depthbits = 24;
        stencilbitsv = 0;
        loopi(sizeof(depthformats)/sizeof(depthformats[0]))
        {
            if(depthformats[i].stencil && !stencilbits) continue;
            if(!SDL_GPUTextureSupportsFormat(dev, depthformats[i].format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) continue;
            depthformat = depthformats[i].format;
            depthbits = depthformats[i].depth;
            stencilbitsv = depthformats[i].stencil;
            break;
        }
        samples = 1;
        samplecount = SDL_GPU_SAMPLECOUNT_1;
        if(wantsamples >= 2)
        {
            SDL_GPUSampleCount want = wantsamples >= 8 ? SDL_GPU_SAMPLECOUNT_8 : (wantsamples >= 4 ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_2);
            for(; want > SDL_GPU_SAMPLECOUNT_1; want = SDL_GPUSampleCount(want - 1))
            {
                if(SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, want) &&
                   SDL_GPUTextureSupportsSampleCount(dev, depthformat, want)) break;
            }
            samplecount = want;
            samples = 1 << int(want);
        }

        // the texture of units without one (texture 0, or one without an image): black, as GL samples an incomplete texture
        SDL_GPUTextureCreateInfo ti;
        memset(&ti, 0, sizeof(ti));
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = ti.height = 1;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        emptytex = SDL_CreateGPUTexture(dev, &ti);
        texupload *u = new texupload;
        u->tex = (GLuint)-1; u->level = 0; u->w = u->h = 1;
        loopi(3) u->rgba.add(0);
        u->rgba.add(255);
        texuploads.add(u);
        texuploadbytes += 4;

        conoutf("renderer: SDL GPU (%s), %d bit depth, %dx multisampling", SDL_GetGPUDeviceDriver(dev), depthbits, samples);
        return true;
    }

    // How frames reach the display: with vsync unless "vsync 0" (then immediately, or mailbox where that's all there
    // is). Measured on a 144 Hz display, with the window in front: vsync is paced to within 0.1 ms in a window and in
    // fullscreen, while presenting immediately in a window misses refreshes (frames of 13 ms) because the compositor
    // still shows one frame per refresh. "gpupresent" overrides the mode for testing.
    int wantvsync;
    SDL_GPUPresentMode presentmode;

    void displaychanged()
    {
        if(!dev || !window) return;
        bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
        if(gpupresent >= 0) mode = SDL_GPUPresentMode(gpupresent == 0 ? SDL_GPU_PRESENTMODE_IMMEDIATE : (gpupresent == 1 ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_MAILBOX));
        else if(wantvsync == 0)
        {
            if(SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
            else if(SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_MAILBOX)) mode = SDL_GPU_PRESENTMODE_MAILBOX;
        }
        if(mode == presentmode) return;
        if(!SDL_SetGPUSwapchainParameters(dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
        {
            conoutf("\f3SDL GPU: present mode %d not set: %s", int(mode), SDL_GetError());
            return;
        }
        presentmode = mode;
        clientlogf("SDL GPU: %s, vsync %d: presenting %s", fullscreen ? "fullscreen" : "window", wantvsync,
            mode == SDL_GPU_PRESENTMODE_VSYNC ? "with vsync" : (mode == SDL_GPU_PRESENTMODE_IMMEDIATE ? "immediately" : "in mailbox mode"));
    }

    int getdepthbits() { return depthbits; }
    int getstencilbits() { return stencilbitsv; }
    int getsamples() { return samples; }

    void releasetargets()
    {
        if(colortex) SDL_ReleaseGPUTexture(dev, colortex);
        if(msaatex) SDL_ReleaseGPUTexture(dev, msaatex);
        if(depthtex) SDL_ReleaseGPUTexture(dev, depthtex);
        if(posttex) SDL_ReleaseGPUTexture(dev, posttex);
        if(postdepth) SDL_ReleaseGPUTexture(dev, postdepth);
        colortex = msaatex = depthtex = posttex = postdepth = lastfinal = NULL;
        targetw = targeth = 0;
    }

    void setuppostrargets()
    {
        if(posttex) return;
        posttex = createtarget(targetw, targeth, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, SDL_GPU_SAMPLECOUNT_1);
        postdepth = createtarget(targetw, targeth, depthformat, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, SDL_GPU_SAMPLECOUNT_1);
    }

    SDL_GPUShader *makepostshader(const char *entry, SDL_GPUShaderStage stage, int samplers, int uniforms)
    {
        SDL_GPUShaderCreateInfo si;
        memset(&si, 0, sizeof(si));
        si.code = (const Uint8 *)mslpost;
        si.code_size = strlen(mslpost);
        si.entrypoint = entry;
        si.format = SDL_GPU_SHADERFORMAT_MSL;
        si.stage = stage;
        si.num_samplers = samplers;
        si.num_uniform_buffers = uniforms;
        SDL_GPUShader *sh = SDL_CreateGPUShader(dev, &si);
        if(!sh) conoutf("\f3SDL GPU: shader %s failed to compile: %s", entry, SDL_GetError());
        return sh;
    }

    SDL_GPUGraphicsPipeline *postpipeline(SDL_GPUShader *fs, SDL_GPUTextureFormat format)
    {
        if(!postvs) postvs = makepostshader("vpost", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        if(!postvs || !fs) return NULL;
        SDL_GPUColorTargetDescription ctd;
        memset(&ctd, 0, sizeof(ctd));
        ctd.format = format;
        SDL_GPUGraphicsPipelineCreateInfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.vertex_shader = postvs;
        pi.fragment_shader = fs;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.rasterizer_state.enable_depth_clip = true;
        pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pi.target_info.color_target_descriptions = &ctd;
        pi.target_info.num_color_targets = 1;
        SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if(!p) conoutf("\f3SDL GPU: post-processing pipeline failed: %s", SDL_GetError());
        if(!postsampler)
        {
            SDL_GPUSamplerCreateInfo si;
            memset(&si, 0, sizeof(si));
            si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
            si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            postsampler = SDL_CreateGPUSampler(dev, &si);
        }
        return p;
    }

    // a full-screen pass: src through a post-processing shader into dst
    void runpostpass(SDL_GPUCommandBuffer *cmd, SDL_GPUGraphicsPipeline *pipe, SDL_GPUTexture *src, SDL_GPUTexture *dst, const float *params)
    {
        if(!pipe || !src || !dst) return;
        SDL_GPUColorTargetInfo ct;
        memset(&ct, 0, sizeof(ct));
        ct.texture = dst;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
        if(!rp) return;
        SDL_BindGPUGraphicsPipeline(rp, pipe);
        SDL_GPUTextureSamplerBinding tsb = { src, postsampler };
        SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
        float u[4] = { params[0], params[1], params[2], params[3] };
        SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof(u));
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
    }

    // shaders.cpp's post-processing: 0 = FXAA of the scene (before the HUD), 1 = gamma (applied when the frame is shown)
    bool postpass(int kind, float param)
    {
        if(!dev) return false;
        beginrecording();
        if(kind == 1) { gammaexponent = param; return true; }
        if(postfxactive) return true;
        if(!fxaafs) fxaafs = makepostshader("ffxaa", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        if(!fxaapipe) fxaapipe = postpipeline(fxaafs, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        if(!fxaapipe) return false;
        if(curpass < 0 && passes.empty()) openpass(TARGET_MAIN); // (an empty scene)
        curpass = -1;
        gpupass &p = openpass(TARGET_FXAA);
        p.clearcolor = p.cleardepth = false;
        curpass = -1;
        postfxactive = true; // the HUD and anything after it go into the filtered image
        return true;
    }

    int maintarget() { return postfxactive ? TARGET_POST : TARGET_MAIN; }

    SDL_GPUTexture *createtarget(int w, int h, SDL_GPUTextureFormat fmt, SDL_GPUTextureUsageFlags usage, SDL_GPUSampleCount sc)
    {
        SDL_GPUTextureCreateInfo ti;
        memset(&ti, 0, sizeof(ti));
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = fmt;
        ti.usage = usage;
        ti.width = w;
        ti.height = h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        ti.sample_count = sc;
        return SDL_CreateGPUTexture(dev, &ti);
    }

    void setuptargets(int w, int h)
    {
        if(colortex && targetw == w && targeth == h) return;
        releasetargets();
        colortex = createtarget(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, SDL_GPU_SAMPLECOUNT_1);
        if(samples > 1) msaatex = createtarget(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, samplecount);
        depthtex = createtarget(w, h, depthformat, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, samplecount);
        targetw = w;
        targeth = h;
        if(postfxactive) setuppostrargets();
        if(!pickcolor)
        {
            pickcolor = createtarget(1, 1, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, SDL_GPU_SAMPLECOUNT_1);
            pickdepth = createtarget(1, 1, depthformat, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, SDL_GPU_SAMPLECOUNT_1);
            SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, 256, 0 };
            pickdownload = SDL_CreateGPUTransferBuffer(dev, &tbi);
        }
    }

    // --- shaders and pipelines

    SDL_GPUShader *makeshader(const char *body, const char *entry, SDL_GPUShaderStage stage, int variant, int samplers, int uniforms)
    {
        defformatstring(defs)("#define HAS_TC %d\n#define HAS_COLOR %d\n#define HAS_TC1 %d\n#define TEXMAT %d\n#define LINEAA %d\n",
            variant&1 ? 1 : 0, variant&2 ? 1 : 0, variant&4 ? 1 : 0, variant&8 ? 1 : 0, variant&16 ? 1 : 0);
        vector<char> src;
        src.put(defs, strlen(defs));
        src.put(mslcommon, strlen(mslcommon));
        src.put(body, strlen(body));
        src.add('\0');
        SDL_GPUShaderCreateInfo si;
        memset(&si, 0, sizeof(si));
        si.code = (const Uint8 *)src.getbuf();
        si.code_size = src.length() - 1;
        si.entrypoint = entry;
        si.format = SDL_GPU_SHADERFORMAT_MSL;
        si.stage = stage;
        si.num_samplers = samplers;
        si.num_uniform_buffers = uniforms;
        SDL_GPUShader *s = SDL_CreateGPUShader(dev, &si);
        if(!s) conoutf("\f3SDL GPU: shader %s failed to compile: %s", entry, SDL_GetError());
        return s;
    }

    SDL_GPUShader *vertexshader(int variant)
    {
        if(!vshaders[variant]) vshaders[variant] = makeshader(mslvertex, "vmain", SDL_GPU_SHADERSTAGE_VERTEX, variant, 0, 1);
        return vshaders[variant];
    }
    SDL_GPUShader *fragmentshader(bool pick, int variant) // variant: 1 = texture matrices, 2 = smooth line
    {
        SDL_GPUShader *&s = (pick ? pickshaders : fshaders)[variant];
        if(!s) s = makeshader(pick ? mslpick : mslfragment, pick ? "fpick" : "fmain", SDL_GPU_SHADERSTAGE_FRAGMENT, 7 | (variant&1 ? 8 : 0) | (variant&2 ? 16 : 0), 2, 1);
        return s;
    }

    static SDL_GPUBlendFactor blendfactor(GLenum f)
    {
        switch(f)
        {
            case GL_ZERO: return SDL_GPU_BLENDFACTOR_ZERO;
            case GL_ONE: return SDL_GPU_BLENDFACTOR_ONE;
            case GL_SRC_COLOR: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
            case GL_ONE_MINUS_SRC_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
            case GL_DST_COLOR: return SDL_GPU_BLENDFACTOR_DST_COLOR;
            case GL_ONE_MINUS_DST_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
            case GL_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            case GL_ONE_MINUS_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            case GL_DST_ALPHA: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
            case GL_ONE_MINUS_DST_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
            case GL_SRC_ALPHA_SATURATE: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
            default: return SDL_GPU_BLENDFACTOR_ONE;
        }
    }

    static SDL_GPUCompareOp compareop(GLenum f)
    {
        switch(f)
        {
            case GL_NEVER: return SDL_GPU_COMPAREOP_NEVER;
            case GL_LESS: return SDL_GPU_COMPAREOP_LESS;
            case GL_EQUAL: return SDL_GPU_COMPAREOP_EQUAL;
            case GL_LEQUAL: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
            case GL_GREATER: return SDL_GPU_COMPAREOP_GREATER;
            case GL_NOTEQUAL: return SDL_GPU_COMPAREOP_NOT_EQUAL;
            case GL_GEQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
            default: return SDL_GPU_COMPAREOP_ALWAYS;
        }
    }

    int getpipeline(const pipelinekey &k)
    {
        int *idx = pipelineindex.access(k);
        if(idx) return *idx;

        SDL_GPUVertexBufferDescription vbd[NUMATTRS];
        SDL_GPUVertexAttribute va[NUMATTRS];
        int n = 0;
        loopi(NUMATTRS) if(k.attrs[i])
        {
            vbd[n].slot = i;
            vbd[n].pitch = k.pitch[i];
            vbd[n].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            vbd[n].instance_step_rate = 0;
            va[n].location = i;
            va[n].buffer_slot = i;
            va[n].format = SDL_GPUVertexElementFormat(k.attrs[i]);
            va[n].offset = 0;
            n++;
        }
        int variant = (k.attrs[ATTR_TC] ? 1 : 0) | (k.attrs[ATTR_COLOR] ? 2 : 0) | (k.attrs[ATTR_TC1] && !k.lineaa ? 4 : 0) | (k.texmatrix ? 8 : 0) | (k.lineaa ? 16 : 0);

        SDL_GPUColorTargetDescription ctd;
        memset(&ctd, 0, sizeof(ctd));
        ctd.format = k.target == TARGET_PICK ? SDL_GPU_TEXTUREFORMAT_R32_FLOAT : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        if(k.target == TARGET_PICK) ctd.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R;
        else
        {
            ctd.blend_state.enable_blend = k.blend != 0;
            ctd.blend_state.src_color_blendfactor = ctd.blend_state.src_alpha_blendfactor = blendfactor(k.srcblend == 0xFF ? GL_ONE : GLenum(k.srcblend + (k.srcblend >= 0x10 ? 0x300 - 0x10 : 0)));
            ctd.blend_state.dst_color_blendfactor = ctd.blend_state.dst_alpha_blendfactor = blendfactor(k.dstblend == 0xFF ? GL_ONE : GLenum(k.dstblend + (k.dstblend >= 0x10 ? 0x300 - 0x10 : 0)));
            ctd.blend_state.color_blend_op = ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            ctd.blend_state.color_write_mask = k.colormask;
            ctd.blend_state.enable_color_write_mask = true;
        }

        SDL_GPUGraphicsPipelineCreateInfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.vertex_shader = vertexshader(variant);
        pi.fragment_shader = fragmentshader(k.target == TARGET_PICK, (k.texmatrix ? 1 : 0) | (k.lineaa ? 2 : 0));
        if(!pi.vertex_shader || !pi.fragment_shader) return -1;
        pi.vertex_input_state.vertex_buffer_descriptions = vbd;
        pi.vertex_input_state.num_vertex_buffers = n;
        pi.vertex_input_state.vertex_attributes = va;
        pi.vertex_input_state.num_vertex_attributes = n;
        pi.primitive_type = SDL_GPUPrimitiveType(k.topology);
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = k.cull == 1 ? SDL_GPU_CULLMODE_FRONT : (k.cull == 2 ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE);
        pi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pi.rasterizer_state.enable_depth_bias = k.depthbias != 0;
        pi.rasterizer_state.depth_bias_constant_factor = k.biasunits;
        pi.rasterizer_state.depth_bias_slope_factor = k.biasfactor;
        pi.rasterizer_state.enable_depth_clip = true;
        pi.multisample_state.sample_count = k.samples > 1 ? samplecount : SDL_GPU_SAMPLECOUNT_1;
        pi.multisample_state.enable_alpha_to_coverage = k.alphacoverage != 0;
        pi.depth_stencil_state.enable_depth_test = true;
        pi.depth_stencil_state.compare_op = k.depthtest ? compareop(GLenum(GL_NEVER + k.depthfunc)) : SDL_GPU_COMPAREOP_ALWAYS;
        pi.depth_stencil_state.enable_depth_write = k.depthtest && k.depthwrite;
        if(k.stenciltest)
        {
            pi.depth_stencil_state.enable_stencil_test = true;
            loopi(2)
            {
                const uchar *f = i ? k.sback : k.sfront;
                SDL_GPUStencilOpState &o = i ? pi.depth_stencil_state.back_stencil_state : pi.depth_stencil_state.front_stencil_state;
                o.compare_op = compareop(GLenum(GL_NEVER + f[0]));
                o.fail_op = stencilgpuop(f[1]);
                o.depth_fail_op = stencilgpuop(f[2]);
                o.pass_op = stencilgpuop(f[3]);
            }
            pi.depth_stencil_state.compare_mask = k.stencilmask;
            pi.depth_stencil_state.write_mask = 0xFF;
        }
        pi.target_info.color_target_descriptions = &ctd;
        pi.target_info.num_color_targets = 1;
        pi.target_info.depth_stencil_format = depthformat;
        pi.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if(!p) conoutf("\f3SDL GPU: pipeline creation failed: %s", SDL_GetError());
        else if(gpudebug) conoutf("SDL GPU: pipeline %d (target %d, topology %d, attributes %d/%d/%d/%d, blend %d, depth %d/%d/%d, cull %d)",
            pipelines.length(), k.target, k.topology, k.attrs[0], k.attrs[1], k.attrs[2], k.attrs[3], k.blend, k.depthtest, k.depthfunc, k.depthwrite, k.cull);
        pipelines.add(p);
        pipelinekeys.add(k);
        pipelineindex[k] = pipelines.length() - 1;
        return p ? pipelines.length() - 1 : -1;
    }

    static uchar packblend(GLenum f) { return f <= 1 ? uchar(f) : (f >= 0x300 && f <= 0x308 ? uchar(f - 0x300 + 0x10) : 0xFF); }

    // --- GL state

    int capindex(GLenum cap)
    {
        switch(cap)
        {
            case GL_TEXTURE_2D: return 0;
            case GL_FOG: return 1;
            case GL_BLEND: return 2;
            case GL_ALPHA_TEST: return 3;
            case GL_CULL_FACE: return 4;
            case GL_DEPTH_TEST: return 5;
            case GL_SAMPLE_ALPHA_TO_COVERAGE: return 6;
            case GL_POLYGON_OFFSET_FILL: return 7;
            case GL_POLYGON_OFFSET_LINE: return 8;
            case GL_LINE_SMOOTH: return 9;
            case GL_SCISSOR_TEST: return 10;
            case GL_STENCIL_TEST: return 11;
            case GL_DITHER: return 12;
            case GL_STENCIL_TEST_TWO_SIDE_EXT: return 13;
            default: return 15;
        }
    }

    void enable(GLenum cap) { if(cap == GL_TEXTURE_2D) texenabled[activetex] = true; else caps[capindex(cap)] = true; }
    void disable(GLenum cap) { if(cap == GL_TEXTURE_2D) texenabled[activetex] = false; else caps[capindex(cap)] = false; }
    bool isenabled(GLenum cap) { return cap == GL_TEXTURE_2D ? texenabled[activetex] : caps[capindex(cap)]; }
    void blendfunc(GLenum src, GLenum dst) { blendsrc = src; blenddst = dst; }
    void depthfunc(GLenum func) { depthfn = func; }
    void depthmask(GLboolean on) { depthwrite = on != GL_FALSE; }
    void colormask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { colormaskbits[0] = r; colormaskbits[1] = g; colormaskbits[2] = b; colormaskbits[3] = a; }
    void cullface(GLenum face) { cullmode = face; }
    void alphafunc(GLenum func, GLfloat ref) { alphafn = func; alpharef = ref; }
    void polygonoffset(GLfloat factor, GLfloat units) { offsetfactor = factor; offsetunits = units; }
    void linewidth(GLfloat w) { linewidthv = w; }
    void polygonmode(GLenum face, GLenum mode) { polymode = mode == GL_LINE ? GL_LINE : GL_FILL; } // (GL_POINT: filled)
    void color3f(GLfloat r, GLfloat g, GLfloat b) { curcolor[0] = r; curcolor[1] = g; curcolor[2] = b; curcolor[3] = 1; }
    void color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { curcolor[0] = r; curcolor[1] = g; curcolor[2] = b; curcolor[3] = a; }
    void color3ub(GLubyte r, GLubyte g, GLubyte b) { color3f(r/255.0f, g/255.0f, b/255.0f); }
    void color4ubv(const GLubyte *v) { color4f(v[0]/255.0f, v[1]/255.0f, v[2]/255.0f, v[3]/255.0f); }
    void color4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { color4f(r/255.0f, g/255.0f, b/255.0f, a/255.0f); }
    void fogi(GLenum pname, GLint v) { fogf(pname, float(v)); }
    void fogf(GLenum pname, GLfloat v) { if(pname == GL_FOG_START) fogstart = v; else if(pname == GL_FOG_END) fogend = v; }
    void fogfv(GLenum pname, const GLfloat *v) { if(pname == GL_FOG_COLOR) loopi(4) fogcolor[i] = v[i]; else if(v) fogf(pname, v[0]); }
    void hint(GLenum target, GLenum mode) {}
    void shademodel(GLenum mode) {}
    float cleardepthv;
    void cleardepth(GLdouble d) { cleardepthv = float(d); }
    void texenvi(GLenum pname, GLint v) {}             // the combiners are read from texture.cpp's tmus (tmuismodulate2x)
    void texenvfv(GLenum pname, const GLfloat *v) {}
    void activetexture(GLenum unit) { activetex = clamp(int(unit - GL_TEXTURE0_ARB), 0, 3); }
    void clientactivetexture(GLenum unit) { clientunit = clamp(int(unit - GL_TEXTURE0_ARB), 0, 3); }
    void getintegerv(GLenum pname, GLint *v)
    {
        switch(pname)
        {
            case GL_MAX_TEXTURE_SIZE: *v = 16384; break;
            case GL_MAX_TEXTURE_UNITS_ARB: *v = 4; break;
            case GL_SAMPLE_BUFFERS: *v = samples > 1 ? 1 : 0; break;
            case GL_VIEWPORT: loopi(4) v[i] = viewportv[i]; break;
            default: *v = 0; break;
        }
    }

    void viewport(int x, int y, int w, int h) { viewportv[0] = x; viewportv[1] = y; viewportv[2] = w; viewportv[3] = h; }
    int scissorv[4];
    void scissor(int x, int y, int w, int h) { scissorv[0] = x; scissorv[1] = y; scissorv[2] = w; scissorv[3] = h; }
    void clearcolor(float r, float g, float b, float a) { clearcol[0] = r; clearcol[1] = g; clearcol[2] = b; clearcol[3] = a; }
    void clearstencil(int s) { clearstencilv = s; }

    // stencil (stencil shadows): the faces stencilfunc() and stencilop() set, both or the active one
    void stencilfaces(int &first, int &last)
    {
        if(caps[capindex(GL_STENCIL_TEST_TWO_SIDE_EXT)]) first = last = stencilface == GL_BACK ? 1 : 0;
        else { first = 0; last = 1; }
    }
    void stencilfunc(GLenum func, GLint ref, GLuint mask)
    {
        int first, last;
        stencilfaces(first, last);
        for(int i = first; i <= last; i++) stencilstate[i][0] = func;
        stencilrefv = ref; // (one reference and compare mask for both faces: all the GPU APIs have)
        stencilmaskv = mask;
    }
    void stencilop(GLenum fail, GLenum zfail, GLenum zpass)
    {
        int first, last;
        stencilfaces(first, last);
        for(int i = first; i <= last; i++) { stencilstate[i][1] = fail; stencilstate[i][2] = zfail; stencilstate[i][3] = zpass; }
    }
    void stencilfuncseparate(GLenum frontfunc, GLenum backfunc, GLint ref, GLuint mask)
    {
        stencilstate[0][0] = frontfunc;
        stencilstate[1][0] = backfunc;
        stencilrefv = ref;
        stencilmaskv = mask;
    }
    void stencilopseparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass)
    {
        loopi(2) if(face == GL_FRONT_AND_BACK || face == (i ? GL_BACK : GL_FRONT))
        {
            stencilstate[i][1] = fail; stencilstate[i][2] = zfail; stencilstate[i][3] = zpass;
        }
    }
    void activestencilface(GLenum face) { stencilface = face; }

    static uchar stencilcode(GLenum op)
    {
        switch(op)
        {
            case GL_ZERO: return 1;
            case GL_REPLACE: return 2;
            case GL_INCR: return 3;
            case GL_DECR: return 4;
            case GL_INVERT: return 5;
            case GL_INCR_WRAP_EXT: return 6;
            case GL_DECR_WRAP_EXT: return 7;
            default: return 0; // GL_KEEP
        }
    }
    static SDL_GPUStencilOp stencilgpuop(uchar code)
    {
        static const SDL_GPUStencilOp ops[8] =
        {
            SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_ZERO, SDL_GPU_STENCILOP_REPLACE, SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP,
            SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP, SDL_GPU_STENCILOP_INVERT, SDL_GPU_STENCILOP_INCREMENT_AND_WRAP,
            SDL_GPU_STENCILOP_DECREMENT_AND_WRAP
        };
        return ops[code&7];
    }
    void finish() { if(dev) SDL_WaitForGPUIdle(dev); }

    // --- matrices

    // (GL_TEXTURE applies to the texture unit active at the time of each call)
    int curstack() { return matrixmodeidx == 2 ? 2 + activetex : matrixmodeidx; }
    gmat &top() { return stacks[curstack()][stackpos[curstack()]]; }
    void matrixmode(GLenum mode) { matrixmodeidx = mode == GL_PROJECTION ? 1 : (mode == GL_TEXTURE ? 2 : 0); }
    void loadidentity() { top().identity(); }
    void pushmatrix() { int k = curstack(), &p = stackpos[k]; if(p < 31) { stacks[k][p+1] = stacks[k][p]; p++; } }
    void popmatrix() { int &p = stackpos[curstack()]; if(p > 0) p--; }
    void loadmatrix(const GLfloat *m) { memcpy(top().v, m, sizeof(top().v)); }
    void multmatrix(const GLfloat *m) { top().mulright(m); }
    void translate(GLfloat x, GLfloat y, GLfloat z)
    {
        float m[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x, y, z, 1 };
        top().mulright(m);
    }
    void rotate(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
    {
        float len = sqrtf(x*x + y*y + z*z);
        if(len <= 0) return;
        x /= len; y /= len; z /= len;
        float a = angle*RAD, c = cosf(a), s = sinf(a), t = 1 - c;
        float m[16] =
        {
            x*x*t + c,   y*x*t + z*s, x*z*t - y*s, 0,
            x*y*t - z*s, y*y*t + c,   y*z*t + x*s, 0,
            x*z*t + y*s, y*z*t - x*s, z*z*t + c,   0,
            0,           0,           0,           1
        };
        top().mulright(m);
    }
    void scale(GLfloat x, GLfloat y, GLfloat z)
    {
        float m[16] = { x, 0, 0, 0,  0, y, 0, 0,  0, 0, z, 0,  0, 0, 0, 1 };
        top().mulright(m);
    }
    void frustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
    {
        float m[16] =
        {
            float(2*n/(r-l)), 0, 0, 0,
            0, float(2*n/(t-b)), 0, 0,
            float((r+l)/(r-l)), float((t+b)/(t-b)), float(-(f+n)/(f-n)), -1,
            0, 0, float(-2*f*n/(f-n)), 0
        };
        top().mulright(m);
    }
    void ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
    {
        float m[16] =
        {
            float(2/(r-l)), 0, 0, 0,
            0, float(2/(t-b)), 0, 0,
            0, 0, float(-2/(f-n)), 0,
            float(-(r+l)/(r-l)), float(-(t+b)/(t-b)), float(-(f+n)/(f-n)), 1
        };
        top().mulright(m);
    }
    void getfloatv(GLenum pname, GLfloat *v)
    {
        switch(pname)
        {
            case GL_MODELVIEW_MATRIX: memcpy(v, stacks[0][stackpos[0]].v, 16*sizeof(float)); break;
            case GL_PROJECTION_MATRIX: memcpy(v, stacks[1][stackpos[1]].v, 16*sizeof(float)); break;
            case GL_CURRENT_COLOR: loopi(4) v[i] = curcolor[i]; break;
            case GL_LINE_WIDTH: v[0] = linewidthv; break;
            default: v[0] = 0; break;
        }
    }

    // --- textures

    void gentextures(GLsizei n, GLuint *t)
    {
        loopi(n)
        {
            int idx;
            if(freetextures.length()) idx = freetextures.pop();
            else { idx = textures.length(); textures.add(); }
            gputexture &g = textures[idx];
            memset(&g, 0, sizeof(g));
            g.used = true;
            g.wraps = g.wrapt = GL_REPEAT;
            g.magfilter = GL_LINEAR;
            g.minfilter = GL_NEAREST_MIPMAP_LINEAR;
            t[i] = idx;
        }
    }

    void deletetextures(GLsizei n, const GLuint *t)
    {
        loopi(n)
        {
            GLuint idx = t[i];
            if(!idx || idx >= (GLuint)textures.length() || !textures[idx].used) continue;
            gputexture &g = textures[idx];
            if(g.tex) SDL_ReleaseGPUTexture(dev, g.tex);
            g.tex = NULL;
            g.used = false;
            loopvj(texuploads) if(texuploads[j]->tex == idx) { texuploadbytes -= texuploads[j]->rgba.length(); delete texuploads.remove(j--); }
            freetextures.add(idx);
            loopj(4) if(boundtex[j] == idx) boundtex[j] = 0;
        }
    }

    void bindtexture(GLenum target, GLuint t) { boundtex[activetex] = t; }

    void texparameteri(GLenum target, GLenum pname, GLint v)
    {
        GLuint idx = boundtex[activetex];
        if(!idx || idx >= (GLuint)textures.length()) return;
        gputexture &g = textures[idx];
        switch(pname)
        {
            case GL_TEXTURE_WRAP_S: g.wraps = v; break;
            case GL_TEXTURE_WRAP_T: g.wrapt = v; break;
            case GL_TEXTURE_MAG_FILTER: g.magfilter = v; break;
            case GL_TEXTURE_MIN_FILTER: g.minfilter = v; break;
            case GL_TEXTURE_MAX_ANISOTROPY_EXT: g.aniso = v; break;
        }
    }

    void pixelstorei(GLenum pname, GLint v) {}

    static bool mipmapped(GLint minfilter) { return minfilter != GL_NEAREST && minfilter != GL_LINEAR; }

    SDL_GPUTexture *newtexture(int w, int h, int levels, bool rendertarget)
    {
        SDL_GPUTextureCreateInfo ti;
        memset(&ti, 0, sizeof(ti));
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (rendertarget ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
        ti.width = w;
        ti.height = h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = levels;
        return SDL_CreateGPUTexture(dev, &ti);
    }

    // texture data in any of the game's formats, as RGBA8 (SDL GPU has no RGB or luminance formats)
    static void torgba(vector<uchar> &out, int w, int h, GLenum format, const uchar *src)
    {
        uchar *d = out.pad(w*h*4);
        int n = w*h;
        switch(format)
        {
            case GL_RGBA: memcpy(d, src, n*4); break;
            case GL_RGB: loopi(n) { d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = 255; d += 4; src += 3; } break;
            case GL_LUMINANCE: loopi(n) { d[0] = d[1] = d[2] = src[0]; d[3] = 255; d += 4; src++; } break;
            case GL_ALPHA: loopi(n) { d[0] = d[1] = d[2] = 255; d[3] = src[0]; d += 4; src++; } break;
            case GL_LUMINANCE_ALPHA: loopi(n) { d[0] = d[1] = d[2] = src[0]; d[3] = src[1]; d += 4; src += 2; } break;
            default: memset(d, 255, n*4); break;
        }
    }

    void teximage2d(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels)
    {
        GLuint idx = boundtex[activetex];
        if(!idx || idx >= (GLuint)textures.length() || w <= 0 || h <= 0) return;
        gputexture &g = textures[idx];
        if(level == 0)
        {
            int levels = 1;
            if(mipmapped(g.minfilter)) for(int s = max(w, h); s > 1; s /= 2) levels++;
            if(!g.tex || g.w != w || g.h != h || g.levels != levels)
            {
                if(g.tex) SDL_ReleaseGPUTexture(dev, g.tex);
                g.tex = newtexture(w, h, levels, g.rendertarget);
                g.w = w; g.h = h; g.levels = levels;
            }
        }
        if(!g.tex || level >= g.levels) return;
        texupload *u = new texupload;
        u->tex = idx; u->level = level; u->w = w; u->h = h;
        if(pixels) torgba(u->rgba, w, h, format, (const uchar *)pixels);
        else { u->rgba.pad(w*h*4); memset(u->rgba.getbuf(), 0, w*h*4); }
        texuploads.add(u);
        texuploadbytes += u->rgba.length();
        if(!recording && texuploadbytes > (64<<20)) flushuploads(); // loading: don't keep too much in memory
    }

    void texsubimage2d(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels)
    {
        // (only the light grid uses this, and it's GL-only for now)
    }

    // glCopyTexSubImage2D: part of the frame (as drawn so far) into the bound texture. Recorded after the current pass,
    // which ends there; executed as a blit flipped vertically, so the texture's rows are bottom-up as in GL (its users
    // compute texture coordinates the GL way). A blit needs a render target, so the texture becomes one on first use.
    void copytexsubimage2d(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei w, GLsizei h)
    {
        GLuint idx = boundtex[activetex];
        if(!idx || idx >= (GLuint)textures.length() || !textures[idx].tex || level || w <= 0 || h <= 0) return;
        gputexture &g = textures[idx];
        if(!g.rendertarget)
        {
            SDL_ReleaseGPUTexture(dev, g.tex);
            g.tex = newtexture(g.w, g.h, g.levels, true);
            g.rendertarget = true;
        }
        beginrecording();
        if(curpass < 0) openpass(maintarget());
        if(passes[curpass].target != TARGET_MAIN && passes[curpass].target != TARGET_POST) return;
        gpucopy &c = copies.add();
        c.tex = idx; c.dx = xoffset; c.dy = yoffset; c.sx = x; c.sy = y; c.w = w; c.h = h;
        passes[curpass].numcopies++;
        curpass = -1; // later draws go into a new pass that loads what's there
    }

    // --- buffer objects

    void genbuffers(GLsizei n, GLuint *b)
    {
        loopi(n)
        {
            int idx;
            if(freebuffers.length()) idx = freebuffers.pop();
            else { idx = buffers.length(); buffers.add(); }
            gpubuffer &g = buffers[idx];
            g.buf = NULL; g.size = 0; g.data.setsize(0); g.dirty = false; g.used = true;
            g.streamed = false; g.arenaframe = 0; g.arenaoffset = 0;
            b[i] = idx;
        }
    }

    void deletebuffers(GLsizei n, const GLuint *b)
    {
        loopi(n)
        {
            GLuint idx = b[i];
            if(!idx || idx >= (GLuint)buffers.length() || !buffers[idx].used) continue;
            gpubuffer &g = buffers[idx];
            if(g.buf) SDL_ReleaseGPUBuffer(dev, g.buf);
            g.buf = NULL; g.size = 0; g.data.setsize(0); g.dirty = false; g.used = false;
            freebuffers.add(idx);
            if(arraybuffer == idx) arraybuffer = 0;
            if(elementbuffer == idx) elementbuffer = 0;
        }
    }

    void bindbuffer(GLenum target, GLuint b)
    {
        if(target == GL_ARRAY_BUFFER_ARB) arraybuffer = b;
        else if(target == GL_ELEMENT_ARRAY_BUFFER_ARB) elementbuffer = b;
    }

    gpubuffer *boundbuffer(GLenum target)
    {
        GLuint idx = target == GL_ARRAY_BUFFER_ARB ? arraybuffer : (target == GL_ELEMENT_ARRAY_BUFFER_ARB ? elementbuffer : 0);
        return idx && idx < (GLuint)buffers.length() ? &buffers[idx] : NULL;
    }

    void bufferdata(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
    {
        gpubuffer *g = boundbuffer(target);
        if(!g) return;
        g->data.setsize(0);
        uchar *d = g->data.pad(size);
        if(data) memcpy(d, data, size); else memset(d, 0, size);
        g->streamed = usage == GL_STREAM_DRAW_ARB && target == GL_ARRAY_BUFFER_ARB;
        g->dirty = !g->streamed;
        g->arenaframe = 0; // (put into this frame's arena when a draw uses it)
    }

    // where a streamed buffer's current contents are in this frame's vertex arena
    uint streamedoffset(gpubuffer &g)
    {
        if(g.arenaframe != framecount)
        {
            g.arenaoffset = arenaput(g.data.getbuf(), g.data.length());
            g.arenaframe = framecount;
        }
        return g.arenaoffset;
    }

    void buffersubdata(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
    {
        gpubuffer *g = boundbuffer(target);
        if(!g || offset + size > g->data.length()) return;
        memcpy(g->data.getbuf() + offset, data, size);
        g->dirty = true;
    }

    // --- vertex arrays

    int arrayindex(GLenum array)
    {
        switch(array)
        {
            case GL_VERTEX_ARRAY: return ATTR_POS;
            case GL_COLOR_ARRAY: return ATTR_COLOR;
            case GL_TEXTURE_COORD_ARRAY: return clientunit == 0 ? ATTR_TC : (clientunit == 1 ? ATTR_TC1 : -1);
            default: return -1;
        }
    }
    void enableclientstate(GLenum array) { int i = arrayindex(array); if(i >= 0) arrays[i].enabled = true; }
    void disableclientstate(GLenum array) { int i = arrayindex(array); if(i >= 0) arrays[i].enabled = false; }
    void setarray(int i, GLint size, GLenum type, GLsizei stride, const void *p)
    {
        gpuarray &a = arrays[i];
        a.size = size; a.type = type; a.ptr = p; a.buffer = arraybuffer;
        a.stride = stride ? stride : size*(type == GL_UNSIGNED_BYTE ? 1 : 4);
    }
    void vertexpointer(GLint size, GLenum type, GLsizei stride, const void *p) { setarray(ATTR_POS, size, type, stride, p); }
    void colorpointer(GLint size, GLenum type, GLsizei stride, const void *p) { setarray(ATTR_COLOR, size, type, stride, p); }
    void normalpointer(GLenum type, GLsizei stride, const void *p) {} // (no lighting: normals are unused)
    void texcoordpointer(GLint size, GLenum type, GLsizei stride, const void *p) { if(clientunit <= 1) setarray(clientunit ? ATTR_TC1 : ATTR_TC, size, type, stride, p); }

    static SDL_GPUVertexElementFormat elementformat(int attr, GLint size, GLenum type)
    {
        if(type == GL_UNSIGNED_BYTE) return size == 4 ? SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM : SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM;
        switch(size)
        {
            case 1: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
            case 2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            case 3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            default: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        }
    }

    static int elementbytes(GLint size, GLenum type) { return size * (type == GL_UNSIGNED_BYTE ? 1 : 4); }

    // --- recording

    uint framecount;

    void beginrecording()
    {
        if(recording) return;
        recording = true;
        framecount++;
        postfxactive = false;
        gammaexponent = 1;
        passes.setsize(0);
        draws.setsize(0);
        copies.setsize(0);
        curpass = -1;
        vertexarena.setsize(0);
        indexarena.setsize(0);
    }

    gpupass &openpass(int target)
    {
        beginrecording();
        if(curpass >= 0 && passes[curpass].target == target) return passes[curpass];
        // a target's first pass in a frame always clears (on tile-based GPUs that's cheaper than loading the old contents)
        // (not after a submission in the middle of the frame: the main target continues what was drawn before it)
        bool first = !(midframe && target == TARGET_MAIN);
        loopv(passes) if(passes[i].target == target) first = false;
        gpupass &p = passes.add();
        p.target = target;
        p.clearcolor = p.cleardepth = p.clearstencil = first;
        p.stencilclear = clearstencilv;
        if(target == TARGET_POST) p.clearcolor = false; // (it starts with the FXAA output)
        loopi(4) p.color[i] = target == TARGET_PICK ? 1.0f : clearcol[i];
        p.firstdraw = draws.length();
        p.depthclear = cleardepthv;
        p.firstcopy = copies.length();
        p.numcopies = 0;
        p.numdraws = 0;
        curpass = passes.length() - 1;
        return p;
    }

    void clear(GLbitfield mask)
    {
        if(!(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))) return;
        beginrecording();
        if(curpass >= 0 && passes[curpass].target == TARGET_PICK) return; // (cleared when the pick pass begins)
        if(curpass < 0 || passes[curpass].numdraws)
        {
            curpass = -1;
            gpupass &p = openpass(maintarget());
            if(mask & GL_COLOR_BUFFER_BIT) p.clearcolor = true;
            if(mask & GL_DEPTH_BUFFER_BIT) p.cleardepth = true;
            if(mask & GL_STENCIL_BUFFER_BIT) { p.clearstencil = true; p.stencilclear = clearstencilv; }
        }
        else
        {
            gpupass &p = passes[curpass];
            if(mask & GL_COLOR_BUFFER_BIT) { p.clearcolor = true; loopi(4) p.color[i] = clearcol[i]; }
            if(mask & GL_DEPTH_BUFFER_BIT) p.cleardepth = true;
            if(mask & GL_STENCIL_BUFFER_BIT) { p.clearstencil = true; p.stencilclear = clearstencilv; }
        }
        if(mask & GL_COLOR_BUFFER_BIT) loopi(4) passes[curpass].color[i] = clearcol[i];
    }

    uint arenaput(const void *data, int len)
    {
        int off = (vertexarena.length() + 15) & ~15;
        vertexarena.pad(off + len - vertexarena.length());
        memcpy(vertexarena.getbuf() + off, data, len);
        return off;
    }

    int getsampler(const gputexture &t)
    {
        samplerkey k = { t.wraps, t.wrapt, t.magfilter, t.levels > 1 ? t.minfilter : (t.minfilter == GL_NEAREST ? GL_NEAREST : GL_LINEAR), t.levels > 1 ? t.aniso : 0 };
        loopv(samplerkeys) if(!memcmp(&samplerkeys[i], &k, sizeof(k))) return i;
        SDL_GPUSamplerCreateInfo si;
        memset(&si, 0, sizeof(si));
        si.mag_filter = k.magfilter == GL_NEAREST ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
        bool minlinear = k.minfilter == GL_LINEAR || k.minfilter == GL_LINEAR_MIPMAP_NEAREST || k.minfilter == GL_LINEAR_MIPMAP_LINEAR;
        si.min_filter = minlinear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = k.minfilter == GL_NEAREST_MIPMAP_LINEAR || k.minfilter == GL_LINEAR_MIPMAP_LINEAR ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = k.wraps == GL_REPEAT ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = k.wrapt == GL_REPEAT ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        si.min_lod = 0;
        si.max_lod = mipmapped(k.minfilter) ? 1000.0f : 0.0f;
        si.enable_anisotropy = k.aniso > 1;
        si.max_anisotropy = k.aniso > 1 ? float(k.aniso) : 1.0f;
        SDL_GPUSampler *s = SDL_CreateGPUSampler(dev, &si);
        samplers.add(s);
        samplerkeys.add(k);
        return samplers.length() - 1;
    }

    // records a draw with the current state. The vertices of every array come from [base, base + numverts); indices
    // are relative to base, either 32-bit in the index arena (firstindex: where they start) or 16-bit in the element
    // buffer bound now (ibuffer).
    void record(GLenum mode, int base, int numverts, uint firstindex, uint numindices, bool ibuffer, uint ibufferoffset, bool immediate = false)
    {
        if(!numindices || !dev) return;
        gpupass &pass = openpass(curpass >= 0 ? passes[curpass].target : maintarget());
        gpudraw &d = draws.add();
        memset(&d, 0, sizeof(d));
        pipelinekey k;
        memset(&k, 0, sizeof(k));
        k.target = pass.target;
        bool picking = pass.target == TARGET_PICK;

        // vertex sources
        if(immediate)
        {
            // (quadverts start with the same fields as immverts)
            ushort stride = widedraw ? sizeof(quadvert) : sizeof(immvert);
            uint off = widedraw ? arenaput(quadverts.getbuf(), quadverts.length()*sizeof(quadvert)) : arenaput(immverts.getbuf(), immverts.length()*sizeof(immvert));
            k.attrs[ATTR_POS] = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; k.pitch[ATTR_POS] = stride;
            d.vbsrc[ATTR_POS] = SRC_ARENA; d.vboffset[ATTR_POS] = off + offsetof(immvert, pos);
            k.attrs[ATTR_TC] = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; k.pitch[ATTR_TC] = stride;
            d.vbsrc[ATTR_TC] = SRC_ARENA; d.vboffset[ATTR_TC] = off + offsetof(immvert, tc);
            k.attrs[ATTR_COLOR] = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; k.pitch[ATTR_COLOR] = stride;
            d.vbsrc[ATTR_COLOR] = SRC_ARENA; d.vboffset[ATTR_COLOR] = off + offsetof(immvert, color);
            if(widedraw && widesmooth)
            {
                k.attrs[ATTR_TC1] = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; k.pitch[ATTR_TC1] = stride;
                d.vbsrc[ATTR_TC1] = SRC_ARENA; d.vboffset[ATTR_TC1] = off + offsetof(quadvert, aa);
                k.lineaa = 1;
            }
        }
        else loopi(NUMATTRS)
        {
            gpuarray &a = arrays[i];
            if(!a.enabled || (i == ATTR_TC && !texenabled[0]) || (i == ATTR_TC1 && !texenabled[1])) continue;
            k.attrs[i] = elementformat(i, a.size, a.type);
            k.pitch[i] = a.stride;
            if(a.buffer && a.buffer < (GLuint)buffers.length() && buffers[a.buffer].streamed)
            {
                d.vbsrc[i] = SRC_ARENA;
                d.vboffset[i] = streamedoffset(buffers[a.buffer]) + uint((uintptr_t)a.ptr) + base*a.stride;
            }
            else if(a.buffer)
            {
                d.vbsrc[i] = SRC_BUFFER;
                d.vbbuffer[i] = a.buffer;
                d.vboffset[i] = uint((uintptr_t)a.ptr) + base*a.stride;
            }
            else
            {
                d.vbsrc[i] = SRC_ARENA;
                d.vboffset[i] = arenaput((const uchar *)a.ptr + base*a.stride, (numverts - 1)*a.stride + elementbytes(a.size, a.type));
            }
        }
        if(!k.attrs[ATTR_POS]) { draws.pop(); return; }

        // primitives (everything without a GL element buffer has been turned into lists already; polygons in GL_LINE
        // mode into their edges, wide lines into quads)
        bool edges = drawsedges(mode);
        if(widedraw) k.topology = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        else if(edges) k.topology = SDL_GPU_PRIMITIVETYPE_LINELIST;
        else switch(mode)
        {
            case GL_TRIANGLE_STRIP: k.topology = ibuffer ? SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; break;
            case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: k.topology = SDL_GPU_PRIMITIVETYPE_LINELIST; break;
            case GL_POINTS: k.topology = SDL_GPU_PRIMITIVETYPE_POINTLIST; break;
            default: k.topology = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; break;
        }
        d.ibsrc = ibuffer ? SRC_BUFFER : SRC_ARENA;
        d.ibbuffer = ibuffer ? elementbuffer : 0;
        d.firstindex = ibuffer ? ibufferoffset/sizeof(ushort) : firstindex;
        d.numindices = numindices;

        // fixed-function state
        bool multisampled = pass.target == TARGET_MAIN && samples > 1;
        k.samples = multisampled ? samples : 1;
        k.alphacoverage = multisampled && caps[capindex(GL_SAMPLE_ALPHA_TO_COVERAGE)] && caps[capindex(GL_ALPHA_TEST)];
        k.blend = !picking && caps[capindex(GL_BLEND)];
        k.srcblend = k.blend ? packblend(blendsrc) : 0;
        k.dstblend = k.blend ? packblend(blenddst) : 0;
        k.colormask = picking ? 1 : (colormaskbits[0] ? 1 : 0) | (colormaskbits[1] ? 2 : 0) | (colormaskbits[2] ? 4 : 0) | (colormaskbits[3] ? 8 : 0);
        k.depthtest = caps[capindex(GL_DEPTH_TEST)];
        k.depthfunc = k.depthtest ? uchar(depthfn - GL_NEVER) : 0;
        k.depthwrite = k.depthtest && depthwrite;
        // (culling applies to filled polygons only: the edges of culled polygons in GL_LINE mode aren't left out; the
        // editor draws them with culling off)
        bool polygons = k.topology == SDL_GPU_PRIMITIVETYPE_TRIANGLELIST || k.topology == SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        k.cull = caps[capindex(GL_CULL_FACE)] && polygons && !widedraw ? (cullmode == GL_FRONT ? 1 : (cullmode == GL_BACK ? 2 : 0)) : 0;
        // GL's polygon offset: GL_POLYGON_OFFSET_FILL for filled polygons, GL_POLYGON_OFFSET_LINE for polygons drawn as
        // lines, none for line primitives
        bool offset = widedraw ? widedraw == 2 && caps[capindex(GL_POLYGON_OFFSET_LINE)] :
                      (edges ? caps[capindex(GL_POLYGON_OFFSET_LINE)] : polygons && caps[capindex(GL_POLYGON_OFFSET_FILL)]);
        if(offset) { k.depthbias = 1; k.biasfactor = offsetfactor; k.biasunits = offsetunits; }
        if(caps[capindex(GL_STENCIL_TEST)] && stencilbitsv && !picking)
        {
            k.stenciltest = 1;
            k.stencilmask = uchar(stencilmaskv);
            loopi(2)
            {
                uchar *f = i ? k.sback : k.sfront;
                f[0] = uchar(stencilstate[i][0] - GL_NEVER);
                loopj(3) f[1+j] = stencilcode(stencilstate[i][1+j]);
            }
        }
        d.stencilref = clamp(int(stencilrefv), 0, 255);
        // the shader variant with texture matrices and projective coordinates: a unit with a texture matrix that isn't
        // the identity (only the water's reflection lookup has one), or four texture coordinates per vertex
        loopi(2) if(texenabled[i])
        {
            const float *m = stacks[2+i][stackpos[2+i]].v;
            int a = i && k.attrs[ATTR_TC1] ? ATTR_TC1 : ATTR_TC;
            bool identity = true;
            loopj(16) if(m[j] != ((j%5) == 0 ? 1.0f : 0.0f)) { identity = false; break; }
            if(!identity || k.attrs[a] == SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4) k.texmatrix = 1;
        }
        d.texmatrix = k.texmatrix;
        d.pipeline = getpipeline(k);
        if(d.pipeline < 0) { draws.pop(); return; }

        // textures (units 0 and 1, with their combiners) and uniforms
        loopi(2)
        {
            GLuint t = texenabled[i] ? boundtex[i] : 0;
            if(t && (t >= (GLuint)textures.length() || !textures[t].tex)) t = 0;
            d.tex[i] = t;
            d.samplerkey[i] = getsampler(textures[t]);
            d.fs.units[i] = texenabled[i] ? 1 : 0;
            extern void encodetmu(int n, int *rgb, int *alpha, float *color);
            gpuunit &u = d.fs.unit[i];
            int rgb[8], alpha[8];
            encodetmu(i, rgb, alpha, u.k);
            loopj(4) { u.rgb[j] = rgb[j]; u.alpha[j] = alpha[j]; }
            loopj(3) { u.rgbop[j] = rgb[4+j]; u.alphaop[j] = alpha[4+j]; }
            u.rgbop[3] = rgb[7]; u.alphaop[3] = alpha[7];
        }

        gmat proj = stacks[1][stackpos[1]], mv = stacks[0][stackpos[0]], zfix, pm, mvp;
        zfix.identity();
        zfix.v[10] = 0.5f; zfix.v[14] = 0.5f; // GL's clip-space z [-w, w] to SDL GPU's [0, w]
        pm.mul(zfix, proj);
        mvp.mul(pm, mv);
        memcpy(d.vs.mvp, mvp.v, sizeof(d.vs.mvp));
        memcpy(d.vs.mv, mv.v, sizeof(d.vs.mv));
        loopi(4) d.vs.color[i] = curcolor[i];
        memcpy(d.vs.tex0, stacks[2][stackpos[2]].v, sizeof(d.vs.tex0));
        memcpy(d.vs.tex1, stacks[3][stackpos[3]].v, sizeof(d.vs.tex1));
        loopi(4) d.fs.fogcolor[i] = fogcolor[i];
        d.fs.fog[0] = fogend;
        d.fs.fog[1] = fogend != fogstart ? 1.0f/(fogend - fogstart) : 0;
        d.fs.fog[2] = !picking && caps[capindex(GL_FOG)] ? 1 : 0;
        d.fs.params[1] = caps[capindex(GL_ALPHA_TEST)] && alphafn == GL_GREATER ? alpharef : -1;

        // viewport, from the top
        int th = picking ? 1 : targeth;
        d.viewport[0] = viewportv[0];
        d.viewport[1] = th - (viewportv[1] + viewportv[3]);
        d.viewport[2] = viewportv[2];
        d.viewport[3] = viewportv[3];
        if(caps[capindex(GL_SCISSOR_TEST)])
        {
            int x1 = max(scissorv[0], 0), y1 = max(th - (scissorv[1] + scissorv[3]), 0),
                x2 = min(scissorv[0] + scissorv[2], picking ? 1 : targetw), y2 = min(th - scissorv[1], th);
            d.scissor[0] = x1; d.scissor[1] = y1; d.scissor[2] = max(x2 - x1, 0); d.scissor[3] = max(y2 - y1, 0);
        }
        else { d.scissor[0] = d.scissor[1] = 0; d.scissor[2] = picking ? 1 : targetw; d.scissor[3] = th; }
        if(!d.scissor[2] || !d.scissor[3]) { draws.pop(); return; }
        pass.numdraws++;
    }

    static bool ispolygon(GLenum mode)
    {
        return mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN || mode == GL_QUADS || mode == GL_POLYGON;
    }
    bool drawsedges(GLenum mode) const { return polymode == GL_LINE && ispolygon(mode); }

    // the edges of polygons (GL_LINE polygon mode), as GL draws them: each triangle's three, a quad's or polygon's outline
    // (not the diagonals they're split into for filling)
    void listedges(GLenum mode, int o, int count)
    {
        #define EDGE(a, b) { indexarena.add(o+(a)); indexarena.add(o+(b)); }
        switch(mode)
        {
            case GL_TRIANGLES: for(int i = 0; i + 2 < count; i += 3) { EDGE(i, i+1); EDGE(i+1, i+2); EDGE(i+2, i); } break;
            case GL_TRIANGLE_STRIP: for(int i = 0; i + 2 < count; i++) { EDGE(i, i+1); EDGE(i+1, i+2); EDGE(i+2, i); } break;
            case GL_TRIANGLE_FAN: for(int i = 1; i + 1 < count; i++) { EDGE(0, i); EDGE(i, i+1); EDGE(i+1, 0); } break;
            case GL_QUADS: for(int i = 0; i + 3 < count; i += 4) loopj(4) EDGE(i+j, i+(j+1)%4); break;
            case GL_POLYGON: if(count >= 3) loopi(count) EDGE(i, (i+1)%count); break;
        }
        #undef EDGE
    }

    // GL primitives as 32-bit index lists (relative to the first vertex)
    void listindices(GLenum mode, int first, int count, int base)
    {
        int o = first - base;
        if(drawsedges(mode)) { listedges(mode, o, count); return; }
        switch(mode)
        {
            case GL_TRIANGLES: case GL_LINES: case GL_POINTS:
                loopi(count) indexarena.add(o + i);
                break;
            case GL_TRIANGLE_STRIP:
                for(int i = 0; i + 2 < count; i++)
                {
                    if(i&1) { indexarena.add(o+i+1); indexarena.add(o+i); indexarena.add(o+i+2); }
                    else { indexarena.add(o+i); indexarena.add(o+i+1); indexarena.add(o+i+2); }
                }
                break;
            case GL_TRIANGLE_FAN: case GL_POLYGON:
                for(int i = 1; i + 1 < count; i++) { indexarena.add(o); indexarena.add(o+i); indexarena.add(o+i+1); }
                break;
            case GL_QUADS:
                for(int i = 0; i + 3 < count; i += 4)
                {
                    indexarena.add(o+i); indexarena.add(o+i+1); indexarena.add(o+i+3);
                    indexarena.add(o+i+1); indexarena.add(o+i+2); indexarena.add(o+i+3);
                }
                break;
            case GL_LINE_STRIP:
                for(int i = 0; i + 1 < count; i++) { indexarena.add(o+i); indexarena.add(o+i+1); }
                break;
            case GL_LINE_LOOP:
                for(int i = 0; i < count; i++) { indexarena.add(o+i); indexarena.add(o+(i+1)%count); }
                break;
        }
    }

    void drawarrays(GLenum mode, GLint first, GLsizei count)
    {
        multidrawarrays(mode, &first, &count, 1);
    }

    void multidrawarrays(GLenum mode, const GLint *first, const GLsizei *count, GLsizei n)
    {
        if(n <= 0) return;
        beginrecording();
        int lo = first[0], hi = first[0] + count[0];
        loopi(n) { lo = min(lo, first[i]); hi = max(hi, first[i] + count[i]); }
        // vertices in buffer objects are used where they are; client arrays are copied from the lowest vertex
        bool clientarrays = false;
        loopi(NUMATTRS) if(arrays[i].enabled && !arrays[i].buffer) clientarrays = true;
        int base = clientarrays ? lo : 0;
        uint start = indexarena.length();
        loopi(n) listindices(mode, first[i], count[i], base);
        record(mode, base, hi - base, start, indexarena.length() - start, false, 0);
    }

    template<class T> void elementindices(const T *idx, GLsizei count, GLenum mode, int base)
    {
        // as if drawn with glDrawArrays over the index list
        uint start = indexarena.length();
        vector<uint> tmp;
        loopi(count) tmp.add(uint(idx[i]) - base);
        indexarena.setsize(start);
        uint s0 = indexarena.length();
        listindices(mode, 0, count, 0);
        for(uint i = s0; i < (uint)indexarena.length(); i++) indexarena[i] = tmp[indexarena[i]];
    }

    void drawelements(GLenum mode, GLsizei count, GLenum type, const void *indices)
    {
        drawrangeelements(mode, 0, 0, count, type, indices);
    }

    void drawrangeelements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices)
    {
        if(count <= 0) return;
        beginrecording();
        gpubuffer *eb = boundbuffer(GL_ELEMENT_ARRAY_BUFFER_ARB);
        if(eb && type == GL_UNSIGNED_SHORT && (mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP) && !drawsedges(mode))
        {
            // indices in a buffer object: drawn from there (vertices from buffer objects too)
            record(mode, 0, 0, 0, count, true, uint((uintptr_t)indices));
            return;
        }
        const uchar *ip = eb ? eb->data.getbuf() + (uintptr_t)indices : (const uchar *)indices;
        if(!ip) return;
        uint lo = ~0U, hi = 0;
        loopi(count)
        {
            uint v = type == GL_UNSIGNED_SHORT ? ((const ushort *)ip)[i] : (type == GL_UNSIGNED_INT ? ((const uint *)ip)[i] : ip[i]);
            lo = min(lo, v); hi = max(hi, v);
        }
        bool clientarrays = false;
        loopi(NUMATTRS) if(arrays[i].enabled && !arrays[i].buffer) clientarrays = true;
        int base = clientarrays ? lo : 0;
        uint first = indexarena.length();
        if(type == GL_UNSIGNED_SHORT) elementindices((const ushort *)ip, count, mode, base);
        else if(type == GL_UNSIGNED_INT) elementindices((const uint *)ip, count, mode, base);
        else elementindices(ip, count, mode, base);
        record(mode == GL_TRIANGLE_STRIP ? GL_TRIANGLES : mode, base, hi - base + 1, first, indexarena.length() - first, false, 0);
    }

    // --- immediate mode

    void begin(GLenum mode) { immmode = mode; immverts.setsize(0); }
    void texcoord2f(GLfloat s, GLfloat t) { immtc[0] = s; immtc[1] = t; }
    void texcoord2i(GLint s, GLint t) { texcoord2f(float(s), float(t)); }
    void vertex3f(GLfloat x, GLfloat y, GLfloat z)
    {
        immvert &v = immverts.add();
        v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
        v.tc[0] = immtc[0]; v.tc[1] = immtc[1];
        loopi(4) v.color[i] = curcolor[i];
    }
    void vertex2f(GLfloat x, GLfloat y) { vertex3f(x, y, 0); }
    void vertex3fv(const GLfloat *v) { vertex3f(v[0], v[1], v[2]); }
    void end()
    {
        if(immverts.empty()) return;
        beginrecording();
        uint start = indexarena.length();
        listindices(immmode, 0, immverts.length(), 0);
        bool lines = drawsedges(immmode) || immmode == GL_LINES || immmode == GL_LINE_STRIP || immmode == GL_LINE_LOOP;
        if(lines && (caps[capindex(GL_LINE_SMOOTH)] || int(linewidthv + 0.5f) >= 2)) widelines(start, drawsedges(immmode));
        // immediate-mode vertices always carry a texture coordinate and a colour (the current ones)
        else record(immmode, 0, immverts.length(), start, indexarena.length() - start, false, 0, true);
        immverts.setsize(0);
    }

    // Lines as GL draws them where Metal and Vulkan can't (Apple's GL, measured with a test program):
    // - smooth lines (GL_LINE_SMOOTH, which the game always enables): fragments wherever a band (max(w, 1) + 1) / 2
    //   pixels either side of the line along its minor axis covers a pixel centre (or sample), with alpha times the
    //   pixel row's overlap with [-w/2, w/2] - so with blending off, a 1-pixel line colours two rows;
    // - aliased lines 2 pixels or wider: a band of round(w) pixels along the minor axis.
    // Each segment of the index list from start becomes a quad (a parallelogram: the band, not extended past the ends).
    // The corners are computed in eye space, so depth, fog and clipping work as for the line itself; segments are
    // clipped to the near plane first. (Immediate mode only: the game draws all its lines that way.)
    void widelines(uint start, bool edges)
    {
        const gmat &mv = stacks[0][stackpos[0]], &proj = stacks[1][stackpos[1]];
        glmatrixf p, pinv;
        memcpy(p.v, proj.v, sizeof(p.v));
        if(!pinv.invert(p)) { indexarena.setsize(start); return; }
        bool smooth = caps[capindex(GL_LINE_SMOOTH)];
        float w = clamp(linewidthv, 0.0f, 16.0f), band = smooth ? (max(w, 1.0f) + 1) * 0.5f : int(w + 0.5f) * 0.5f, cover = w * 0.5f,
              sx = max(viewportv[2], 1) * 0.5f, sy = max(viewportv[3], 1) * 0.5f;
        quadverts.setsize(0);
        for(int i = start; i + 1 < indexarena.length(); i += 2)
        {
            immvert v[2] = { immverts[indexarena[i]], immverts[indexarena[i+1]] };
            vec4 e[2], c[2];
            loopj(2)
            {
                const float *a = v[j].pos;
                loopk(4) e[j][k] = mv.v[k]*a[0] + mv.v[4+k]*a[1] + mv.v[8+k]*a[2] + mv.v[12+k];
                loopk(4) c[j][k] = p.v[k]*e[j].x + p.v[4+k]*e[j].y + p.v[8+k]*e[j].z + p.v[12+k]*e[j].w;
            }
            // near plane (GL clip space: z >= -w)
            float d0 = c[0].z + c[0].w, d1 = c[1].z + c[1].w;
            if(d0 < 0 && d1 < 0) continue;
            loopj(2) if((j ? d1 : d0) < 0)
            {
                float t = (j ? d1 : d0) / ((j ? d1 : d0) - (j ? d0 : d1));
                immvert &a = v[j], &b = v[1-j];
                loopk(3) a.pos[k] += (b.pos[k] - a.pos[k]) * t;
                loopk(2) a.tc[k] += (b.tc[k] - a.tc[k]) * t;
                loopk(4) a.color[k] += (b.color[k] - a.color[k]) * t;
                loopk(4) { e[j][k] += (e[1-j][k] - e[j][k]) * t; c[j][k] += (c[1-j][k] - c[j][k]) * t; }
            }
            if(c[0].w <= 1e-6f || c[1].w <= 1e-6f) continue;
            float dx = (c[1].x/c[1].w - c[0].x/c[0].w) * sx, dy = (c[1].y/c[1].w - c[0].y/c[0].w) * sy;
            if(dx == 0 && dy == 0) continue; // (GL draws nothing for a zero-length line)
            bool xmajor = fabs(dx) >= fabs(dy);
            loopj(2)
            {
                // the band's half-height in pixels, as an eye-space offset at this end's depth
                vec4 o(xmajor ? 0 : band / sx * c[j].w, xmajor ? band / sy * c[j].w : 0, 0, 0), d;
                loopk(4) d[k] = pinv.v[k]*o.x + pinv.v[4+k]*o.y;
                loopk(2)
                {
                    quadvert &q = quadverts.add();
                    memcpy(q.pos, v[j].pos, sizeof(immvert));
                    float sgn = k ? -1 : 1, ew = e[j].w + sgn*d.w;
                    loopl(3) q.pos[l] = (e[j][l] + sgn*d[l]) / ew;
                    q.aa[0] = sgn*band; q.aa[1] = cover;
                }
            }
        }
        indexarena.setsize(start);
        if(quadverts.empty()) return;
        for(int i = 0; i + 3 < quadverts.length(); i += 4)
        {
            indexarena.add(i); indexarena.add(i+1); indexarena.add(i+2);
            indexarena.add(i+1); indexarena.add(i+3); indexarena.add(i+2);
        }
        // drawn in eye space (the modelview matrix is in the vertices now)
        gmat keep = stacks[0][stackpos[0]];
        stacks[0][stackpos[0]].identity();
        widedraw = edges ? 2 : 1;
        widesmooth = smooth;
        record(GL_TRIANGLES, 0, quadverts.length(), start, indexarena.length() - start, false, 0, true);
        widedraw = 0;
        widesmooth = false;
        stacks[0][stackpos[0]] = keep;
    }

    // --- crosshair pick pass

    bool beginpick(int depthbits)
    {
        if(!dev) return false;
        beginrecording();
        curpass = -1;
        openpass(TARGET_PICK);
        viewport(0, 0, 1, 1);
        return true;
    }

    void endpick(bool async)
    {
        curpass = -1; // the main pass follows
    }

    bool readpick(float &depth)
    {
        // in the frame that drew it (the edit cursor): submit what's recorded so far, then wait as for the next frame
        if(recording) loopv(passes) if(passes[i].target == TARGET_PICK) { submitframe(false, true); break; }
        if(!pickpending || !pickfence) return false;
        SDL_WaitForGPUFences(dev, true, &pickfence, 1);
        SDL_ReleaseGPUFence(dev, pickfence);
        pickfence = NULL;
        pickpending = false;
        const float *d = (const float *)SDL_MapGPUTransferBuffer(dev, pickdownload, false);
        if(!d) return false;
        depth = *d;
        SDL_UnmapGPUTransferBuffer(dev, pickdownload);
        return true;
    }

    bool readdepthasync(int x, int y) { return false; }
    bool resolvedepthasync(float &depth) { return readpick(depth); }
    void readdepth(int x, int y, float &depth) { depth = 1; } // (the main pass can't be read in the middle of a frame)
    void cleanuppick() {}

    // --- frame submission

    void uploadtextures(SDL_GPUCopyPass *copy)
    {
        if(texuploads.empty()) return;
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, texuploadbytes, 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        uchar *p = (uchar *)SDL_MapGPUTransferBuffer(dev, tb, false);
        uint off = 0;
        loopv(texuploads) { memcpy(p + off, texuploads[i]->rgba.getbuf(), texuploads[i]->rgba.length()); off += texuploads[i]->rgba.length(); }
        SDL_UnmapGPUTransferBuffer(dev, tb);
        off = 0;
        loopv(texuploads)
        {
            texupload &u = *texuploads[i];
            SDL_GPUTexture *t = u.tex == (GLuint)-1 ? emptytex : (u.tex < (GLuint)textures.length() ? textures[u.tex].tex : NULL);
            if(t)
            {
                SDL_GPUTextureTransferInfo src = { tb, off, (Uint32)u.w, (Uint32)u.h };
                SDL_GPUTextureRegion dst;
                memset(&dst, 0, sizeof(dst));
                dst.texture = t;
                dst.mip_level = u.level;
                dst.w = u.w; dst.h = u.h; dst.d = 1;
                SDL_UploadToGPUTexture(copy, &src, &dst, false);
            }
            off += u.rgba.length();
            delete texuploads[i];
        }
        texuploads.setsize(0);
        texuploadbytes = 0;
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }

    void flushuploads()
    {
        if(texuploads.empty()) return;
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if(!cmd) return;
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        uploadtextures(copy);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    // The arena and upload buffers grow for the biggest frame (the minimap draws the whole map: 4-8 MB) and are cycled
    // every frame; kept that large, they made every later frame up to 30% slower when the GPU is the limit (more
    // frames in flight hold more large buffers). So after 100 frames that would fit in a quarter of them, they're
    // released and grow again from 1 MB.
    void shrinkbuffers()
    {
        const uint minsize = 1u<<20;
        if(arenavbsize <= minsize && arenaibsize <= minsize && frameuploadsize <= minsize) { smallframes = 0; return; }
        bool small = uint(vertexarena.length())*4 <= max(arenavbsize, minsize) && uint(indexarena.length()*sizeof(uint))*4 <= max(arenaibsize, minsize) &&
                     lastuploadsize*4 <= max(frameuploadsize, minsize);
        if(!small) { smallframes = 0; return; }
        if(++smallframes < 100) return;
        smallframes = 0;
        if(arenavbsize > minsize && arenavb) { SDL_ReleaseGPUBuffer(dev, arenavb); arenavb = NULL; arenavbsize = 0; }
        if(arenaibsize > minsize && arenaib) { SDL_ReleaseGPUBuffer(dev, arenaib); arenaib = NULL; arenaibsize = 0; }
        if(frameuploadsize > minsize && frameupload) { SDL_ReleaseGPUTransferBuffer(dev, frameupload); frameupload = NULL; frameuploadsize = 0; }
        if(gpudebug) conoutf("SDL GPU: arena and upload buffers shrunk");
    }

    SDL_GPUBuffer *growbuffer(SDL_GPUBuffer *buf, uint &cursize, uint need)
    {
        if(buf && cursize >= need) return buf;
        if(buf) SDL_ReleaseGPUBuffer(dev, buf);
        uint size = max(cursize ? cursize : 1u<<20, 1u<<16);
        while(size < need) size *= 2;
        SDL_GPUBufferCreateInfo bi = { SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_INDEX, size, 0 };
        cursize = size;
        return SDL_CreateGPUBuffer(dev, &bi);
    }

    // the frame's buffer uploads all go through one transfer buffer, kept from frame to frame (mapped with cycling,
    // so frames still in flight keep theirs)
    struct pendingupload { SDL_GPUBuffer *buf; const void *data; uint size, offset; };
    vector<pendingupload> pendinguploads;
    SDL_GPUTransferBuffer *frameupload;
    uint frameuploadsize;

    void upload(SDL_GPUBuffer *buf, const void *data, uint size)
    {
        if(!size || !buf) return;
        pendingupload &u = pendinguploads.add();
        u.buf = buf; u.data = data; u.size = size;
        u.offset = pendinguploads.length() > 1 ? (pendinguploads[pendinguploads.length()-2].offset + pendinguploads[pendinguploads.length()-2].size + 15) & ~15U : 0;
    }

    void flushbufferuploads(SDL_GPUCopyPass *copy)
    {
        if(pendinguploads.empty()) return;
        const pendingupload &last = pendinguploads.last();
        uint total = last.offset + last.size;
        lastuploadsize = total;
        if(!frameupload || frameuploadsize < total)
        {
            if(frameupload) SDL_ReleaseGPUTransferBuffer(dev, frameupload);
            frameuploadsize = max(frameuploadsize ? frameuploadsize : 1U<<20, 1U<<16);
            while(frameuploadsize < total) frameuploadsize *= 2;
            SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, frameuploadsize, 0 };
            frameupload = SDL_CreateGPUTransferBuffer(dev, &tbi);
        }
        uchar *p = (uchar *)SDL_MapGPUTransferBuffer(dev, frameupload, true);
        if(p)
        {
            loopv(pendinguploads) memcpy(p + pendinguploads[i].offset, pendinguploads[i].data, pendinguploads[i].size);
            SDL_UnmapGPUTransferBuffer(dev, frameupload);
            loopv(pendinguploads)
            {
                SDL_GPUTransferBufferLocation src = { frameupload, pendinguploads[i].offset };
                SDL_GPUBufferRegion dst = { pendinguploads[i].buf, 0, pendinguploads[i].size };
                SDL_UploadToGPUBuffer(copy, &src, &dst, true);
            }
        }
        pendinguploads.setsize(0);
    }

    SDL_GPUBuffer *drawbuffer(uchar src, GLuint idx)
    {
        if(src == SRC_ARENA) return arenavb;
        return idx < (GLuint)buffers.length() ? buffers[idx].buf : NULL;
    }

    void runpass(SDL_GPUCommandBuffer *cmd, gpupass &p)
    {
        SDL_GPUColorTargetInfo ct;
        memset(&ct, 0, sizeof(ct));
        SDL_GPUDepthStencilTargetInfo dt;
        memset(&dt, 0, sizeof(dt));
        if(p.target == TARGET_FXAA)
        {
            float params[4] = { 1.0f/targetw, 1.0f/targeth, 0, 0 };
            runpostpass(cmd, fxaapipe, colortex, posttex, params);
            return;
        }
        bool pick = p.target == TARGET_PICK, post = p.target == TARGET_POST;
        ct.texture = pick ? pickcolor : (post ? posttex : (msaatex ? msaatex : colortex));
        ct.load_op = p.clearcolor ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        ct.clear_color.r = p.color[0]; ct.clear_color.g = p.color[1]; ct.clear_color.b = p.color[2]; ct.clear_color.a = p.color[3];
        if(!pick && !post && msaatex)
        {
            ct.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
            ct.resolve_texture = colortex;
        }
        else ct.store_op = SDL_GPU_STOREOP_STORE;
        dt.texture = pick ? pickdepth : (post ? postdepth : depthtex);
        dt.clear_depth = p.depthclear;
        dt.load_op = p.cleardepth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        dt.store_op = SDL_GPU_STOREOP_STORE; // (not storing the last pass's depth measured no faster: 8 interleaved pairs)
        if(stencilbitsv)
        {
            dt.clear_stencil = Uint8(p.stencilclear);
            dt.stencil_load_op = p.clearstencil ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
        }
        else
        {
            dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        }
        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
        if(!rp) return;
        int lastpipeline = -1, lastsampler[2] = { -1, -1 }, laststencilref = -1;
        GLuint lasttex[2] = { ~0U, ~0U };
        float lastvp[4] = { -1, -1, -1, -1 };
        int lastsc[4] = { -1, -1, -1, -1 };
        loopi(p.numdraws)
        {
            gpudraw &d = draws[p.firstdraw + i];
            if(d.pipeline != lastpipeline) { SDL_BindGPUGraphicsPipeline(rp, pipelines[d.pipeline]); lastpipeline = d.pipeline; }
            if(stencilbitsv && d.stencilref != laststencilref) { SDL_SetGPUStencilReference(rp, Uint8(d.stencilref)); laststencilref = d.stencilref; }
            if(memcmp(lastvp, d.viewport, sizeof(lastvp)))
            {
                SDL_GPUViewport vp = { d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3], 0, 1 };
                SDL_SetGPUViewport(rp, &vp);
                memcpy(lastvp, d.viewport, sizeof(lastvp));
            }
            if(memcmp(lastsc, d.scissor, sizeof(lastsc)))
            {
                SDL_Rect r = { d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3] };
                SDL_SetGPUScissor(rp, &r);
                memcpy(lastsc, d.scissor, sizeof(lastsc));
            }
            loopj(NUMATTRS) if(d.vbsrc[j])
            {
                SDL_GPUBufferBinding b = { drawbuffer(d.vbsrc[j], d.vbbuffer[j]), d.vboffset[j] };
                if(b.buffer) SDL_BindGPUVertexBuffers(rp, j, &b, 1);
            }
            SDL_GPUBufferBinding ib = { d.ibsrc == SRC_ARENA ? arenaib : drawbuffer(SRC_BUFFER, d.ibbuffer), 0 };
            if(!ib.buffer) continue;
            SDL_BindGPUIndexBuffer(rp, &ib, d.ibsrc == SRC_ARENA ? SDL_GPU_INDEXELEMENTSIZE_32BIT : SDL_GPU_INDEXELEMENTSIZE_16BIT);
            if(d.tex[0] != lasttex[0] || d.samplerkey[0] != lastsampler[0] || d.tex[1] != lasttex[1] || d.samplerkey[1] != lastsampler[1])
            {
                SDL_GPUTextureSamplerBinding tsb[2];
                loopj(2)
                {
                    tsb[j].texture = d.tex[j] ? textures[d.tex[j]].tex : emptytex;
                    tsb[j].sampler = samplers[d.samplerkey[j]];
                    lasttex[j] = d.tex[j]; lastsampler[j] = d.samplerkey[j];
                }
                SDL_BindGPUFragmentSamplers(rp, 0, tsb, 2);
            }
            SDL_PushGPUVertexUniformData(cmd, 0, &d.vs, d.texmatrix ? sizeof(d.vs) : offsetof(vsuniforms, tex0));
            SDL_PushGPUFragmentUniformData(cmd, 0, &d.fs, sizeof(d.fs));
            SDL_DrawGPUIndexedPrimitives(rp, d.numindices, 1, d.firstindex, 0, 0);
        }
        SDL_EndGPURenderPass(rp);

        // copies of the frame into textures (glCopyTexSubImage2D), flipped so the rows are bottom-up as in GL
        loopi(p.numcopies)
        {
            const gpucopy &c = copies[p.firstcopy + i];
            if(c.tex >= (GLuint)textures.length() || !textures[c.tex].tex) continue;
            gputexture &t = textures[c.tex];
            int w = min(c.w, min(targetw - c.sx, t.w - c.dx)), h = min(c.h, min(targeth - c.sy, t.h - c.dy));
            if(w <= 0 || h <= 0 || c.sx < 0 || c.sy < 0 || c.dx < 0 || c.dy < 0) continue;
            SDL_GPUBlitInfo bi;
            memset(&bi, 0, sizeof(bi));
            bi.source.texture = post ? posttex : colortex;
            bi.source.x = c.sx; bi.source.y = targeth - (c.sy + h); bi.source.w = w; bi.source.h = h;
            bi.destination.texture = t.tex;
            bi.destination.x = c.dx; bi.destination.y = c.dy; bi.destination.w = w; bi.destination.h = h;
            bi.load_op = SDL_GPU_LOADOP_LOAD;
            bi.flip_mode = SDL_FLIP_VERTICAL;
            bi.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cmd, &bi);
        }
    }

    void present() { submitframe(true); }

    // submits the recorded frame; show: also copies it to the swapchain (present()), else it's only rendered into the
    // offscreen target, for readpixels()
    // show: the end of the frame (presented); otherwise submitted to read something back, and continuing: the rest of
    // the frame follows (the main target keeps what was drawn)
    void submitframe(bool show, bool continuing = false)
    {
        if(!dev) return;
        extern int screenw, screenh;
        setuptargets(screenw, screenh);
        beginrecording();
        if(passes.empty()) { openpass(TARGET_MAIN); }

        // 1. uploads and the pick pass, in a command buffer of their own that the next frame waits for
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if(!cmd) { recording = false; return; }
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        uploadtextures(copy);
        loopv(buffers) if(buffers[i].used && buffers[i].dirty && buffers[i].data.length())
        {
            gpubuffer &b = buffers[i];
            if(!b.buf || b.size < (uint)b.data.length())
            {
                if(b.buf) SDL_ReleaseGPUBuffer(dev, b.buf);
                SDL_GPUBufferCreateInfo bi = { SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_INDEX, (Uint32)b.data.length(), 0 };
                b.buf = SDL_CreateGPUBuffer(dev, &bi);
                b.size = b.data.length();
            }
            upload(b.buf, b.data.getbuf(), b.data.length());
            b.dirty = false;
        }
        shrinkbuffers();
        if(vertexarena.length()) { arenavb = growbuffer(arenavb, arenavbsize, vertexarena.length()); upload(arenavb, vertexarena.getbuf(), vertexarena.length()); }
        if(indexarena.length()) { arenaib = growbuffer(arenaib, arenaibsize, indexarena.length()*sizeof(uint)); upload(arenaib, indexarena.getbuf(), indexarena.length()*sizeof(uint)); }
        flushbufferuploads(copy);
        SDL_EndGPUCopyPass(copy);

        bool picked = false;
        loopv(passes) if(passes[i].target == TARGET_PICK) { runpass(cmd, passes[i]); picked = true; }
        if(picked)
        {
            SDL_GPUCopyPass *dl = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src;
            memset(&src, 0, sizeof(src));
            src.texture = pickcolor; src.w = src.h = src.d = 1;
            SDL_GPUTextureTransferInfo dst = { pickdownload, 0, 1, 1 };
            SDL_DownloadFromGPUTexture(dl, &src, &dst);
            SDL_EndGPUCopyPass(dl);
            if(pickfence) SDL_ReleaseGPUFence(dev, pickfence);
            pickfence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            pickpending = pickfence != NULL;
            cmd = SDL_AcquireGPUCommandBuffer(dev);
            if(!cmd) { recording = false; return; }
        }

        if(gpudebug >= 2 && framecount%500 == 0)
        {
            int npasses = 0, ndraws = 0, nblended = 0;
            loopv(passes) if(passes[i].target != TARGET_PICK)
            {
                npasses++;
                ndraws += passes[i].numdraws;
                loopj(passes[i].numdraws) if(pipelinekeys[draws[passes[i].firstdraw + j].pipeline].blend) nblended++;
            }
            conoutf("SDL GPU frame %u: %d passes, %d draws (%d blended, %d in total), %d KB vertices, %d KB indices, %d copies; "
                "arena buffers %u/%u KB, upload buffer %u KB", framecount, npasses, ndraws, nblended, draws.length(),
                vertexarena.length()/1024, indexarena.length()*4/1024, copies.length(), arenavbsize/1024, arenaibsize/1024, frameuploadsize/1024);
        }

        // 2. the main passes (scene, FXAA, HUD), gamma and presentation
        if(postfxactive) setuppostrargets();
        loopv(passes) if(passes[i].target != TARGET_PICK) runpass(cmd, passes[i]);
        SDL_GPUTexture *final = postfxactive ? posttex : colortex;
        if(gammaexponent != 1)
        {
            // into the other texture, so screenshots include it as with GL (where the gamma pass draws into the back buffer)
            if(!gammafs) gammafs = makepostshader("fgamma", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
            if(!gammapipe) gammapipe = postpipeline(gammafs, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
            if(final == colortex) setuppostrargets();
            SDL_GPUTexture *dst = final == colortex ? posttex : colortex;
            float params[4] = { gammaexponent, 0, 0, 0 };
            if(gammapipe) { runpostpass(cmd, gammapipe, final, dst, params); final = dst; }
        }
        lastfinal = final;
        SDL_GPUTexture *swap = NULL;
        Uint32 sw = 0, sh = 0;
        if(show && !benchnopresent && SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh) && swap)
        {
            SDL_GPUBlitInfo bi;
            memset(&bi, 0, sizeof(bi));
            bi.source.texture = final; bi.source.w = targetw; bi.source.h = targeth;
            bi.destination.texture = swap; bi.destination.w = sw; bi.destination.h = sh;
            bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
            bi.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cmd, &bi);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
        recording = false;
        curpass = -1;
        midframe = continuing;
    }

    // as glReadPixels() would return it (bottom row first): the frame being recorded (which is submitted for this, e.g.
    // by the blob shadow generation in vertmodel.h), or else the last frame (screenshots)
    void readpixels(int x, int y, int w, int h, GLenum format, GLenum type, void *data)
    {
        if(recording) submitframe(false);
        if(!dev || !colortex || w <= 0 || h <= 0) return;
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, (Uint32)(w*h*4), 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src;
        memset(&src, 0, sizeof(src));
        src.texture = lastfinal ? lastfinal : colortex; src.x = x; src.y = targeth - (y + h); src.w = w; src.h = h; src.d = 1;
        SDL_GPUTextureTransferInfo dst = { tb, 0, (Uint32)w, (Uint32)h };
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(dev, true, &f, 1);
        SDL_ReleaseGPUFence(dev, f);
        const uchar *p = (const uchar *)SDL_MapGPUTransferBuffer(dev, tb, false);
        if(p)
        {
            int bpp = format == GL_RGBA ? 4 : (format == GL_RGB ? 3 : 1), first = format == GL_ALPHA ? 3 : (format == GL_GREEN ? 1 : (format == GL_BLUE ? 2 : 0));
            uchar *out = (uchar *)data;
            loop(row, h)
            {
                const uchar *s = p + (h - 1 - row)*w*4 + first;
                loop(col, w) { loopk(bpp) out[k] = s[k]; out += bpp; s += 4; }
            }
            SDL_UnmapGPUTransferBuffer(dev, tb);
        }
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }

    // level 0 of a texture as glGetTexImage returns it (mapshot: the minimap). Textures are stored in GL's row order
    // (copies of the frame are flipped into it), so the rows come out as they are.
    void gettexture(GLuint t, GLenum format, void *pixels)
    {
        if(recording) submitframe(false);
        else flushuploads();
        if(!dev || t >= (GLuint)textures.length() || !textures[t].tex) return;
        int w = textures[t].w, h = textures[t].h, bpp = format == GL_RGBA ? 4 : 3;
        if(w <= 0 || h <= 0) return;
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, (Uint32)(w*h*4), 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if(!tb || !cmd) { if(tb) SDL_ReleaseGPUTransferBuffer(dev, tb); if(cmd) SDL_CancelGPUCommandBuffer(cmd); return; }
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src;
        memset(&src, 0, sizeof(src));
        src.texture = textures[t].tex; src.w = w; src.h = h; src.d = 1;
        SDL_GPUTextureTransferInfo dst = { tb, 0, (Uint32)w, (Uint32)h };
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if(f) { SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f); }
        const uchar *p = (const uchar *)SDL_MapGPUTransferBuffer(dev, tb, false);
        if(p)
        {
            uchar *out = (uchar *)pixels;
            loopi(w*h) { loopk(bpp) out[k] = p[k]; out += bpp; p += 4; }
            SDL_UnmapGPUTransferBuffer(dev, tb);
        }
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }
};

static sdlgpubackend *sdlgpuinstance = NULL;
static void setframesinflight() { if(sdlgpuinstance && sdlgpuinstance->dev) SDL_SetGPUAllowedFramesInFlight(sdlgpuinstance->dev, gpuframes); }
static void setpresentmode() { if(sdlgpuinstance) sdlgpuinstance->displaychanged(); }
gfxdevice *gfx_sdlgpu()
{
    if(!sdlgpuinstance) sdlgpuinstance = new sdlgpubackend;
    return sdlgpuinstance;
}
