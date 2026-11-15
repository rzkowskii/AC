// stbimage.cpp: PNG and JPEG decoding for textures, with stb_image configured the way SDL2_image's STB backend
// configures it (SDL2_image 2.8.12, src/IMG_stb.c), the decoder the macOS app has always shipped with.

#include <SDL.h> // (not cube.h: its macros would collide with stb_image's code)

typedef unsigned char uchar;
static inline int clampint(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

#define STB_IMAGE_STATIC
#define STBI_NO_THREAD_LOCALS
#define STBI_FAILURE_USERMSG
#if defined(__ARM_NEON)
#define STBI_NEON
#endif
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT SDL_assert
#define STBI_MALLOC SDL_malloc   // so that a surface wrapping the pixels can free them (see loadimage() in texture.cpp)
#define STBI_REALLOC SDL_realloc
#define STBI_FREE SDL_free
#define IMG_SetError SDL_SetError // (SDL_image's copy reports errors through SDL)
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct stbmemory { const uchar *data; int len, pos; };

static int stbread(void *user, char *data, int size)
{
    stbmemory &m = *(stbmemory *)user;
    int n = clampint(m.len - m.pos, 0, size);
    memcpy(data, m.data + m.pos, n);
    m.pos += n;
    return n;
}

static void stbskip(void *user, int n)
{
    stbmemory &m = *(stbmemory *)user;
    m.pos = clampint(m.pos + n, 0, m.len);
}

static int stbeof(void *user)
{
    stbmemory &m = *(stbmemory *)user;
    return m.pos >= m.len;
}

// decodes a PNG or JPEG file in memory into tightly packed 8-bit pixels with 1 (grey), 2 (grey, alpha), 3 (RGB) or
// 4 (RGBA) channels, allocated with SDL_malloc; NULL if the data can't be decoded
uchar *stbdecode(const uchar *data, int len, int &w, int &h, int &channels)
{
    stbmemory m = { data, len, 0 };
    stbi_io_callbacks cb = { stbread, stbskip, stbeof };
    w = h = channels = 0;
    return stbi_load_from_callbacks(&cb, &m, &w, &h, &channels, STBI_default);
}

const char *stberror() { return SDL_GetError(); }
