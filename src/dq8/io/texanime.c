#include <dq8/io/texanime.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Find `key` at or after *pos (and before end); returns pointer just past it. */
static const char *find(const char *p, const char *end, const char *key)
{
    size_t k = strlen(key);
    for (; p + k <= end; p++) {
        if (memcmp(p, key, k) == 0) {
            return p + k;
        }
    }
    return NULL;
}

/* Read a quoted string after `p` into out. */
static const char *quoted(const char *p, const char *end, char *out, size_t len)
{
    const char *q0 = memchr(p, '"', (size_t)(end - p));
    const char *q1 = q0 ? memchr(q0 + 1, '"', (size_t)(end - q0 - 1)) : NULL;
    if (!q1) {
        out[0] = 0;
        return NULL;
    }
    size_t n = (size_t)(q1 - q0 - 1) < len - 1 ? (size_t)(q1 - q0 - 1) : len - 1;
    memcpy(out, q0 + 1, n);
    out[n] = 0;
    return q1 + 1;
}

/* Up to `max` comma-separated numbers after `p` (stops at ';'). */
static int numbers(const char *p, const char *end, float *out, int max)
{
    int n = 0;
    char buf[128];
    const char *semi = memchr(p, ';', (size_t)(end - p));
    size_t len = semi ? (size_t)(semi - p) : (size_t)(end - p);
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, p, len);
    buf[len] = 0;
    for (char *s = buf; *s && n < max;) {
        char *e;
        float v = strtof(s, &e);
        if (e == s) {
            s++;
            continue;
        }
        out[n++] = v;
        s = e;
    }
    return n;
}

Dq8TexAnimSet *dq8_texanim_parse(const char *text, size_t len)
{
    if (!text) {
        return NULL;
    }
    const char *end = text + len, *p = text;
    Dq8TexAnimSet *set = calloc(1, sizeof(*set));
    int cap = 0;
    while ((p = find(p, end, "TEX_ANIME \"")) != NULL) {
        const char *block_end = find(p, end, "TEX_ANIME_END");
        if (!block_end) {
            block_end = end;
        }
        if (set->count == cap) {
            cap = cap ? cap * 2 : 8;
            set->anims = realloc(set->anims, sizeof(Dq8TexAnim) * (size_t)cap);
        }
        Dq8TexAnim *an = &set->anims[set->count++];
        memset(an, 0, sizeof(*an));
        const char *after = quoted(p - 1, block_end, an->name, sizeof(an->name));
        float flag[1];
        an->autoplay = after && numbers(after, block_end, flag, 1) == 1 ? (int)flag[0] : 1;
        int fcap = 0;
        const char *q = p;
        while ((q = find(q, block_end, "TEX_ANIME_DATA ")) != NULL) {
            const char *fend = find(q, block_end, "TEX_ANIME_DATA_END");
            if (!fend) {
                fend = block_end;
            }
            if (an->frame_count == fcap) {
                fcap = fcap ? fcap * 2 : 8;
                an->frames = realloc(an->frames, sizeof(Dq8TexAnimFrame) * (size_t)fcap);
            }
            Dq8TexAnimFrame *f = &an->frames[an->frame_count];
            memset(f, 0, sizeof(*f));
            f->wait = 1;
            float v[6];
            const char *s = find(q, fend, "SRC_TEX");
            const char *d = find(q, fend, "DEST_TEX");
            if (s && d) {
                const char *after = quoted(s, fend, f->src, sizeof(f->src));
                if (after && numbers(after, fend, v, 4) == 4) {
                    f->sx = (int)v[0];
                    f->sy = (int)v[1];
                    f->w = (int)v[2];
                    f->h = (int)v[3];
                }
                after = quoted(d, fend, f->dst, sizeof(f->dst));
                if (after && numbers(after, fend, v, 2) == 2) {
                    f->dx = (int)v[0];
                    f->dy = (int)v[1];
                }
                const char *sc = find(q, fend, "SCROLL");
                if (sc && numbers(sc, fend, v, 2) == 2) {
                    f->scroll_x = v[0];
                    f->scroll_y = v[1];
                }
                const char *w = find(q, fend, "WAIT");
                if (w && numbers(w, fend, v, 1) == 1 && v[0] >= 1) {
                    f->wait = (int)v[0];
                }
                if (f->src[0] && f->dst[0] && f->w > 0 && f->h > 0) {
                    an->frame_count++;
                }
            }
            q = fend;
        }
        p = block_end;
    }
    if (set->count == 0) {
        dq8_texanim_free(set);
        return NULL;
    }
    return set;
}

void dq8_texanim_free(Dq8TexAnimSet *set)
{
    if (set) {
        for (int i = 0; i < set->count; i++) {
            free(set->anims[i].frames);
        }
        free(set->anims);
        free(set);
    }
}

static int wrap(int v, int n)
{
    v %= n;
    return v < 0 ? v + n : v;
}

static void apply_anim(const Dq8TexAnim *an, float time, Dq8TexLookup lookup, void *ctx);

void dq8_texanim_apply(const Dq8TexAnimSet *set, float time, Dq8TexLookup lookup, void *ctx)
{
    for (int i = 0; set && i < set->count; i++) {
        if (set->anims[i].autoplay) {
            apply_anim(&set->anims[i], time, lookup, ctx);
        }
    }
}

void dq8_texanim_apply_one(const Dq8TexAnimSet *set, int index, float time, Dq8TexLookup lookup,
                           void *ctx)
{
    if (set && index >= 0 && index < set->count) {
        apply_anim(&set->anims[index], time, lookup, ctx);
    }
}

static void apply_anim(const Dq8TexAnim *an, float time, Dq8TexLookup lookup, void *ctx)
{
    {
        if (an->frame_count == 0) {
            return;
        }
        int total = 0;
        for (int k = 0; k < an->frame_count; k++) {
            total += an->frames[k].wait;
        }
        int t = wrap((int)floorf(time), total), k = 0;
        while (t >= an->frames[k].wait) {
            t -= an->frames[k].wait;
            k++;
        }
        const Dq8TexAnimFrame *f = &an->frames[k];
        Dq8Tim2 *src = lookup(ctx, f->src), *dst = lookup(ctx, f->dst);
        if (!src || !dst || !src->rgba || !dst->rgba) {
            return;
        }
        int ox = (int)floorf(f->scroll_x * time), oy = (int)floorf(f->scroll_y * time);
        for (int y = 0; y < f->h; y++) {
            /* bottom-left origin -> top-down rows */
            int sy = src->height - 1 - (f->sy + wrap(y + oy, f->h));
            int dy = dst->height - 1 - (f->dy + y);
            if (sy < 0 || sy >= src->height || dy < 0 || dy >= dst->height) {
                continue;
            }
            for (int x = 0; x < f->w; x++) {
                int sx = f->sx + wrap(x + ox, f->w), dx = f->dx + x;
                if (sx < 0 || sx >= src->width || dx < 0 || dx >= dst->width) {
                    continue;
                }
                dst->rgba[dy * dst->width + dx] = src->rgba[sy * src->width + sx];
            }
        }
    }
}
