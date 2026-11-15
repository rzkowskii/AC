// gfx.h: the renderer interface. The game draws through "gfx", which is one of two backends:
//
// - gfx_gl.cpp: OpenGL, the original renderer. Each call is the GL call the game used to make, so the GL path's
//   images and performance stay what they were.
// - gfx_sdlgpu.cpp: SDL3's GPU API (Metal on macOS, Vulkan on Linux), selected with --gpu. It implements the same
//   fixed-function semantics with shaders and cached pipelines, and records each frame so it can upload everything
//   first and draw each target in a single render pass.
//
// The vocabulary is the fixed-function GL the game is written in (GL enums for primitives, state and formats), so call
// sites port one to one. The few things that only exist with GL (display lists, shaders.cpp's GLSL programs,
// framebuffer objects for the water reflection, asynchronous depth reads, extension queries) still call GL directly,
// behind gfx_legacygl().

#ifndef GFX_H
#define GFX_H

enum { GFX_OPENGL = 0, GFX_SDLGPU };

struct gfxdevice
{
    virtual ~gfxdevice() {}
    virtual int api() const = 0;
    virtual const char *name() const = 0;

    // --- device and presentation
    virtual bool init(SDL_Window *window, int samples, int vsync) { return true; } // (the GL context is made by main.cpp)
    virtual int getdepthbits() { return 0; }
    virtual int getstencilbits() { return 0; }                  // (GPU backend: 8 if stencilbits asked for one)
    virtual int getsamples() { return 0; }
    virtual void present() = 0;                                 // shows the frame (GL: SDL_GL_SwapWindow)
    virtual void displaychanged() {}                            // re-applies the presentation settings (GPU backend)
    // post-processing done by the backend itself (GPU backend; GL uses shaders.cpp): 0 = FXAA of the scene so far,
    // 1 = gamma with exponent param, applied to the finished frame. false: not available
    virtual bool postpass(int kind, float param) { return false; }
    virtual void readpixels(int x, int y, int w, int h, GLenum format, GLenum type, void *data) = 0; // like glReadPixels

    // --- frame and targets
    virtual void viewport(int x, int y, int w, int h) = 0;
    virtual void scissor(int x, int y, int w, int h) = 0;
    virtual void clearcolor(float r, float g, float b, float a) = 0;
    virtual void clearstencil(int s) = 0;
    virtual void clear(GLbitfield mask) = 0;
    virtual void finish() = 0;                                  // wait for the GPU (gpusync)

    // --- matrix stacks (GL_MODELVIEW, GL_PROJECTION, GL_TEXTURE)
    virtual void matrixmode(GLenum mode) = 0;
    virtual void loadidentity() = 0;
    virtual void pushmatrix() = 0;
    virtual void popmatrix() = 0;
    virtual void loadmatrix(const GLfloat *m) = 0;
    virtual void multmatrix(const GLfloat *m) = 0;
    virtual void translate(GLfloat x, GLfloat y, GLfloat z) = 0;
    virtual void rotate(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) = 0;
    virtual void scale(GLfloat x, GLfloat y, GLfloat z) = 0;
    virtual void frustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) = 0;
    virtual void ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) = 0;
    virtual void getfloatv(GLenum pname, GLfloat *v) = 0;      // GL_MODELVIEW_MATRIX, GL_PROJECTION_MATRIX, GL_CURRENT_COLOR

    // --- fixed-function state
    virtual void enable(GLenum cap) = 0;
    virtual void disable(GLenum cap) = 0;
    virtual bool isenabled(GLenum cap) = 0;
    virtual void blendfunc(GLenum src, GLenum dst) = 0;
    virtual void depthfunc(GLenum func) = 0;
    virtual void depthmask(GLboolean on) = 0;
    virtual void colormask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) = 0;
    virtual void cullface(GLenum face) = 0;
    virtual void alphafunc(GLenum func, GLfloat ref) = 0;
    virtual void polygonoffset(GLfloat factor, GLfloat units) = 0;
    virtual void linewidth(GLfloat w) = 0;
    virtual void polygonmode(GLenum face, GLenum mode) = 0;    // GL_FILL or GL_LINE (the editor's wireframe cursor)
    virtual void color3f(GLfloat r, GLfloat g, GLfloat b) = 0;
    virtual void color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) = 0;
    virtual void color3ub(GLubyte r, GLubyte g, GLubyte b) = 0;
    virtual void color4ubv(const GLubyte *v) = 0;
    virtual void color4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) = 0;
    virtual void fogi(GLenum pname, GLint v) = 0;
    virtual void fogf(GLenum pname, GLfloat v) = 0;
    virtual void fogfv(GLenum pname, const GLfloat *v) = 0;
    virtual void hint(GLenum target, GLenum mode) = 0;
    virtual void shademodel(GLenum mode) = 0;
    virtual void cleardepth(GLdouble d) = 0;
    virtual void stencilfunc(GLenum func, GLint ref, GLuint mask) = 0;
    virtual void stencilop(GLenum fail, GLenum zfail, GLenum zpass) = 0;
    // separate front and back stencil state (shadow.cpp): GL_ATI_separate_stencil, or GL_EXT_stencil_two_side's
    // active face (GL_STENCIL_TEST_TWO_SIDE_EXT enabled: stencilfunc/stencilop set the active face only)
    virtual void stencilfuncseparate(GLenum frontfunc, GLenum backfunc, GLint ref, GLuint mask) = 0;
    virtual void stencilopseparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass) = 0;
    virtual void activestencilface(GLenum face) = 0;
    virtual void texenvi(GLenum pname, GLint v) = 0;           // texture combiners of the active unit (texture.cpp, setuptmu)
    virtual void texenvfv(GLenum pname, const GLfloat *v) = 0;
    virtual void activetexture(GLenum unit) = 0;
    virtual void clientactivetexture(GLenum unit) = 0;           // which unit's texture coordinate array the array calls set
    virtual void getintegerv(GLenum pname, GLint *v) = 0;      // GL_MAX_TEXTURE_SIZE, GL_MAX_TEXTURE_UNITS_ARB, GL_SAMPLE_BUFFERS

    // --- textures (a texture's handle is its GL name in the GL backend)
    virtual void gentextures(GLsizei n, GLuint *t) = 0;
    virtual void deletetextures(GLsizei n, const GLuint *t) = 0;
    virtual void bindtexture(GLenum target, GLuint t) = 0;
    virtual void texparameteri(GLenum target, GLenum pname, GLint v) = 0;
    virtual void pixelstorei(GLenum pname, GLint v) = 0;
    virtual void teximage2d(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels) = 0;
    virtual void texsubimage2d(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels) = 0;
    virtual void copytexsubimage2d(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei w, GLsizei h) = 0; // from the frame
    virtual void gettexture(GLuint t, GLenum format, void *pixels) = 0; // level 0 as unsigned bytes, GL_RGB or GL_RGBA, like glGetTexImage

    // --- buffer objects (GL_ARRAY_BUFFER_ARB, GL_ELEMENT_ARRAY_BUFFER_ARB)
    virtual void genbuffers(GLsizei n, GLuint *b) = 0;
    virtual void deletebuffers(GLsizei n, const GLuint *b) = 0;
    virtual void bindbuffer(GLenum target, GLuint b) = 0;
    virtual void bufferdata(GLenum target, GLsizeiptr size, const void *data, GLenum usage) = 0;
    virtual void buffersubdata(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) = 0;

    // --- vertex arrays and draws
    virtual void enableclientstate(GLenum array) = 0;
    virtual void disableclientstate(GLenum array) = 0;
    virtual void vertexpointer(GLint size, GLenum type, GLsizei stride, const void *p) = 0;
    virtual void colorpointer(GLint size, GLenum type, GLsizei stride, const void *p) = 0;
    virtual void normalpointer(GLenum type, GLsizei stride, const void *p) = 0;
    virtual void texcoordpointer(GLint size, GLenum type, GLsizei stride, const void *p) = 0;
    virtual void drawarrays(GLenum mode, GLint first, GLsizei count) = 0;
    virtual void multidrawarrays(GLenum mode, const GLint *first, const GLsizei *count, GLsizei n) = 0;
    virtual void drawelements(GLenum mode, GLsizei count, GLenum type, const void *indices) = 0;
    virtual void drawrangeelements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices) = 0;

    // --- immediate mode (the GPU backend collects the vertices into its per-frame vertex stream)
    virtual void begin(GLenum mode) = 0;
    virtual void end() = 0;
    virtual void texcoord2f(GLfloat s, GLfloat t) = 0;
    virtual void texcoord2i(GLint s, GLint t) = 0;
    virtual void vertex2f(GLfloat x, GLfloat y) = 0;
    virtual void vertex3f(GLfloat x, GLfloat y, GLfloat z) = 0;
    virtual void vertex3fv(const GLfloat *v) = 0;

    // --- crosshair pick pass (rendergl.cpp, renderdepthpick()) and depth reads
    virtual bool beginpick(int depthbits) = 0;                 // binds the 1x1 pick target (depth cleared, colour writes off)
    virtual void endpick(bool async) = 0;                       // back to the screen; async: start copying the depth out
    virtual bool readpick(float &depth) = 0;                    // the pick pass's depth (in the next frame; or in the same one, waiting)
    virtual bool readdepthasync(int x, int y) = 0;              // starts copying one depth value of the screen out
    virtual bool resolvedepthasync(float &depth) = 0;           // ...and fetches it (call in the next frame)
    virtual void readdepth(int x, int y, float &depth) = 0;     // synchronous read of the screen's depth
    virtual void cleanuppick() = 0;
};

extern gfxdevice *gfx;
extern gfxdevice *gfx_opengl();
extern gfxdevice *gfx_sdlgpu();

// true while the GL backend is active: the GL-only parts of the renderer that call OpenGL directly run only then
static inline bool gfx_legacygl() { return gfx->api() == GFX_OPENGL; }

#endif
