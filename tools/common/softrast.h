#ifndef DQ8_TOOLS_SOFTRAST_H
#define DQ8_TOOLS_SOFTRAST_H

/* Small software rasterizer shared by the viewer.
 * Orthographic camera, flat shading, nearest texel, alpha test, back-face
 * culling; passes: opaque, translucent (blended), additive (material flag
 * 0x10), environment shine (view-space normal lookup). Not game code: the
 * port's real renderer will be GPU based. */

#include <dq8/io/tim2.h>

#include <stdint.h>

enum {
    SR_TEXTURED = 0,
    SR_UNTEXTURED,   /* material without a texture: light grey */
    SR_MISSING,      /* texture not found anywhere: magenta */
    SR_CANVAS,       /* runtime canvas nothing fills: cyan */
};

typedef struct SrTri {
    float          p[3][3];
    float          uv[3][2];
    const Dq8Tim2 *tex;
    int            tex_state;  /* SR_* */
    float          alpha;      /* material opacity */
    int            additive;
    float          nrm[3][3];  /* posed corner normals (environment pass) */
    const Dq8Tim2 *env;        /* environment texture or NULL */
    float          env_rgb[3]; /* environment strength (env material diffuse) */
} SrTri;

typedef struct SrTriList {
    SrTri *t;
    int    n, cap;
} SrTriList;

SrTri *sr_push(SrTriList *l);
void sr_list_free(SrTriList *l);

typedef struct SrImage {
    int       w, h;
    uint32_t *px;   /* 0xFFRRGGBB */
    float    *z;
} SrImage;

void sr_image_init(SrImage *im, int w, int h);
void sr_image_free(SrImage *im);
/* Clear a rectangle (color + depth). */
void sr_clear(SrImage *im, int x0, int y0, int w, int h, uint32_t color);
int sr_write_ppm(const SrImage *im, const char *path);

typedef struct SrCamera {
    float center[3];
    float scale;          /* pixels per model unit */
    float yaw, pitch;     /* radians */
} SrCamera;

/* Camera framing every triangle of `n` lists in a viewport of `size` pixels
 * (the smaller viewport side), at the given angles. */
SrCamera sr_fit(const SrTriList *lists, int n, int size, float yaw, float pitch);

/* Draw into the viewport (x0, y0, w, h) of `im`. */
void sr_draw(SrImage *im, int x0, int y0, int w, int h, const SrTriList *tl, const SrCamera *cam);

#endif /* DQ8_TOOLS_SOFTRAST_H */
