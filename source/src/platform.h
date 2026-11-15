#ifdef __GNUC__
    #ifdef _FORTIFY_SOURCE
        #undef _FORTIFY_SOURCE
    #endif
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <math.h>
#include <locale.h>
#include <stdint.h>
#ifdef __GNUC__
    #include <new>
    #include <signal.h>
#else
    #include <new.h>
#endif

#include <zlib.h>
#include <enet/enet.h>

#ifdef WIN32
    #define WIN32_LEAN_AND_MEAN
    #include "windows.h"
    #ifndef _WINDOWS
      #define _WINDOWS
    #endif
    #include <tlhelp32.h>
    #ifndef __GNUC__
        #include <Dbghelp.h>
    #endif
    #define ZLIB_DLL
    #define NO_POSIX_R
#endif

#ifndef STANDALONE
    #include <SDL3/SDL.h>
    #if !defined(WIN32) && !defined(__APPLE__)
        #define AC_X11_GRAB         // X11 pointer-grab workaround (main.cpp, sdl_xgrab_bug); SDL2's SDL_syswm.h used to include Xlib
        #include <X11/Xlib.h>
    #endif

    #define GL_GLEXT_LEGACY
    #define __glext_h__
    #define NO_SDL_GLEXT
    #include <SDL3/SDL_opengl.h>
    #undef __glext_h__

    #include "GL/glext.h"

    #ifdef __APPLE__
        #ifdef AC_OPENAL_SOFT
            #include "AL/al.h"              // OpenAL Soft (bundled with the app, see source/macos/build-deps.sh)
            #include "AL/alc.h"
        #else
            #include "OpenAL/al.h"          // Apple's (deprecated) OpenAL.framework
            #include "OpenAL/alc.h"
        #endif
        #if defined(__has_include) && __has_include(<vorbis/vorbisfile.h>)
            #include <vorbis/vorbisfile.h>  // libvorbis built from source, Homebrew, MacPorts
        #else
            #include "Vorbis/vorbisfile.h"  // legacy Vorbis.framework (Xcode project)
        #endif
        #define MOD_KEYS_CTRL (SDL_KMOD_LGUI|SDL_KMOD_RGUI)
    #else
        #include "AL/al.h"
        #include "AL/alc.h"
        #include "vorbis/vorbisfile.h"
        #define MOD_KEYS_CTRL (SDL_KMOD_LCTRL|SDL_KMOD_RCTRL)
    #endif

    #include <setjmp.h>
#endif

#if defined(WIN32) || defined(__APPLE__) || !defined(STANDALONE) || defined(AC_FORCE_SDL_THREADS)
    #define AC_USE_SDL_THREADS
#endif

