// gfx_gl.cpp: the OpenGL backend of the renderer interface (gfx.h). Every call is the GL call the renderer made
// before the interface existed, so this backend's output and performance are the original renderer's.

#include "cube.h"

extern PFNGLMAPBUFFERPROC glMapBuffer_;
extern PFNGLUNMAPBUFFERPROC glUnmapBuffer_;
extern bool hasPBO, hasFBO, hasDBF;
extern PFNGLGENFRAMEBUFFERSEXTPROC glGenFramebuffers_;
extern PFNGLDELETEFRAMEBUFFERSEXTPROC glDeleteFramebuffers_;
extern PFNGLBINDFRAMEBUFFEREXTPROC glBindFramebuffer_;
extern PFNGLGENRENDERBUFFERSEXTPROC glGenRenderbuffers_;
extern PFNGLDELETERENDERBUFFERSEXTPROC glDeleteRenderbuffers_;
extern PFNGLBINDRENDERBUFFEREXTPROC glBindRenderbuffer_;
extern PFNGLRENDERBUFFERSTORAGEEXTPROC glRenderbufferStorage_;
extern PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC glFramebufferRenderbuffer_;
extern PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC glCheckFramebufferStatus_;

#ifndef GL_DEPTH_COMPONENT32F
#define GL_DEPTH_COMPONENT32F 0x8CAC
#endif

// "benchnopresent 1" (testing): frames are rendered but not shown, which measures rendering without the display's limits
// (a Metal window can't present faster than the display refreshes; Apple's GL can, though the extra frames are never seen)
VAR(benchnopresent, 0, 0, 1);

struct glbackend : gfxdevice
{
    // crosshair pick target and the pixel buffer object depth reads go through
    GLuint pickfbo, pickrb[2], depthpbo;

    glbackend() : pickfbo(0), depthpbo(0) { pickrb[0] = pickrb[1] = 0; }

    int api() const { return GFX_OPENGL; }
    const char *name() const { return "OpenGL"; }

    void present() { if(benchnopresent) glFlush(); else SDL_GL_SwapWindow(screen); }
    void readpixels(int x, int y, int w, int h, GLenum format, GLenum type, void *data) { glReadPixels(x, y, w, h, format, type, data); }

    void viewport(int x, int y, int w, int h) { glViewport(x, y, w, h); }
    void clearcolor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
    void clearstencil(int s) { glClearStencil(s); }
    void clear(GLbitfield mask) { glClear(mask); }
    void finish() { glFinish(); }

    void matrixmode(GLenum mode) { glMatrixMode(mode); }
    void loadidentity() { glLoadIdentity(); }
    void pushmatrix() { glPushMatrix(); }
    void popmatrix() { glPopMatrix(); }
    void loadmatrix(const GLfloat *m) { glLoadMatrixf(m); }
    void multmatrix(const GLfloat *m) { glMultMatrixf(m); }
    void translate(GLfloat x, GLfloat y, GLfloat z) { glTranslatef(x, y, z); }
    void rotate(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) { glRotatef(angle, x, y, z); }
    void scale(GLfloat x, GLfloat y, GLfloat z) { glScalef(x, y, z); }
    void frustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) { glFrustum(l, r, b, t, n, f); }
    void ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) { glOrtho(l, r, b, t, n, f); }
    void getfloatv(GLenum pname, GLfloat *v) { glGetFloatv(pname, v); }

    void enable(GLenum cap) { glEnable(cap); }
    void disable(GLenum cap) { glDisable(cap); }
    bool isenabled(GLenum cap) { return glIsEnabled(cap) != GL_FALSE; }
    void blendfunc(GLenum src, GLenum dst) { glBlendFunc(src, dst); }
    void depthfunc(GLenum func) { glDepthFunc(func); }
    void depthmask(GLboolean on) { glDepthMask(on); }
    void colormask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { glColorMask(r, g, b, a); }
    void cullface(GLenum face) { glCullFace(face); }
    void alphafunc(GLenum func, GLfloat ref) { glAlphaFunc(func, ref); }
    void polygonoffset(GLfloat factor, GLfloat units) { glPolygonOffset(factor, units); }
    void linewidth(GLfloat w) { glLineWidth(w); }
    void color3f(GLfloat r, GLfloat g, GLfloat b) { glColor3f(r, g, b); }
    void color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { glColor4f(r, g, b, a); }
    void color3ub(GLubyte r, GLubyte g, GLubyte b) { glColor3ub(r, g, b); }
    void color4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { glColor4ub(r, g, b, a); }
    void fogi(GLenum pname, GLint v) { glFogi(pname, v); }
    void fogf(GLenum pname, GLfloat v) { glFogf(pname, v); }
    void fogfv(GLenum pname, const GLfloat *v) { glFogfv(pname, v); }
    void hint(GLenum target, GLenum mode) { glHint(target, mode); }
    void shademodel(GLenum mode) { glShadeModel(mode); }
    void cleardepth(GLdouble d) { glClearDepth(d); }
    void texenvi(GLenum pname, GLint v) { glTexEnvi(GL_TEXTURE_ENV, pname, v); }
    void texenvfv(GLenum pname, const GLfloat *v) { glTexEnvfv(GL_TEXTURE_ENV, pname, v); }
    void activetexture(GLenum unit) { glActiveTexture_(unit); }
    void clientactivetexture(GLenum unit) { glClientActiveTexture_(unit); }
    void getintegerv(GLenum pname, GLint *v) { glGetIntegerv(pname, v); }

    void gentextures(GLsizei n, GLuint *t) { glGenTextures(n, t); }
    void deletetextures(GLsizei n, const GLuint *t) { glDeleteTextures(n, t); }
    void bindtexture(GLenum target, GLuint t) { glBindTexture(target, t); }
    void texparameteri(GLenum target, GLenum pname, GLint v) { glTexParameteri(target, pname, v); }
    void pixelstorei(GLenum pname, GLint v) { glPixelStorei(pname, v); }
    void teximage2d(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels)
    {
        glTexImage2D(target, level, internal, w, h, border, format, type, pixels);
    }
    void texsubimage2d(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels)
    {
        glTexSubImage2D(target, level, x, y, w, h, format, type, pixels);
    }

    void genbuffers(GLsizei n, GLuint *b) { glGenBuffers_(n, b); }
    void deletebuffers(GLsizei n, const GLuint *b) { glDeleteBuffers_(n, b); }
    void bindbuffer(GLenum target, GLuint b) { glBindBuffer_(target, b); }
    void bufferdata(GLenum target, GLsizeiptr size, const void *data, GLenum usage) { glBufferData_(target, size, data, usage); }
    void buffersubdata(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) { glBufferSubData_(target, offset, size, data); }

    void enableclientstate(GLenum array) { glEnableClientState(array); }
    void disableclientstate(GLenum array) { glDisableClientState(array); }
    void vertexpointer(GLint size, GLenum type, GLsizei stride, const void *p) { glVertexPointer(size, type, stride, p); }
    void colorpointer(GLint size, GLenum type, GLsizei stride, const void *p) { glColorPointer(size, type, stride, p); }
    void texcoordpointer(GLint size, GLenum type, GLsizei stride, const void *p) { glTexCoordPointer(size, type, stride, p); }
    void drawarrays(GLenum mode, GLint first, GLsizei count) { glDrawArrays(mode, first, count); }
    void multidrawarrays(GLenum mode, const GLint *first, const GLsizei *count, GLsizei n) { glMultiDrawArrays_(mode, (GLint *)first, (GLsizei *)count, n); } // (older glext.h declares non-const arrays)
    void drawelements(GLenum mode, GLsizei count, GLenum type, const void *indices) { glDrawElements(mode, count, type, indices); }
    void drawrangeelements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices)
    {
        glDrawRangeElements_(mode, start, end, count, type, indices);
    }

    void begin(GLenum mode) { glBegin(mode); }
    void end() { glEnd(); }
    void texcoord2f(GLfloat s, GLfloat t) { glTexCoord2f(s, t); }
    void texcoord2i(GLint s, GLint t) { glTexCoord2i(s, t); }
    void vertex2f(GLfloat x, GLfloat y) { glVertex2f(x, y); }
    void vertex3f(GLfloat x, GLfloat y, GLfloat z) { glVertex3f(x, y, z); }
    void vertex3fv(const GLfloat *v) { glVertex3fv(v); }

    // --- crosshair pick target: a 1x1 framebuffer object with the main depth buffer's precision

    void cleanuppick()
    {
        if(pickfbo) glDeleteFramebuffers_(1, &pickfbo);
        if(pickrb[0]) glDeleteRenderbuffers_(2, pickrb);
        pickfbo = pickrb[0] = pickrb[1] = 0;
        if(depthpbo) glDeleteBuffers_(1, &depthpbo);
        depthpbo = 0;
    }

    bool setuppick(int depthbits)
    {
        if(pickfbo) return true;
        // same depth precision as the main depth buffer; try the closest formats first
        GLenum formats[3];
        int numformats = 0;
        if(depthbits >= 32)
        {
            if(hasDBF) formats[numformats++] = GL_DEPTH_COMPONENT32F;
            formats[numformats++] = GL_DEPTH_COMPONENT32;
        }
        formats[numformats++] = GL_DEPTH_COMPONENT24;
        GLenum status = 0;
        loopi(numformats)
        {
            glGenRenderbuffers_(2, pickrb);
            glBindRenderbuffer_(GL_RENDERBUFFER_EXT, pickrb[0]);
            glRenderbufferStorage_(GL_RENDERBUFFER_EXT, GL_RGBA8, 1, 1);
            glBindRenderbuffer_(GL_RENDERBUFFER_EXT, pickrb[1]);
            glRenderbufferStorage_(GL_RENDERBUFFER_EXT, formats[i], 1, 1);
            glBindRenderbuffer_(GL_RENDERBUFFER_EXT, 0);
            glGenFramebuffers_(1, &pickfbo);
            glBindFramebuffer_(GL_FRAMEBUFFER_EXT, pickfbo);
            glFramebufferRenderbuffer_(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, pickrb[0]);
            glFramebufferRenderbuffer_(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, pickrb[1]);
            status = glCheckFramebufferStatus_(GL_FRAMEBUFFER_EXT);
            glBindFramebuffer_(GL_FRAMEBUFFER_EXT, 0);
            if(status == GL_FRAMEBUFFER_COMPLETE_EXT) return true;
            if(pickfbo) glDeleteFramebuffers_(1, &pickfbo);
            if(pickrb[0]) glDeleteRenderbuffers_(2, pickrb);
            pickfbo = pickrb[0] = pickrb[1] = 0;
        }
        conoutf("crosshair pick buffer not supported (0x%X): reading the depth in the middle of the frame instead", status);
        return false;
    }

    bool beginpick(int depthbits)
    {
        if(!hasFBO || !setuppick(depthbits)) return false;
        glBindFramebuffer_(GL_FRAMEBUFFER_EXT, pickfbo);
        glViewport(0, 0, 1, 1);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glClear(GL_DEPTH_BUFFER_BIT);
        return true;
    }

    void bindpbo()
    {
        if(!depthpbo)
        {
            glGenBuffers_(1, &depthpbo);
            glBindBuffer_(GL_PIXEL_PACK_BUFFER_ARB, depthpbo);
            glBufferData_(GL_PIXEL_PACK_BUFFER_ARB, sizeof(float), NULL, GL_STREAM_READ);
        }
        else glBindBuffer_(GL_PIXEL_PACK_BUFFER_ARB, depthpbo);
    }

    void endpick(bool async)
    {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if(async) // on drivers that read asynchronously, start copying the result right away
        {
            bindpbo();
            glReadPixels(0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
            glBindBuffer_(GL_PIXEL_PACK_BUFFER_ARB, 0);
        }
        glBindFramebuffer_(GL_FRAMEBUFFER_EXT, 0);
    }

    bool readpick(float &depth)
    {
        if(!pickfbo) return false;
        glBindFramebuffer_(GL_FRAMEBUFFER_EXT, pickfbo);
        glReadPixels(0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        glBindFramebuffer_(GL_FRAMEBUFFER_EXT, 0);
        return true;
    }

    bool readdepthasync(int x, int y)
    {
        if(!hasPBO) return false;
        bindpbo();
        glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, NULL); // into the buffer object: returns without waiting
        glBindBuffer_(GL_PIXEL_PACK_BUFFER_ARB, 0);
        return true;
    }

    bool resolvedepthasync(float &depth)
    {
        glBindBuffer_(GL_PIXEL_PACK_BUFFER_ARB, depthpbo);
        const float *d = (const float *)glMapBuffer_(GL_PIXEL_PACK_BUFFER_ARB, GL_READ_ONLY);
        if(d) { depth = *d; glUnmapBuffer_(GL_PIXEL_PACK_BUFFER_ARB); }
        glBindBuffer_(GL_PIXEL_PACK_BUFFER_ARB, 0);
        return d != NULL;
    }

    void readdepth(int x, int y, float &depth) { glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth); }
};

static glbackend glbackendinstance;
gfxdevice *gfx_opengl() { return &glbackendinstance; }
gfxdevice *gfx = &glbackendinstance;
