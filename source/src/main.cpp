// main.cpp: initialisation & main loop

#include "cube.h"
#include <SDL3/SDL_main.h> // (only here) on Windows: SDL3's WinMain, which calls main() below; SDL2 included it with SDL.h

void cleanup(char *msg)         // single program exit point;
{
    if(clientlogfile) clientlogfile->fflush();
    if(!msg)
    {
        cleanupclient();
        audiomgr.soundcleanup();
        cleanupserver();
    }
    SDL_ShowCursor();
    if(screen) SDL_SetWindowRelativeMouseMode(screen, false);
    if(msg)
    {
        #ifdef WIN32
        MessageBox(NULL, msg, "AssaultCube fatal error", MB_OK|MB_SYSTEMMODAL|MB_ICONERROR);
        #else
        printf("%s", msg);
        #endif
        if(clientlogfile)
        {
            clientlogfile->printf("%s\n", msg);
            dumpexecutionstack(clientlogfile);
        }
    }
    SDL_Quit();
    sl_skipsemaphoredestroy = true; // the process is about to exit: don't destroy static SDL semaphores after SDL_Quit
}

VAR(resetcfg, 0, 0, 1);

static void cleargpulock();

void quit()                     // normal exit
{
    if(clientlogfile) clientlogfile->fflush();
    const char *onquit = getalias("onQuit");
    setcontext("hook", "onQuit");
    if(onquit && onquit[0]) execute(onquit);
    resetcontext();
    alias("onQuit", "");
    alias("mapstartonce", "");
    cleargpulock();
    extern void writeinitcfg();
    writeinitcfg();
    writeservercfg();
    writepcksourcecfg();
    if(resetcfg) deletecfg();
    else writecfg();
    savehistory();
    writemapmodelattributes();
    entropy_save();
    writeallxmaps();
    cleanup(NULL);
    popscontext();
    DELETEP(clientlogfile);
    exit(EXIT_SUCCESS);
}
COMMAND(quit, "");

void fatal(const char *s, ...)    // failure exit
{
    static int errors = 0;
    errors++;

    if(errors <= 2)
    {
        defvformatstring(msg,s,s);
        if(errors <= 1)
        {
            defvformatstring(msg,s,s);
            defformatstring(msgerr)("%s (%s)\n", msg, SDL_GetError());
            cleanup(msgerr);
        }
        else puts(msg);
    }
    DELETEP(clientlogfile);
    exit(EXIT_FAILURE);
}

static int initing = NOT_INITING;
static bool restoredinits = false;

bool initwarning(const char *desc, int level, int type)
{
    if(initing < level)
    {
        addchange(desc, type);
        return true;
    }
    return false;
}

SDL_Window *screen = NULL;
SDL_GLContext glcontext = 0;

VAR(screenw, 1, 0, 0);   // actual current resolution of screen/window
VAR(screenh, 1, 0, 0);
#define SCR_MINW 320
#define SCR_MINH 200
#define SCR_MAXW 10000
#define SCR_MAXH 10000
VARF(scr_w, SCR_MINW, -1, SCR_MAXW, initwarning("screen resolution")); // screen or window resolution, except for "fullscreen desktop" mode
VARF(scr_h, SCR_MINH, -1, SCR_MAXH, initwarning("screen resolution"));
VARF(depthbits, 0, 0, 32, initwarning("depth-buffer precision"));
VARF(stencilbits, 0, 0, 32, initwarning("stencil-buffer precision"));
VARF(fsaa, -1, -1, 16, initwarning("anti-aliasing"));
VARF(vsync, -1, -1, 1, initwarning("vertical sync"));
VARF(highdpi, 0, 1, 1, initwarning("high-DPI rendering")); // render at native pixel resolution on Retina/HiDPI displays instead of being upscaled

// The renderer: 0 = OpenGL, 1 = the SDL GPU backend (gfx_sdlgpu.cpp: Metal on macOS, Vulkan on Linux), -1 = the
// platform's default: the GPU backend on macOS, OpenGL elsewhere (Vulkan on Linux is opt-in until it has been tested on
// real GPUs). Saved in init.cfg; --gl and --gpu override it for one run. A change takes effect at the next start.
VARF(renderer, -1, -1, 1, initwarning("renderer", INIT_RESET, CHANGE_RESTART));
static int forcerenderer = -1; // --gl: 0, --gpu: 1
bool usegpu = false;           // this run uses the GPU backend (if it starts: otherwise OpenGL)
// A start with the GPU backend that never got going (a crash, or a hang the player had to end) makes the next start use
// OpenGL, so a Mac where Metal fails doesn't lock players out of the menu: config/gpustart.lock is written before the
// GPU backend starts and removed after 300 frames or at a normal exit. (Not with --gpu, which asks for it explicitly.)
static string gpulockfile = "";
static void cleargpulock() { if(gpulockfile[0]) { delfile(gpulockfile); gpulockfile[0] = '\0'; } }
static int windoww = 0, windowh = 0; // window size in screen coordinates (points); screenw/screenh are the drawable size in pixels

VAR(desktopw, 1, 0, 0); // resolution of desktop (assumed to be maximum resolution)
VAR(desktoph, 1, 0, 0);

#ifdef WIN32
// SDL_WarpMouseInWindow behaves erratically on Windows, so force relative mouse instead.
VARN(relativemouse, userelativemouse, 1, 1, 0);
#else
VARNP(relativemouse, userelativemouse, 0, 1, 1);
#endif

static bool shouldgrab = false, grabinput = false, minimized = false, centerwindow = true, canrelativemouse = true, relativemouse = false;

#ifdef __APPLE__
// Relative mouse mode on macOS: with SDL2, "macmousewarp 1" (default) had SDL compute mouse motion by warping the cursor
// (the fix for https://github.com/assaultcube/AC/issues/300), which follows macOS's pointer acceleration. SDL3 always
// reads raw mouse deltas in relative mode; "macmousewarp 1" keeps the feel by applying the system's pointer scaling
// (acceleration) to them, "macmousewarp 0" uses the raw deltas. It takes effect immediately.
static void applymacmousewarp();
VARFP(macmousewarp, 0, 1, 1, applymacmousewarp());
static void applymacmousewarp()
{
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, macmousewarp ? "1" : "0", SDL_HINT_OVERRIDE);
}
#endif

#ifdef AC_X11_GRAB
VAR(sdl_xgrab_bug, 0, 0, 1);
#endif

void inputgrab(bool on, bool delay = false)
{
#ifdef AC_X11_GRAB
    bool wasrelativemouse = relativemouse;
#endif
    if (on)
    {
        SDL_HideCursor();
        if (canrelativemouse && userelativemouse)
        {
            if (SDL_SetWindowRelativeMouseMode(screen, true))
            {
                SDL_SetWindowMouseGrab(screen, true);
                relativemouse = true;
            }
            else
            {
                conoutf("\f3relative mouse mode is not available (%s): using absolute mouse positions", SDL_GetError());
                SDL_SetWindowMouseGrab(screen, false);
                canrelativemouse = false;
                relativemouse = false;
            }
        }
    }
    else
    {
        SDL_ShowCursor();
        if (relativemouse)
        {
            SDL_SetWindowMouseGrab(screen, false);
            SDL_SetWindowRelativeMouseMode(screen, false);
            relativemouse = false;
        }
    }
    shouldgrab = delay;

#ifdef AC_X11_GRAB
    if ((relativemouse || wasrelativemouse) && sdl_xgrab_bug)
    {
        // Workaround for buggy SDL X11 pointer grabbing (only when the window is an X11 window)
        SDL_PropertiesID props = SDL_GetWindowProperties(screen);
        Display *display = (Display *)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        Window window = (Window)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (display && window)
        {
            if (relativemouse)
            {
                uint mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask;
                XGrabPointer(display, window, True, mask, GrabModeAsync, GrabModeAsync, window, None, CurrentTime);
            }
            else XUngrabPointer(display, CurrentTime);
        }
    }
#endif
}


// which fullscreen mode the window uses: the desktop's ("fullscreendesktop 1", a borderless window covering the display)
// or an exclusive video mode of scr_w x scr_h (the closest mode the display has)
static void setfullscreenmode()
{
    extern int fullscreendesktop;
    SDL_DisplayMode mode;
    if(!fullscreendesktop && SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(screen), scr_w, scr_h, 0.0f, highdpi != 0, &mode))
        SDL_SetWindowFullscreenMode(screen, &mode);
    else SDL_SetWindowFullscreenMode(screen, NULL);
}

void setfullscreen(bool enable)
{
    if(!screen) return;
    if(enable == !(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN))
    {
        if(enable) setfullscreenmode();
        SDL_SetWindowFullscreen(screen, enable);
        inputgrab(grabinput = enable || grabinput);
        if(!enable) SDL_SetWindowSize(screen, scr_w, scr_h);
        if(!enable && centerwindow) SDL_SetWindowPosition(screen, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

VARF(fullscreen, 0, 1, 1, setfullscreen(fullscreen != 0));

void resetfullscreen()
{
    setfullscreen(false);
    setfullscreen(true);
}

VARF(fullscreendesktop, 0, 1, 1, if(fullscreen) resetfullscreen());

SVARFP(lang, "en", filterlang(lang, lang));

void writeinitcfg()
{
    if(!restoredinits) return;
    filerotate("config/init", "cfg", CONFIGROTATEMAX); // keep five old init config sets
    stream *f = openfile(path("config/init.cfg", true), "w");
    if(!f) return;
    f->printf("// automatically written on exit, DO NOT MODIFY\n// modify settings in game\n");
    f->printf("fullscreen %d\n", fullscreen);
    f->printf("fullscreendesktop %d\n", fullscreendesktop);
    f->printf("scr_w %d\n", scr_w);
    f->printf("scr_h %d\n", scr_h);
    f->printf("depthbits %d\n", depthbits);
    f->printf("stencilbits %d\n", stencilbits);
    f->printf("fsaa %d\n", fsaa);
    f->printf("vsync %d\n", vsync);
    f->printf("highdpi %d\n", highdpi);
    f->printf("renderer %d\n", renderer);
    extern int audio, soundchannels, igraphdefaultframetime;
    f->printf("audio %d\n", audio > 0 ? 1 : 0);
    f->printf("soundchannels %d\n", soundchannels);
    f->printf("igraphdefaultframetime %d\n", igraphdefaultframetime);
    if(lang && *lang) f->printf("lang %s\n", lang);
    extern void writezipmodconfig(stream *f);
    writezipmodconfig(f);
    delete f;
}

#if 0
VARP(highprocesspriority, 0, 1, 1);

void setprocesspriority(bool high)
{
#if defined(WIN32) && !defined(_DEBUG)
    SetPriorityClass(GetCurrentProcess(), high && highprocesspriority && fullscreen ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
#endif
}
#endif

VARP(pngcompress, 0, 9, 9);

void writepngchunk(stream *f, const char *type, const void *data = NULL, uint len = 0)
{
    f->putbig<uint>(len);
    f->write(type, 4);
    if(data) f->write(data, len);

    uint crc = crc32(0, Z_NULL, 0);
    crc = ~crc32(crc, (const Bytef *)type, 4);
    if(data) loopi(len) enet_crc32_inc(&crc, ((const uchar *)data)[i]);
    f->putbig<uint>(~crc);
}

int save_png(const char *filename, SDL_Surface *image)
{
    uchar *data = (uchar *)image->pixels;
    int iw = image->w, ih = image->h, pitch = image->pitch;

    stream *f = openfile(filename, "wb");
    if(!f) { conoutf("could not write to %s", filename); return -1; }
    const char *scores = asciiscores(true);

    uchar signature[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    f->write(signature, sizeof(signature));

    struct pngihdr
    {
        uint width, height;
        uchar bitdepth, colortype, compress, filter, interlace;
    } ihdr = { bigswap<uint>(iw), bigswap<uint>(ih), 8, 2, 0, 0, 0 };
    writepngchunk(f, "IHDR", (uchar *)&ihdr, 13);

    int idat = f->tell();
    uint len = 0;
    f->write("\0\0\0\0IDAT", 8);
    uint crc = crc32(0, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)"IDAT", 4);

    z_stream z;
    z.zalloc = NULL;
    z.zfree = NULL;
    z.opaque = NULL;

    if(deflateInit(&z, pngcompress) != Z_OK)
        goto error;

    uchar buf[1<<12];
    z.next_out = (Bytef *)buf;
    z.avail_out = sizeof(buf);

    loopi(ih)
    {
        uchar filter = 0;
        loopj(2)
        {
            z.next_in = j ? (Bytef *)data + i*pitch : (Bytef *)&filter;
            z.avail_in = j ? iw*3 : 1;
            while(z.avail_in > 0)
            {
                if(deflate(&z, Z_NO_FLUSH) != Z_OK) goto cleanuperror;
                #define FLUSHZ do { \
                    int flush = sizeof(buf) - z.avail_out; \
                    crc = crc32(crc, buf, flush); \
                    len += flush; \
                    f->write(buf, flush); \
                    z.next_out = (Bytef *)buf; \
                    z.avail_out = sizeof(buf); \
                } while(0)
                FLUSHZ;
            }
        }
    }

    for(;;)
    {
        int err = deflate(&z, Z_FINISH);
        if(err != Z_OK && err != Z_STREAM_END) goto cleanuperror;
        FLUSHZ;
        if(err == Z_STREAM_END) break;
    }

    deflateEnd(&z);

    f->seek(idat, SEEK_SET);
    f->putbig<uint>(len);
    f->seek(0, SEEK_END);
    f->putbig<uint>(crc);

    writepngchunk(f, "tEXt", "\123o\165rc\145\0\101ss\141" "\165\154\164\103\165\142\145", 18);
    writepngchunk(f, "tEXt", scores - 8, strlen(scores) + 8);
    writepngchunk(f, "IEND");

    delete f;
    return 0;

cleanuperror:
    deflateEnd(&z);

error:
    delete f;

    return -1;
}

#include "jpegenc.h"

void mapscreenshot(const char *imagepath, bool mapshot, int fileformat, float scale, int height, int quality)
{
    extern int minimaplastsize;
    int src_w = mapshot ? minimaplastsize : screenw;
    int src_h = mapshot ? minimaplastsize : screenh;
    int dst_w = mapshot ? src_w : src_w * scale, img_w = dst_w;
    int dst_h = mapshot ? src_h : src_h * scale, img_h = dst_h;
    if(height)
    { // create 4:3 of exact height
        if(dst_w >= (dst_h * 4) / 3)
        { // 4:3 or wider: crop width
            dst_w = (src_w * height) / src_h;
            img_h = dst_h = height;
            img_w = (img_h * 4) / 3;
        }
        else
        { // probably 5:4: crop height
            img_h = height;
            dst_w = img_w = (img_h * 4) / 3;
            dst_h = (src_h * dst_w) / src_w;
        }
    }

    SDL_Surface *image = creatergbsurface(img_w, img_h);
    if(!image) return;

    int tmpdstsize = dst_w * dst_h * 3, tmpdstpitch = dst_w * 3;
    uchar *tmpdst = new uchar[tmpdstsize], *dst = (uchar *)image->pixels;

    if(mapshot)
    {
        extern GLuint minimaptex;
        if(minimaptex) gfx->gettexture(minimaptex, GL_RGB, tmpdst);
        else
        {
            conoutf("no mapshot prepared!");
            SDL_DestroySurface(image);
            delete[] tmpdst;
            return;
        }
        loopi(img_h)
        { // copy image
            memcpy(dst, &tmpdst[tmpdstpitch * i], image->pitch);
            dst += image->pitch;
        }
    }
    else
    {
        gfx->pixelstorei(GL_PACK_ALIGNMENT, 1);
        if(src_w != dst_w || src_h != dst_h)
        { // scale image
            uchar *tmpsrc = new uchar[src_w * src_h * 3];
            gfx->readpixels(0, 0, src_w, src_h, GL_RGB, GL_UNSIGNED_BYTE, tmpsrc);
            scaletexture(tmpsrc, src_w, src_h, 3, tmpdst, dst_w, dst_h);
            delete[] tmpsrc;
        }
        else gfx->readpixels(0, 0, dst_w, dst_h, GL_RGB, GL_UNSIGNED_BYTE, tmpdst);
        int crop_w = height ? ((dst_w - img_w) / 2) * 3 : 0;
        int crop_h = height ? (dst_h - img_h) / 2 : 0;
        loopirev(img_h)
        { // flip image
            memcpy(dst, &tmpdst[tmpdstpitch * (i + crop_h) + crop_w], image->pitch);
            dst += image->pitch;
        }
    }
    entropy_add_block(tmpdst, tmpdstsize);
    delete[] tmpdst;
    if(!mapshot && imagepath) audiomgr.playsound(S_CAMERA);

    switch(fileformat)
    {
        case 0: // bmp
        {
            stream *file = openfile(imagepath, "wb");
            if(!file) conoutf("failed to create: %s", imagepath);
            else
            {
                SDL_SaveBMP_IO(image, file->rwops(), 1);
                delete file;
            }
            break;
        }
        case 1: // jpeg
        {
            jpegenc jpegencoder;
            jpegencoder.encode(imagepath, image, quality, asciiscores(true));
            break;
        }
        case 2: // png
            if(save_png(imagepath, image) < 0) conoutf("\f3Error saving png file");
            break;
    }

    SDL_DestroySurface(image);
}

FVARP(screenshotscale, 0.1f, 1.0f, 1.0f);
VARP(jpegquality, 10, 85, 100);  // best: 100 - good: 85 [default] - bad: 70 - terrible: 50
VARP(screenshottype, 0, 1, 2);

const char *getscrext()
{
    const char *screxts[] = { ".bmp", ".jpg", ".png" };
    return screxts[screenshottype % 3];
}
COMMANDF(getscrext, "", () { result(getscrext()); });

void screenshot(const char *filename)
{
    static string buf;
    if(filename && filename[0]) formatstring(buf)("screenshots/%s%s", filename, getscrext());
    else if(getclientmap()[0]) formatstring(buf)("screenshots/%s_%s_%s%s", timestring(), behindpath(getclientmap()), modestr(gamemode, true), getscrext());
    else formatstring(buf)("screenshots/%s%s", timestring(), getscrext());
    path(buf);
    mapscreenshot(buf, false, screenshottype, screenshotscale, 0, jpegquality);
}
COMMAND(screenshot, "s");

void mapshot()
{
    defformatstring(buf)("screenshots" PATHDIVS "mapshot_%s_%s%s", behindpath(getclientmap()), timestring(), getscrext());
    mapscreenshot(buf, true, screenshottype, screenshotscale, 0, jpegquality);
}
COMMAND(mapshot, "");

void screenshotpreview(int *res)
{
    static int lastres = 240;
    defformatstring(buf)("packages" PATHDIVS "maps" PATHDIVS "%spreview" PATHDIVS "%s.jpg", securemapcheck(getclientmap(), false) ? "official" PATHDIVS : "", behindpath(getclientmap()));
    mapscreenshot(buf, false, 1, 1.0f, (lastres = clamp(((*res ? *res : lastres) / 48) * 48, 144, 480)), 80);
    reloadtexture(*textureload(buf, 3));
}
COMMAND(screenshotpreview, "i");

bool needsautoscreenshot = false;

int hextodec(uchar a){ return (a >= 48 && a <= 57) ? (a - 48) : (a >= 97 && a <= 102) ? (a - 87) : 0; }
int twohextodec(uchar a, uchar b){ return hextodec(a) * 16 + hextodec(b); }

Texture *getidenticon(char uhash[PUBKEYSAFE])
{
    prepidenticon(uhash);
    defformatstring(imagepath)("packages" PATHDIVS "identicons" PATHDIVS "%s.png", uhash);
    return textureload(imagepath);
}

int huecharindex(int byte)
{
    // 0–3 | 4–6 | 7–8 | 9–11 | 12–15 <==> [0–4]
    //  ∆4 |  ∆3 |  ∆2 |  ∆3  |   ∆4
    static int grouping[16] = {0,0,0,0,1,1,1,2,2,3,3,3,4,4,4,4};
    if(byte >= 0 && byte <= 15) return grouping[byte];
    return 0; // first hueblockindex is fallback
}

int hueblockindex(int sum)
{
    // 0–9 | 10–19 | 20–25 | 26–35 | 36–45 <==> [0–4]
    // ∆10 |  ∆10  |   ∆6  |  ∆10  |  ∆10
    if(sum > 35)
    {
        return 4;
    }else{
        if(sum > 25)
        {
            return 3;
        }else{
            if(sum > 19)
            {
                return 2;
            }else{
                if(sum > 9) return 1;
            }
        }
    }
    return 0; // first hueblockindex is fallback
}

int hueblocksum(char uhash[PUBKEYSAFE], int offset)
{
    if(offset>=0 && offset<=PUBKEYSAFE-4)
    {
        return hextodec(uhash[offset]) + hextodec(uhash[offset+1]) + hextodec(uhash[offset+2]);
    }
    return 0; // no use indicating failure
}

int hexfromhash3c(char uhash[PUBKEYSAFE], int offset){
    if(offset>=0 && offset<=PUBKEYSAFE-4)
    {
        return 256 * hextodec(uhash[offset]) + 16 * hextodec(uhash[offset+1]) + hextodec(uhash[offset+2]);
    }
    return 0; // no use indicating failure
}

void prepidenticon(char uhash[PUBKEYSAFE])
{
    // see source/DEVELOPMENT.txt for more than the comments here
    static int imagesize = 128;
    static int blocksize = 16;
    static int padding = 8;
    static int padhues[5] = {0x00,0x40,0x80,0xC0,0xFF};
    extern string homedir; // we need to fileexists check profile explicitly, but installation has some prerendered too
    defformatstring(imagepath_read)("%spackages" PATHDIVS "identicons" PATHDIVS "%s.png", homedir, uhash);
    defformatstring(imagepath_save)("packages" PATHDIVS "identicons" PATHDIVS "%s.png", uhash);
    if( fileexists(imagepath_read, "r") || fileexists(imagepath_save, "r") ){ return; } // both paths may have files to *load*, textureload will find them but this is faster
    SDL_Surface *image = creatergbsurface(imagesize, imagesize);
    if(!image) return;
    SDL_Rect useRect;
    int CC[6];
    int ccidx = 0;
    for(int charidx=45; charidx<57; charidx+=2) CC[ccidx++] = twohextodec(uhash[charidx],uhash[charidx+1]);
    int sum4blocks = CC[0]+CC[1]+CC[2];
    if(sum4blocks>3*128){ loopi(3) CC[i] /= 2; } // if the colour is too bright we halve each channel, so it has contrast to the white backdrop
    SDL_FillSurfaceRect(image, NULL, SDL_MapSurfaceRGB(image, CC[3], CC[4], CC[5])); // border
    Uint32 iiFG = SDL_MapSurfaceRGB(image, CC[0], CC[1], CC[2]); // blocks
    int curhue = padhues[hueblockindex(hueblocksum(uhash, 58))];
    Uint32 iiCC = SDL_MapSurfaceRGB(image, curhue, curhue, curhue); // corners
    curhue = padhues[hueblockindex(hueblocksum(uhash, 61))];
    Uint32 iiEC = SDL_MapSurfaceRGB(image, curhue, curhue, curhue); // edges

    // now we fill the padding blocks with their respective hues by 3-tuple of bytes
    // first the corners with their squares ..
    useRect.w = useRect.h = 2 * blocksize;
    useRect.x = useRect.y = padding;
    SDL_FillSurfaceRect(image, &useRect, iiCC);
    useRect.x += 5 * blocksize;
    SDL_FillSurfaceRect(image, &useRect, iiCC);
    useRect.y += 5 * blocksize;
    SDL_FillSurfaceRect(image, &useRect, iiCC);
    useRect.x -= 5 * blocksize;
    SDL_FillSurfaceRect(image, &useRect, iiCC);
    // .. now the horizontal edges ..
    useRect.y -= 3 * blocksize;
    useRect.h = 3 * blocksize;
    SDL_FillSurfaceRect(image, &useRect, iiEC);
    useRect.x += 5 * blocksize;
    SDL_FillSurfaceRect(image, &useRect, iiEC);
    // .. now the vertical edges ..
    useRect.x = padding + 2 * blocksize;
    useRect.y = padding;
    useRect.w = 3 * blocksize;
    useRect.h = 2 * blocksize;
    SDL_FillSurfaceRect(image, &useRect, iiEC);
    useRect.y += 5 * blocksize;
    SDL_FillSurfaceRect(image, &useRect, iiEC);

    // white backdrop for the inner pattern .. leaving it out changes optics considerably .. but otherwise the sum4blocks hack becomes more involved.
    useRect.w = useRect.h = 5 * blocksize;
    useRect.x = useRect.y = padding + blocksize;
    SDL_FillSurfaceRect(image, &useRect, SDL_MapSurfaceRGB(image, 255, 255, 255));

    // the central pattern
    useRect.w = blocksize;
    useRect.h = blocksize;
    loopi(5) // rows
    {
        int coff = i * 9; // char offset
        int dots[3] = { hexfromhash3c(uhash, coff) % 2, hexfromhash3c(uhash, coff+3) % 2, hexfromhash3c(uhash, coff+6) % 2 };
        useRect.y = padding + (i+1) * blocksize;
        loopk(3)
        {
            if(dots[k])
            {
                useRect.x = padding + (k+1) * blocksize; // 0 | 1 | 2
                SDL_FillSurfaceRect(image, &useRect, iiFG);
                if(k<2)
                {
                    useRect.x += (k==0?4:2) * blocksize; // 0&4 | 1&3
                    SDL_FillSurfaceRect(image, &useRect, iiFG);
                }
            }
        }
    }
    if(save_png(imagepath_save, image) < 0) conoutf("\f3Error saving identicon [%s]", uhash);
}
COMMAND(prepidenticon, "s");

void updatescreensize()
{
    SDL_GetWindowSize(screen, &windoww, &windowh);
    // on HiDPI displays the GL drawable is larger than the window (e.g. 2x on Retina): render at the drawable size
    SDL_GetWindowSizeInPixels(screen, &screenw, &screenh);
    if(screenw < 1 || screenh < 1) { screenw = windoww; screenh = windowh; }
    if(screenw < 1) screenw = 1;
    if(screenh < 1) screenh = 1;
    VIRTW = screenw * VIRTH / screenh;
    gfx->viewport(0, 0, screenw, screenh);
    static int lastw = 0, lasth = 0;
    if((screenw != lastw || screenh != lasth) && (screenw != windoww || screenh != windowh))
        conoutf("high-DPI rendering: %dx%d pixels in a %dx%d window", screenw, screenh, windoww, windowh);
    lastw = screenw; lasth = screenh;
}

void screenres(int w, int h)
{
    if(!screen) return;
    scr_w = clamp(w, SCR_MINW, SCR_MAXW);
    scr_h = clamp(h, SCR_MINH, SCR_MAXH);
    if(fullscreendesktop)
    {
        scr_w = min(scr_w, desktopw);
        scr_h = min(scr_h, desktoph);
    }
    if(!(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN)) SDL_SetWindowSize(screen, scr_w, scr_h);
    else if(!fullscreendesktop) initwarning("screen resolution");
}
#if defined(WIN32) || defined(__APPLE__) || !defined(WIN32)
void setresdata(char *s, enet_uint32 c)
{
    extern hashtable<char *, enet_uint32> &resdata;
    resdata[newstring(s)] = c;
}
#endif

COMMANDF(screenres, "ii", (int *w, int *h) { screenres(*w, *h); });

// "gamma" is applied by a post-processing shader ("gammashader", shaders.cpp), so it only affects the game's image. SDL3
// has no hardware gamma ramp: without the shader (no GLSL, or "gammashader 0") gamma stays at 100.
void applygamma()
{
    extern int vgamma;
    extern bool postfxgamma();
    static bool warned = false;
    if(vgamma != 100 && !postfxgamma() && !warned)
    {
        conoutf("\f3gamma can't be applied: it needs the gamma shader (\"gammashader 1\" and GLSL support)");
        warned = true;
    }
}

VARNFP(gamma, vgamma, 30, 100, 300, applygamma());

void restoregamma() { applygamma(); }

void getdisplayresolutions()
{
    string res = "";
    int lh = 0, lw = 0, n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n);
    loopi(n)
    {
        const SDL_DisplayMode &mode = *modes[i];
        if(lw != mode.w && lh != mode.h) concatformatstring(res, "%s%d %d", *res ? " " : "", (lw = mode.w), (lh = mode.h));
    }
    SDL_free(modes);
    result(res);
}
COMMAND(getdisplayresolutions, "");

void setupscreen(int &useddepthbits, int &usedfsaa)
{
    if(gfx != gfx_opengl()) // resetgl: the GPU backend starts over with the new window, as GL does with a new context
    {
        gfx = gfx_opengl();
        gfx_sdlgpu_destroy();
    }
    if(glcontext)
    {
        SDL_GL_DestroyContext(glcontext);
        glcontext = NULL;
    }
    if(screen)
    {
        SDL_DestroyWindow(screen);
        screen = NULL;
    }

    SDL_Rect desktop;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    if(!SDL_GetDisplayBounds(display, &desktop)) fatal("failed querying desktop bounds: %s", SDL_GetError());
    desktopw = desktop.w;
    desktoph = desktop.h;
    if(scr_w < 0 || scr_h < 0) { scr_w = desktopw; scr_h = desktoph; } // first run: set to fullscreen res

    int nummodes = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &nummodes);
    if(nummodes >= 1)
    {
        bool hasmode = false;
        loopi(nummodes) if(scr_w <= modes[i]->w && scr_h <= modes[i]->h) { hasmode = true; break; }
        if(!hasmode) { scr_w = modes[0]->w; scr_h = modes[0]->h; } // the largest mode
    }
    SDL_free(modes);
    int winw = fullscreen && fullscreendesktop ? desktopw : scr_w;
    int winh = fullscreen && fullscreendesktop ? desktoph : scr_h;

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if(highdpi) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if(fullscreen) flags |= SDL_WINDOW_FULLSCREEN;

    if(usegpu)
    {
        // SDL GPU renderer: no GL context; the GPU device takes over the window. Falls back to OpenGL if it can't.
        screen = SDL_CreateWindow("AssaultCube", winw, winh, flags);
        if(screen && gfx_sdlgpu()->init(screen, fsaa > 0 ? fsaa : 0, vsync))
        {
            gfx = gfx_sdlgpu();
            if(fullscreen) setfullscreenmode();
            gfx->displaychanged();
            updatescreensize();
            inputgrab(grabinput = fullscreen ? true : false);
            useddepthbits = gfx->getdepthbits();
            usedfsaa = gfx->getsamples() > 1 ? gfx->getsamples() : 0;
            hasstencil = gfx->getstencilbits() >= 8;
            return;
        }
        conoutf("\f3SDL GPU renderer not available: using OpenGL");
        if(screen) SDL_DestroyWindow(screen);
        screen = NULL;
        usegpu = false;
    }

    static int configs[] =
    {
        0x7, /* try everything */
        0x6, 0x5, 0x3, /* try disabling one at a time */
        0x4, 0x2, 0x1, /* try disabling two at a time */
        0 /* try disabling everything */
    };
    int config = 0;
    // depthbits 0 means "auto": ask for a 24 bit z-buffer (supported by every GPU of the last two decades) and
    // fall back to 16 bit. 16 bit depth causes z-fighting on distant geometry and decals.
    const int wantdepthbits = depthbits ? depthbits : 24;
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    if(!fsaa)
    {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }
    loopi(sizeof(configs)/sizeof(configs[0]))
    {
        config = configs[i];
        if(!stencilbits && config&2) continue;
        if(fsaa<=0 && config&4) continue;
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, config&1 ? wantdepthbits : 16);
        if(stencilbits)
        {
            SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, config&2 ? stencilbits : 0);
            hasstencil = (config&2)!=0;
        }
        else hasstencil = false;
        if(fsaa>0)
        {
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, config&4 ? 1 : 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, config&4 ? fsaa : 0);
        }
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        screen = SDL_CreateWindow("AssaultCube", winw, winh, SDL_WINDOW_OPENGL | flags);
        if(screen)
        {
            glcontext = SDL_GL_CreateContext(screen);
            if(glcontext) break;
            SDL_DestroyWindow(screen);
        }
    }
    if(!screen) fatal("Unable to create OpenGL screen");
    else
    {
        if(depthbits && (config&1)==0) conoutf("%d bit z-buffer not supported - disabling", depthbits);
        if(stencilbits && (config&2)==0) conoutf("%d bit stencil buffer not supported - disabling", stencilbits);
        if(fsaa>0 && (config&4)==0) conoutf("%dx anti-aliasing not supported - disabling", fsaa);
    }
    if(fullscreen) setfullscreenmode(); // (SDL3 creates fullscreen windows in the desktop's mode)

    if(vsync>=0) SDL_GL_SetSwapInterval(vsync);

    updatescreensize();

    inputgrab(grabinput = fullscreen ? true : false);

    useddepthbits = config&1 ? wantdepthbits : 16;
    int gotdepthbits = 0;
    if(SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &gotdepthbits) && gotdepthbits > 0) useddepthbits = gotdepthbits;
    conoutf("z-buffer: %d bit", useddepthbits);
    usedfsaa = config&4 ? fsaa : 0; // bit 2 is multisampling (bit 1 is stencil)
}

extern int hirestextures;

void resetgl()
{
    clearchanges(CHANGE_GFX);

    loadingscreen();

    extern void cleanupparticles();
    extern void cleanupmodels();
    extern void cleanuptextures();
    extern void cleanuptmus();
    extern void cleanupgl();
    cleanupparticles();
    cleanupmodels();
    cleanuptextures();
    cleanuptmus();
    cleanupgl();
    uniformtexres = !hirestextures;
    c2skeepalive();

    int useddepthbits = 0, usedfsaa = 0;
    setupscreen(useddepthbits, usedfsaa);
    gl_init(screenw, screenh, useddepthbits, usedfsaa);

    extern void reloadfonts();
    extern Texture *startscreen;
    c2skeepalive();
    if(!reloadtexture(*notexture) ||
       !reloadtexture(*startscreen))
        fatal("failed to reload core texture");
    loadingscreen();
    c2skeepalive();
    restoregamma();
    c2skeepalive();
    reloadfonts();
    reloadtextures();
    c2skeepalive();
    drawscope(true);
    preload_playermodels();
    c2skeepalive();
    preload_hudguns();
    c2skeepalive();
    preload_entmodels();
    c2skeepalive();
    preload_mapmodels();
    c2skeepalive();
}

COMMAND(resetgl, "");

VARFP(maxfps, 0, 200, 1000, if(maxfps && maxfps < 25) maxfps = 25);
VARP(gpusync, 0, 0, 1);
VARP(fpslimitspin, 0, 250, 2000); // microseconds at the end of each capped frame that are busy-waited instead of slept, to absorb OS timer jitter
// While the window is in the background (another window has the input focus, or it's minimized) there's no point in
// drawing at full speed: it only costs battery and heat. 60 fps keeps the game, network updates and a local server
// running as usual (they need 25-60 frames per second) and the picture smooth on a second display. 0: no extra cap.
VARP(maxfpsbackground, 0, 60, 1000);

// high-resolution clock: milliseconds with sub-millisecond precision, on the same epoch as SDL_GetTicks()
double hrclockms()
{
    static Uint64 start = 0;
    static double msperticks = 0, startms = 0;
    Uint64 now = SDL_GetPerformanceCounter();
    if(!start)
    {
        start = now;
        startms = SDL_GetTicks();
        msperticks = 1000.0 / double(SDL_GetPerformanceFrequency());
    }
    return startms + double(now - start) * msperticks;
}

// frame-time benchmark: "benchmark <seconds>" records real frame times, e.g. during demo playback
// ("demo <name>; benchmark 60"), then prints average fps, 1%/0.1% lows and frame-time percentiles.
// "benchmark 0" stops a running benchmark early and prints the result so far.
static vector<float> benchframes;
static double benchwaituntil = 0; // recording starts then (after raising the window)
VAR(benchmarkdump, 0, 0, 1); // "benchmarkdump 1": the benchmark also writes every frame's time to the log, in order
static double benchstart = -1, benchprev = -1;
static int benchseconds = 0;
static bool benchactive = false;

static int benchcmp(const float *a, const float *b) { return *a < *b ? 1 : (*a > *b ? -1 : 0); } // slowest frame first

// "x% low": average fps over the slowest x% of frames
static double benchlowfps(vector<float> &slowestfirst, double fraction)
{
    int k = max(1, int(slowestfirst.length() * fraction));
    double t = 0;
    loopi(k) t += slowestfirst[i];
    return t > 0 ? 1000.0 * k / t : 0;
}

static void benchreport()
{
    benchactive = false;
    benchprev = -1;
    int n = benchframes.length();
    if(n < 10) { conoutf("benchmark: not enough frames recorded (%d)", n); benchframes.setsize(0); return; }
    double total = 0;
    loopv(benchframes) total += benchframes[i];
    if(benchmarkdump) // every frame's time in order, for looking at frame pacing
    {
        string line = "";
        loopv(benchframes)
        {
            concatformatstring(line, "%s%.3f", *line ? " " : "", benchframes[i]);
            if(strlen(line) > 200 || i == benchframes.length() - 1) { clientlogf("benchmark frames: %s", line); line[0] = '\0'; }
        }
    }
    benchframes.sort(benchcmp);
    extern int maxfps, vsync;
    conoutf("benchmark: %d frames in %.1f s: avg %.1f fps (%.3f ms), 1%% low %.1f fps, 0.1%% low %.1f fps",
            n, total / 1000.0, 1000.0 * n / total, total / n, benchlowfps(benchframes, 0.01), benchlowfps(benchframes, 0.001));
    conoutf("benchmark: frame time median %.3f ms, 99th percentile %.3f ms, 99.9th percentile %.3f ms, worst %.3f ms (maxfps %d, vsync %d, %dx%d)",
            benchframes[n / 2], benchframes[n / 100], benchframes[n / 1000], benchframes[0], maxfps, vsync, screenw, screenh);
    benchframes.setsize(0);
}

// renderer profile: CPU time per frame phase while a benchmark runs (see rpmark() in protos.h)
static const char *rpnames[RP_NUM] = { "game update + input", "occlusion rays", "audio, network, other", "frame setup + dynamic lights",
    "water reflection", "world geometry (CPU)", "crosshair pick pass", "world draw calls", "map models + shadows", "depth readback (issue)",
    "depth readback (fetch, next frame)", "players + items",
    "HUD gun", "particles + water", "HUD + text", "swap / present" };
static double rpacc[RP_NUM], rpcpu[RP_NUM], rplast = 0, rpcpulast = 0;
static int rpframes = 0;
// per-frame phase times, so the slowest frames can be compared with typical ones
struct rpframe { float start, total, phase[RP_NUM]; };
static vector<rpframe> rpframelog;
static float rpcur[RP_NUM];

// CPU time used by the calling thread: compared with wall time, it shows whether a phase is busy or waiting (on the GPU)
static double threadcpums()
{
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec ts;
    if(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
    return 0;
}

void rpmark(int phase)
{
    if(!benchactive) return;
    double t = hrclockms(), c = threadcpums();
    rpcur[phase] += t - rplast;
    rpacc[phase] += t - rplast;
    rpcpu[phase] += c - rpcpulast;
    rplast = t;
    rpcpulast = c;
}

static int rpframecmp(const rpframe *a, const rpframe *b) { return a->total < b->total ? 1 : (a->total > b->total ? -1 : 0); } // slowest first
static int floatcmpasc(const float *a, const float *b) { return *a < *b ? -1 : (*a > *b ? 1 : 0); }

// where the time goes in the slowest 1% of frames, compared with frames around the median
static void rpspikereport()
{
    int n = rpframelog.length();
    if(n < 200) return;
    rpframelog.sort(rpframecmp);
    int k = max(1, n / 100);
    float slow[RP_NUM + 1], med[RP_NUM + 1];
    loopi(RP_NUM + 1) slow[i] = med[i] = 0;
    int m0 = n * 45 / 100, m1 = n * 55 / 100;
    loopj(k) { const rpframe &f = rpframelog[j]; float sum = 0; loopi(RP_NUM) { slow[i] += f.phase[i]; sum += f.phase[i]; } slow[RP_NUM] += f.total - sum; }
    for(int j = m0; j < m1; j++) { const rpframe &f = rpframelog[j]; float sum = 0; loopi(RP_NUM) { med[i] += f.phase[i]; sum += f.phase[i]; } med[RP_NUM] += f.total - sum; }
    float slowtotal = 0, medtotal = 0;
    loopj(k) slowtotal += rpframelog[j].total;
    for(int j = m0; j < m1; j++) medtotal += rpframelog[j].total;
    conoutf("benchmark: slowest 1%% of frames (%d, avg %.3f ms) vs median frames (avg %.3f ms), by phase:", k, slowtotal / k, medtotal / (m1 - m0));
    loopi(RP_NUM + 1)
    {
        float s = slow[i] / k, md = med[i] / (m1 - m0);
        if(s >= 0.01f || md >= 0.01f) conoutf("benchmark:   %-36s median %7.3f ms   slowest %7.3f ms", i < RP_NUM ? rpnames[i] : "outside the tracked phases", md, s);
    }
    // are the slow frames periodic? (interval between consecutive slow frames)
    vector<float> starts;
    loopj(k) starts.add(rpframelog[j].start);
    starts.sort(floatcmpasc);
    vector<float> gaps;
    loopj(starts.length() - 1) gaps.add(starts[j+1] - starts[j]);
    if(gaps.length() >= 3)
    {
        gaps.sort(floatcmpasc);
        conoutf("benchmark:   slow frames recur every %.1f ms (median gap; quartiles %.1f / %.1f ms)", gaps[gaps.length()/2], gaps[gaps.length()/4], gaps[gaps.length()*3/4]);
    }
}

static void rpreport()
{
    if(rpframes <= 0) return;
    rpspikereport();
    double total = 0, totalcpu = 0;
    loopi(RP_NUM) { total += rpacc[i]; totalcpu += rpcpu[i]; }
    conoutf("benchmark: main thread time per frame by phase (avg over %d frames: %.3f ms, of which %.0f%% busy on the CPU):",
            rpframes, total / rpframes, total > 0 ? 100 * totalcpu / total : 0);
    loopi(RP_NUM) if(rpacc[i] / rpframes >= 0.0005)
        conoutf("benchmark:   %-36s %7.3f ms %5.1f%%  (CPU busy %3.0f%%)", rpnames[i], rpacc[i] / rpframes, total > 0 ? 100 * rpacc[i] / total : 0,
                rpacc[i] > 0 ? min(100.0, 100 * rpcpu[i] / rpacc[i]) : 0);
}

// optional camera spin: one full turn over the benchmark, from the current position (singleplayer only)
static bool benchspin = false;
static float benchspinyaw = 0;

static void benchframe(double now) // called once per frame with the frame's start time
{
    if(!benchactive || now < benchwaituntil) return;
    if(benchprev >= 0)
    {
        benchframes.add(float(now - benchprev));
        rpframes++;
        rpframe &f = rpframelog.add();
        f.start = float(benchprev - benchstart);
        f.total = float(now - benchprev);
        loopi(RP_NUM) f.phase[i] = rpcur[i];
    }
    else
    {
        benchstart = now;
        loopi(RP_NUM) rpacc[i] = rpcpu[i] = 0;
        rpframes = 0;
        rpframelog.setsize(0);
    }
    loopi(RP_NUM) rpcur[i] = 0;
    benchprev = now;
    rplast = now;
    rpcpulast = threadcpums();
    if(benchspin && !multiplayer(NULL))
    {
        float yaw = benchspinyaw + 360.0f * float((now - benchstart) / (benchseconds * 1000.0));
        player1->yaw = camera1->yaw = fmodf(yaw, 360.0f);
    }
    if(now - benchstart >= benchseconds * 1000.0)
    {
        rpreport();
        benchreport();
    }
}

// a window without focus can be composited at a lower rate (measured: 120 instead of 144 Hz for a Metal window):
// measure the window the way it's played, in front. Returns whether it had to be raised.
static bool benchraisewindow()
{
    if(!screen || (SDL_GetWindowFlags(screen) & SDL_WINDOW_INPUT_FOCUS)) return false;
    SDL_RaiseWindow(screen);
    return true;
}

static void benchbegin(int seconds, bool spin, double waituntil)
{
    benchframes.setsize(0);
    benchseconds = seconds;
    benchprev = -1;
    benchspin = spin;
    benchspinyaw = player1->yaw;
    benchactive = true;
    benchwaituntil = waituntil;
    if(screen) clientlogf("benchmark: %s, %s, renderer %s", SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN ? "fullscreen" : "window",
        SDL_GetWindowFlags(screen) & SDL_WINDOW_INPUT_FOCUS ? "focused" : "not focused", gfx->name());
}

void benchmark(int *seconds, int *spin)
{
    if(benchactive) { rpreport(); benchreport(); }
    else if(*seconds <= 0) conoutf("benchmark: not running");
    if(*seconds <= 0) return;
    bool turn = *spin != 0;
    if(turn && multiplayer(NULL)) { conoutf("benchmark: the camera spin is only available in singleplayer"); turn = false; }
    conoutf("benchmark: recording frame times for %d seconds%s", *seconds, turn ? ", turning the camera through 360 degrees" : "");
    benchbegin(*seconds, turn, benchraisewindow() ? hrclockms() + 1000 : 0); // once bringing the window to the front has settled
}
COMMAND(benchmark, "ii");

// timedemo: "timedemo <demo> [game seconds] [screenshot every n game seconds]" plays a demo as fast as the computer can
// draw it, with the game time advancing by a fixed timedemostep ms every frame. Times count from when the (local)
// server starts playing the demo, and the random generator and the physics steps are reset then: every run draws the
// same frames. That makes it a repeatable benchmark of real gameplay (players, weapons, particles; reported like
// "benchmark", from 2 demo seconds in until the demo or the given time ends), and with a screenshot interval it saves
// the same frames in every run, as screenshots/timedemo_<demo>_<demo ms>, for comparing builds or renderers.
// (The same frames of the game: the console, the HUD messages and the fps counter run on real time, so scripts that
// compare screenshots hide them: "showstats 0; consize 0; hidehudmsgs 1".)
VARP(timedemostep, 1, 16, 100); // whole milliseconds, so every run splits the demo into the same frames
static string timedemoname = "";
static int timedemostate = 0, timedemoseconds = 0, timedemoshotinterval = 0, timedemogame = 0, timedemonextshot = 0, timedemoframes = 0;
static double timedemostarted = 0;
static bool timedemomapdrawn = false; // a frame was drawn with a map loaded, while waiting to start the demo
enum { TD_OFF = 0, TD_WAITMAP, TD_LOADING, TD_PLAYING };
static const int TIMEDEMO_SETTLE = 2000; // game ms before frame times are recorded (map load, first packets)

void timedemo(char *name, int *seconds, int *shotinterval)
{
    if(!name[0]) { conoutf("usage: timedemo <demo> [game seconds] [screenshot every n game seconds]"); return; }
    if(multiplayer(NULL)) { conoutf("timedemo: not available while connected to a server"); return; }
    if(benchactive) { rpreport(); benchreport(); }
    copystring(timedemoname, name);
    timedemoseconds = max(0, *seconds);
    timedemoshotinterval = max(0, *shotinterval) * 1000;
    timedemogame = timedemoframes = timedemonextshot = 0;
    timedemomapdrawn = false;
    timedemostarted = hrclockms();
    benchraisewindow(); // in front well before recording starts
    timedemostate = TD_WAITMAP; // at startup (-e), the startup map has to be loaded first, or it replaces the demo
}
COMMAND(timedemo, "sii");

static void timedemostartdemo()
{
    timedemostate = TD_LOADING;
    timedemostarted = hrclockms();
    seedMT(1);
    defformatstring(cmd)("demo %s", escapestring(timedemoname));
    execute(cmd);
}

static void timedemostop(const char *why)
{
    double fps = benchactive && benchframes.length() && benchprev > benchstart ? 1000.0 * benchframes.length() / (benchprev - benchstart) : 0;
    if(benchactive) { rpreport(); benchreport(); }
    conoutf("timedemo: %s %s after %.1f demo seconds, %d frames (game time step %d ms)", timedemoname, why, timedemogame / 1000.0f, timedemoframes, timedemostep);
    timedemostate = TD_OFF;
    exechook(HOOK_SP, "onTimedemoEnd", "%d %d %.1f", timedemoframes, timedemogame / 1000, fps); // e.g. for scripts that quit then
}

// this frame's game time step while a timedemo runs (curtime otherwise)
static int timedemotime(int curtime)
{
    if(timedemostate == TD_OFF) return curtime;
    if(timedemostate == TD_WAITMAP)
    {
        extern int maploads;
        // The local server answers the connect at startup from a thread (polldeferredprocessing()), so the startup map
        // arrives a frame earlier or later from run to run. Two things make the runs the same from there:
        // - the game clock stands still until the demo is started (after its first step: nothing is updated at 0), so
        //   its value (which animations use) is the same in every run started the same way (e.g. "-e" at startup)
        // - the demo is started after a frame with the startup map was drawn: the first frames of the program aren't
        //   drawn, so the demo's first frame (which draws the minimap, at that game time) was drawn or not otherwise
        if(maploads && (timedemomapdrawn || minimized)) timedemostartdemo();
        else if(lastmillis) return 0;
        // fixed steps from here on: the demo's clock starts with the demo command
        return timedemostep;
    }
    extern int localdemomillis();
    int demomillis = localdemomillis();
    if(timedemostate == TD_LOADING)
    {
        if(demomillis >= 0)
        {
            // the demo started on the server: from here on every run is the same. Reset the random numbers, and the
            // physics steps' phase (which carries the earlier frames' timing over otherwise)
            timedemostate = TD_PLAYING;
            seedMT(1);
            extern int lastphysframe;
            lastphysframe = lastmillis;
        }
        else if(hrclockms() - timedemostarted > 15000) { timedemostop("didn't start"); return curtime; }
    }
    else if(timedemostate == TD_PLAYING)
    {
        if(demomillis < 0 || !watchingdemo) { timedemostop("ended"); return curtime; }
        timedemogame = demomillis;
        timedemoframes++;
        if(!benchactive && timedemogame >= TIMEDEMO_SETTLE)
        {
            closemenu(NULL); // a benchmark of the game, not of a menu (the main menu opens with the demo's map)
            benchbegin(1000000, false, 0);
        }
        if(timedemoseconds && timedemogame >= timedemoseconds * 1000) { timedemostop("finished"); return curtime; }
    }
    return timedemostep;
}

// a timedemo plays the same in every run: the mouse and keyboard don't steer it (in a demo, clicks and the mouse wheel
// switch the spectated player and view). Escape stops it.
static bool timedemoignores(const SDL_Event &e)
{
    if(timedemostate == TD_OFF) return false;
    switch(e.type)
    {
        case SDL_EVENT_KEY_DOWN:
            if(e.key.key == SDLK_ESCAPE) timedemostop("stopped");
            return true;
        case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
            return true;
        default:
            return false;
    }
}

// called after the frame is drawn, before it's presented: the frame for this game time is complete
static void timedemoframedrawn()
{
    extern int maploads;
    if(timedemostate == TD_WAITMAP && maploads) timedemomapdrawn = true;
    if(timedemostate != TD_PLAYING || !timedemoshotinterval || timedemogame < timedemonextshot) return;
    if(!timedemonextshot) timedemonextshot = timedemoshotinterval; // no screenshot of the loading frames
    else
    {
        defformatstring(name)("timedemo_%s_%06d", behindpath(timedemoname), timedemonextshot);
        screenshot(name);
        timedemonextshot += timedemoshotinterval;
    }
}

#ifndef WIN32
#include <sched.h>
#endif

// sleep for about "ms" milliseconds; sub-millisecond durations are honoured where the OS allows it
static void precisesleep(double ms)
{
    if(ms <= 0) return;
#ifdef WIN32
    if(ms >= 1) SDL_Delay(Uint32(ms));
#else
    struct timespec ts;
    ts.tv_sec = time_t(ms / 1000);
    ts.tv_nsec = long((ms - ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
#endif
}

// this frame's frame rate cap: maxfps, or maxfpsbackground while the window is in the background (except during a benchmark)
static int framecap(bool &background)
{
    background = false;
    if(!maxfpsbackground || benchactive || !screen) return maxfps;
    if(!minimized && (SDL_GetWindowFlags(screen) & SDL_WINDOW_INPUT_FOCUS)) return maxfps;
    background = true;
    return maxfps ? min(maxfps, maxfpsbackground) : maxfpsbackground;
}

// frame limiter: frames are started on a fixed grid of 1000/maxfps ms, measured with the high-resolution clock.
// The old limiter slept whole milliseconds with SDL_Delay and assumed the sleep was exact, so rounding and OS oversleep
// showed up as uneven frame times (and a lower than requested fps).
void limitfps()
{
    static double nextframe = -1, oversleep = 0;
    bool background;
    int fpscap = framecap(background);
    if(!fpscap) { nextframe = -1; return; }
    const double period = 1000.0 / fpscap;
    double now = hrclockms();
    if(nextframe < 0 || now - nextframe > period) nextframe = now; // first frame, or more than one frame late: resync instead of rushing to catch up
    else if(background)
    {
        // in the background exact timing doesn't matter, and the OS wakes background apps up later than usual: just sleep
        if(nextframe > now) precisesleep(nextframe - now);
    }
    else
    {
        // The OS wakes sleeping threads up late, by a varying amount (timer coalescing, scheduling), so the last part of the
        // wait is busy-waited. Its length adapts to the lateness actually observed: a slowly decaying maximum, plus margin,
        // never less than fpslimitspin and never more than 2 ms.
        double spin = clamp(max(fpslimitspin / 1000.0, oversleep * 1.25 + 0.05), 0.0, 2.0);
        double wakeup = nextframe - spin;
        if(wakeup > now)
        {
            precisesleep(wakeup - now);
            double late = hrclockms() - wakeup;
            oversleep = max(late, oversleep * 0.995);
        }
        while(hrclockms() < nextframe)
        {
#ifndef WIN32
            sched_yield();
#endif
        }
    }
    nextframe += period;
}

int lowfps = 30, highfps = 40;

void fpsrange(int *low, int *high)
{
    if(*low>*high || *low<1) return;
    lowfps = *low;
    highfps = *high;
}

COMMAND(fpsrange, "ii");

// how the world's level of detail changes this frame: it follows the measured frame rate, which differs between runs, so
// a timedemo keeps it at its most detailed (where any fast computer has it), and every run draws the same world
static float lodchange(float fps)
{
    if(timedemostate) return 1000.0f; // (any factor this large takes it straight to the most detailed)
    return fps<lowfps ? fps/lowfps : (fps>highfps ? fps/highfps : 1.0f);
}

int keyrepeatmask = 0, textinputmask = 0; // bunches of flags for the same functions
Uint64 textinputtime = 0;

void keyrepeat(bool on, int mask)
{
    if(on) keyrepeatmask |= mask;
    else keyrepeatmask &= ~mask;
}

void textinput(bool on, int mask)
{
    if(on)
    {
        if(!textinputmask)
        {
            SDL_StartTextInput(screen);
            textinputtime = SDL_GetTicks();
        }
        textinputmask |= mask;
    }
    else
    {
        textinputmask &= ~mask;
        if(!textinputmask) SDL_StopTextInput(screen);
    }
}

vector<SDL_Event> events;

void pushevent(const SDL_Event &e)
{
    events.add(e);
}

bool interceptkey(int sym)
{
    static int lastintercept = SDLK_UNKNOWN;
    int len = lastintercept == sym ? events.length() : 0;
    SDL_Event event;
    while(SDL_PollEvent(&event)) switch(event.type)
    {
        case SDL_EVENT_MOUSE_MOTION: break;
        default: pushevent(event); break;
    }
    lastintercept = sym;
    if(sym != SDLK_UNKNOWN) for(int i = len; i < events.length(); i++)
    {
        if(events[i].type == SDL_EVENT_KEY_DOWN && events[i].key.key == (SDL_Keycode)sym) { events.remove(i); return true; }
    }
    return false;
}

static void resetmousemotion()
{
    if (grabinput && !relativemouse && !(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN))
    {
        SDL_WarpMouseInWindow(screen, windoww / 2, windowh / 2); // window coordinates, not drawable pixels
    }
}

static inline bool skipmousemotion(SDL_Event &event)
{
    if(event.type != SDL_EVENT_MOUSE_MOTION) return true;
    if(!(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN))
    {
        if(event.motion.x == windoww / 2 && event.motion.y == windowh / 2) return true;  // ignore any motion events generated SDL_WarpMouse
    }
    return false;
}

static float mouseremainx = 0, mouseremainy = 0; // fractions of mouse motion not passed on yet (SDL3's deltas are floats)

static void addmousemotion(SDL_Event &event, int &dx, int &dy)
{
    float fx = event.motion.xrel + mouseremainx, fy = event.motion.yrel + mouseremainy;
    int ix = int(fx), iy = int(fy);
    mouseremainx = fx - ix; mouseremainy = fy - iy;
    dx += ix; dy += iy;
}

static void checkmousemotion(int &dx, int &dy)
{
    loopv(events)
    {
        SDL_Event &event = events[i];
        if(skipmousemotion(event))
        {
            if(i > 0) events.remove(0, i);
            return;
        }
        addmousemotion(event, dx, dy);
    }
    events.setsize(0);
    SDL_Event event;
    while(SDL_PollEvent(&event))
    {
        if(skipmousemotion(event))
        {
            events.add(event);
            return;
        }
        addmousemotion(event, dx, dy);
    }
}

int ignoremouse = 5, bootstrapentropy = 2;
#define EVENTDEBUG(x) x
//#define EVENTDEBUG(x)
EVENTDEBUG(VAR(debugevents, 0, 0, 2));

void checkinput()
{
    SDL_Event event;
    Uint32 lasttype = 0, lastbut = 0;
    int tdx=0,tdy=0;
    int focused = 0;
    while(events.length() || SDL_PollEvent(&event))
    {
        if(events.length()) event = events.remove(0);
        if(timedemoignores(event)) continue;

        EVENTDEBUG(int thres = 1; defformatstring(eb)("EVENT %d", event.type));

        if (focused && !(event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)) { if (grabinput != (focused > 0)) inputgrab(grabinput = focused > 0, shouldgrab); focused = 0; }

        switch(event.type)
        {
            case SDL_EVENT_QUIT:
                quit();
                break;

            case SDL_EVENT_KEY_DOWN:
                if(bootstrapentropy > 0 && (--bootstrapentropy & 2)) mapscreenshot(NULL, false, -1, 1000.0f / (1000 + rnd(400)), 0, 0);
            case SDL_EVENT_KEY_UP:
                EVENTDEBUG(thres = 2; concatstring(eb, event.type == SDL_EVENT_KEY_UP ? "(SDL_EVENT_KEY_UP)" : "(SDL_EVENT_KEY_DOWN)"));
                entropy_add_byte(event.key.key ^ totalmillis);
                EVENTDEBUG(concatformatstring(eb, " sym %d (%Xh), scancode %d (%Xh), down %d, repeat %d", event.key.key, event.key.key, event.key.scancode, event.key.scancode, event.key.down, event.key.repeat));
                if(event.key.key == SDLK_SCANCODE_MASK) event.key.key |= event.key.scancode; // workaround SDL 2.0.5 bug which returns sym == 40000000h for all dead keys
                if(!event.key.repeat || keyrepeatmask) keypress(event.key.key, event.key.scancode, event.key.down, (SDL_Keymod)event.key.mod);
                break;

            case SDL_EVENT_TEXT_INPUT:
                EVENTDEBUG(thres = 2; concatformatstring(eb, "(SDL_EVENT_TEXT_INPUT) %s %d %d", escapestring(event.text.text), textinputmask, int(SDL_NS_TO_MS(event.text.timestamp) - textinputtime)));
                if(textinputmask && int(SDL_NS_TO_MS(event.text.timestamp) - textinputtime) >= 5) // mute textinput for a few milliseconds, to avoid receiving the "t" that switched on the console
                {
                    processtextinput(event.text.text);
                }
                break;

            case SDL_EVENT_WINDOW_SHOWN: // window has been shown
                EVENTDEBUG(concatstring(eb, " SDL_EVENT_WINDOW_SHOWN"));
                break;

            case SDL_EVENT_WINDOW_HIDDEN: // window has been hidden
                EVENTDEBUG(concatstring(eb, " SDL_EVENT_WINDOW_HIDDEN"));
                break;

            case SDL_EVENT_WINDOW_EXPOSED: // window has been exposed and should be redrawn
                EVENTDEBUG(concatstring(eb, " SDL_EVENT_WINDOW_EXPOSED"));
                break;

            case SDL_EVENT_WINDOW_MOVED: // window has been moved to data1, data2
                EVENTDEBUG(concatformatstring(eb, " SDL_EVENT_WINDOW_MOVED x %d, y %d", event.window.data1, event.window.data2));
                if(!(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN)) centerwindow = false;
                break;

            case SDL_EVENT_WINDOW_RESIZED: // window has been resized to data1 x data2; this is event is always preceded by SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
                EVENTDEBUG(concatformatstring(eb, " SDL_EVENT_WINDOW_RESIZED %d x %d", event.window.data1, event.window.data2));
                if(!fullscreendesktop || !(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN))
                {
                    scr_w = clamp(event.window.data1, SCR_MINW, SCR_MAXW);
                    scr_h = clamp(event.window.data2, SCR_MINH, SCR_MAXH);
                }
                break;

            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: // window size has changed, either as a result of an API call or through the system or user changing the window size; this event is followed by SDL_EVENT_WINDOW_RESIZED if the size was changed by an external event, i.e. the user or the window manager
                updatescreensize();
                EVENTDEBUG(concatformatstring(eb, " SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED %dx%d", screenw, screenh));
                break;

            case SDL_EVENT_WINDOW_MINIMIZED: // window has been minimized
                EVENTDEBUG(concatstring(eb, " SDL_EVENT_WINDOW_MINIMIZED"));
                inputgrab(false);
                minimized = 1;
                break;

            case SDL_EVENT_WINDOW_MAXIMIZED: // window has been maximized
            case SDL_EVENT_WINDOW_RESTORED: // window has been restored to normal size and position
                EVENTDEBUG(concatstring(eb, event.type == SDL_EVENT_WINDOW_RESTORED ? "SDL_EVENT_WINDOW_RESTORED" : "SDL_EVENT_WINDOW_MAXIMIZED"));
                minimized = 0;
                break;

            case SDL_EVENT_WINDOW_MOUSE_ENTER: // window has gained mouse focus
                EVENTDEBUG(concatstring(eb, " SDL_EVENT_WINDOW_MOUSE_ENTER"));
                shouldgrab = false;
                focused = 1;
                break;

            case SDL_EVENT_WINDOW_MOUSE_LEAVE: // window has lost mouse focus
                EVENTDEBUG(concatstring(eb, " SDL_EVENT_WINDOW_MOUSE_LEAVE"));
                shouldgrab = false;
                focused = -1;
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED: // window has gained keyboard focus
                EVENTDEBUG(concatstring(eb, " SDL_EVENT_WINDOW_FOCUS_GAINED"));
                shouldgrab = true;
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST: // window has lost keyboard focus
                EVENTDEBUG(concatstring(eb, " SDL_EVENT_WINDOW_FOCUS_LOST"));
                shouldgrab = false;
                focused = -1;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                EVENTDEBUG(thres = 2; concatformatstring(eb, "(SDL_EVENT_MOUSE_MOTION) x %.1f, y %.1f, dx %.2f, dy %.2f", event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel));
                if(ignoremouse) { ignoremouse--; break; }
                if(grabinput && !skipmousemotion(event))
                {
                    int dx = 0, dy = 0;
                    addmousemotion(event, dx, dy);
                    entropy_add_byte(dx ^ (256 - dy));
                    checkmousemotion(dx, dy);
                    resetmousemotion();
                    tdx+=dx;tdy+=dy;
                }
                else if (shouldgrab) inputgrab(grabinput = true);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if(!grabinput) // click-to-grab
                {
                    EVENTDEBUG(concatformatstring(eb, "(SDL_EVENT_MOUSE_BUTTON_DOWN) button %d, down %d, clicks %d, x %.1f, y %.1f", event.button.button, event.button.down, event.button.clicks, event.button.x, event.button.y));
                    inputgrab(grabinput = true);
                    #ifdef WIN32
                    if(!fullscreen)
                    {
                        // If AC runs in windowed mode on Windows 10 and if you press WIN+D, then move mouse on desktop, press WIN+D again, click in the AC window,
                        // the mouse cursor remains visible and the mouse is not contrained to the AC window anymore. It is as if SDL2 does not properly give focus
                        // to the AC window and so our workaround is to explicitly trigger the focus by hiding and showing the window. This issue exists in
                        // SDL 2.0.12 and is fixed in 2.0.14 however we currently can not use 2.0.14 because it has problems with handling of ALT+TAB hotkey behavior.
                        // Please re-test and remove this workaround once a fixed version of SDL is available.
                        SDL_HideWindow(screen);
                        SDL_ShowWindow(screen);
                    }
                    #endif
                    break;
                }

            case SDL_EVENT_MOUSE_BUTTON_UP:
                EVENTDEBUG(concatformatstring(eb, "(SDL_MOUSEBUTTON%s) button %d, down %d, clicks %d, x %.1f, y %.1f", event.type == SDL_EVENT_MOUSE_BUTTON_UP ? "UP" : "DOWN", event.button.button, event.button.down, event.button.clicks, event.button.x, event.button.y));
                if(lasttype==event.type && lastbut==event.button.button) break;
                keypress(-(event.button.button > 3 ? (event.button.button + 4) : event.button.button), 0, event.button.down);
                lasttype = event.type;
                lastbut = event.button.button;
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                EVENTDEBUG(concatformatstring(eb, "(SDL_EVENT_MOUSE_WHEEL) x %d, y %d", event.wheel.integer_x, event.wheel.integer_y));
                if(event.wheel.integer_y)
                {
                    int key = event.wheel.integer_y > 0 ? SDL_AC_BUTTON_WHEELUP : SDL_AC_BUTTON_WHEELDOWN;
                    keypress(key, 0, true); // Emulate SDL1-style mouse wheel events by immediately "releasing" the wheel "button"
                    keypress(key, 0, false);
                }
                if(event.wheel.integer_x)
                {
                    int key = event.wheel.integer_x > 0 ? SDL_AC_BUTTON_WHEELRIGHT : SDL_AC_BUTTON_WHEELLEFT;
                    keypress(key, 0, true);
                    keypress(key, 0, false);
                }
                break;
        }
        EVENTDEBUG(if(debugevents >= thres) conoutf("%s", eb));
    }
    if(tdx || tdy)
    {
        entropy_add_byte(tdy + 5 * tdx);
        mousemove(tdx, tdy);
    }
    if (focused) { if (grabinput != (focused > 0)) inputgrab(grabinput = focused > 0, shouldgrab); focused = 0; }
}

VARF(gamespeed, 10, 100, 1000, if(multiplayer()) gamespeed = 100);
VARF(paused, 0, 0, 1, if(multiplayer()) paused = 0);

bool inmainloop = false;
static double clockrealbase = 0;
static int clockvirtbase = 0;
static void clockreset() { clockrealbase = hrclockms(); clockvirtbase = totalmillis; }
VARFP(clockerror, 990000, 1000000, 1010000, clockreset());
VARFP(clockfix, 0, 0, 1, clockreset());

const char *rndmapname()
{
    vector<char *> maps;
    listfiles("packages/maps/official", "cgz", maps);
    if (maps.length() > 0)
    {
        char *map = newstring(maps[rnd(maps.length())]);
        maps.deletearrays();
        return map;
    }
    else return "";
}

const int rndgamemode()
{
    static int potmodes[] = { 2, 5, 6, 9, 10, 13, 15 }; // DM CTF PF LSS OSOK HTF KTF | PARKOUR GEMA
    return potmodes[rnd(sizeof(potmodes)/sizeof(potmodes[0]))];
}

#define AUTOSTARTPATH "config" PATHDIVS "autostart" PATHDIVS

void autostartscripts(const char *prefix)
{
    static vector<char *> *files = NULL;
    if(!files)
    {  // first run: fetch file names and sort them
        files = new vector<char *>;
        listfiles(AUTOSTARTPATH, "cfg", *files, stringsort);
    }
    loopv(*files)
    {
        if(*prefix && strncmp((*files)[i], prefix, strlen(prefix))) continue;
        defformatstring(p)(AUTOSTARTPATH "%s.cfg", (*files)[i]);
        execfile(p);
        delstring(files->remove(i--));
    }
}

void createconfigtemplates(const char *templatezip)  // create customisable config files in homedir - only if missing
{
    vector<const char *> files;
    void *mz = zipmanualopen(openfile(templatezip, "rb"), files);
    if(mz)
    {
        loopv(files)
        {
            const char *filename = behindpath(files[i]); // only look for config files in the zip file, ignore all paths in the zip
            if(strlen(filename) > 4 && !strcmp(filename + strlen(filename) - 4, ".cfg"))
            {
                defformatstring(fname)("config" PATHDIVS "%s", files[i]);
                if(getfilesize(fname) <= 0) // config does not exist or is empty
                {
                    stream *zf = openfile(fname, "wb");
                    if(zf)
                    {
                        zipmanualread(mz, i, zf, MAXCFGFILESIZE); // fetch file content from zip, write to new config file
                        delete zf;
                        conoutf("created %s from template %s", fname, templatezip);
                    }
                }
            }
        }
        zipmanualclose(mz);
    }
    findfile(AUTOSTARTPATH "dummy", "w"); // create empty autostart directory, if it doesn't exist yet
}

void connectprotocol(char *protocolstring) // assaultcube://example.org[:28763][/][?[port=28763][&password=secret]]
{
    urlparse u;
    u.set(protocolstring);
    if(strcmp(u.scheme, "assaultcube") || !*u.domain) { conoutf("\f3bad commandline syntax (\"%s\")", protocolstring); return; }
    if(*u.userpassword) clientlogf(" ignoring user:password part of url (%s)", u.userpassword);
    if(*u.fragment) clientlogf(" ignoring fragment part of url (%s)", u.fragment);
    if(*u.path) clientlogf(" ignoring path part of url (%s)", u.path);
    int port = ATOI(u.port);
    const char *q = u.query, *passwd = NULL;
    while(q && *q)
    {
        if(*q == '&') q++;
        if(!strncmp(q, "port=", 5))
        {
            q += 5;
            port = ATOI(q);
            q = strchr(q, '&');
        }
        else if(!strncmp(q, "password=", 9))
        {
            q += 9;
            const char *e = strchr(q, '&');
            passwd = e ? newstring(q, e - q) : newstring(q);
            q = e;
        }
        else break;
    }
    defformatstring(cmd)("connect %s %d %s", u.domain, port, passwd ? passwd : "");
    DEBUGCODE(clientlogf("connectprotocol: %s", cmd));
    addsleep(5, cmd, true);
    DELSTRING(passwd);
}

int bakshowmenuonstartup = -1;
char *aaconnect = NULL;
void afterauth()
{
    // currently only for delayed connectprotocol
    if(aaconnect)
    {
        conoutf("\fUAfter Auth");
        if(bakshowmenuonstartup > -1)
        {
            alias("showmenuonstartup",bakshowmenuonstartup==1?"1":"0",false,false);
        }
    }
    if(aaconnect)
    {
        connectprotocol(aaconnect);
        DELSTRING(aaconnect);
    }
}

#ifdef THISISNOTDEFINED //WIN32
static char *parsecommandline(const char *src, vector<char *> &args)
{
    char *buf = new char[strlen(src) + 1], *dst = buf;
    for(;;)
    {
        while(isspace(*src)) src++;
        if(!*src) break;
        args.add(dst);
        for(bool quoted = false; *src && (quoted || !isspace(*src)); src++)
        {
            if(*src != '"') *dst++ = *src;
            else if(dst > buf && src[-1] == '\\') dst[-1] = '"';
            else quoted = !quoted;
        }
        *dst++ = '\0';
    }
    args.add(NULL);
    return buf;
}


int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int sw)
{
    vector<char *> args;
    char *buf = parsecommandline(GetCommandLine(), args);
    SDL_SetModuleHandle(hInst);
    int status = SDL_main(args.length()-1, args.getbuf());
    delete[] buf;
    exit(status);
    return 0;
}
#endif

void initclientlog()  // rotate old logfiles and create new one
{
    filerotate("clientlog", "txt", 5); // keep five old logfiles
    clientlogfile = openfile("clientlog.txt", "w");
    if(clientlogfile)
    {
        if(bootclientlog && bootclientlog->length()) clientlogfile->write(bootclientlog->getbuf(), bootclientlog->length());
    }
    else conoutf("could not create logfile \"%s\"", findfile("clientlog.txt", "w"));
    DELETEP(bootclientlog);
}

#ifdef _DEBUG
void sanitychecks()
{
    ASSERT((GMMASK__BOT ^ GMMASK__MP ^ GMMASK__TEAM ^ GMMASK__FFA ^ GMMASK__TEAMSPAWN ^ GMMASK__FFASPAWN ^ GMMASK__KARP) == GMMASK__ALL);
    float fa = 1 << LARGEST_FACTOR, fb = fa;
    while(fa != fa + fb) fb /= 2;
    ASSERT(2 * fb < NEARZERO);
}
#endif

#define DEFAULTPROFILEPATH "profile"

#ifdef __APPLE__
#include <unistd.h>
#include <pthread.h>
#include <sys/qos.h>
// When AssaultCube.app is started from Finder, the Dock or "open", the working directory is "/": switch to the game data
// bundled in Contents/Resources. Returns true when running from an app bundle.
static bool macbundlechdir()
{
    const char *base = SDL_GetBasePath(); // ".../AssaultCube.app/Contents/Resources/" inside a bundle, the executable's directory otherwise (owned by SDL)
    return base && strstr(base, ".app/Contents/Resources/") && !chdir(base);
}

// A bundle launch without --home/--init behaves like the old macOS launcher did ("--home=<user folder> --init"), with the
// per-user folder named after the release like in assaultcube.sh/.bat: ~/Library/Application Support/assaultcube/v1.4
static void macbundlehome(int argc, char **argv, bool &restoredinits)
{
    for(int i = 1; i < argc; i++) if(!strncmp(argv[i], "--home", 6) || !strncmp(argv[i], "--init", 6)) return;
    const char *userhome = getenv("HOME");
    if(!userhome || !*userhome) return;
    defformatstring(parent)("%s/Library/Application Support/assaultcube", userhome);
    createdir(parent);
    defformatstring(userdir)("%s/v1.4", parent);
    sethomedir(userdir);
    execfile((char *)"config/init.cfg");
    restoredinits = true;
}
#endif

int main(int argc, char **argv)
{
#ifdef __APPLE__
    const bool inmacbundle = macbundlechdir(); // before anything reads game data relative to the working directory
    // the main thread runs the game: as "user interactive", macOS schedules it ahead of background work and doesn't
    // coalesce its timers, which otherwise wake the frame limiter up late by a varying amount
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    sl_registerexithandler();
    DEBUGCODE(sanitychecks());
    extern struct servercommandline scl;
    #ifdef WIN32
    //atexit((void (__cdecl *)(void))_CrtDumpMemoryLeaks);
    #ifndef _DEBUG
    #ifndef __GNUC__
    __try {
    #endif
    #endif
    // VS2017+ does not support %n in format strings by default due to security concerns.
    // However, currently we are making use of this functionality and therefore it is being enabled below.
    // In the future we should get rid of this. See also https://docs.microsoft.com/en-us/previous-versions/hf4y5e3w(v=vs.140)?redirectedfrom=MSDN
    _set_printf_count_output(1);
    #endif

    bool dedicated = false;
    bool quitdirectly = false;
    char *initscript = NULL;
    char *initdemo = NULL;

    setlocale(LC_ALL, "POSIX");
    if(bootclientlog) cvecprintf(*bootclientlog, "######## start logging: %s\n", timestring(true));

    const char *initmap = rndmapname();
    int initmode = rndgamemode();
    loopi(NUMGUNS) crosshairnames[i] = gunnames[i] = guns[i].modelname;
    crosshairnames[GUN_AKIMBO] = gunnames[GUN_AKIMBO] = "akimbo";
    crosshairnames[CROSSHAIR_DEFAULT] = "default";
    crosshairnames[CROSSHAIR_TEAMMATE] = "teammate";
    crosshairnames[CROSSHAIR_SCOPE] = "scope";
    crosshairnames[CROSSHAIR_EDIT] = "edit";
    crosshairnames[CROSSHAIR_NUM] = gunnames[NUMGUNS] = "";

    pushscontext(IEXC_CFG);
    persistidents = false;

    #define initlog(s) clientlogf("init: " s)

    if(*stream_capabilities()) clientlogf("info: %s", stream_capabilities());

    bool waitforauth = false;
    initing = INIT_RESET;
#ifdef __APPLE__
    if(inmacbundle) macbundlehome(argc, argv, restoredinits);
#endif
    for(int i = 1; i<argc; i++)
    {
        clientlogf("parsing commandline argument %d: \"%s\"", i, argv[i]);
        // server: ufimNFTLAckyxpDWrXBKIoOnPMVCQ
        if(!scl.checkarg(argv[i]))
        {
            char *a = &argv[i][2];
            if(argv[i][0]=='-') switch(argv[i][1])
            {
                case '-':
                    if(!strcmp(argv[i], "--home"))
                    {
                        sethomedir(DEFAULTPROFILEPATH);
                    }
                    else if(!strncmp(argv[i], "--home=", 7))
                    {
                        sethomedir(&argv[i][7]);
                    }
                    else if(!strncmp(argv[i], "--mod=", 6))
                    {
                        addpackagedir(&argv[i][6]);
                    }
                    else if(!strcmp(argv[i], "--init"))
                    {
                        if(!havehomedir()) sethomedir(DEFAULTPROFILEPATH);
                        execfile((char *)"config/init.cfg");
                        restoredinits = true;
                    }
                    else if(!strncmp(argv[i], "--init=", 7))
                    {
                        if(!havehomedir()) sethomedir(DEFAULTPROFILEPATH);
                        execfile(&argv[i][7]);
                        restoredinits = true;
                    }
                    else if(!strcmp(argv[i], "--version"))
                    {
                        printf("%.3f\n", AC_VERSION/1000.0f);
                        quitdirectly = true;
                    }
                    else if(!strcmp(argv[i], "--protocol"))
                    {
                        printf("%d\n", PROTOCOL_VERSION);
                        quitdirectly = true;
                    }
                    else if(!strncmp(argv[i], "--loadmap=", 10))
                    {
                        initmap = &argv[i][10];
                    }
                    else if(!strncmp(argv[i], "--loadmode=", 11))
                    {
                        initmode = atoi(&argv[i][11]);
                    }
                    else if(!strcmp(argv[i], "--gpu")) forcerenderer = 1; // the SDL GPU renderer (Metal, Vulkan) for this run
                    else if(!strcmp(argv[i], "--gl")) forcerenderer = 0;  // OpenGL for this run
                    else if(!strncmp(argv[i], "--loaddemo=", 11))
                    {
                        initdemo = &argv[i][11];
                    }
                    else conoutf("\f3unknown commandline switch: %s", argv[i]);
                    break;
                case 'd': dedicated = true; break;
                case 't': fullscreen = atoi(a); break;
                case 'w': scr_w  = atoi(a); break;
                case 'h': scr_h  = atoi(a); break;
                case 'z': depthbits = atoi(a); break;
                case 's': stencilbits = atoi(&argv[i][2]); break;
                case 'a': fsaa = atoi(a); break;
                case 'v': vsync = atoi(a); break;
                case 'e': initscript = &argv[i][2]; break;
                default:  conoutf("\f3unknown commandline option: -%c", argv[i][1]);
            }
            else if(!strncmp(argv[i], "assaultcube://", 14)) // browser direct connection
            {
                waitforauth = true;
                conoutf("\fUwill \f2connect\fU when authenticated."); // individual notification - usually first in console, so off screen when menu pops up
                aaconnect = newstring(argv[i]);//triggers connectprotocol(argv[i]); during afterauth()
            }
            else conoutf("\f3unknown commandline argument: %c", argv[i][0]);
        }
    }
    if(!havehomedir()) sethomedir(DEFAULTPROFILEPATH);
    entropy_init(time(NULL) + (uint)(size_t)&initscript + (uint)(size_t)entropy_init);
    initclientlog();
    if(quitdirectly) return EXIT_SUCCESS;
    if(waitforauth)
    {
        hudoutf("\f5.. \f2waiting \f4for \fUauth\f5entication .."); // general notification - more prominent to show things are about to happen
    }

    createconfigtemplates("config" PATHDIVS "configtemplates.zip");

    initing = NOT_INITING;

    #define SDLVERSIONSTRING  STRINGIFY(SDL_MAJOR_VERSION) "." STRINGIFY(SDL_MINOR_VERSION) "." STRINGIFY(SDL_MICRO_VERSION)
    initlog("sdl (" SDLVERSIONSTRING ")");
    if(!SDL_Init(SDL_INIT_VIDEO)) fatal("Unable to initialize SDL");
    int sdlver = SDL_GetVersion();
    if(SDL_VERSION != sdlver)
        clientlogf("SDL: compiled version " SDLVERSIONSTRING ", linked version %d.%d.%d", SDL_VERSIONNUM_MAJOR(sdlver), SDL_VERSIONNUM_MINOR(sdlver), SDL_VERSIONNUM_MICRO(sdlver));

#if 0
    if(highprocesspriority) setprocesspriority(true);
#endif

    if (!dedicated) initlog("net (" STRINGIFY(ENET_VERSION_MAJOR) "." STRINGIFY(ENET_VERSION_MINOR) "." STRINGIFY(ENET_VERSION_PATCH) ")");
    if(enet_initialize()<0) fatal("Unable to initialise network module");

    if (!dedicated) initclient();

    initserver(dedicated);  // never returns if dedicated

    initlog("world (" STRINGIFY(AC_VERSION) ")");
    empty_world(7, true);

    initlog("video: sdl");
    if(!SDL_InitSubSystem(SDL_INIT_VIDEO)) fatal("Unable to initialize SDL Video");
    if(SDL_GetCurrentVideoDriver()) conoutf("video driver: %s", SDL_GetCurrentVideoDriver()); // e.g. x11, wayland, cocoa

    initlog("video: mode");
    int wantrenderer = forcerenderer >= 0 ? forcerenderer : renderer;
#ifdef __APPLE__
    usegpu = wantrenderer != 0;
#else
    usegpu = wantrenderer == 1;
#endif
    if(usegpu && forcerenderer < 0)
    {
        string lock;
        copystring(lock, findfile(path("config/gpustart.lock", true), "w"));
        if(fileexists(lock, "r"))
        {
            conoutf("\f3the Metal/Vulkan renderer didn't get going last time: using OpenGL (\"renderer 1\" or the video menu to try it again)");
            usegpu = false;
            renderer = 0; // (saved)
            delfile(lock);
        }
        else if(FILE *f = fopen(lock, "w")) // (quietly: the game's own file writes are logged to the console)
        {
            fputs("the GPU renderer is starting\n", f);
            fclose(f);
            copystring(gpulockfile, lock);
        }
    }
    int useddepthbits = 0, usedfsaa = 0;
    setupscreen(useddepthbits, usedfsaa);

    initlog("video: misc");

#ifndef __APPLE__
    extern SDL_Surface *loadimage(const char *filename);
    SDL_Surface *icon = loadimage("packages/misc/icon.png");
    SDL_SetWindowIcon(screen, icon);
#endif

#ifdef __APPLE__
    applymacmousewarp();
#endif

    SDL_HideCursor();

    initlog("gl");
    gl_checkextensions();
    gl_init(screenw, screenh, useddepthbits, usedfsaa);

    notexture = noworldtexture = textureload("packages/misc/notexture.jpg");
    if(!notexture) fatal("could not find core textures (hint: run AssaultCube from the parent of the bin directory)");

    initlog("console");
    updateigraphs();
    autostartscripts("_veryfirst_");
    // Main font file, all other font files execute from here.
    if(!execfile("config/font.cfg")) fatal("cannot find default font definitions");
    // Check these 2 standard fonts have been executed.
    if(!setfont("mono")) fatal("no mono font specified");
    if(!setfont("default")) fatal("no default font specified");

    loadingscreen();

    particleinit();

    initlog("sound");
    audiomgr.initsound();

    initlog("cfg");
    extern void *scoremenu, *servmenu, *searchmenu, *serverinfomenu, *kickmenu, *banmenu, *forceteammenu, *giveadminmenu, *docmenu, *applymenu, *downloaddemomenu;
    scoremenu = addmenu("score", "columns", false, renderscores, NULL, false, true);
    servmenu = addmenu("server", NULL, true, refreshservers, serverskey);
    searchmenu = addmenu("search", NULL, true, refreshservers, serverskey);
    serverinfomenu = addmenu("serverinfo", "extended server information (F5: refresh)", true, refreshservers, serverinfokey);
    kickmenu = addmenu("kick player", NULL, true, refreshsopmenu);
    banmenu = addmenu("ban player", NULL, true, refreshsopmenu);
    forceteammenu = addmenu("force team", NULL, true, refreshsopmenu);
    giveadminmenu = addmenu("give admin", NULL, true, refreshsopmenu);
    docmenu = addmenu("reference", NULL, true, renderdocmenu);
    applymenu = addmenu("apply", "apply changes now?", true, refreshapplymenu);
    downloaddemomenu = addmenu("Download demo", NULL, true);

    exec("config/scontext.cfg");
    exec("config/keymap.cfg");
    execfile("config/mapmodelattributes.cfg");
    exec("config/menus.cfg");
    exec("config/scripts.cfg");
    exec("config/prefabs.cfg");
    registerdefaultsounds();
    exec("config/sounds.cfg");
    exec("config/securemaps.cfg");
    exec("config/admin.cfg");
    execfile("config/servers.cfg");
    execfile("config/pcksources.cfg");
    execfile("config/authkeys.cfg");
    execfile("config/authprivate.cfg");
    loadcertdir();
    loadhistory();
    setupautodownload();
    int xmn = loadallxmaps();
    if(xmn) conoutf("loaded %d xmaps", xmn);
    persistidents = true;

    static char resdata[] = { 112, 97, 99, 107, 97, 103, 101, 115, 47, 116, 101, 120, 116, 117, 114, 101, 115, 47, 107, 117, 114, 116, 47, 107, 108, 105, 116, 101, 50, 46, 106, 112, 103, 0 };
    stream *f = opengzfile(resdata, "rb");
    if(f)
    {
        int n = f->getlil<int>();
        loopi(n)
        {
            string s;
            f->read(s, sizeof(string));
            enet_uint32 c = f->getlil<enet_uint32>();
            setresdata(s, c);
        }
        delete f;
    }

    autostartscripts("_beforesaved_");
    initing = INIT_LOAD;
    if(!execfile("config/saved.cfg"))
    {
        exec("config/defaults.cfg");
        bootstrapentropy += 5 + rnd(7);
    }
    autostartscripts("_aftersaved_");
    exechook(HOOK_SP_MP, "afterinit", "");
    if(waitforauth)
    {
        bakshowmenuonstartup = !strcmp(getalias("showmenuonstartup"),"1")?1:0;
        alias("showmenuonstartup","0");
    }
    autostartscripts("");    // all remaining scripts
    execfile("config/autoexec.cfg");
    exechook(HOOK_SP_MP, "autoexec", "");




    initing = NOT_INITING;
    uniformtexres = !hirestextures;

    initlog("models");
    preload_playermodels();
    preload_hudguns();
    preload_entmodels();

    initlog("docs");
    persistidents = false;
    execfile("config/docs.cfg");
    persistidents = true;

    initlog("localconnect");
    extern string clientmap;
    extern int gamemode;

    if(initdemo)
    {
        gamemode = -1;
        copystring(clientmap, initdemo);
    }else{
        gamemode = initmode;
        copystring(clientmap, initmap); // ac_complex for 1.0, ac_shine for 1.1, .. rndmap for 1.2+
    }

    localconnect();

    if(initscript) execute(initscript);

    initlog("mainloop");

    inputgrab(grabinput = true);

    inmainloop = true;
    static bool onceauthed = true;
#ifdef _DEBUG
    int lastflush = 0;
#endif
    if(scl.logident[0])
    { // "-Nxxx" sets the number of client log lines to xxx (after init)
        extern int clientloglinesremaining;
        clientloglinesremaining = atoi(scl.logident);  // dual-use for scl.logident
    }
    for(;;)
    {
        static int frames = 0;
        static float fps = 10.0f;
        limitfps(); // wait for this frame's start first, then read the clock: the frame time is measured, not assumed
        double framestart = hrclockms();
        benchframe(framestart);
        double realmillis = framestart - clockrealbase;
        if(clockfix) realmillis *= double(clockerror)/1000000;
        int millis = int(realmillis) + clockvirtbase;
        float millisfrac = float(realmillis - floor(realmillis)); // sub-millisecond part, used for render interpolation
        if(millis<totalmillis) { millis = totalmillis; millisfrac = 0; }
        int elapsed = millis-totalmillis;
        entropy_add_byte(elapsed);
        if(multiplayer(NULL))
        {
            if(ispaused) curtime = 0;
            else curtime = elapsed;
        }
        else
        {
            static int timeerr = 0;
            int scaledtime = elapsed*gamespeed + timeerr;
            curtime = scaledtime/100;
            timeerr = scaledtime%100;
            curtime = timedemotime(curtime);
            if(!watchingdemo) skipmillis = 0;
            if(skipmillis && watchingdemo)
            {
                int skipmillisnow = skipmillis > 5000 ? skipmillis - 5000 : min(1000, skipmillis);
                curtime += skipmillisnow;
                skipmillis -= skipmillisnow;
            }
            if(paused) curtime = 0;
        }
        lastmillis += curtime;
        totalmillis = millis;
        // game time only has the real clock's sub-millisecond phase when it advances 1:1 with real time
        extern float lastmillisfrac;
        lastmillisfrac = curtime == elapsed && !timedemostate && !(multiplayer(NULL) ? ispaused : paused) ? millisfrac : 0;

        resolvedepth(); // worldpos from the previous frame's depth read, before anything (input, shooting) can use it
        rpmark(RP_DEPTHRESOLVE);

        checkinput();

        if(lastmillis) updateworld(curtime, lastmillis);
        rpmark(RP_UPDATE);

        if(needsautoscreenshot) showscores(true);

        serverslice(0);

        if(elapsed) fps = (1000.0f/elapsed+fps*10)/11; // avoid DIV-by-0
        frames++;

        audiomgr.updateaudio();
        rpmark(RP_OTHER);

        computeraytable(camera1->o.x, camera1->o.y, dynfov());
        rpmark(RP_OCCLUSION);
        if(frames>3 && !minimized)
        {
            gl_drawframe(screenw, screenh, lodchange(fps), fps, elapsed);
            timedemoframedrawn();
            if(frames>4) gfx->present();
            if(frames>300 && gpulockfile[0]) cleargpulock(); // the GPU backend got going
            // "gpusync 1": wait for the GPU to finish the frame, so the driver can't queue frames ahead. Costs throughput
            // (CPU and GPU no longer overlap) but keeps input-to-display latency at one frame.
            extern int gpusync;
            if(gpusync) gfx->finish();
            rpmark(RP_SWAP);
        }

        if(needsautoscreenshot)
        {
            showscores(true);
            // draw again after swapping buffers, to make sure menu is captured
            // in the screenshot regardless of which frame buffer is current
            if(!minimized)
            {
                gl_drawframe(screenw, screenh, lodchange(fps), fps, elapsed);
            }
            addsleep(0, "screenshot");
            needsautoscreenshot = false;
        }
#ifdef _DEBUG
        if(millis>lastflush+60000) { fflush(stdout); lastflush = millis; }
#endif
        pollautodownloadresponse();
        if(onceauthed && sk)
        {
            onceauthed = false;
            afterauth();
        }
    }

    quit();
    return EXIT_SUCCESS;

#if defined(WIN32) && !defined(_DEBUG) && !defined(__GNUC__)
    } __except(stackdumper(0, GetExceptionInformation()), EXCEPTION_CONTINUE_SEARCH) { return 0; }
#endif
}

VAR(version, 1, AC_VERSION, 0);
VAR(protocol, 1, PROTOCOL_VERSION, 0);
