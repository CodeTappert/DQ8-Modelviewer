#include <dq8/io/tim2.h>

#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* --- GS swizzle (DCExtractor TM2 spec) --- */
static const uint8_t INTERLACE[8] = { 0x00, 0x10, 0x02, 0x12, 0x11, 0x01, 0x13, 0x03 };
static const int MATRIX_Y[4] = { 0, 1, -1, 0 };
static const int TILE_X[2] = { 4, -4 };

static void unswizzle(const uint8_t *in, uint8_t *out, int w, int h)
{
    size_t sz = (size_t)w * h;
    for (int y = 0; y < h; y++) {
        int odd = y & 1;
        int yoff = y * w;
        if (odd) {
            yoff -= w;
        }
        int yidx = y + MATRIX_Y[y % 4];
        for (int x = 0; x < w; x++) {
            int ii = (x >> 2) & 3;
            if (odd) {
                ii += 4;
            }
            int xidx = x + ((y >> 2) & 1) * TILE_X[(x >> 2) & 1];
            int bidx = INTERLACE[ii] + ((x << 2) & 0x0F) + ((x >> 4) << 5) + yoff;
            int pidx = yidx * w + xidx;
            if (bidx < 0 || pidx < 0 || (size_t)pidx >= sz || (size_t)bidx >= sz) {
                continue;
            }
            out[pidx] = in[bidx];
        }
    }
}

/* --- palette --- */
static uint32_t abgr1555(const uint8_t *b)
{
    uint32_t v = rd16(b);
    uint32_t r = (v & 0x1F) * 255 / 31;
    uint32_t g = ((v >> 5) & 0x1F) * 255 / 31;
    uint32_t bl = ((v >> 10) & 0x1F) * 255 / 31;
    uint32_t a = (v & 0x8000) ? 0xFF : 0;
    return (a << 24) | (bl << 16) | (g << 8) | r;
}

static uint32_t rgba32(const uint8_t *b)
{
    /* R,G,B,A bytes -> 0xAABBGGRR. Level-5 stores alpha at half intensity. */
    uint32_t a = (uint32_t)b[3] * 2u;
    if (a > 255) {
        a = 255;
    }
    return (a << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
}

static uint32_t rgb24(const uint8_t *b)
{
    return 0xFF000000u | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
}

static uint32_t *decode_palette(const uint8_t *pal, int entries, int color_size,
                                int linear)
{
    uint32_t *c = calloc((size_t)(entries > 0 ? entries : 1), 4);
    if (!c) {
        return NULL;
    }
    for (int i = 0; i < entries; i++) {
        const uint8_t *p = pal + (size_t)i * color_size;
        switch (color_size) {
        case 2: c[i] = abgr1555(p); break;
        case 3: c[i] = rgb24(p); break;
        case 4: c[i] = rgba32(p); break;
        default: c[i] = 0xFF000000u; break;
        }
    }
    /* de-interlace (blocks of 8, 2 stripes, 2 blocks) */
    if (!linear && entries >= 32) {
        uint32_t *lin = calloc((size_t)entries, 4);
        int parts = entries / 32;
        if (lin) {
            int d = 0;
            for (int part = 0; part < parts; part++)
                for (int blk = 0; blk < 2; blk++)
                    for (int st = 0; st < 2; st++)
                        for (int k = 0; k < 8; k++)
                            lin[d++] = c[part * 32 + blk * 8 + st * 16 + k];
            free(c);
            c = lin;
        }
    }
    return c;
}

Dq8Tim2 *dq8_tim2_open(const uint8_t *data, size_t size)
{
    if (!data || size < 0x40 || memcmp(data, "TIM2", 4) != 0) {
        return NULL;
    }
    const uint8_t *t = data;
    uint32_t pal_size = rd32(t + 0x14);
    uint32_t img_size = rd32(t + 0x18);
    uint16_t hdr_size = rd16(t + 0x1c);
    uint16_t entries = rd16(t + 0x1e);
    uint8_t clut_fmt = t[0x22];
    uint8_t bpp = t[0x23];
    int w = rd16(t + 0x24);
    int h = rd16(t + 0x26);

    size_t img_off = 0x10 + hdr_size;
    size_t pal_off = img_off + img_size;
    if (w <= 0 || h <= 0 || img_off + img_size > size || pal_off + pal_size > size
        || (bpp == 4 && img_size < (uint32_t)(w * h + 1) / 2)) {
        return NULL;
    }

    int linear = (clut_fmt & 0x80) != 0;
    int csize = (bpp > 8) ? (bpp / 8) : ((clut_fmt & 0x07) + 1);

    uint32_t *pal = NULL;
    if (entries > 0 && pal_size > 0) {
        pal = decode_palette(t + pal_off, entries, csize, linear);
        if (!pal) {
            return NULL;
        }
    }

    uint8_t *un = calloc((size_t)w * h, 1);
    uint32_t *rgba = calloc((size_t)w * h, 4);
    if (!un || !rgba) {
        free(pal);
        free(un);
        free(rgba);
        return NULL;
    }
    /* 8bpp is GS-swizzled; 4bpp is stored linearly (two pixels per byte, low
     * nibble first) — verified on the hero's c001a02/c001a03. */
    const uint8_t *src = un;
    if (bpp == 4) {
        src = t + img_off;
    } else {
        unswizzle(t + img_off, un, w, h);
    }
    for (int i = 0; i < w * h; i++) {
        if (bpp <= 8 && pal) {
            uint8_t ix;
            if (bpp == 4) {
                ix = (i & 1) ? (src[i >> 1] >> 4) : (src[i >> 1] & 0x0F);
            } else {
                ix = src[i];
            }
            rgba[i] = pal[ix % (entries > 0 ? entries : 1)];
        } else {
            rgba[i] = 0xFF00FF00u;   /* true-colour not handled here */
        }
    }
    free(pal);
    free(un);

    Dq8Tim2 *out = calloc(1, sizeof(*out));
    if (!out) {
        free(rgba);
        return NULL;
    }
    out->width = w;
    out->height = h;
    out->bits = (bpp == 1) ? 16 : (bpp == 2) ? 24 : (bpp == 3) ? 32
              : (bpp == 4) ? 4 : 8;
    out->rgba = rgba;
    return out;
}

void dq8_tim2_free(Dq8Tim2 *t)
{
    if (t) {
        free(t->rgba);
        free(t);
    }
}

uint32_t dq8_tim2_sample(const Dq8Tim2 *t, float u, float v)
{
    if (!t || !t->rgba) {
        return 0;
    }
    int x = (int)(u * t->width);
    int y = (int)(v * t->height);
    if (x < 0 || y < 0 || x >= t->width || y >= t->height) {
        return 0;
    }
    return t->rgba[y * t->width + x];
}

Dq8Tim2 *dq8_tim2_canvas(const uint8_t *data, size_t size)
{
    if (!data || size < 0x28 || memcmp(data, "TIM2", 4) != 0) {
        return NULL;
    }
    int w = rd16(data + 0x24), h = rd16(data + 0x26);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        return NULL;
    }
    Dq8Tim2 *t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->width = w;
    t->height = h;
    t->bits = 32;
    t->rgba = calloc((size_t)w * h, 4);
    if (!t->rgba) {
        free(t);
        return NULL;
    }
    return t;
}
