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
// Not ported yet (stage 1): multitexturing and the other texture combiners (particles, water), stencil, render-to-
// texture (reflections, minimap), line width and polygon mode (editor), the post-processing shaders. The renderer only
// uses these where gfx_legacygl() is true.

#include "cube.h"

VAR(gpudebug, 0, 0, 1); // log every pipeline the GPU backend creates
extern int benchnopresent;
static void setframesinflight();
VARF(gpuframes, 1, 2, 3, setframesinflight()); // frames the GPU backend lets the CPU queue ahead of the GPU

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
    "struct VSU { float4x4 mvp; float4x4 mv; float4 color; };\n"
    "struct FSU { float4 fogcolor; float4 fog; float4 params; };\n" // fog: end, 1/(end-start), on; params: colour scale, alpha test reference (<0: off)
    "struct VIn\n"
    "{\n"
    "    float3 pos [[attribute(0)]];\n"
    "#if HAS_TC\n"
    "    float2 tc [[attribute(1)]];\n"
    "#endif\n"
    "#if HAS_COLOR\n"
    "    float4 color [[attribute(2)]];\n"
    "#endif\n"
    "};\n"
    "struct VOut { float4 pos [[position]]; float2 tc; float4 color; float fogdist; };\n";

static const char *mslvertex =
    "vertex VOut vmain(VIn in [[stage_in]], constant VSU &u [[buffer(0)]])\n"
    "{\n"
    "    VOut o;\n"
    "    float4 p = float4(in.pos, 1.0);\n"
    "    o.pos = u.mvp * p;\n"
    "#if HAS_TC\n"
    "    o.tc = in.tc;\n"
    "#else\n"
    "    o.tc = float2(0.0);\n"
    "#endif\n"
    "#if HAS_COLOR\n"
    "    o.color = in.color;\n"
    "#else\n"
    "    o.color = u.color;\n"
    "#endif\n"
    "    o.fogdist = length((u.mv * p).xyz);\n" // radial distance, like shaders.cpp's worldshader
    "    return o;\n"
    "}\n";

// texture x colour (x 2 for the world's and models' "T * P x 2" combiner), alpha test, linear fog
static const char *mslfragment =
    "fragment float4 fmain(VOut in [[stage_in]], constant FSU &u [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])\n"
    "{\n"
    "    float4 c = tex.sample(smp, in.tc) * in.color;\n"
    "    c.rgb = min(c.rgb * u.params.x, 1.0);\n"
    "    if(u.params.y >= 0.0 && c.a <= u.params.y) discard_fragment();\n"
    "    if(u.fog.z > 0.5) c.rgb = mix(u.fogcolor.rgb, c.rgb, clamp((u.fog.x - in.fogdist) * u.fog.y, 0.0, 1.0));\n"
    "    return c;\n"
    "}\n";

// crosshair pick pass: the depth of the nearest fragment, written to an R32F target (alpha-tested skins still cut out)
static const char *mslpick =
    "fragment float fpick(VOut in [[stage_in]], constant FSU &u [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])\n"
    "{\n"
    "    if(u.params.y >= 0.0 && tex.sample(smp, in.tc).a * in.color.a <= u.params.y) discard_fragment();\n"
    "    return in.pos.z;\n"
    "}\n";

// the same shaders in GLSL 450 for Vulkan (SDL GPU's SPIR-V resource sets: vertex uniforms in set 1, fragment samplers
// in set 2, fragment uniforms in set 3)
const char *gpuglslsources[] =
{
    "#version 450\n"
    "layout(location = 0) in vec3 pos;\n"
    "#if HAS_TC\n"
    "layout(location = 1) in vec2 tc;\n"
    "#endif\n"
    "#if HAS_COLOR\n"
    "layout(location = 2) in vec4 color;\n"
    "#endif\n"
    "layout(set = 1, binding = 0) uniform VSU { mat4 mvp; mat4 mv; vec4 ucolor; };\n"
    "layout(location = 0) out vec2 otc;\n"
    "layout(location = 1) out vec4 ocolor;\n"
    "layout(location = 2) out float ofogdist;\n"
    "void main()\n"
    "{\n"
    "    vec4 p = vec4(pos, 1.0);\n"
    "    gl_Position = mvp * p;\n"
    "#if HAS_TC\n"
    "    otc = tc;\n"
    "#else\n"
    "    otc = vec2(0.0);\n"
    "#endif\n"
    "#if HAS_COLOR\n"
    "    ocolor = color;\n"
    "#else\n"
    "    ocolor = ucolor;\n"
    "#endif\n"
    "    ofogdist = length((mv * p).xyz);\n"
    "}\n",

    "#version 450\n"
    "layout(location = 0) in vec2 tc;\n"
    "layout(location = 1) in vec4 color;\n"
    "layout(location = 2) in float fogdist;\n"
    "layout(set = 2, binding = 0) uniform sampler2D tex;\n"
    "layout(set = 3, binding = 0) uniform FSU { vec4 fogcolor; vec4 fog; vec4 params; };\n"
    "layout(location = 0) out vec4 ocolor;\n"
    "void main()\n"
    "{\n"
    "    vec4 c = texture(tex, tc) * color;\n"
    "    c.rgb = min(c.rgb * params.x, 1.0);\n"
    "    if(params.y >= 0.0 && c.a <= params.y) discard;\n"
    "    if(fog.z > 0.5) c.rgb = mix(fogcolor.rgb, c.rgb, clamp((fog.x - fogdist) * fog.y, 0.0, 1.0));\n"
    "    ocolor = c;\n"
    "}\n",

    "#version 450\n"
    "layout(location = 0) in vec2 tc;\n"
    "layout(location = 1) in vec4 color;\n"
    "layout(location = 2) in float fogdist;\n"
    "layout(set = 2, binding = 0) uniform sampler2D tex;\n"
    "layout(set = 3, binding = 0) uniform FSU { vec4 fogcolor; vec4 fog; vec4 params; };\n"
    "layout(location = 0) out float odepth;\n"
    "void main()\n"
    "{\n"
    "    if(params.y >= 0.0 && texture(tex, tc).a * color.a <= params.y) discard;\n"
    "    odepth = gl_FragCoord.z;\n"
    "}\n",
};

struct vsuniforms { float mvp[16], mv[16], color[4]; };
struct fsuniforms { float fogcolor[4], fog[4], params[4]; };

// --- resources

struct gputexture
{
    SDL_GPUTexture *tex;
    int w, h, levels;
    GLint wraps, wrapt, magfilter, minfilter, aniso;
    bool used;
};

struct gpubuffer
{
    SDL_GPUBuffer *buf;
    uint size;                  // of buf
    vector<uchar> data;         // CPU copy (uploaded when dirty)
    bool dirty, used;
};

enum { ATTR_POS = 0, ATTR_TC, ATTR_COLOR, NUMATTRS };
enum { TARGET_MAIN = 0, TARGET_PICK };

struct gpuarray { bool enabled; GLint size; GLenum type; GLsizei stride; const void *ptr; GLuint buffer; };

// everything a pipeline fixes
struct pipelinekey
{
    uchar target, topology, samples, alphacoverage;
    uchar attrs[NUMATTRS];      // SDL_GPUVertexElementFormat, 0 = absent
    ushort pitch[NUMATTRS];
    uchar blend, srcblend, dstblend, colormask;
    uchar depthtest, depthfunc, depthwrite, cull;
    uchar depthbias, pad[3];
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
    int pipeline;
    uchar vbsrc[NUMATTRS];
    GLuint vbbuffer[NUMATTRS];
    uint vboffset[NUMATTRS];
    uchar ibsrc;                // SRC_BUFFER (16-bit, a GL element buffer) or SRC_ARENA (32-bit)
    GLuint ibbuffer;
    uint firstindex, numindices;
    GLuint tex;                 // 0: white
    int samplerkey;
    float viewport[4];          // x, y (from the top), w, h
    vsuniforms vs;
    fsuniforms fs;
};

struct gpupass
{
    int target;
    bool clearcolor, cleardepth;
    float color[4];
    int firstdraw, numdraws;
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
    gmat stacks[3][32];
    int stackpos[3];
    float curcolor[4], clearcol[4], fogstart, fogend, fogcolor[4];
    bool caps[16], texenabled[4];
    GLenum blendsrc, blenddst, depthfn, cullmode, alphafn;
    float alpharef, offsetfactor, offsetunits;
    bool depthwrite, colormaskbits[4];
    int viewportv[4];
    int activetex;
    GLuint boundtex[4];
    GLuint arraybuffer, elementbuffer;
    gpuarray arrays[NUMATTRS];

    // immediate mode
    struct immvert { float pos[3], tc[2], color[4]; };
    vector<immvert> immverts;
    GLenum immmode;
    float immtc[2];

    // resources
    vector<gputexture> textures;
    vector<int> freetextures;
    vector<gpubuffer> buffers;
    vector<int> freebuffers;
    SDL_GPUTexture *whitetex;
    vector<SDL_GPUSampler *> samplers;
    vector<samplerkey> samplerkeys;
    hashtable<pipelinekey, int> pipelineindex;
    vector<SDL_GPUGraphicsPipeline *> pipelines;
    SDL_GPUShader *vshaders[4], *fshader, *pickshader;

    // the frame being recorded
    bool recording;
    vector<gpupass> passes;
    vector<gpudraw> draws;
    int curpass;
    vector<uchar> vertexarena;
    vector<uint> indexarena;
    struct texupload { GLuint tex; int level, w, h; vector<uchar> rgba; };
    vector<texupload *> texuploads;
    uint texuploadbytes;

    // targets
    SDL_GPUTexture *colortex, *msaatex, *depthtex, *pickcolor, *pickdepth;
    int targetw, targeth;
    SDL_GPUBuffer *arenavb, *arenaib;
    uint arenavbsize, arenaibsize;
    SDL_GPUTransferBuffer *pickdownload;
    SDL_GPUFence *pickfence;
    bool pickpending;

    sdlgpubackend() : dev(NULL), window(NULL), format(SDL_GPU_SHADERFORMAT_INVALID), samples(1), depthbits(32),
        depthformat(SDL_GPU_TEXTUREFORMAT_D32_FLOAT), samplecount(SDL_GPU_SAMPLECOUNT_1), matrixmodeidx(0), activetex(0),
        arraybuffer(0), elementbuffer(0), immmode(0), whitetex(NULL), fshader(NULL), pickshader(NULL), recording(false),
        curpass(-1), texuploadbytes(0), colortex(NULL), msaatex(NULL), depthtex(NULL), pickcolor(NULL), pickdepth(NULL),
        targetw(0), targeth(0), arenavb(NULL), arenaib(NULL), arenavbsize(0), arenaibsize(0), pickdownload(NULL),
        pickfence(NULL), pickpending(false)
    {
        loopi(4) vshaders[i] = NULL;
        resetstate();
        textures.add(); // handle 0: no texture
        memset(&textures[0], 0, sizeof(gputexture));
        textures[0].used = true;
        buffers.add();
        buffers[0].buf = NULL; buffers[0].size = 0; buffers[0].dirty = false; buffers[0].used = true;
    }

    void resetstate()
    {
        loopi(3) { stackpos[i] = 0; stacks[i][0].identity(); }
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
        loopi(4) viewportv[i] = 0;
        activetex = 0;
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
        SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
        if(vsync == 0)
        {
            if(SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
            else if(SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_MAILBOX)) mode = SDL_GPU_PRESENTMODE_MAILBOX;
        }
        if(!SDL_SetGPUSwapchainParameters(dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
            conoutf("\f3SDL GPU: present mode %d not set: %s", int(mode), SDL_GetError());
        conoutf("SDL GPU: vsync %d, present mode %s (immediate %s, mailbox %s)", vsync, mode == SDL_GPU_PRESENTMODE_VSYNC ? "vsync" : (mode == SDL_GPU_PRESENTMODE_IMMEDIATE ? "immediate" : "mailbox"),
            SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_IMMEDIATE) ? "supported" : "not supported",
            SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_MAILBOX) ? "supported" : "not supported");
        SDL_SetGPUAllowedFramesInFlight(dev, gpuframes);

        // depth: 32-bit float (Apple's GL gives 32 bits too), else 24-bit
        depthformat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        depthbits = 32;
        if(!SDL_GPUTextureSupportsFormat(dev, depthformat, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        {
            depthformat = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
            depthbits = 24;
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

        // a white texture for untextured drawing
        SDL_GPUTextureCreateInfo ti;
        memset(&ti, 0, sizeof(ti));
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = ti.height = 1;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        whitetex = SDL_CreateGPUTexture(dev, &ti);
        texupload *u = new texupload;
        u->tex = (GLuint)-1; u->level = 0; u->w = u->h = 1;
        loopi(4) u->rgba.add(255);
        texuploads.add(u);
        texuploadbytes += 4;

        conoutf("renderer: SDL GPU (%s), %d bit depth, %dx multisampling", SDL_GetGPUDeviceDriver(dev), depthbits, samples);
        return true;
    }

    int getdepthbits() { return depthbits; }
    int getsamples() { return samples; }

    void releasetargets()
    {
        if(colortex) SDL_ReleaseGPUTexture(dev, colortex);
        if(msaatex) SDL_ReleaseGPUTexture(dev, msaatex);
        if(depthtex) SDL_ReleaseGPUTexture(dev, depthtex);
        colortex = msaatex = depthtex = NULL;
        targetw = targeth = 0;
    }

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
        if(!pickcolor)
        {
            pickcolor = createtarget(1, 1, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, SDL_GPU_SAMPLECOUNT_1);
            pickdepth = createtarget(1, 1, depthformat, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, SDL_GPU_SAMPLECOUNT_1);
            SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, 256, 0 };
            pickdownload = SDL_CreateGPUTransferBuffer(dev, &tbi);
        }
    }

    // --- shaders and pipelines

    SDL_GPUShader *makeshader(const char *body, const char *entry, SDL_GPUShaderStage stage, bool hastc, bool hascolor, int samplers, int uniforms)
    {
        defformatstring(defs)("#define HAS_TC %d\n#define HAS_COLOR %d\n", hastc ? 1 : 0, hascolor ? 1 : 0);
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

    bool loadshaders()
    {
        if(fshader) return true;
        loopi(4) if(!(vshaders[i] = makeshader(mslvertex, "vmain", SDL_GPU_SHADERSTAGE_VERTEX, (i&1) != 0, (i&2) != 0, 0, 1))) return false;
        fshader = makeshader(mslfragment, "fmain", SDL_GPU_SHADERSTAGE_FRAGMENT, true, true, 1, 1);
        pickshader = makeshader(mslpick, "fpick", SDL_GPU_SHADERSTAGE_FRAGMENT, true, true, 1, 1);
        return fshader && pickshader;
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
        if(!loadshaders()) return -1;

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
        bool hastc = k.attrs[ATTR_TC] != 0, hascolor = k.attrs[ATTR_COLOR] != 0;

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
        pi.vertex_shader = vshaders[(hastc ? 1 : 0) | (hascolor ? 2 : 0)];
        pi.fragment_shader = k.target == TARGET_PICK ? pickshader : fshader;
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
        pi.multisample_state.sample_count = k.target == TARGET_PICK ? SDL_GPU_SAMPLECOUNT_1 : samplecount;
        pi.multisample_state.enable_alpha_to_coverage = k.alphacoverage != 0;
        pi.depth_stencil_state.enable_depth_test = true;
        pi.depth_stencil_state.compare_op = k.depthtest ? compareop(GLenum(GL_NEVER + k.depthfunc)) : SDL_GPU_COMPAREOP_ALWAYS;
        pi.depth_stencil_state.enable_depth_write = k.depthtest && k.depthwrite;
        pi.target_info.color_target_descriptions = &ctd;
        pi.target_info.num_color_targets = 1;
        pi.target_info.depth_stencil_format = depthformat;
        pi.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if(!p) conoutf("\f3SDL GPU: pipeline creation failed: %s", SDL_GetError());
        else if(gpudebug) conoutf("SDL GPU: pipeline %d (target %d, topology %d, attributes %d/%d/%d, blend %d, depth %d/%d/%d, cull %d)",
            pipelines.length(), k.target, k.topology, k.attrs[0], k.attrs[1], k.attrs[2], k.blend, k.depthtest, k.depthfunc, k.depthwrite, k.cull);
        pipelines.add(p);
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
            default: return 15;
        }
    }
    bool texturing() { return texenabled[0]; } // (only texture unit 0 is implemented)

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
    void linewidth(GLfloat w) {} // (SDL GPU draws 1-pixel lines only)
    void color3f(GLfloat r, GLfloat g, GLfloat b) { curcolor[0] = r; curcolor[1] = g; curcolor[2] = b; curcolor[3] = 1; }
    void color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { curcolor[0] = r; curcolor[1] = g; curcolor[2] = b; curcolor[3] = a; }
    void color3ub(GLubyte r, GLubyte g, GLubyte b) { color3f(r/255.0f, g/255.0f, b/255.0f); }
    void fogi(GLenum pname, GLint v) { fogf(pname, float(v)); }
    void fogf(GLenum pname, GLfloat v) { if(pname == GL_FOG_START) fogstart = v; else if(pname == GL_FOG_END) fogend = v; }
    void fogfv(GLenum pname, const GLfloat *v) { if(pname == GL_FOG_COLOR) loopi(4) fogcolor[i] = v[i]; else if(v) fogf(pname, v[0]); }
    void hint(GLenum target, GLenum mode) {}
    void shademodel(GLenum mode) {}
    void cleardepth(GLdouble d) {}
    void texenvi(GLenum pname, GLint v) {}             // the combiners are read from texture.cpp's tmus (tmuismodulate2x)
    void texenvfv(GLenum pname, const GLfloat *v) {}
    void activetexture(GLenum unit) { activetex = clamp(int(unit - GL_TEXTURE0_ARB), 0, 3); }
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
    void clearcolor(float r, float g, float b, float a) { clearcol[0] = r; clearcol[1] = g; clearcol[2] = b; clearcol[3] = a; }
    void clearstencil(int s) {}
    void finish() { if(dev) SDL_WaitForGPUIdle(dev); }

    // --- matrices

    gmat &top() { return stacks[matrixmodeidx][stackpos[matrixmodeidx]]; }
    void matrixmode(GLenum mode) { matrixmodeidx = mode == GL_PROJECTION ? 1 : (mode == GL_TEXTURE ? 2 : 0); }
    void loadidentity() { top().identity(); }
    void pushmatrix() { int &p = stackpos[matrixmodeidx]; if(p < 31) { stacks[matrixmodeidx][p+1] = stacks[matrixmodeidx][p]; p++; } }
    void popmatrix() { int &p = stackpos[matrixmodeidx]; if(p > 0) p--; }
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
                SDL_GPUTextureCreateInfo ti;
                memset(&ti, 0, sizeof(ti));
                ti.type = SDL_GPU_TEXTURETYPE_2D;
                ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
                ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
                ti.width = w;
                ti.height = h;
                ti.layer_count_or_depth = 1;
                ti.num_levels = levels;
                g.tex = SDL_CreateGPUTexture(dev, &ti);
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
        g->dirty = true;
    }

    void buffersubdata(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
    {
        gpubuffer *g = boundbuffer(target);
        if(!g || offset + size > g->data.length()) return;
        memcpy(g->data.getbuf() + offset, data, size);
        g->dirty = true;
    }

    // --- vertex arrays

    int arrayindex(GLenum array) { return array == GL_VERTEX_ARRAY ? ATTR_POS : (array == GL_TEXTURE_COORD_ARRAY ? ATTR_TC : (array == GL_COLOR_ARRAY ? ATTR_COLOR : -1)); }
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
    void texcoordpointer(GLint size, GLenum type, GLsizei stride, const void *p) { setarray(ATTR_TC, size, type, stride, p); }

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

    void beginrecording()
    {
        if(recording) return;
        recording = true;
        passes.setsize(0);
        draws.setsize(0);
        curpass = -1;
        vertexarena.setsize(0);
        indexarena.setsize(0);
    }

    gpupass &openpass(int target)
    {
        beginrecording();
        if(curpass >= 0 && passes[curpass].target == target) return passes[curpass];
        // a target's first pass in a frame always clears (on tile-based GPUs that's cheaper than loading the old contents)
        bool first = true;
        loopv(passes) if(passes[i].target == target) first = false;
        gpupass &p = passes.add();
        p.target = target;
        p.clearcolor = p.cleardepth = first;
        loopi(4) p.color[i] = target == TARGET_PICK ? 1.0f : clearcol[i];
        p.firstdraw = draws.length();
        p.numdraws = 0;
        curpass = passes.length() - 1;
        return p;
    }

    void clear(GLbitfield mask)
    {
        if(!(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT))) return;
        beginrecording();
        if(curpass >= 0 && passes[curpass].target == TARGET_PICK) return; // (cleared when the pick pass begins)
        if(curpass < 0 || passes[curpass].numdraws)
        {
            curpass = -1;
            gpupass &p = openpass(TARGET_MAIN);
            if(mask & GL_COLOR_BUFFER_BIT) p.clearcolor = true;
            if(mask & GL_DEPTH_BUFFER_BIT) p.cleardepth = true;
        }
        else
        {
            gpupass &p = passes[curpass];
            if(mask & GL_COLOR_BUFFER_BIT) { p.clearcolor = true; loopi(4) p.color[i] = clearcol[i]; }
            if(mask & GL_DEPTH_BUFFER_BIT) p.cleardepth = true;
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
        gpupass &pass = openpass(curpass >= 0 ? passes[curpass].target : TARGET_MAIN);
        gpudraw &d = draws.add();
        memset(&d, 0, sizeof(d));
        pipelinekey k;
        memset(&k, 0, sizeof(k));
        k.target = pass.target;
        bool picking = pass.target == TARGET_PICK;

        // vertex sources
        if(immediate)
        {
            uint off = arenaput(immverts.getbuf(), immverts.length()*sizeof(immvert));
            k.attrs[ATTR_POS] = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; k.pitch[ATTR_POS] = sizeof(immvert);
            d.vbsrc[ATTR_POS] = SRC_ARENA; d.vboffset[ATTR_POS] = off + offsetof(immvert, pos);
            k.attrs[ATTR_TC] = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; k.pitch[ATTR_TC] = sizeof(immvert);
            d.vbsrc[ATTR_TC] = SRC_ARENA; d.vboffset[ATTR_TC] = off + offsetof(immvert, tc);
            k.attrs[ATTR_COLOR] = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; k.pitch[ATTR_COLOR] = sizeof(immvert);
            d.vbsrc[ATTR_COLOR] = SRC_ARENA; d.vboffset[ATTR_COLOR] = off + offsetof(immvert, color);
        }
        else loopi(NUMATTRS)
        {
            gpuarray &a = arrays[i];
            if(!a.enabled || (i == ATTR_TC && !texturing())) continue;
            k.attrs[i] = elementformat(i, a.size, a.type);
            k.pitch[i] = a.stride;
            if(a.buffer)
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

        // primitives (everything without a GL element buffer has been turned into lists already)
        switch(mode)
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
        k.samples = picking ? 1 : samples;
        k.alphacoverage = !picking && samples > 1 && caps[capindex(GL_SAMPLE_ALPHA_TO_COVERAGE)] && caps[capindex(GL_ALPHA_TEST)];
        k.blend = !picking && caps[capindex(GL_BLEND)];
        k.srcblend = k.blend ? packblend(blendsrc) : 0;
        k.dstblend = k.blend ? packblend(blenddst) : 0;
        k.colormask = picking ? 1 : (colormaskbits[0] ? 1 : 0) | (colormaskbits[1] ? 2 : 0) | (colormaskbits[2] ? 4 : 0) | (colormaskbits[3] ? 8 : 0);
        k.depthtest = caps[capindex(GL_DEPTH_TEST)];
        k.depthfunc = k.depthtest ? uchar(depthfn - GL_NEVER) : 0;
        k.depthwrite = k.depthtest && depthwrite;
        k.cull = caps[capindex(GL_CULL_FACE)] && k.topology != SDL_GPU_PRIMITIVETYPE_LINELIST ? (cullmode == GL_FRONT ? 1 : (cullmode == GL_BACK ? 2 : 0)) : 0;
        if(caps[capindex(GL_POLYGON_OFFSET_FILL)]) { k.depthbias = 1; k.biasfactor = offsetfactor; k.biasunits = offsetunits; }
        d.pipeline = getpipeline(k);
        if(d.pipeline < 0) { draws.pop(); return; }

        // texture and uniforms
        bool tex = texturing() && k.attrs[ATTR_TC];
        d.tex = tex ? boundtex[0] : 0;
        if(d.tex && (d.tex >= (GLuint)textures.length() || !textures[d.tex].tex)) d.tex = 0;
        d.samplerkey = d.tex ? getsampler(textures[d.tex]) : getsampler(textures[0]);

        gmat proj = stacks[1][stackpos[1]], mv = stacks[0][stackpos[0]], zfix, pm, mvp;
        zfix.identity();
        zfix.v[10] = 0.5f; zfix.v[14] = 0.5f; // GL's clip-space z [-w, w] to SDL GPU's [0, w]
        pm.mul(zfix, proj);
        mvp.mul(pm, mv);
        memcpy(d.vs.mvp, mvp.v, sizeof(d.vs.mvp));
        memcpy(d.vs.mv, mv.v, sizeof(d.vs.mv));
        loopi(4) d.vs.color[i] = curcolor[i];
        loopi(4) d.fs.fogcolor[i] = fogcolor[i];
        d.fs.fog[0] = fogend;
        d.fs.fog[1] = fogend != fogstart ? 1.0f/(fogend - fogstart) : 0;
        d.fs.fog[2] = !picking && caps[capindex(GL_FOG)] ? 1 : 0;
        extern bool tmuismodulate2x(int n);
        d.fs.params[0] = tex && tmuismodulate2x(0) ? 2 : 1;
        d.fs.params[1] = caps[capindex(GL_ALPHA_TEST)] && alphafn == GL_GREATER ? alpharef : -1;
        if(picking && !tex) d.fs.params[1] = -1;

        // viewport, from the top
        int th = picking ? 1 : targeth;
        d.viewport[0] = viewportv[0];
        d.viewport[1] = th - (viewportv[1] + viewportv[3]);
        d.viewport[2] = viewportv[2];
        d.viewport[3] = viewportv[3];
        pass.numdraws++;
    }

    // GL primitives as 32-bit index lists (relative to the first vertex)
    void listindices(GLenum mode, int first, int count, int base)
    {
        int o = first - base;
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
                    indexarena.add(o+i); indexarena.add(o+i+1); indexarena.add(o+i+2);
                    indexarena.add(o+i); indexarena.add(o+i+2); indexarena.add(o+i+3);
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
        if(eb && type == GL_UNSIGNED_SHORT && (mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP))
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
    void vertex3f(GLfloat x, GLfloat y, GLfloat z)
    {
        immvert &v = immverts.add();
        v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
        v.tc[0] = immtc[0]; v.tc[1] = immtc[1];
        loopi(4) v.color[i] = curcolor[i];
    }
    void vertex2f(GLfloat x, GLfloat y) { vertex3f(x, y, 0); }
    void end()
    {
        if(immverts.empty()) return;
        beginrecording();
        uint start = indexarena.length();
        listindices(immmode, 0, immverts.length(), 0);
        // immediate-mode vertices always carry a texture coordinate and a colour (the current ones)
        bool keeptex = texturing();
        record(immmode, 0, immverts.length(), start, indexarena.length() - start, false, 0, true);
        (void)keeptex;
        immverts.setsize(0);
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
            SDL_GPUTexture *t = u.tex == (GLuint)-1 ? whitetex : (u.tex < (GLuint)textures.length() ? textures[u.tex].tex : NULL);
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

    void upload(SDL_GPUCopyPass *copy, SDL_GPUBuffer *buf, const void *data, uint size, bool cycle)
    {
        if(!size) return;
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        void *p = SDL_MapGPUTransferBuffer(dev, tb, false);
        memcpy(p, data, size);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_GPUTransferBufferLocation src = { tb, 0 };
        SDL_GPUBufferRegion dst = { buf, 0, size };
        SDL_UploadToGPUBuffer(copy, &src, &dst, cycle);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
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
        bool pick = p.target == TARGET_PICK;
        ct.texture = pick ? pickcolor : (msaatex ? msaatex : colortex);
        ct.load_op = p.clearcolor ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        ct.clear_color.r = p.color[0]; ct.clear_color.g = p.color[1]; ct.clear_color.b = p.color[2]; ct.clear_color.a = p.color[3];
        if(!pick && msaatex)
        {
            ct.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
            ct.resolve_texture = colortex;
        }
        else ct.store_op = SDL_GPU_STOREOP_STORE;
        dt.texture = pick ? pickdepth : depthtex;
        dt.clear_depth = 1;
        dt.load_op = p.cleardepth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
        if(!rp) return;
        int lastpipeline = -1, lastsampler = -1;
        GLuint lasttex = ~0U;
        float lastvp[4] = { -1, -1, -1, -1 };
        loopi(p.numdraws)
        {
            gpudraw &d = draws[p.firstdraw + i];
            if(d.pipeline != lastpipeline) { SDL_BindGPUGraphicsPipeline(rp, pipelines[d.pipeline]); lastpipeline = d.pipeline; }
            if(memcmp(lastvp, d.viewport, sizeof(lastvp)))
            {
                SDL_GPUViewport vp = { d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3], 0, 1 };
                SDL_SetGPUViewport(rp, &vp);
                memcpy(lastvp, d.viewport, sizeof(lastvp));
            }
            loopj(NUMATTRS) if(d.vbsrc[j])
            {
                SDL_GPUBufferBinding b = { drawbuffer(d.vbsrc[j], d.vbbuffer[j]), d.vboffset[j] };
                if(b.buffer) SDL_BindGPUVertexBuffers(rp, j, &b, 1);
            }
            SDL_GPUBufferBinding ib = { d.ibsrc == SRC_ARENA ? arenaib : drawbuffer(SRC_BUFFER, d.ibbuffer), 0 };
            if(!ib.buffer) continue;
            SDL_BindGPUIndexBuffer(rp, &ib, d.ibsrc == SRC_ARENA ? SDL_GPU_INDEXELEMENTSIZE_32BIT : SDL_GPU_INDEXELEMENTSIZE_16BIT);
            if(d.tex != lasttex || d.samplerkey != lastsampler)
            {
                SDL_GPUTextureSamplerBinding tsb = { d.tex ? textures[d.tex].tex : whitetex, samplers[d.samplerkey] };
                SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
                lasttex = d.tex; lastsampler = d.samplerkey;
            }
            SDL_PushGPUVertexUniformData(cmd, 0, &d.vs, sizeof(d.vs));
            SDL_PushGPUFragmentUniformData(cmd, 0, &d.fs, sizeof(d.fs));
            SDL_DrawGPUIndexedPrimitives(rp, d.numindices, 1, d.firstindex, 0, 0);
        }
        SDL_EndGPURenderPass(rp);
    }

    void present()
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
            upload(copy, b.buf, b.data.getbuf(), b.data.length(), true);
            b.dirty = false;
        }
        if(vertexarena.length()) { arenavb = growbuffer(arenavb, arenavbsize, vertexarena.length()); upload(copy, arenavb, vertexarena.getbuf(), vertexarena.length(), true); }
        if(indexarena.length()) { arenaib = growbuffer(arenaib, arenaibsize, indexarena.length()*sizeof(uint)); upload(copy, arenaib, indexarena.getbuf(), indexarena.length()*sizeof(uint), true); }
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

        // 2. the main pass and presentation
        loopv(passes) if(passes[i].target == TARGET_MAIN) runpass(cmd, passes[i]);
        SDL_GPUTexture *swap = NULL;
        Uint32 sw = 0, sh = 0;
        if(!benchnopresent && SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh) && swap)
        {
            SDL_GPUBlitInfo bi;
            memset(&bi, 0, sizeof(bi));
            bi.source.texture = colortex; bi.source.w = targetw; bi.source.h = targeth;
            bi.destination.texture = swap; bi.destination.w = sw; bi.destination.h = sh;
            bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
            bi.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cmd, &bi);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
        recording = false;
        curpass = -1;
    }

    // screenshots: the last presented frame, as glReadPixels() would return it (bottom row first)
    void readpixels(int x, int y, int w, int h, GLenum format, GLenum type, void *data)
    {
        if(!dev || !colortex || w <= 0 || h <= 0) return;
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, (Uint32)(w*h*4), 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src;
        memset(&src, 0, sizeof(src));
        src.texture = colortex; src.x = x; src.y = targeth - (y + h); src.w = w; src.h = h; src.d = 1;
        SDL_GPUTextureTransferInfo dst = { tb, 0, (Uint32)w, (Uint32)h };
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(dev, true, &f, 1);
        SDL_ReleaseGPUFence(dev, f);
        const uchar *p = (const uchar *)SDL_MapGPUTransferBuffer(dev, tb, false);
        if(p)
        {
            int bpp = format == GL_RGBA ? 4 : 3;
            uchar *out = (uchar *)data;
            loop(row, h)
            {
                const uchar *s = p + (h - 1 - row)*w*4;
                loop(col, w) { loopk(bpp) out[k] = s[k]; out += bpp; s += 4; }
            }
            SDL_UnmapGPUTransferBuffer(dev, tb);
        }
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }
};

static sdlgpubackend *sdlgpuinstance = NULL;
static void setframesinflight() { if(sdlgpuinstance && sdlgpuinstance->dev) SDL_SetGPUAllowedFramesInFlight(sdlgpuinstance->dev, gpuframes); }
gfxdevice *gfx_sdlgpu()
{
    if(!sdlgpuinstance) sdlgpuinstance = new sdlgpubackend;
    return sdlgpuinstance;
}
