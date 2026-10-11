#include "softrast.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SrTri *sr_push(SrTriList *l)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4096;
        l->t = realloc(l->t, sizeof(SrTri) * (size_t)l->cap);
    }
    SrTri *t = &l->t[l->n++];
    memset(t, 0, sizeof(*t));
    return t;
}

void sr_list_free(SrTriList *l)
{
    free(l->t);
    memset(l, 0, sizeof(*l));
}

void sr_image_init(SrImage *im, int w, int h)
{
    im->w = w;
    im->h = h;
    im->px = malloc(sizeof(uint32_t) * (size_t)w * h);
    im->z = malloc(sizeof(float) * (size_t)w * h);
    sr_clear(im, 0, 0, w, h, 0xFF282828u);
}

void sr_image_free(SrImage *im)
{
    free(im->px);
    free(im->z);
    memset(im, 0, sizeof(*im));
}

void sr_clear(SrImage *im, int x0, int y0, int w, int h, uint32_t color)
{
    for (int y = y0; y < y0 + h && y < im->h; y++) {
        for (int x = x0; x < x0 + w && x < im->w; x++) {
            im->px[(size_t)y * im->w + x] = color;
            im->z[(size_t)y * im->w + x] = -1e30f;
        }
    }
}

int sr_write_ppm(const SrImage *im, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return 0;
    }
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    for (int i = 0; i < im->w * im->h; i++) {
        uint8_t rgb[3] = { (uint8_t)(im->px[i] >> 16), (uint8_t)(im->px[i] >> 8), (uint8_t)im->px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 1;
}

SrCamera sr_fit(const SrTriList *lists, int n, int size, float yaw, float pitch)
{
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int s = 0; s < n; s++) {
        for (int i = 0; i < lists[s].n; i++) {
            for (int e = 0; e < 3; e++) {
                for (int c = 0; c < 3; c++) {
                    lo[c] = fminf(lo[c], lists[s].t[i].p[e][c]);
                    hi[c] = fmaxf(hi[c], lists[s].t[i].p[e][c]);
                }
            }
        }
    }
    SrCamera cam = { { 0, 0, 0 }, 1, yaw, pitch };
    float radius = 0;
    for (int c = 0; c < 3; c++) {
        cam.center[c] = lo[c] <= hi[c] ? (lo[c] + hi[c]) / 2 : 0;
        radius = fmaxf(radius, (hi[c] - lo[c]) / 2);
    }
    cam.scale = radius > 0 ? size * 0.46f / (radius * 1.15f) : 1;
    return cam;
}

static uint8_t clamp8(float v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void sr_draw(SrImage *im, int ox, int oy, int vw, int vh, const SrTriList *tl, const SrCamera *cam)
{
    float cy = cosf(cam->yaw), sy = sinf(cam->yaw), cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    const float L[3] = { 0.3f / 0.98f, 0.4f / 0.98f, 0.85f / 0.98f };
    /* pass 0 opaque, 1 translucent (blended), 2 additive (no depth writes),
     * 3 environment shine added on top of visible opaque surfaces */
    for (int pass = 0; pass < 4; pass++)
    for (int i = 0; i < tl->n; i++) {
        const SrTri *t = &tl->t[i];
        int want = t->additive ? 2 : (t->alpha >= 0.99f ? 0 : 1);
        if (pass == 3 ? (!t->env || want != 0) : want != pass) {
            continue;
        }
        float eu[3] = { 0 }, ev[3] = { 0 };   /* env-map coords from view-space normals */
        if (pass == 3) {
            for (int e = 0; e < 3; e++) {
                const float *nn = t->nrm[e];
                float a = cy * nn[0] - sy * nn[2], b = nn[1], c = sy * nn[0] + cy * nn[2];
                float vx = a, vy = cp * b - sp * c, vz = sp * b + cp * c;
                float l = sqrtf(vx * vx + vy * vy + vz * vz);
                l = l > 0 ? l : 1;
                eu[e] = vx / l * 0.5f + 0.5f;
                ev[e] = 0.5f - vy / l * 0.5f;
            }
        }
        float q[3][3], x[3], y[3];
        for (int e = 0; e < 3; e++) {
            float v[3] = { t->p[e][0] - cam->center[0], t->p[e][1] - cam->center[1],
                           t->p[e][2] - cam->center[2] };
            float a = cy * v[0] - sy * v[2], b = v[1], c = sy * v[0] + cy * v[2];  /* yaw */
            q[e][0] = a;
            q[e][1] = cp * b - sp * c;                                             /* pitch */
            q[e][2] = sp * b + cp * c;
            x[e] = q[e][0] * cam->scale + vw / 2.0f + ox;
            y[e] = vh / 2.0f - q[e][1] * cam->scale + oy;
        }
        float e1[3] = { q[1][0] - q[0][0], q[1][1] - q[0][1], q[1][2] - q[0][2] };
        float e2[3] = { q[2][0] - q[0][0], q[2][1] - q[0][1], q[2][2] - q[0][2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0] };
        float ln = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (ln == 0 || (n[2] <= 0 && !t->additive)) {
            continue;   /* back face (glow cards are two-sided) */
        }
        float sh = 0.55f + 0.45f * fabsf((n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / ln);
        if (t->additive) {
            sh = 1;   /* glows are unlit */
        }
        float den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
        if (fabsf(den) < 1e-9f) {
            continue;
        }
        int x0 = (int)fmaxf(floorf(fminf(x[0], fminf(x[1], x[2]))), (float)ox);
        int x1 = (int)fminf(ceilf(fmaxf(x[0], fmaxf(x[1], x[2]))), (float)(ox + vw - 1));
        int y0 = (int)fmaxf(floorf(fminf(y[0], fminf(y[1], y[2]))), (float)oy);
        int y1 = (int)fminf(ceilf(fmaxf(y[0], fmaxf(y[1], y[2]))), (float)(oy + vh - 1));
        x1 = x1 < im->w - 1 ? x1 : im->w - 1;
        y1 = y1 < im->h - 1 ? y1 : im->h - 1;
        const Dq8Tim2 *tex = t->tex_state == SR_TEXTURED ? t->tex : NULL;
        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                float fx = px + 0.5f, fy = py + 0.5f;
                float a = ((y[1] - y[2]) * (fx - x[2]) + (x[2] - x[1]) * (fy - y[2])) / den;
                float b = ((y[2] - y[0]) * (fx - x[2]) + (x[0] - x[2]) * (fy - y[2])) / den;
                float c = 1 - a - b;
                if (a < -1e-4f || b < -1e-4f || c < -1e-4f) {
                    continue;
                }
                float z = a * q[0][2] + b * q[1][2] + c * q[2][2];
                size_t at = (size_t)py * im->w + px;
                uint32_t *dst = im->px + at;
                float dr = (float)((*dst >> 16) & 0xff), dg = (float)((*dst >> 8) & 0xff),
                      db = (float)(*dst & 0xff);
                if (pass == 3) {   /* only where this surface is the visible one */
                    if (z < im->z[at] - 1e-3f * (1 + fabsf(z))) {
                        continue;
                    }
                    const Dq8Tim2 *et = t->env;
                    float uu = a * eu[0] + b * eu[1] + c * eu[2];
                    float vv = a * ev[0] + b * ev[1] + c * ev[2];
                    int tx = (int)(uu * et->width), ty = (int)(vv * et->height);
                    tx = tx < 0 ? 0 : tx >= et->width ? et->width - 1 : tx;
                    ty = ty < 0 ? 0 : ty >= et->height ? et->height - 1 : ty;
                    uint32_t ec = et->rgba[ty * et->width + tx];   /* 0xAABBGGRR */
                    float ea = (float)(ec >> 24) / 255.0f;          /* GS additive blend weights by source alpha */
                    *dst = 0xFF000000u | (uint32_t)clamp8(dr + (ec & 0xff) * ea * t->env_rgb[0]) << 16
                         | (uint32_t)clamp8(dg + ((ec >> 8) & 0xff) * ea * t->env_rgb[1]) << 8
                         | clamp8(db + ((ec >> 16) & 0xff) * ea * t->env_rgb[2]);
                    continue;
                }
                if (z <= im->z[at]) {
                    continue;
                }
                float r, g, bl;
                if (t->tex_state == SR_UNTEXTURED) {
                    r = g = bl = 176;
                } else if (t->tex_state == SR_MISSING) {
                    r = 255; g = 0; bl = 255;
                } else if (!tex) {
                    r = 0; g = 220; bl = 220;
                } else {
                    float u = a * t->uv[0][0] + b * t->uv[1][0] + c * t->uv[2][0];
                    float v = a * t->uv[0][1] + b * t->uv[1][1] + c * t->uv[2][1];
                    int tx = (int)floorf(u * tex->width) % tex->width;
                    int ty = (int)floorf(v * tex->height) % tex->height;
                    tx += tx < 0 ? tex->width : 0;
                    ty += ty < 0 ? tex->height : 0;
                    uint32_t col = tex->rgba[ty * tex->width + tx];   /* 0xAABBGGRR */
                    if ((col >> 24) == 0) {
                        continue;                /* alpha test */
                    }
                    r = (float)(col & 0xff); g = (float)((col >> 8) & 0xff); bl = (float)((col >> 16) & 0xff);
                }
                float al = t->alpha;
                if (pass == 0) {
                    im->z[at] = z;
                    dr = (float)(uint8_t)(r * sh); dg = (float)(uint8_t)(g * sh); db = (float)(uint8_t)(bl * sh);
                } else if (pass == 2) {
                    dr += r * al; dg += g * al; db += bl * al;
                } else {
                    dr = dr * (1 - al) + r * sh * al;
                    dg = dg * (1 - al) + g * sh * al;
                    db = db * (1 - al) + bl * sh * al;
                }
                *dst = 0xFF000000u | (uint32_t)clamp8(dr) << 16 | (uint32_t)clamp8(dg) << 8 | clamp8(db);
            }
        }
    }
}
