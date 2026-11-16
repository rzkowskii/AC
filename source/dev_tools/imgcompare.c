// imgcompare: compares two screenshots (PNG, BMP, JPEG) pixel by pixel, for checking that a change to the renderer
// leaves the picture alone (see timedemo-compare.sh).
//
//   cc -O2 -I../src -o imgcompare imgcompare.c $(pkg-config --cflags --libs sdl3) -lm
//   (macOS with Homebrew: cc -O2 -I../src -I/opt/homebrew/include -o imgcompare imgcompare.c -L/opt/homebrew/lib -lSDL3)
//   imgcompare a.png b.png [tolerance] [minimum % within tolerance] [diff.png]
//
// It uses the game's copy of stb_image (SDL_image's, which needs SDL's headers), so images decode as in the game.
//
// Prints the share of identical pixels, the share within the tolerance (largest channel difference, default 8 of 255),
// how many pixels those leave, the mean and largest difference. Exits with 1 when the share within the tolerance is
// below the minimum (if given), 2 when the images can't be read or differ in size. With a fifth argument it writes a
// difference image: grey where the pixels match within the tolerance, red where they don't.

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// configured like the game's stbimage.cpp (SDL_image's copy loads from memory only)
#define IMG_SetError SDL_SetError
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#include "stb_image.h"

struct membuf { const unsigned char *data; size_t len, pos; };
static int memread(void *user, char *out, int size)
{
    struct membuf *m = (struct membuf *)user;
    size_t n = m->len - m->pos < (size_t)size ? m->len - m->pos : (size_t)size;
    memcpy(out, m->data + m->pos, n);
    m->pos += n;
    return (int)n;
}
static void memskip(void *user, int n) { struct membuf *m = (struct membuf *)user; m->pos = n < 0 && (size_t)-n > m->pos ? 0 : (m->pos + n > m->len ? m->len : m->pos + n); }
static int memeof(void *user) { struct membuf *m = (struct membuf *)user; return m->pos >= m->len; }

static unsigned char *loadimage(const char *name, int *w, int *h)
{
    size_t len = 0;
    void *data = SDL_LoadFile(name, &len);
    if(!data) return NULL;
    struct membuf m = { (const unsigned char *)data, len, 0 };
    stbi_io_callbacks cb = { memread, memskip, memeof };
    int n;
    unsigned char *pixels = stbi_load_from_callbacks(&cb, &m, w, h, &n, 3);
    SDL_free(data);
    return pixels;
}

// minimal uncompressed PNG writer (stored deflate blocks), so the tool needs nothing but stb_image
static unsigned int crctable[256];
static void makecrctable(void)
{
    for(unsigned int n = 0; n < 256; n++)
    {
        unsigned int c = n;
        for(int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        crctable[n] = c;
    }
}
static unsigned int crc(unsigned int c, const unsigned char *buf, size_t len)
{
    c ^= 0xFFFFFFFFU;
    for(size_t i = 0; i < len; i++) c = crctable[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFU;
}
static void put32(unsigned char *p, unsigned int v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const unsigned char *data, unsigned int len)
{
    unsigned char hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if(len) fwrite(data, 1, len, f);
    unsigned int c = crc(crc(0, (const unsigned char *)type, 4), data, len);
    unsigned char t[4];
    put32(t, c);
    fwrite(t, 1, 4, f);
}
static int writepng(const char *name, const unsigned char *rgb, int w, int h)
{
    FILE *f = fopen(name, "wb");
    if(!f) return 0;
    makecrctable();
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    put32(ihdr, w); put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0; // 8 bit RGB
    chunk(f, "IHDR", ihdr, 13);
    size_t rowlen = (size_t)w * 3 + 1, rawlen = rowlen * h, blocks = (rawlen + 65534) / 65535;
    size_t zlen = 2 + rawlen + blocks * 5 + 4;
    unsigned char *z = malloc(zlen), *raw = malloc(rawlen);
    if(!z || !raw) { fclose(f); free(z); free(raw); return 0; }
    for(int y = 0; y < h; y++) { raw[y * rowlen] = 0; memcpy(raw + y * rowlen + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t o = 0, done = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    unsigned int a = 1, b = 0;
    while(done < rawlen)
    {
        size_t n = rawlen - done > 65535 ? 65535 : rawlen - done;
        z[o++] = done + n == rawlen ? 1 : 0;
        z[o++] = n & 0xFF; z[o++] = n >> 8; z[o++] = ~n & 0xFF; z[o++] = (~n >> 8) & 0xFF;
        memcpy(z + o, raw + done, n);
        for(size_t i = 0; i < n; i++) { a = (a + raw[done + i]) % 65521; b = (b + a) % 65521; }
        o += n; done += n;
    }
    put32(z + o, (b << 16) | a); o += 4;
    chunk(f, "IDAT", z, (unsigned int)o);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z); free(raw);
    return 1;
}

int main(int argc, char **argv)
{
    if(argc < 3)
    {
        fprintf(stderr, "usage: imgcompare a.png b.png [tolerance (default 8)] [minimum %% within tolerance] [diff.png]\n");
        return 2;
    }
    int tolerance = argc > 3 ? atoi(argv[3]) : 8;
    double minimum = argc > 4 ? atof(argv[4]) : -1;
    int wa, ha, wb, hb;
    unsigned char *a = loadimage(argv[1], &wa, &ha), *b = loadimage(argv[2], &wb, &hb);
    if(!a || !b) { fprintf(stderr, "imgcompare: can't read %s\n", !a ? argv[1] : argv[2]); return 2; }
    if(wa != wb || ha != hb) { fprintf(stderr, "imgcompare: sizes differ (%dx%d, %dx%d)\n", wa, ha, wb, hb); return 2; }
    size_t pixels = (size_t)wa * ha, same = 0, close = 0;
    double sum = 0;
    int largest = 0;
    unsigned char *diff = argc > 5 ? malloc(pixels * 3) : NULL;
    for(size_t i = 0; i < pixels; i++)
    {
        int d = 0;
        for(int c = 0; c < 3; c++) { int e = abs(a[i * 3 + c] - b[i * 3 + c]); if(e > d) d = e; }
        sum += d;
        if(d > largest) largest = d;
        if(!d) same++;
        if(d <= tolerance) close++;
        if(diff)
        {
            unsigned char g = (a[i * 3] + a[i * 3 + 1] + a[i * 3 + 2]) / 6;
            diff[i * 3] = d <= tolerance ? g : 255;
            diff[i * 3 + 1] = diff[i * 3 + 2] = d <= tolerance ? g : 0;
        }
    }
    double within = 100.0 * close / pixels;
    // (percentages are rounded down, so only an exact match prints as 100.00%; the pixel counts say how far off it is)
    printf("%.2f%% identical, %.2f%% within %d (%zu pixels differ, %zu by more than %d), "
           "mean difference %.3f, largest %d (%dx%d)\n",
           ((unsigned long long)same * 10000 / pixels) / 100.0, ((unsigned long long)close * 10000 / pixels) / 100.0,
           tolerance, pixels - same, pixels - close, tolerance, sum / pixels, largest, wa, ha);
    if(diff && !writepng(argv[5], diff, wa, ha)) fprintf(stderr, "imgcompare: can't write %s\n", argv[5]);
    free(diff);
    stbi_image_free(a);
    stbi_image_free(b);
    return minimum >= 0 && within < minimum ? 1 : 0;
}
