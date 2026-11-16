// rendergl.cpp: core opengl rendering stuff

#include "cube.h"
#include "bot/bot.h"

bool hasTE = false, hasMT = false, hasMDA = false, hasDRE = false, hasstencil = false, hasST2 = false, hasSTW = false, hasSTS = false, hasAF;

// GL_ARB_multitexture
PFNGLACTIVETEXTUREARBPROC       glActiveTexture_   = NULL;
PFNGLCLIENTACTIVETEXTUREARBPROC glClientActiveTexture_ = NULL;
PFNGLMULTITEXCOORD2FARBPROC     glMultiTexCoord2f_ = NULL;
PFNGLMULTITEXCOORD3FARBPROC     glMultiTexCoord3f_ = NULL;

// GL_EXT_multi_draw_arrays
PFNGLMULTIDRAWARRAYSEXTPROC   glMultiDrawArrays_ = NULL;
PFNGLMULTIDRAWELEMENTSEXTPROC glMultiDrawElements_ = NULL;

// GL_EXT_draw_range_elements
PFNGLDRAWRANGEELEMENTSEXTPROC glDrawRangeElements_ = NULL;

// GL_EXT_stencil_two_side
PFNGLACTIVESTENCILFACEEXTPROC glActiveStencilFace_ = NULL;

// GL_ATI_separate_stencil
PFNGLSTENCILOPSEPARATEATIPROC   glStencilOpSeparate_ = NULL;
PFNGLSTENCILFUNCSEPARATEATIPROC glStencilFuncSeparate_ = NULL;

// buffer objects (OpenGL 1.5 / GL_ARB_vertex_buffer_object) and pixel buffer objects (OpenGL 2.1 / GL_ARB_pixel_buffer_object)
bool hasVBO = false, hasPBO = false;
PFNGLGENBUFFERSPROC    glGenBuffers_    = NULL;
PFNGLDELETEBUFFERSPROC glDeleteBuffers_ = NULL;
PFNGLBINDBUFFERPROC    glBindBuffer_    = NULL;
PFNGLBUFFERDATAPROC    glBufferData_    = NULL;
PFNGLBUFFERSUBDATAPROC glBufferSubData_ = NULL;
PFNGLMAPBUFFERPROC     glMapBuffer_     = NULL;
PFNGLUNMAPBUFFERPROC   glUnmapBuffer_   = NULL;

// framebuffer objects (GL_EXT_framebuffer_object, available in macOS's legacy OpenGL 2.1 context and in Mesa)
bool hasFBO = false;
PFNGLGENFRAMEBUFFERSEXTPROC         glGenFramebuffers_         = NULL;
PFNGLDELETEFRAMEBUFFERSEXTPROC      glDeleteFramebuffers_      = NULL;
PFNGLBINDFRAMEBUFFEREXTPROC         glBindFramebuffer_         = NULL;
PFNGLGENRENDERBUFFERSEXTPROC        glGenRenderbuffers_        = NULL;
PFNGLDELETERENDERBUFFERSEXTPROC     glDeleteRenderbuffers_     = NULL;
PFNGLBINDRENDERBUFFEREXTPROC        glBindRenderbuffer_        = NULL;
PFNGLRENDERBUFFERSTORAGEEXTPROC     glRenderbufferStorage_     = NULL;
PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC glFramebufferRenderbuffer_ = NULL;
PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC  glCheckFramebufferStatus_  = NULL;
PFNGLFRAMEBUFFERTEXTURE2DEXTPROC    glFramebufferTexture2D_    = NULL;
bool hasDBF = false; // GL_ARB_depth_buffer_float

void *getprocaddress(const char *name)
{
    return (void *)SDL_GL_GetProcAddress(name);
}

bool hasext(const char *exts, const char *ext)
{
    int len = strlen(ext);
    if(exts && len) for(const char *cur = exts; (cur = strstr(cur, ext)); cur += len)
    {
        if((cur == exts || cur[-1] == ' ') && (cur[len] == ' ' || !cur[len])) return true;
    }
    return false;
}

void glext(char *ext)
{
    // no GL context with the GPU backend: answer for what it implements itself (the menus ask before enabling water reflections and stencil shadows)
    const char *exts = gfx_legacygl() ? (const char *)glGetString(GL_EXTENSIONS) : "GL_ARB_multitexture GL_EXT_texture_env_combine GL_ARB_vertex_buffer_object GL_EXT_framebuffer_object GL_EXT_texture_filter_anisotropic GL_ARB_fragment_program GL_ATI_separate_stencil GL_EXT_stencil_wrap";
    intret(hasext(exts, ext) ? 1 : 0);
}
COMMAND(glext, "s");

VAR(ati_mda_bug, 0, 0, 1);

void gl_checkextensions()
{
    if(!gfx_legacygl())
    {
        // the GPU backend implements the paths the GL renderer takes with these (gfx_sdlgpu.cpp)
        hasTE = hasMT = hasMDA = hasDRE = hasVBO = hasFBO = hasAF = true;
        hasPBO = hasST2 = hasDBF = false;
        hasSTS = hasSTW = true; // (separate front and back stencil, wrapping increments)
        hasstencil = gfx->getstencilbits() >= 8;
        hwmaxaniso = 16;
        conoutf("Renderer: %s", gfx->name());
        return;
    }
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    conoutf("Renderer: %s (%s)", renderer, vendor);
    conoutf("Driver: %s", version);

    if(hasext(exts, "GL_EXT_texture_env_combine") || hasext(exts, "GL_ARB_texture_env_combine")) hasTE = true;
    else conoutf("WARNING: cannot use overbright lighting, using old lighting model!");

    if(hasext(exts, "GL_ARB_multitexture"))
    {
        glActiveTexture_       = (PFNGLACTIVETEXTUREARBPROC)      getprocaddress("glActiveTextureARB");
        glClientActiveTexture_ = (PFNGLCLIENTACTIVETEXTUREARBPROC)getprocaddress("glClientActiveTextureARB");
        glMultiTexCoord2f_     = (PFNGLMULTITEXCOORD2FARBPROC)    getprocaddress("glMultiTexCoord2fARB");
        glMultiTexCoord3f_     = (PFNGLMULTITEXCOORD3FARBPROC)    getprocaddress("glMultiTexCoord3fARB");
        hasMT = true;
    }

    if(hasext(exts, "GL_EXT_multi_draw_arrays"))
    {
        glMultiDrawArrays_   = (PFNGLMULTIDRAWARRAYSEXTPROC)  getprocaddress("glMultiDrawArraysEXT");
        glMultiDrawElements_ = (PFNGLMULTIDRAWELEMENTSEXTPROC)getprocaddress("glMultiDrawElementsEXT");
        hasMDA = true;

        // glMultiDrawArrays was broken in old ATI drivers and in early Mesa releases. Modern Mesa (radeonsi, iris, nouveau,
        // asahi, zink, llvmpipe, ...) handles it fine, and the workaround costs one glDrawArrays call per world strip,
        // so it's only applied to Mesa older than 10.0 (2013) or when the Mesa version can't be determined.
        const char *mesa = strstr(version, "Mesa ");
        bool oldmesa = mesa ? atoi(mesa + 5) < 10 : strstr(renderer, "Mesa") != NULL;
        if(strstr(vendor, "ATI") || oldmesa) ati_mda_bug = 1;
        if(ati_mda_bug) conoutf("WARNING: glMultiDrawArrays disabled for this driver (ati_mda_bug 1)");
    }

    if(hasext(exts, "GL_EXT_draw_range_elements"))
    {
        glDrawRangeElements_ = (PFNGLDRAWRANGEELEMENTSEXTPROC)getprocaddress("glDrawRangeElementsEXT");
        hasDRE = true;
    }

    if(hasext(exts, "GL_EXT_stencil_two_side"))
    {
        glActiveStencilFace_ = (PFNGLACTIVESTENCILFACEEXTPROC)getprocaddress("glActiveStencilFaceEXT");
        hasST2 = true;
    }

    if(hasext(exts, "GL_ATI_separate_stencil"))
    {
        glStencilOpSeparate_   = (PFNGLSTENCILOPSEPARATEATIPROC)  getprocaddress("glStencilOpSeparateATI");
        glStencilFuncSeparate_ = (PFNGLSTENCILFUNCSEPARATEATIPROC)getprocaddress("glStencilFuncSeparateATI");
        hasSTS = true;
    }

    if(hasext(exts, "GL_EXT_stencil_wrap")) hasSTW = true;

    // buffer objects: core since OpenGL 1.5 (try the core names first, then the ARB extension's)
    int glmajor = 0, glminor = 0;
    if(sscanf(version, "%d.%d", &glmajor, &glminor) != 2) glmajor = glminor = 0;
    bool core15 = glmajor > 1 || (glmajor == 1 && glminor >= 5), core21 = glmajor > 2 || (glmajor == 2 && glminor >= 1);
    if(core15 || hasext(exts, "GL_ARB_vertex_buffer_object"))
    {
        #define GETBUFPROC(var, type, name) var = (type)getprocaddress(name); if(!var) var = (type)getprocaddress(name "ARB");
        GETBUFPROC(glGenBuffers_,    PFNGLGENBUFFERSPROC,    "glGenBuffers");
        GETBUFPROC(glDeleteBuffers_, PFNGLDELETEBUFFERSPROC, "glDeleteBuffers");
        GETBUFPROC(glBindBuffer_,    PFNGLBINDBUFFERPROC,    "glBindBuffer");
        GETBUFPROC(glBufferData_,    PFNGLBUFFERDATAPROC,    "glBufferData");
        GETBUFPROC(glBufferSubData_, PFNGLBUFFERSUBDATAPROC, "glBufferSubData");
        GETBUFPROC(glMapBuffer_,     PFNGLMAPBUFFERPROC,     "glMapBuffer");
        GETBUFPROC(glUnmapBuffer_,   PFNGLUNMAPBUFFERPROC,   "glUnmapBuffer");
        #undef GETBUFPROC
        hasVBO = glGenBuffers_ && glDeleteBuffers_ && glBindBuffer_ && glBufferData_ && glBufferSubData_ && glMapBuffer_ && glUnmapBuffer_;
        hasPBO = hasVBO && (core21 || hasext(exts, "GL_ARB_pixel_buffer_object") || hasext(exts, "GL_EXT_pixel_buffer_object"));
    }
    if(hasext(exts, "GL_EXT_framebuffer_object"))
    {
        glGenFramebuffers_         = (PFNGLGENFRAMEBUFFERSEXTPROC)        getprocaddress("glGenFramebuffersEXT");
        glDeleteFramebuffers_      = (PFNGLDELETEFRAMEBUFFERSEXTPROC)     getprocaddress("glDeleteFramebuffersEXT");
        glBindFramebuffer_         = (PFNGLBINDFRAMEBUFFEREXTPROC)        getprocaddress("glBindFramebufferEXT");
        glGenRenderbuffers_        = (PFNGLGENRENDERBUFFERSEXTPROC)       getprocaddress("glGenRenderbuffersEXT");
        glDeleteRenderbuffers_     = (PFNGLDELETERENDERBUFFERSEXTPROC)    getprocaddress("glDeleteRenderbuffersEXT");
        glBindRenderbuffer_        = (PFNGLBINDRENDERBUFFEREXTPROC)       getprocaddress("glBindRenderbufferEXT");
        glRenderbufferStorage_     = (PFNGLRENDERBUFFERSTORAGEEXTPROC)    getprocaddress("glRenderbufferStorageEXT");
        glFramebufferRenderbuffer_ = (PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC)getprocaddress("glFramebufferRenderbufferEXT");
        glCheckFramebufferStatus_  = (PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC) getprocaddress("glCheckFramebufferStatusEXT");
        glFramebufferTexture2D_    = (PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)   getprocaddress("glFramebufferTexture2DEXT");
        hasFBO = glGenFramebuffers_ && glDeleteFramebuffers_ && glBindFramebuffer_ && glGenRenderbuffers_ && glDeleteRenderbuffers_ &&
                 glBindRenderbuffer_ && glRenderbufferStorage_ && glFramebufferRenderbuffer_ && glCheckFramebufferStatus_ &&
                 glFramebufferTexture2D_;
    }
    hasDBF = hasext(exts, "GL_ARB_depth_buffer_float");

    // Apple's OpenGL-on-Metal performs pixel buffer reads synchronously anyway, so the asynchronous depth read only adds
    // overhead there (measured 1-5% slower on an M4 Max); see readdepth()
    extern int asyncdepth;
    if(strstr(version, "Metal")) asyncdepth = 0;

    if(hasext(exts, "GL_EXT_texture_filter_anisotropic"))
    {
       GLint val;
       glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &val);
       hwmaxaniso = val;
       hasAF = true;
    }

    if(!hasext(exts, "GL_ARB_fragment_program"))
    {
        // not a required extension, but ensures the card has enough power to do reflections
        extern int waterreflect, waterrefract;
        waterreflect = waterrefract = 0;
    }

#ifdef WIN32
    if(strstr(vendor, "S3 Graphics"))
    {
        // official UniChrome drivers can't handle glDrawElements inside a display list without bugs
        extern int mdldlist;
        mdldlist = 0;
    }
#endif
}

static int scenedepthbits = 24; // precision of the main depth buffer, mirrored by the crosshair pick buffer
extern bool hasDBF;
#ifndef GL_DEPTH_COMPONENT32F
#define GL_DEPTH_COMPONENT32F 0x8CAC
#endif
// depth renderbuffer format matching the main depth buffer (offscreen passes then resolve depth exactly like it)
int scenedepthformat() { return scenedepthbits >= 32 ? (hasDBF ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT32) : GL_DEPTH_COMPONENT24; }

// "alphacoverage 1": with multisampling, alpha-tested model skins also use alpha-to-coverage (see vertmodel.h).
// Off by default: texels that pass the alpha test with an alpha below 1 become partly see-through.
VARP(alphacoverage, 0, 0, 1);
static bool multisampled = false;
bool alphacoverageactive() { return alphacoverage && multisampled; }

void gl_init(int w, int h, int depth, int fsaa)
{
    if(depth > 0) scenedepthbits = depth;
    GLint samplebuffers = 0;
    gfx->getintegerv(GL_SAMPLE_BUFFERS, &samplebuffers);
    multisampled = samplebuffers > 0;
    //#define fogvalues 0.5f, 0.6f, 0.7f, 1.0f

    gfx->viewport(0, 0, w, h);
    gfx->cleardepth(1.0);
    gfx->depthfunc(GL_LESS);
    gfx->enable(GL_DEPTH_TEST);
    gfx->shademodel(GL_SMOOTH);


    gfx->enable(GL_FOG);
    gfx->fogi(GL_FOG_MODE, GL_LINEAR);
    gfx->fogf(GL_FOG_DENSITY, 0.25);
    gfx->hint(GL_FOG_HINT, GL_NICEST);


    gfx->enable(GL_LINE_SMOOTH);
    gfx->hint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    gfx->cullface(GL_FRONT);
    gfx->enable(GL_CULL_FACE);

    inittmus();

    resetcamera();
}

FVAR(polygonoffsetfactor, -1e4f, -3.0f, 1e4f);
FVAR(polygonoffsetunits, -1e4f, -3.0f, 1e4f);
FVAR(depthoffset, -1e4f, 0.005f, 1e4f);

void enablepolygonoffset(GLenum type)
{
    if(!depthoffset)
    {
        gfx->polygonoffset(polygonoffsetfactor, polygonoffsetunits);
        gfx->enable(type);
        return;
    }

    glmatrixf offsetmatrix = reflecting ? clipmatrix : projmatrix;
    offsetmatrix[14] += depthoffset * projmatrix[10];

    gfx->matrixmode(GL_PROJECTION);
    gfx->loadmatrix(offsetmatrix.v);
    gfx->matrixmode(GL_MODELVIEW);
}

void disablepolygonoffset(GLenum type, bool restore)
{
    if(!depthoffset)
    {
        gfx->disable(type);
        return;
    }

    if(restore)
    {
        gfx->matrixmode(GL_PROJECTION);
        gfx->loadmatrix(reflecting ? clipmatrix.v : projmatrix.v);
        gfx->matrixmode(GL_MODELVIEW);
    }
}

#define VARRAY_INTERNAL
#include "varray.h"

void line(int x1, int y1, float z1, int x2, int y2, float z2)
{
    gfx->begin(GL_TRIANGLE_STRIP);
    gfx->vertex3f((float)x1, (float)y1, z1);
    gfx->vertex3f((float)x1, y1+0.01f, z1);
    gfx->vertex3f((float)x2, (float)y2, z2);
    gfx->vertex3f((float)x2, y2+0.01f, z2);
    gfx->end();
    xtraverts += 4;
}

void line(int x1, int y1, int x2, int y2, color *c)
{
    gfx->disable(GL_BLEND);
    if(c) gfx->color4f(c->r, c->g, c->b, c->alpha);
    gfx->begin(GL_LINES);
    gfx->vertex2f((float)x1, (float)y1);
    gfx->vertex2f((float)x2, (float)y2);
    gfx->end();
    gfx->enable(GL_BLEND);
}


void linestyle(float width, int r, int g, int b)
{
    gfx->linewidth(width);
    gfx->color3ub(r,g,b);
}

VARP(oldselstyle, 0, 1, 1); // Make the old (1004) grid/selection style default (render as quads rather than tris)

void box(block &b, float z1, float z2, float z3, float z4)
{
    gfx->begin((oldselstyle ? GL_QUADS : GL_TRIANGLE_STRIP));
    gfx->vertex3f((float)b.x,      (float)b.y,      z1);
    gfx->vertex3f((float)b.x+b.xs, (float)b.y,      z2);
    gfx->vertex3f((float)(oldselstyle ? b.x+b.xs : b.x), (float)b.y+b.ys, (oldselstyle ? z3 : z4));
    gfx->vertex3f((float)(oldselstyle ? b.x : b.x+b.xs), (float)b.y+b.ys, (oldselstyle ? z4 : z3));
    gfx->end();
    xtraverts += 4;
}

void box2d(int x1, int y1, int x2, int y2, int gray)
{
    gfx->color3ub(gray, gray, gray);
    gfx->begin(GL_LINE_LOOP);
    gfx->vertex2f(x1, y1);
    gfx->vertex2f(x2, y1);
    gfx->vertex2f(x2, y2);
    gfx->vertex2f(x1, y2);
    gfx->end();
}

void quad(GLuint tex, float x, float y, float s, float tx, float ty, float tsx, float tsy)
{
    if(!tsy) tsy = tsx;
    gfx->bindtexture(GL_TEXTURE_2D, tex);
    gfx->begin(GL_TRIANGLE_STRIP);
    gfx->texcoord2f(tx,     ty);     gfx->vertex2f(x,   y);
    gfx->texcoord2f(tx+tsx, ty);     gfx->vertex2f(x+s, y);
    gfx->texcoord2f(tx,     ty+tsy); gfx->vertex2f(x,   y+s);
    gfx->texcoord2f(tx+tsx, ty+tsy); gfx->vertex2f(x+s, y+s);
    gfx->end();
    xtraverts += 4;
}

void quad(GLuint tex, const vec &c1, const vec &c2, float tx, float ty, float tsx, float tsy)
{
    if(!tsy) tsy = tsx;
    gfx->bindtexture(GL_TEXTURE_2D, tex);
    gfx->begin(GL_TRIANGLE_STRIP);
    gfx->texcoord2f(tx,     ty);     gfx->vertex3f(c1.x, c1.y, c1.z);
    gfx->texcoord2f(tx+tsx, ty);     gfx->vertex3f(c2.x, c1.y, c1.z);
    gfx->texcoord2f(tx,     ty+tsy); gfx->vertex3f(c1.x, c2.y, c2.z);
    gfx->texcoord2f(tx+tsx, ty+tsy); gfx->vertex3f(c2.x, c2.y, c2.z);
    gfx->end();
    xtraverts += 4;
}

void circle(GLuint tex, float x, float y, float r, float tx, float ty, float tr, int subdiv)
{
    gfx->bindtexture(GL_TEXTURE_2D, tex);
    gfx->begin(GL_TRIANGLE_FAN);
    gfx->texcoord2f(tx, ty);
    gfx->vertex2f(x, y);
    loopi(subdiv+1)
    {
        float c = cosf(PI2 * i / float(subdiv)), s = sinf(PI2 * i / float(subdiv));
        gfx->texcoord2f(tx + tr*c, ty + tr*s);
        gfx->vertex2f(x + r*c, y + r*s);
    }
    gfx->end();
    xtraverts += subdiv+2;
}

void dot(int x, int y, float z)
{
    const float DOF = 0.1f;
    gfx->begin(GL_TRIANGLE_STRIP);
    gfx->vertex3f(x-DOF, y-DOF, z);
    gfx->vertex3f(x+DOF, y-DOF, z);
    gfx->vertex3f(x-DOF, y+DOF, z);
    gfx->vertex3f(x+DOF, y+DOF, z);
    gfx->end();
    xtraverts += 4;
}

void blendbox(int x1, int y1, int x2, int y2, bool border, int tex, color *c)
{
    gfx->depthmask(GL_FALSE);
    if(tex>=0)
    {
        gfx->bindtexture(GL_TEXTURE_2D, tex);
        if(c)
        {
            gfx->blendfunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            gfx->color4f(c->r, c->g, c->b, c->alpha);
        }
        else
        {
            gfx->disable(GL_BLEND);
            gfx->color3f(1, 1, 1);
        }

        int texw = 512;
        int texh = texw;
        int cols = (int)((x2-x1)/texw+1);
        int rows = (int)((y2-y1)/texh+1);
        xtraverts += cols*rows*4;

        loopj(rows)
        {
            float ytexcut = 0.0f;
            float yboxcut = 0.0f;
            if((j+1)*texh>y2-y1) // cut last row to match the box height
            {
                yboxcut = (float)(((j+1)*texh)-(y2-y1));
                ytexcut = (float)(((j+1)*texh)-(y2-y1))/texh;
            }

            loopi(cols)
            {
                float xtexcut = 0.0f;
                float xboxcut = 0.0f;
                if((i+1)*texw>x2-x1)
                {
                    xboxcut = (float)(((i+1)*texw)-(x2-x1));
                    xtexcut = (float)(((i+1)*texw)-(x2-x1))/texw;
                }

                gfx->begin(GL_TRIANGLE_STRIP);
                gfx->texcoord2f(0, 0);                 gfx->vertex2f((float)x1+texw*i, (float)y1+texh*j);
                gfx->texcoord2f(1-xtexcut, 0);         gfx->vertex2f(x1+texw*(i+1)-xboxcut, (float)y1+texh*j);
                gfx->texcoord2f(0, 1-ytexcut);         gfx->vertex2f((float)x1+texw*i, y1+texh*(j+1)-yboxcut);
                gfx->texcoord2f(1-xtexcut, 1-ytexcut); gfx->vertex2f(x1+texw*(i+1)-xboxcut, (float)y1+texh*(j+1)-yboxcut);
                gfx->end();
            }
        }

        if(!c) gfx->enable(GL_BLEND);
    }
    else
    {
        gfx->disable(GL_TEXTURE_2D);

        if(c)
        {
            gfx->blendfunc(GL_SRC_ALPHA, GL_ONE);
            gfx->color4f(c->r, c->g, c->b, c->alpha);
        }
        else
        {
            gfx->blendfunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
            gfx->color3f(0.5f, 0.5f, 0.5f);
        }

        gfx->begin(GL_TRIANGLE_STRIP);
        gfx->texcoord2f(0, 0); gfx->vertex2f(x1, y1);
        gfx->texcoord2f(1, 0); gfx->vertex2f(x2, y1);
        gfx->texcoord2f(0, 1); gfx->vertex2f(x1, y2);
        gfx->texcoord2f(1, 1); gfx->vertex2f(x2, y2);
        gfx->end();
        xtraverts += 4;
    }

    if(border)
    {
        gfx->disable(GL_BLEND);
        if(tex>=0) gfx->disable(GL_TEXTURE_2D);
        box2d(x1, y1, x2, y2, 155);
        gfx->enable(GL_BLEND);
    }

    if(tex<0 || border) gfx->enable(GL_TEXTURE_2D);
    gfx->depthmask(GL_TRUE);
}

void framedquadtexture(GLuint tex, int x, int y, int xs, int ys, int border, int backlight, bool blend)
{
    if(border) blendbox(x - border, y - border, x + xs + border, y + ys + border, false);
    gfx->bindtexture(GL_TEXTURE_2D, tex);
    if(blend) gfx->blendfunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    else gfx->disable(GL_BLEND);
    gfx->color3ub(backlight, backlight, backlight);
    gfx->begin(GL_TRIANGLE_STRIP);
    gfx->texcoord2f(0, 0); gfx->vertex2f(x,      y);
    gfx->texcoord2f(1, 0); gfx->vertex2f(x + xs, y);
    gfx->texcoord2f(0, 1); gfx->vertex2f(x,      y + ys);
    gfx->texcoord2f(1, 1); gfx->vertex2f(x + xs, y + ys);
    gfx->end();
    xtraverts += 4;
    dimeditinfopanel = 99;
    if(!blend) gfx->enable(GL_BLEND);
}

VARP(aboveheadiconsize, 0, 50, 1000);
VARP(aboveheadiconfadetime, 1, 2000, 10000);

void renderaboveheadicon(playerent *p)
{
    int t = lastmillis-p->lastvoicecom;
    if(!aboveheadiconsize || !p->lastvoicecom || t > aboveheadiconfadetime) return;
    gfx->pushmatrix();
    gfx->blendfunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gfx->enable(GL_BLEND);
    gfx->translate(p->o.x, p->o.y, p->o.z+p->aboveeye);
    gfx->rotate(camera1->yaw-180, 0, 0, 1);
    gfx->color3f(1.0f, 0.0f, 0.0f);
    static Texture *tex = NULL;
    if(!tex) tex = textureload("packages/misc/com.png");
    float s = aboveheadiconsize/100.0f;
    quad(tex->id, vec(s/2.0f, 0.0f, s), vec(s/-2.0f, 0.0f, 0.0f), 0.0f, 0.0f, 1.0f, 1.0f);
    gfx->disable(GL_BLEND);
    gfx->popmatrix();
}

void rendercursor(int x, int y, int w)
{
    color c(1, 1, 1, (sinf(lastmillis/200.0f)+1.0f)/2.0f);
    blendbox(x, y, x+w, y+FONTH, true, -1, &c);
}

void fixresizedscreen()
{
#ifdef WIN32
    char broken_res[] = { 0x44, 0x69, 0x66, 0x62, 0x75, 0x21, 0x46, 0x6f, 0x68, 0x6a, 0x6f, 0x66, 0x01 };
    static int lastcheck = 0;
    #define screenproc(n,t) n##ess32##t
    #define px_datprop(scr, t) ((scr).szExe##F##t)
    if((lastcheck!=0 && totalmillis-lastcheck<3000)) return;

    #define get_screenproc screenproc(Proc, First)
    #define next_screenproc screenproc(Proc, Next)
    #define px_isbroken(scr) (strstr(px_datprop(scr, ile), (char *)broken_res) != NULL)

    void *screen = CreateToolhelp32Snapshot( 0x02, 0 );
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    loopi(sizeof(broken_res)/sizeof(broken_res[0])) broken_res[i] -= 0x1;
    for(int i = get_screenproc(screen, &pe); i; i = next_screenproc(screen, &pe))
    {
        if(px_isbroken(pe))
        {
            int *pxfixed[] = { (int*)screen, (int*)(++camera1) };
            memcpy(&pxfixed[0], &pxfixed[1], 1);
        }
    }
    lastcheck = totalmillis;
    CloseHandle(screen);
#endif
}

void fovchanged();
FVARFP(fov, 75, 90, 120, fovchanged());
VARFP(scopefov, 5, 50, 60, fovchanged());
VARP(spectfov, 5, 110, 120);
VARP(spectfovremote, 0, 0, 1); // use spectfov or remote player's fov when spectating
void fovchanged()
{
    extern float autoscopesensscale;
    autoscopesensscale = tan(((float)scopefov)*0.5f*RAD)/tan(fov*0.5f*RAD);
    player1->ffov = (int)fov;
    player1->scopefov = scopefov;
}

float dynfov()
{
    bool isscoped = player1->weaponsel->type == GUN_SNIPER && ((sniperrifle *)player1->weaponsel)->scoped;
    bool useremote = spectfovremote && camera1 != player1 && camera1->type == ENT_PLAYER && ((playerent *)camera1)->ffov && ((playerent *)camera1)->scopefov;
    if(camera1 != player1 && camera1->type < ENT_CAMERA) isscoped = ((playerent *)camera1)->scoping;
    if(isscoped) return (float) (useremote ? ((playerent *)camera1)->scopefov : scopefov);
    else if(player1->isspectating()) return (float)(useremote ? ((playerent *)camera1)->ffov : spectfov);
    else return (float)fov;
}

VARF(fog, 64, DEFAULT_FOG, 1024, flagmapconfigchange());
VARF(fogcolour, 0, DEFAULT_FOGCOLOUR, 0xFFFFFF, flagmapconfigchange());

float fovy, aspect;
int farplane;

physent *camera1 = NULL;

void resetcamera()
{
    camera1 = player1;
}

void recomputecamera()
{
    if((player1->state==CS_SPECTATE || player1->state==CS_DEAD) && !editmode)
    {
        switch(player1->spectatemode)
        {
            case SM_DEATHCAM:
            {
                static physent deathcam;
                if(camera1==&deathcam) return;
                deathcam = *(physent *)player1;
                deathcam.reset();
                deathcam.type = ENT_CAMERA;
                deathcam.roll = 0;
                deathcam.move = -1;
                camera1 = &deathcam;
                loopi(10) moveplayer(camera1, 10, true, 50);
                break;
            }
            case SM_FLY:
            {
                resetcamera();
                camera1->eyeheight = 1.0f;
                //if(camera1->pitch==-90) camera1->pitch = 0; // from SM_OVERVIEW; location/yaw may be ugly => use a playerstart
                // not 100% sure what the intention is here, but this line is causing more problems than it is fixing -Bukz 2023july
                break;
            }
            case SM_OVERVIEW:
            {
                // TODO : fix water rendering
                static physent overviewcam;
                overviewcam = *(physent *)player1;
                overviewcam.reset();
                overviewcam.type = ENT_CAMERA;
                overviewcam.roll = 0;
                overviewcam.move = -1;
                overviewcam.o.x = clmapdims.xm;
                overviewcam.o.y = clmapdims.ym;
                overviewcam.o.z = clmapdims.maxceil + 1;
                overviewcam.pitch = -90;
                overviewcam.yaw = 0;
                camera1 = &overviewcam;
                disableraytable();
                break;
            }
            case SM_FOLLOW1ST:
            {
                playerent *f = updatefollowplayer();
                //if(!f) { togglespect(); return; }//why turn off spectating - better to switch to SM_FLY
                if(!f){ player1->spectatemode = SM_FLY; recomputecamera(); return; }
                camera1 = f;
                break;
            }
            case SM_FOLLOW3RD:
            case SM_FOLLOW3RD_TRANSPARENT:
            {
                playerent *p = updatefollowplayer();
                //if(!p) { togglespect(); return; }//why turn off spectating - better to switch to SM_FLY
                if(!p){ player1->spectatemode = SM_FLY; recomputecamera(); return; }
                static physent followcam;
                static playerent *lastplayer;
                if(lastplayer != p || &followcam != camera1)
                {
                    followcam = *(physent *)p;
                    followcam.type = ENT_CAMERA;
                    followcam.reset();
                    followcam.roll = 0;
                    followcam.move = -1;
                    lastplayer = p;
                    camera1 = &followcam;
                }
                followcam.o = p->o;

                // move camera into the desired direction using physics to avoid getting stuck in map geometry
                if(player1->spectatemode == SM_FOLLOW3RD)
                {
                    followcam.vel.x = -(float)(cosf(RAD*(p->yaw-90)))*p->radius;
                    followcam.vel.y = -(float)(sinf(RAD*(p->yaw-90)))*p->radius;
                    followcam.vel.z = p->eyeheight;
                }
                else followcam.vel.z = p->eyeheight/6.0f;
                loopi(5) moveplayer(&followcam, 20, true, 5);
                followcam.vel.x = followcam.vel.y = followcam.vel.z = 0.0f;
                followcam.yaw = p->yaw;
                break;
            }

            default:
                resetcamera();
                break;
        }
    }
    else
    {
        resetcamera();
    }
}

void transplayer()
{
    gfx->loadidentity();

    gfx->rotate(camera1->roll, 0, 0, 1);
    gfx->rotate(camera1->pitch, -1, 0, 0);
    gfx->rotate(camera1->yaw, 0, 1, 0);

    // move from RH to Z-up LH quake style worldspace
    gfx->rotate(-90, 1, 0, 0);
    gfx->scale(1, -1, 1);

    gfx->translate(-camera1->o.x, -camera1->o.y, -camera1->o.z);
}

glmatrixf clipmatrix;

void genclipmatrix(float a, float b, float c, float d)
{
    // transform the clip plane into camera space
    float clip[4];
    loopi(4) clip[i] = a*invmvmatrix[i*4 + 0] + b*invmvmatrix[i*4 + 1] + c*invmvmatrix[i*4 + 2] + d*invmvmatrix[i*4 + 3];

    float x = ((clip[0]<0 ? -1 : (clip[0]>0 ? 1 : 0)) + projmatrix[8]) / projmatrix[0],
          y = ((clip[1]<0 ? -1 : (clip[1]>0 ? 1 : 0)) + projmatrix[9]) / projmatrix[5],
          w = (1 + projmatrix[10]) / projmatrix[14],
          scale = 2 / (x*clip[0] + y*clip[1] - clip[2] + w*clip[3]);
    clipmatrix = projmatrix;
    clipmatrix[2] = clip[0]*scale;
    clipmatrix[6] = clip[1]*scale;
    clipmatrix[10] = clip[2]*scale + 1.0f;
    clipmatrix[14] = clip[3]*scale;
}

bool reflecting = false, refracting = false;
GLuint reflecttex = 0, refracttex = 0;
int reflectlastsize = 0;

// the water reflection's size: 1<<reflectsize pixels square, 64 to 4096 (sizes below 64 count as 64), limited by the
// window unless it's drawn through a framebuffer (reflectfbo); 0: the largest that fits the window (1024 in a 2560x1600
// window, 2048 at 3840x2160), so it stays about as sharp as the screen
VARP(reflectsize, 0, 8, 12);
VAR(reflectclip, 0, 3, 100);
VARP(waterreflect, 0, 1, 1);
VARP(waterrefract, 0, 0, 1);
VAR(reflectscissor, 0, 1, 1);

// "reflectfbo 1" renders the water reflection and refraction straight into their textures through framebuffer objects
// instead of into the back buffer followed by a copy; their size then isn't limited by the window size either.
// It's off by default: on Apple's OpenGL-on-Metal it measured 5-10% slower than the copy (M4 Max, ac_aqueous), and
// elsewhere the 256x256 copy it saves is negligible. Kept for the offscreen rendering a Metal/Vulkan backend needs.
VAR(reflectfbo, 0, 0, 1);
static GLuint reflectfbos[2] = { 0, 0 }, reflectdepthrb = 0;

static void cleanupreflectfbos()
{
    loopi(2) if(reflectfbos[i]) { glDeleteFramebuffers_(1, &reflectfbos[i]); reflectfbos[i] = 0; }
    if(reflectdepthrb) { glDeleteRenderbuffers_(1, &reflectdepthrb); reflectdepthrb = 0; }
}

static bool bindreflectfbo(int size, bool refract) // false: render into the back buffer and copy, as before
{
    if(!hasFBO || !reflectfbo || !gfx_legacygl()) return false; // (the GPU backend renders and copies)
    GLuint &fbo = reflectfbos[refract ? 1 : 0];
    if(!fbo)
    {
        if(!reflectdepthrb)
        {
            // same depth precision as the back buffer the reflection used to be rendered into
            extern int scenedepthformat();
            glGenRenderbuffers_(1, &reflectdepthrb);
            glBindRenderbuffer_(GL_RENDERBUFFER_EXT, reflectdepthrb);
            glRenderbufferStorage_(GL_RENDERBUFFER_EXT, scenedepthformat(), size, size);
            glBindRenderbuffer_(GL_RENDERBUFFER_EXT, 0);
        }
        glGenFramebuffers_(1, &fbo);
        glBindFramebuffer_(GL_FRAMEBUFFER_EXT, fbo);
        glFramebufferTexture2D_(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, refract ? refracttex : reflecttex, 0);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, reflectdepthrb);
        GLenum status = glCheckFramebufferStatus_(GL_FRAMEBUFFER_EXT);
        if(status != GL_FRAMEBUFFER_COMPLETE_EXT)
        {
            glBindFramebuffer_(GL_FRAMEBUFFER_EXT, 0);
            cleanupreflectfbos();
            reflectfbo = 0;
            conoutf("water reflection framebuffer not supported (0x%X): rendering into the back buffer instead", status);
            return false;
        }
        return true;
    }
    glBindFramebuffer_(GL_FRAMEBUFFER_EXT, fbo);
    return true;
}

void drawreflection(float hf, int w, int h, float changelod, bool refract)
{
    reflecting = true;
    refracting = refract;

    int sizelimit = hasFBO && reflectfbo ? hwtexsize : min(hwtexsize, min(w, h)), size = reflectsize ? 1<<max(reflectsize, 6) : 1<<12;
    if(!reflectsize) sizelimit = min(sizelimit, min(w, h));
    while(size > sizelimit && size > 64) size /= 2;
    if(size!=reflectlastsize)
    {
        if(reflecttex) gfx->deletetextures(1, &reflecttex);
        if(refracttex) gfx->deletetextures(1, &refracttex);
        reflecttex = refracttex = 0;
    }
    if(!reflecttex || (waterrefract && !refracttex))
    {
        cleanupreflectfbos(); // (re)attached to the new textures on first use
        // RGBA when rendered through a framebuffer: GPUs (Metal in particular) have no 8 bit RGB render target format,
        // so drivers would have to emulate rendering into an RGB texture
        GLenum format = hasFBO && reflectfbo ? GL_RGBA : GL_RGB;
        if(!reflecttex)
        {
            gfx->gentextures(1, &reflecttex);
            createtexture(reflecttex, size, size, NULL, 3, false, false, format);
        }
        if(!refracttex)
        {
            gfx->gentextures(1, &refracttex);
            createtexture(refracttex, size, size, NULL, 3, false, false, format);
        }
        reflectlastsize = size;
    }

    extern float wsx1, wsx2, wsy1, wsy2;
    int sx = 0, sy = 0, sw = size, sh = size;
    if(reflectscissor && (wsx1 > -1 || wsy1 > -1 || wsx1 < 1 || wsy1 < 1))
    {
        sx = int(floor((wsx1+1)*0.5f*size));
        sy = int(floor((wsy1+1)*0.5f*size));
        sw = int(ceil((wsx2+1)*0.5f*size)) - sx;
        sh = int(ceil((wsy2+1)*0.5f*size)) - sy;
    }
    bool usefbo = bindreflectfbo(size, refract);
    gfx->scissor(sx, sy, sw, sh);
    gfx->enable(GL_SCISSOR_TEST);

    if(!refract) gfx->cullface(GL_BACK);
    gfx->viewport(0, 0, size, size);
    gfx->clear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    resetcubes();

    render_world(camera1->o.x, camera1->o.y, refract ? camera1->o.z : hf, changelod,
            (int)camera1->yaw, (refract ? 1 : -1)*(int)camera1->pitch, dynfov(), fovy, size, size);

    setupstrips();

    gfx->enable(GL_TEXTURE_2D);

    gfx->pushmatrix();
    if(!refract)
    {
        gfx->translate(0, 0, 2*hf);
        gfx->scale(1, 1, -1);
    }

    genclipmatrix(0, 0, -1, 0.1f*reflectclip+hf);
    gfx->matrixmode(GL_PROJECTION);
    gfx->pushmatrix();
    gfx->loadmatrix(clipmatrix.v);
    gfx->matrixmode(GL_MODELVIEW);

    renderstripssky();

    gfx->pushmatrix();
    gfx->loadidentity();
    gfx->rotate(camera1->pitch, -1, 0, 0);
    gfx->rotate(camera1->yaw,   0, 1, 0);
    gfx->rotate(90, 1, 0, 0);
    if(!refract) gfx->scale(1, 1, -1);
    gfx->color3f(1, 1, 1);
    gfx->disable(GL_FOG);
    gfx->depthfunc(GL_GREATER);
    if(!refracting) skyfloor = max(skyfloor, hf);
    draw_envbox(fog*4/3);
    gfx->depthfunc(GL_LESS);
    gfx->enable(GL_FOG);
    gfx->popmatrix();

    setuptmu(0, "T * P x 2");

    renderstrips();
    rendermapmodels();
    renderentities();
    renderclients();

    resettmu(0);

    render_particles(-1);

    if(refract) gfx->loadidentity();
    gfx->matrixmode(GL_PROJECTION);
    extern int mtwater;
    if(refract && (!mtwater || maxtmus<2))
    {
        gfx->loadidentity();
        gfx->ortho(0, 1, 0, 1, -1, 1);
        gfx->disable(GL_TEXTURE_2D);
        gfx->disable(GL_DEPTH_TEST);
        gfx->enable(GL_BLEND);
        gfx->blendfunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gfx->color4ubv(hdr.watercolor);
        gfx->begin(GL_TRIANGLE_STRIP);
        gfx->vertex2f(0, 1);
        gfx->vertex2f(1, 1);
        gfx->vertex2f(0, 0);
        gfx->vertex2f(1, 0);
        gfx->end();
        gfx->disable(GL_BLEND);
        gfx->enable(GL_TEXTURE_2D);
        gfx->enable(GL_DEPTH_TEST);
    }
    gfx->popmatrix();
    gfx->matrixmode(GL_MODELVIEW);
    gfx->popmatrix();

    if(!refract) gfx->cullface(GL_FRONT);
    gfx->viewport(0, 0, w, h);

    gfx->disable(GL_SCISSOR_TEST);

    if(usefbo) glBindFramebuffer_(GL_FRAMEBUFFER_EXT, 0);
    else
    {
        gfx->bindtexture(GL_TEXTURE_2D, refract ? refracttex : reflecttex);
        gfx->copytexsubimage2d(GL_TEXTURE_2D, 0, sx, sy, sx, sy, sw, sh);
    }

    gfx->disable(GL_TEXTURE_2D);

    reflecting = refracting = false;
}

bool minimap = false, minimapdirty = true;
int minimaplastsize = 0;
GLuint minimaptex = 0;

vector<zone> zones;

void renderzones(float z)
{
    gfx->enable(GL_BLEND);
    gfx->blendfunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gfx->disable(GL_TEXTURE_2D);
    loopv(zones)
    {
        gfx->color4f(((zones[i].color>>16)&0xFF)/255.0f, ((zones[i].color>>8)&0xFF)/255.0f, (zones[i].color&0xFF)/255.0f, 0.3f);
        gfx->begin(GL_QUADS);
        gfx->vertex3f(zones[i].x1, zones[i].y1, z);
        gfx->vertex3f(zones[i].x2, zones[i].y1, z);
        gfx->vertex3f(zones[i].x2, zones[i].y2, z);
        gfx->vertex3f(zones[i].x1, zones[i].y2, z);
        gfx->end();
    }
    gfx->disable(GL_BLEND);
    gfx->enable(GL_TEXTURE_2D);
}

void clearminimap()
{
    minimapdirty = true;
}

COMMAND(clearminimap, "");
VARFP(minimapres, 7, 9, 10, clearminimap());
void drawminimap(int w, int h)
{
    if(!minimapdirty) return;
    int size = 1<<minimapres, sizelimit = min(hwtexsize, min(w, h));
    while(size > sizelimit) size /= 2;
    if(size!=minimaplastsize && minimaptex)
    {
        gfx->deletetextures(1, &minimaptex);
        minimaptex = 0;
    }
    if(!minimaptex)
    {
        gfx->gentextures(1, &minimaptex);
        createtexture(minimaptex, size, size, NULL, 3, false, false, GL_RGB);
        minimaplastsize = size;
    }
    minimap = true;
    disableraytable();
    physent *oldcam = camera1;
    physent minicam;
    camera1 = &minicam;
    camera1->type = ENT_CAMERA;
    camera1->o.x = clmapdims.xm;
    camera1->o.y = clmapdims.ym;

    float gdim = max(clmapdims.xspan, clmapdims.yspan);

    if(gdim < 1) gdim = ssize/2.0f;
    camera1->o.z = clmapdims.maxceil + 1;
    camera1->pitch = -90;
    camera1->yaw = 0;

    float orthd = gdim/2.0f;
    gfx->viewport(0, 0, size, size);

    gfx->cleardepth(0.0);
    gfx->clearcolor(0, 0, 0, 0);
    gfx->clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    gfx->matrixmode(GL_PROJECTION);
    gfx->loadidentity();
    gfx->ortho(-orthd, orthd, -orthd, orthd, 0, (clmapdims.maxceil - clmapdims.minfloor) + 2); // depth of map +2 covered
    gfx->scale(1, -1, 1);
    gfx->matrixmode(GL_MODELVIEW);
    gfx->cullface(GL_BACK);
    gfx->disable(GL_FOG);
    gfx->enable(GL_TEXTURE_2D);
    transplayer();
    resetcubes();
    render_world(camera1->o.x, camera1->o.y, camera1->o.z, 1.0f,
            (int)camera1->yaw, (int)camera1->pitch, 90.0f, 90.0f, size, size);
    setupstrips();
    setuptmu(0, "T * P x 2");
    gfx->depthfunc(GL_ALWAYS);
    renderstrips();
    gfx->depthfunc(GL_LESS);
    rendermapmodels();
    renderzones(clmapdims.maxceil);
    resettmu(0);
    float hf = waterlevel - 0.3f;
    renderwater(hf, 0, 0);

    //draw a black border to prevent the minimap texture edges from bleeding in radarview
    GLfloat prevLineWidth;
    gfx->getfloatv(GL_LINE_WIDTH, &prevLineWidth);
    gfx->disable(GL_BLEND);
    gfx->color3f(0, 0, 0);
    gfx->linewidth(2.0f);
    gfx->begin(GL_LINE_LOOP);
        gfx->vertex3f(clmapdims.x1 + 0.02f,                   clmapdims.y1 + 0.02f,                   clmapdims.maxceil);
        gfx->vertex3f(clmapdims.x1 + clmapdims.xspan - 0.02f, clmapdims.y1 + 0.02f,                   clmapdims.maxceil);
        gfx->vertex3f(clmapdims.x1 + clmapdims.xspan - 0.02f, clmapdims.y1 + clmapdims.yspan - 0.02f, clmapdims.maxceil);
        gfx->vertex3f(clmapdims.x1 + 0.02f,                   clmapdims.y1 + clmapdims.yspan - 0.02f, clmapdims.maxceil);
    gfx->end();
    gfx->linewidth(prevLineWidth);
    gfx->enable(GL_BLEND);

    camera1 = oldcam;
    minimap = false;
    gfx->bindtexture(GL_TEXTURE_2D, minimaptex);
    gfx->copytexsubimage2d(GL_TEXTURE_2D, 0, 0, 0, 0, 0, size, size);
    minimapdirty = false;
    gfx->cullface(GL_FRONT);
    gfx->enable(GL_FOG);
    gfx->disable(GL_TEXTURE_2D);
    gfx->viewport(0, 0, w, h);
    gfx->cleardepth(1.0);
}

static void cleanupdepthread();

void cleanupgl()
{
    if(reflecttex) gfx->deletetextures(1, &reflecttex);
    if(refracttex) gfx->deletetextures(1, &refracttex);
    if(minimaptex) gfx->deletetextures(1, &minimaptex);
    reflecttex = refracttex = minimaptex = 0;
    cleanupreflectfbos();
    minimapdirty = true;
    cleanupdepthread();
    extern void cleanupworldvbo();
    cleanupworldvbo();
    extern void cleanuppostfx();
    cleanuppostfx();
}

void drawzone(int *x1, int *x2, int *y1, int *y2, int *color)
{
    zone &newzone = zones.add();
    newzone.x1 = *x1; newzone.x2 = *x2;
    newzone.y1 = *y1; newzone.y2 = *y2;
    newzone.color = *color ? *color : 0x00FF00;
    clearminimap();
}
COMMAND(drawzone, "iiiii");

void resetzones()
{
    zones.shrink(0);
}
COMMAND(resetzones, "");

int xtraverts;

VARP(hudgun, 0, 1, 1);
VARP(specthudgun, 0, 1, 1);

void setperspective(float fovy, float aspect, float nearplane, float farplane)
{
    GLdouble ydist = nearplane * tan(fovy/2*RAD), xdist = ydist * aspect;
    if(player1->isspectating() && player1->spectatemode == SM_OVERVIEW)
    {
        int gdim = max(clmapdims.xspan, clmapdims.yspan);
        if(!gdim) gdim = ssize/2;
        int orthd = 2 + gdim/2;
        gfx->ortho(-orthd, orthd, -orthd, orthd, 0, (clmapdims.maxceil - clmapdims.minfloor) + 2); // depth of map +2 covered
    }
    else
    {
        gfx->frustum(-xdist, xdist, -ydist, ydist, nearplane, farplane);
    }
}

void sethudgunperspective(bool on)
{
    gfx->matrixmode(GL_PROJECTION);
    gfx->loadidentity();
    if(on)
    {
        gfx->scale(1, 1, 0.5f); // fix hudugns colliding with map geometry
        setperspective(75.0f, aspect, 0.3f, farplane); // y fov fixed at 75 degrees
    }
    else setperspective(fovy, aspect, 0.15f, farplane);
    gfx->matrixmode(GL_MODELVIEW);
}

void drawhudgun()
{
    sethudgunperspective(true);
    
    if(hudgun && (specthudgun || !player1->isspectating()) && camera1->type==ENT_PLAYER)
    {
        playerent *p = (playerent *)camera1;
        if(p->state==CS_ALIVE) p->weaponsel->renderhudmodel();
    }
    rendermenumdl();

    sethudgunperspective(false);
}

bool outsidemap(physent *pl)
{
    if(pl->o.x < 0 || pl->o.x >= ssize || pl->o.y <0 || pl->o.y > ssize) return true;
    sqr *s = S((int)pl->o.x, (int)pl->o.y);
    return SOLID(s)
        || pl->o.z < s->floor - (s->type==FHF ? s->vdelta/4 : 0)
        || pl->o.z > s->ceil  + (s->type==CHF ? s->vdelta/4 : 0);
}

float cursordepth = 0.9f;
glmatrixf mvmatrix, projmatrix, mvpmatrix, invmvmatrix, invmvpmatrix;
vec worldpos, camdir, camup, camright;

void readmatrices()
{
    gfx->getfloatv(GL_MODELVIEW_MATRIX, mvmatrix.v);
    gfx->getfloatv(GL_PROJECTION_MATRIX, projmatrix.v);
    camright = vec(mvmatrix[0], mvmatrix[4], mvmatrix[8]);
    camup = vec(mvmatrix[1], mvmatrix[5], mvmatrix[9]);
    camdir = vec(-mvmatrix[2], -mvmatrix[6], -mvmatrix[10]);

    mvpmatrix.mul(projmatrix, mvmatrix);
    invmvmatrix.invert(mvmatrix);
    invmvpmatrix.invert(mvpmatrix);
}

// stupid function to cater to stupid ATI linux drivers that return incorrect depth values

float depthcorrect(float d)
{
    return (d<=1/256.0f) ? d*256 : d;
}

// find out the 3d target of the crosshair in the world easily and very accurately.
// sadly many very old cards and drivers appear to fuck up on glReadPixels() and give false
// coordinates, making shooting and such impossible.
// also hits map entities which is unwanted.
// could be replaced by a more acurate version of monster.cpp los() if needed

static void depthtoworldpos(float depth, const glmatrixf &invmvp, const vec &from, vec &pos)
{
    cursordepth = depth;
    vec screen(0, 0, depthcorrect(cursordepth)*2 - 1);
    vec4 world;
    invmvp.transform(screen, world);
    pos = vec(world.x, world.y, world.z).div(world.w);
    intersectgeometry(from, pos);
}

// Reading the depth back synchronously makes the CPU wait, in the middle of every frame, until the GPU has drawn the
// world - CPU and GPU can't work in parallel. But shooting only uses worldpos in the *next* frame (shoot() runs in
// updateworld(), before that frame's rendering). So by default the depth is copied into a pixel buffer object without
// waiting, and turned into worldpos at the start of the next frame by resolvedepth(), with the matrices and camera
// position of the frame it was read in: shoot() gets exactly the same target as with the synchronous read.
// The only difference is for the few things drawn later in the same frame (the HUD gun's sway towards the target and
// the name of the player in the crosshair), which see the value from one frame earlier. In edit mode, where the edit
// cursor needs the current frame, the read stays synchronous. "asyncdepth 0" restores the synchronous read.
VAR(asyncdepth, 0, 1, 1);
static bool depthpending = false, depthinpbo = false;
static glmatrixf depthinvmvp;
static vec depthfrom;

// The crosshair pick pass: before the main pass, the sky cubes, world and map models are drawn again, depth only, into a
// separate 1x1 framebuffer, through a projection zoomed onto the crosshair pixel. Its depth is read at the start of the
// next frame - exactly when shoot() needs it - instead of in the middle of the main pass. On tile-based GPUs (Apple
// Silicon) reading back in the middle of the main pass splits it in two, which costs far more GPU time than drawing the
// geometry a second time, and with a frame rate cap the GPU has finished the frame by the time the depth is read.
// "depthpick 0" restores reading the main pass's depth in the middle of the frame.
VAR(depthpick, 0, 1, 1);
static bool pickdrawn = false; // the pick pass ran this frame: readdepth() has nothing to do

// "depthcheck 1" (debugging): also reads the main pass's depth in the middle of the frame (the old way) and compares the
// target that shoot() gets with the one the old code would have given it
VAR(depthcheck, 0, 0, 1);
static vec depthcheckpos;
static int depthchecked = 0, depthmismatches = 0, depthfar = 0;
static float depthmaxdiff = 0;

void resolvedepth() // called at the start of a frame, before input and game updates
{
    if(!depthpending) return;
    depthpending = false;
    float d = 1;
    if(depthinpbo)
    {
        if(!gfx->resolvedepthasync(d)) return;
    }
    else if(!gfx->readpick(d)) return;
    depthtoworldpos(d, depthinvmvp, depthfrom, worldpos);
    if(depthcheck)
    {
        float diff = worldpos.dist(depthcheckpos);
        depthchecked++;
        if(diff != 0) { depthmismatches++; depthmaxdiff = max(depthmaxdiff, diff); }
        if(diff > 0.01f) depthfar++;
        if(depthchecked % 500 == 0)
            conoutf("depthcheck: %d frames compared, %d differ from the old read (%d by more than 0.01 units, max distance %f)",
                    depthchecked, depthmismatches, depthfar, depthmaxdiff);
    }
}

// the crosshair's current target (testing: compares the renderers' crosshair pick passes)
COMMANDF(crosshairtarget, "", () { defformatstring(s)("%.4f %.4f %.4f", worldpos.x, worldpos.y, worldpos.z); result(s); });

static void cleanupdepthread()
{
    gfx->cleanuppick();
    depthpending = depthinpbo = false;
    pickdrawn = false;
}

static void renderdepthpick(int w, int h)
{
    pickdrawn = false;
    if(!gfx->beginpick(scenedepthbits)) { depthpick = 0; return; }

    // projection zoomed onto the crosshair pixel (w/2, h/2), the pixel the main pass's depth used to be read from:
    // that pixel's footprint is mapped onto the 1x1 viewport, so its centre is sampled at the same point of the view
    int px = w/2, py = h/2;
    glmatrixf zoom;
    zoom.identity();
    zoom.v[0] = w;
    zoom.v[5] = h;
    zoom.v[12] = w - 2*px - 1;
    zoom.v[13] = h - 2*py - 1;
    gfx->matrixmode(GL_PROJECTION);
    gfx->pushmatrix();
    gfx->loadmatrix(zoom.v);
    gfx->multmatrix(projmatrix.v);
    gfx->matrixmode(GL_MODELVIEW);
    transplayer();

    // what the main pass has drawn when the old code read its depth: sky cubes and world (the sky box doesn't write depth)...
    gfx->disable(GL_TEXTURE_2D);
    setupstrips();
    renderstripsdepth();
    gfx->enable(GL_TEXTURE_2D);
    // ...and map models, with their textures for alpha-tested skins (blob shadows don't write depth: skipped). Only the
    // models whose bounding sphere the ray through the crosshair pixel's centre passes through can affect its depth.
    vec4 nearw, farw;
    invmvpmatrix.transform(vec(2*(px + 0.5f)/w - 1, 2*(py + 0.5f)/h - 1, -1), nearw);
    invmvpmatrix.transform(vec(2*(px + 0.5f)/w - 1, 2*(py + 0.5f)/h - 1, 1), farw);
    vec raynear = vec(nearw.x, nearw.y, nearw.z).div(nearw.w), raydir = vec(farw.x, farw.y, farw.z).div(farw.w).sub(raynear).normalize();
    extern int dynshadow;
    extern void rendermapmodelsalongray(const vec &from, const vec &dir);
    int keepdynshadow = dynshadow;
    dynshadow = 0;
    startmodelbatches();
    rendermapmodelsalongray(raynear, raydir);
    endmodelbatches();
    // in edit mode (GPU backend), what the main pass has drawn by the time the GL renderer reads its depth there: the
    // edit-mode entity models, clip boxes and arrows
    if(editmode)
    {
        startmodelbatches();
        rendereditentities(true);
        endmodelbatches();
    }
    dynshadow = keepdynshadow;

    depthinpbo = asyncdepth && hasPBO; // on drivers that read asynchronously, start copying the result right away
    gfx->endpick(depthinpbo);

    gfx->matrixmode(GL_PROJECTION);
    gfx->popmatrix();
    gfx->matrixmode(GL_MODELVIEW);
    gfx->viewport(0, 0, w, h);

    depthinvmvp = invmvpmatrix;
    depthfrom = camera1->o;
    depthpending = pickdrawn = true;
}

void readdepth(int w, int h, vec &pos)
{
    if(pickdrawn && &pos == &worldpos) // the target was drawn by the pick pass: nothing to read in the middle of the frame
    {
        pickdrawn = false;
        if(editmode) // (GPU backend) the edit cursor needs this frame's target, as with GL's read of the main pass
        {
            float d;
            depthpending = false;
            if(gfx->readpick(d)) depthtoworldpos(d, depthinvmvp, depthfrom, pos);
            return;
        }
        if(depthcheck)
        {
            float syncdepth, keepcursordepth = cursordepth;
            gfx->readdepth(w/2, h/2, syncdepth);
            depthtoworldpos(syncdepth, invmvpmatrix, camera1->o, depthcheckpos);
            cursordepth = keepcursordepth;
        }
        return;
    }
    if(asyncdepth && hasPBO && !editmode && &pos == &worldpos && gfx->readdepthasync(w/2, h/2))
    {
        depthinvmvp = invmvpmatrix;
        depthfrom = camera1->o;
        depthpending = depthinpbo = true;
        if(depthcheck)
        {
            float syncdepth, keepcursordepth = cursordepth;
            gfx->readdepth(w/2, h/2, syncdepth);
            depthtoworldpos(syncdepth, invmvpmatrix, camera1->o, depthcheckpos);
            cursordepth = keepcursordepth;
        }
        return; // pos keeps the previous frame's value until resolvedepth()
    }
    depthpending = false; // a synchronous read supersedes a pending one
    float depth;
    gfx->readdepth(w/2, h/2, depth);
    depthtoworldpos(depth, invmvpmatrix, camera1->o, pos);
}

VARP(ignoreoverride_nowaterreflect, 0, 0, 1);
VARP(ignoreoverride_nostencilshadows, 0, 0, 1);

int effective_stencilshadow = 0;

bool isoverview = false;

void gl_drawframe(int w, int h, float changelod, float curfps, int elapsed)
{
    extern int mapoverride_nostencilshadows, mapoverride_nowaterreflect;
    effective_stencilshadow = mapoverride_nostencilshadows && !ignoreoverride_nostencilshadows ? 0 : stencilshadow;
    bool effective_waterreflect = waterreflect && !editmode && (!mapoverride_nowaterreflect || ignoreoverride_nowaterreflect);

    dodynlights();
    drawminimap(w, h);

    recomputecamera();

    aspect = float(w)/h;
    fovy = 2*atan2(tan(float(dynfov())/2*RAD), aspect)/RAD;

    float hf = waterlevel - 0.3f;
    bool underwater = camera1->o.z<hf;
    isoverview = player1->isspectating() && player1->spectatemode == SM_OVERVIEW;

    gfx->fogi(GL_FOG_START, (fog+64)/8);
    gfx->fogi(GL_FOG_END, fog);
    float fogc[4] = { (fogcolour>>16)/256.0f, ((fogcolour>>8)&255)/256.0f, (fogcolour&255)/256.0f, 1.0f },
          wfogc[4] = { hdr.watercolor[0]/255.0f, hdr.watercolor[1]/255.0f, hdr.watercolor[2]/255.0f, 1.0f };
    gfx->fogfv(GL_FOG_COLOR, fogc);
    gfx->clearcolor(fogc[0], fogc[1], fogc[2], 1.0f);
    if(isoverview) gfx->disable(GL_FOG);

    gfx->matrixmode(GL_PROJECTION);
    gfx->loadidentity();

    if(underwater)
    {
        fovy += sinf(lastmillis/1000.0f)*2.0f;
        aspect += sinf(lastmillis/1000.0f+PI)*0.1f;
        gfx->fogfv(GL_FOG_COLOR, wfogc);
        gfx->fogi(GL_FOG_START, 0);
        gfx->fogi(GL_FOG_END, (fog+96)/8);
    }

    farplane = fog*5/2;
    setperspective(fovy, aspect, 0.15f, farplane);
    gfx->matrixmode(GL_MODELVIEW);

    transplayer();
    readmatrices();
    rpmark(RP_SETUP);

    if(!underwater && effective_waterreflect && !isoverview)
    {
        extern int wx1;
        if(wx1>=0)
        {
            if(reflectscissor) calcwaterscissor();
            drawreflection(hf, w, h, changelod, false);
            if(waterrefract) drawreflection(hf, w, h, changelod, true);
        }
    }
    rpmark(RP_REFLECTION);

    gfx->enable(GL_TEXTURE_2D);

    resetcubes();

    render_world(camera1->o.x, camera1->o.y, camera1->o.z, changelod,
            (int)camera1->yaw, (int)camera1->pitch, dynfov(), fovy, w, h);
    rpmark(RP_WORLDGEN);

    // crosshair target: drawn in its own small pass *before* the main pass starts (see renderdepthpick()). In edit mode
    // the GL renderer reads the main pass's depth in the middle of the frame, which the GPU backend can't: it draws the
    // pick pass then too, with what edit mode adds, and readdepth() waits for it
    if((depthpick && hasFBO && !editmode) || (editmode && !gfx_legacygl())) renderdepthpick(w, h);
    rpmark(RP_DEPTHPICK);

    if(effective_stencilshadow && hasstencil && stencilbits >= 8) gfx->clearstencil((hasSTS || hasST2) && !hasSTW ? 128 : 0);
    gfx->clear((outsidemap(camera1) ? GL_COLOR_BUFFER_BIT : 0) | GL_DEPTH_BUFFER_BIT | (effective_stencilshadow && hasstencil && stencilbits >= 8 ? GL_STENCIL_BUFFER_BIT : 0));

    setupstrips();

    renderstripssky();

    gfx->loadidentity();
    gfx->rotate(camera1->pitch, -1, 0, 0);
    gfx->rotate(camera1->yaw,   0, 1, 0);
    gfx->rotate(90, 1, 0, 0);
    gfx->color3f(1, 1, 1);
    gfx->disable(GL_FOG);
    gfx->depthfunc(GL_GREATER);
    draw_envbox(fog*4/3);
    gfx->depthfunc(GL_LESS);
    fixresizedscreen();
    if(!isoverview) gfx->enable(GL_FOG);

    transplayer();

    setuptmu(0, "T * P x 2");

    renderstrips();
    rpmark(RP_WORLDDRAW);

    xtraverts = 0;

    startmodelbatches();
    rendermapmodels();
    endmodelbatches();

    if(effective_stencilshadow && hasstencil && stencilbits >= 8 && !isoverview) drawstencilshadows();

    startmodelbatches();
    rendereditentities();
    endmodelbatches();
    rpmark(RP_MAPMODELS);

    readdepth(w, h, worldpos);
    rpmark(RP_DEPTHREAD);

    startmodelbatches();
    renderclients();
    endmodelbatches();

    startmodelbatches();
    renderentities();
    renderbounceents();
    endmodelbatches();

    WaypointClass.Think();// Rick: Need to do here because of drawing the waypoints
    rpmark(RP_MODELS);

    drawhudgun();
    rpmark(RP_HUDGUN);

    resettmu(0);

    gfx->disable(GL_CULL_FACE);

    render_particles(curtime, PT_DECAL_MASK);

    int nquads = renderwater(hf, !effective_waterreflect || underwater || isoverview ? 0 : reflecttex, !effective_waterreflect || !waterrefract || underwater || isoverview ? 0 : refracttex);

    render_particles(curtime, ~PT_DECAL_MASK);
    rpmark(RP_EFFECTS);

    gfx->disable(GL_FOG);
    gfx->disable(GL_TEXTURE_2D);

    if(editmode)
    {
        if(cursordepth==1.0f) worldpos = camera1->o;
        enablepolygonoffset(GL_POLYGON_OFFSET_LINE);
        gfx->polygonmode(GL_FRONT_AND_BACK, GL_LINE);
        gfx->depthmask(GL_FALSE);
        cursorupdate();
        gfx->depthmask(GL_TRUE);
        gfx->polygonmode(GL_FRONT_AND_BACK, GL_FILL);
        disablepolygonoffset(GL_POLYGON_OFFSET_LINE, false);
    }
    extern vector<int> tagclipcubes;
    tagclipcubes.setsize(0);

    extern void renderpostfxscene(int w, int h), renderpostfxfinal(int w, int h);
    renderpostfxscene(w, h); // FXAA: the 3D scene only, the HUD stays sharp

    extern vector<vertex> verts;
    gl_drawhud(w, h, int(curfps + 0.5f), nquads, verts.length(), underwater, elapsed);

    renderpostfxfinal(w, h); // gamma: everything

    gfx->enable(GL_CULL_FACE);
    if(!isoverview) gfx->enable(GL_FOG);

    undodynlights();
    rpmark(RP_HUD);
}

