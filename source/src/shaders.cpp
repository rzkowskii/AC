// shaders.cpp: GLSL shaders (OpenGL 2.0) used alongside the fixed-function renderer
//
// - scene ("worldshader 1", default off): the world's surfaces and models are drawn with a shader equivalent to their
//   fixed-function state (texture * colour * 2, linear fog), with the same image. It's the first step of moving the
//   renderer off fixed-function OpenGL; on Apple's OpenGL-on-Metal it measured no faster than fixed-function.
// - gamma: the "gamma" setting is applied to the game's image by a shader, with the same curve the hardware gamma
//   ramp used (SDL: output = input ^ (100 / gamma)). The hardware ramp changes the whole display (macOS) or isn't
//   available at all (Wayland), and stays wrong if the game crashes. "gammashader 0" restores the hardware ramp.
// - fxaa: optional fast approximate antialiasing of the 3D scene (before the HUD, so text stays sharp).
//
//
// Each active post-processing pass resolves the back buffer into a texture (glBlitFramebuffer, which also resolves multisampling) and
// draws it back through a fragment shader. Nothing runs, and nothing is allocated, while no pass is active.

#include "cube.h"

VARFP(gammashader, 0, 1, 1, { extern void applygamma(); applygamma(); });
VARP(fxaa, 0, 0, 1);

extern void *getprocaddress(const char *name);
extern bool hasFBO;
extern PFNGLGENFRAMEBUFFERSEXTPROC glGenFramebuffers_;
extern PFNGLDELETEFRAMEBUFFERSEXTPROC glDeleteFramebuffers_;
extern PFNGLBINDFRAMEBUFFEREXTPROC glBindFramebuffer_;
extern PFNGLFRAMEBUFFERTEXTURE2DEXTPROC glFramebufferTexture2D_;
extern PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC glCheckFramebufferStatus_;
extern PFNGLACTIVETEXTUREARBPROC glActiveTexture_;
extern bool hasMT;

#ifndef GL_READ_FRAMEBUFFER_EXT
#define GL_READ_FRAMEBUFFER_EXT 0x8CA8
#define GL_DRAW_FRAMEBUFFER_EXT 0x8CA9
#endif
typedef void (APIENTRYP ac_glblitframebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static PFNGLCREATESHADERPROC glCreateShader_ = NULL;
static PFNGLSHADERSOURCEPROC glShaderSource_ = NULL;
static PFNGLCOMPILESHADERPROC glCompileShader_ = NULL;
static PFNGLGETSHADERIVPROC glGetShaderiv_ = NULL;
static PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog_ = NULL;
static PFNGLCREATEPROGRAMPROC glCreateProgram_ = NULL;
static PFNGLATTACHSHADERPROC glAttachShader_ = NULL;
static PFNGLLINKPROGRAMPROC glLinkProgram_ = NULL;
static PFNGLGETPROGRAMIVPROC glGetProgramiv_ = NULL;
static PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog_ = NULL;
static PFNGLUSEPROGRAMPROC glUseProgram_ = NULL;
static PFNGLDELETESHADERPROC glDeleteShader_ = NULL;
static PFNGLDELETEPROGRAMPROC glDeleteProgram_ = NULL;
static PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation_ = NULL;
static PFNGLUNIFORM1IPROC glUniform1i_ = NULL;
static PFNGLUNIFORM1FPROC glUniform1f_ = NULL;
static PFNGLUNIFORM2FPROC glUniform2f_ = NULL;
static ac_glblitframebuffer glBlitFramebuffer_ = NULL;

static bool hasGLSL = false, glslchecked = false;

static bool checkglsl()
{
    if(glslchecked) return hasGLSL;
    const char *version = (const char *)glGetString(GL_VERSION);
    if(!version) return false; // no GL context yet: ask again later
    glslchecked = true;
    int major = 0, minor = 0;
    if(sscanf(version, "%d.%d", &major, &minor) != 2 || major < 2) return hasGLSL = false;
    #define GETPROC(var, type, name) var = (type)getprocaddress(name); if(!var) return hasGLSL = false;
    GETPROC(glCreateShader_, PFNGLCREATESHADERPROC, "glCreateShader");
    GETPROC(glShaderSource_, PFNGLSHADERSOURCEPROC, "glShaderSource");
    GETPROC(glCompileShader_, PFNGLCOMPILESHADERPROC, "glCompileShader");
    GETPROC(glGetShaderiv_, PFNGLGETSHADERIVPROC, "glGetShaderiv");
    GETPROC(glGetShaderInfoLog_, PFNGLGETSHADERINFOLOGPROC, "glGetShaderInfoLog");
    GETPROC(glCreateProgram_, PFNGLCREATEPROGRAMPROC, "glCreateProgram");
    GETPROC(glAttachShader_, PFNGLATTACHSHADERPROC, "glAttachShader");
    GETPROC(glLinkProgram_, PFNGLLINKPROGRAMPROC, "glLinkProgram");
    GETPROC(glGetProgramiv_, PFNGLGETPROGRAMIVPROC, "glGetProgramiv");
    GETPROC(glGetProgramInfoLog_, PFNGLGETPROGRAMINFOLOGPROC, "glGetProgramInfoLog");
    GETPROC(glUseProgram_, PFNGLUSEPROGRAMPROC, "glUseProgram");
    GETPROC(glDeleteShader_, PFNGLDELETESHADERPROC, "glDeleteShader");
    GETPROC(glDeleteProgram_, PFNGLDELETEPROGRAMPROC, "glDeleteProgram");
    GETPROC(glGetUniformLocation_, PFNGLGETUNIFORMLOCATIONPROC, "glGetUniformLocation");
    GETPROC(glUniform1i_, PFNGLUNIFORM1IPROC, "glUniform1i");
    GETPROC(glUniform1f_, PFNGLUNIFORM1FPROC, "glUniform1f");
    GETPROC(glUniform2f_, PFNGLUNIFORM2FPROC, "glUniform2f");
    #undef GETPROC
    glBlitFramebuffer_ = (ac_glblitframebuffer)getprocaddress("glBlitFramebufferEXT");
    if(!glBlitFramebuffer_) glBlitFramebuffer_ = (ac_glblitframebuffer)getprocaddress("glBlitFramebuffer");
    return hasGLSL = true;
}

static const char *postvs =
    "#version 120\n"
    "varying vec2 tc;\n"
    "void main() { gl_Position = gl_Vertex; tc = gl_MultiTexCoord0.xy; }\n";

static const char *gammafs =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform float exponent;\n"
    "varying vec2 tc;\n"
    "void main() { gl_FragColor = vec4(pow(texture2D(tex, tc).rgb, vec3(exponent)), 1.0); }\n";

// FXAA as described by Timothy Lottes (NVIDIA), in its common compact form
static const char *fxaafs =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 texel;\n"
    "varying vec2 tc;\n"
    "void main()\n"
    "{\n"
    "    const vec3 luma = vec3(0.299, 0.587, 0.114);\n"
    "    vec3 rgbNW = texture2D(tex, tc + vec2(-1.0, -1.0) * texel).rgb;\n"
    "    vec3 rgbNE = texture2D(tex, tc + vec2( 1.0, -1.0) * texel).rgb;\n"
    "    vec3 rgbSW = texture2D(tex, tc + vec2(-1.0,  1.0) * texel).rgb;\n"
    "    vec3 rgbSE = texture2D(tex, tc + vec2( 1.0,  1.0) * texel).rgb;\n"
    "    vec3 rgbM  = texture2D(tex, tc).rgb;\n"
    "    float lNW = dot(rgbNW, luma), lNE = dot(rgbNE, luma), lSW = dot(rgbSW, luma), lSE = dot(rgbSE, luma), lM = dot(rgbM, luma);\n"
    "    float lmin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));\n"
    "    float lmax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
    "    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "    float rcpmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
    "    dir = clamp(dir * rcpmin, vec2(-8.0), vec2(8.0)) * texel;\n"
    "    vec3 rgbA = 0.5 * (texture2D(tex, tc + dir * (1.0 / 3.0 - 0.5)).rgb + texture2D(tex, tc + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture2D(tex, tc - dir * 0.5).rgb + texture2D(tex, tc + dir * 0.5).rgb);\n"
    "    float lB = dot(rgbB, luma);\n"
    "    gl_FragColor = vec4((lB < lmin || lB > lmax) ? rgbA : rgbB, 1.0);\n"
    "}\n";

// World surfaces: the fixed-function state they're drawn with (setuptmu(0, "T * P x 2") and GL_LINEAR fog), as a shader.
// ftransform() keeps positions (and depth) invariant with the fixed-function passes. Fog uses the radial distance to the
// eye, which matches Apple's fixed-function fog and is never thinner than the eye-plane distance other drivers may use,
// so the shader can't show more through the fog than fixed-function rendering does.
static const char *worldvs =
    "#version 120\n"
    "void main()\n"
    "{\n"
    "    gl_Position = ftransform();\n"
    "    gl_TexCoord[0] = gl_TextureMatrix[0] * gl_MultiTexCoord0;\n"
    "    gl_FrontColor = gl_Color;\n"
    "    gl_FogFragCoord = length((gl_ModelViewMatrix * gl_Vertex).xyz);\n"
    "}\n";

static const char *worldfs =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform float fogon;\n"
    "void main()\n"
    "{\n"
    "    vec4 c = texture2D(tex, gl_TexCoord[0].xy) * gl_Color;\n"
    "    c.rgb = min(c.rgb * 2.0, 1.0);\n"
    "    float f = fogon > 0.5 ? clamp((gl_Fog.end - gl_FogFragCoord) * gl_Fog.scale, 0.0, 1.0) : 1.0;\n"
    "    gl_FragColor = vec4(mix(gl_Fog.color.rgb, c.rgb, f), c.a);\n"
    "}\n";

struct postprogram
{
    const char *name, *fs, *vs;
    GLuint program;
    GLint texloc, paramloc;
    bool failed;
};
static postprogram gammaprog = { "gamma", gammafs, NULL, 0, -1, -1, false }, fxaaprog = { "fxaa", fxaafs, NULL, 0, -1, -1, false },
                   worldprog = { "world", worldfs, worldvs, 0, -1, -1, false };

static bool compileprogram(postprogram &p);

static GLuint compileshader(GLenum type, const char *src, const char *name)
{
    GLuint s = glCreateShader_(type);
    glShaderSource_(s, 1, &src, NULL);
    glCompileShader_(s);
    GLint ok = 0;
    glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
    if(!ok)
    {
        char log[1024] = "";
        glGetShaderInfoLog_(s, sizeof(log), NULL, log);
        conoutf("\f3post-processing shader \"%s\" failed to compile: %s", name, log);
        glDeleteShader_(s);
        return 0;
    }
    return s;
}

extern void applygamma();

static bool loadprogram(postprogram &p)
{
    if(p.program) return true;
    if(p.failed || !checkglsl()) return false;
    bool ok = compileprogram(p);
    if(!ok && &p == &gammaprog) applygamma(); // fall back to the hardware gamma ramp
    return ok;
}

static bool compileprogram(postprogram &p)
{
    GLuint vs = compileshader(GL_VERTEX_SHADER, p.vs ? p.vs : postvs, p.name), fs = vs ? compileshader(GL_FRAGMENT_SHADER, p.fs, p.name) : 0;
    if(!vs || !fs) { if(vs) glDeleteShader_(vs); p.failed = true; return false; }
    p.program = glCreateProgram_();
    glAttachShader_(p.program, vs);
    glAttachShader_(p.program, fs);
    glLinkProgram_(p.program);
    glDeleteShader_(vs);
    glDeleteShader_(fs);
    GLint ok = 0;
    glGetProgramiv_(p.program, GL_LINK_STATUS, &ok);
    if(!ok)
    {
        char log[1024] = "";
        glGetProgramInfoLog_(p.program, sizeof(log), NULL, log);
        conoutf("\f3post-processing shader \"%s\" failed to link: %s", p.name, log);
        glDeleteProgram_(p.program);
        p.program = 0;
        p.failed = true;
        return false;
    }
    p.texloc = glGetUniformLocation_(p.program, "tex");
    p.paramloc = glGetUniformLocation_(p.program, p.fs == gammafs ? "exponent" : (p.fs == worldfs ? "fogon" : "texel"));
    return true;
}

// the screen copy: a texture the size of the drawable, with a framebuffer object for resolving into it
static GLuint screentex = 0, screenfbo = 0;
static int screentexw = 0, screentexh = 0;

static bool grabscreen(int w, int h)
{
    if(!screentex || screentexw != w || screentexh != h)
    {
        if(!screentex) glGenTextures(1, &screentex);
        glBindTexture(GL_TEXTURE_2D, screentex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        screentexw = w;
        screentexh = h;
        if(screenfbo) { glDeleteFramebuffers_(1, &screenfbo); screenfbo = 0; }
    }
    if(hasFBO && glBlitFramebuffer_)
    {
        if(!screenfbo)
        {
            glGenFramebuffers_(1, &screenfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER_EXT, screenfbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, screentex, 0);
            GLenum status = glCheckFramebufferStatus_(GL_FRAMEBUFFER_EXT);
            glBindFramebuffer_(GL_FRAMEBUFFER_EXT, 0);
            if(status != GL_FRAMEBUFFER_COMPLETE_EXT) { glDeleteFramebuffers_(1, &screenfbo); screenfbo = 0; }
        }
        if(screenfbo)
        {   // also resolves a multisampled back buffer
            glBindFramebuffer_(GL_READ_FRAMEBUFFER_EXT, 0);
            glBindFramebuffer_(GL_DRAW_FRAMEBUFFER_EXT, screenfbo);
            glBlitFramebuffer_(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer_(GL_FRAMEBUFFER_EXT, 0);
            return true;
        }
    }
    // no framebuffer blit: a plain copy works unless the back buffer is multisampled
    GLint samplebuffers = 0;
    glGetIntegerv(GL_SAMPLE_BUFFERS, &samplebuffers);
    if(samplebuffers > 0) return false;
    glBindTexture(GL_TEXTURE_2D, screentex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    return true;
}

static void drawpass(postprogram &p, int w, int h, float param)
{
    if(!loadprogram(p) || !grabscreen(w, h)) return;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_FOG); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if(hasMT && glActiveTexture_) glActiveTexture_(GL_TEXTURE0_ARB);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, screentex);
    glUseProgram_(p.program);
    glUniform1i_(p.texloc, 0);
    if(p.fs == gammafs) glUniform1f_(p.paramloc, param);
    else glUniform2f_(p.paramloc, 1.0f / w, 1.0f / h);
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glVertex2f( 1, -1);
    glTexCoord2f(0, 1); glVertex2f(-1,  1);
    glTexCoord2f(1, 1); glVertex2f( 1,  1);
    glEnd();
    glUseProgram_(0);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glPopAttrib();
}

// can the gamma setting be applied by the shader? (otherwise the hardware gamma ramp is used)
bool postfxgamma()
{
    return gammashader && checkglsl() && !gammaprog.failed;
}

void renderpostfxscene(int w, int h) // after the 3D scene, before the HUD
{
    if(fxaa) drawpass(fxaaprog, w, h, 0);
}

void renderpostfxfinal(int w, int h) // after everything, including the HUD and menus
{
    extern int vgamma;
    if(vgamma != 100 && postfxgamma()) drawpass(gammaprog, w, h, 100.0f / vgamma);
}

VARP(worldshader, 0, 0, 1);

// Binds the scene shader if it can replace the current fixed-function state: texturing on, with texture unit 0 set up as
// setuptmu(0, "T * P x 2"). Used for the world's strips and for models; alpha test, alpha-to-coverage and blending are
// fixed-function operations after the shader and keep working. Call endworldshader() after drawing if it returned true.
bool beginworldshader()
{
    extern bool tmuismodulate2x(int n);
    if(!worldshader || !tmuismodulate2x(0) || !glIsEnabled(GL_TEXTURE_2D) || !loadprogram(worldprog)) return false;
    glUseProgram_(worldprog.program);
    glUniform1i_(worldprog.texloc, 0);
    glUniform1f_(worldprog.paramloc, glIsEnabled(GL_FOG) ? 1.0f : 0.0f);
    return true;
}

void endworldshader() { glUseProgram_(0); }


void cleanuppostfx() // the GL context is about to be destroyed (resetgl)
{
    if(screentex) glDeleteTextures(1, &screentex);
    if(screenfbo) glDeleteFramebuffers_(1, &screenfbo);
    screentex = screenfbo = 0;
    screentexw = screentexh = 0;
    if(gammaprog.program) glDeleteProgram_(gammaprog.program);
    if(fxaaprog.program) glDeleteProgram_(fxaaprog.program);
    if(worldprog.program) glDeleteProgram_(worldprog.program);
    gammaprog.program = fxaaprog.program = worldprog.program = 0;
    gammaprog.failed = fxaaprog.failed = worldprog.failed = false;
    glslchecked = false;
}
