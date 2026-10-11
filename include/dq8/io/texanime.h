#ifndef DQ8_IO_TEXANIME_H
#define DQ8_IO_TEXANIME_H

#include <dq8/io/tim2.h>

#include <stddef.h>

/* Texture animation scripts (the format notes "Texture animation"). They live in
 * an `.img` as an entry named "#texanime" (plain Shift-JIS text) or as
 * standalone texanime*.cfg files, and build/animate textures at runtime by
 * copying rectangles between TIM2s:
 *
 *   TEX_ANIME "name",auto;       auto 1 = runs by itself; 0 = triggered by the
 *                                game (e.g. "ねむり" sleep face swap)
 *   TEX_ANIME_DATA 0,"frame";
 *   SRC_TEX  "src",x,y,w,h;      source rectangle
 *   DEST_TEX "dst",x,y[,w,h,f];  destination position
 *   SCROLL dx,dy;                optional: scroll the source (pixels per frame)
 *   WAIT n,0;                    frames to hold (0 = 1)
 *   TEX_ANIME_DATA_END;
 *   TEX_ANIME_END;
 *
 * Rectangles use a bottom-left origin (y up); textures are stored top-down.
 * Each TEX_ANIME cycles through its frames independently.
 * Not handled yet: ALPHA_BLEND, CLUT_COPY (parsed and ignored). */

typedef struct Dq8TexAnimFrame {
    char  src[32], dst[32];
    int   sx, sy, w, h;      /* source rectangle (bottom-left origin) */
    int   dx, dy;            /* destination position (bottom-left origin) */
    float scroll_x, scroll_y;
    int   wait;              /* frames this step is held (>= 1) */
} Dq8TexAnimFrame;

typedef struct Dq8TexAnim {
    char             name[64];   /* raw Shift-JIS */
    int              autoplay;   /* TEX_ANIME 2nd value: 1 = always running, 0 = triggered */
    int              frame_count;
    Dq8TexAnimFrame *frames;
} Dq8TexAnim;

typedef struct Dq8TexAnimSet {
    int         count;
    Dq8TexAnim *anims;
} Dq8TexAnimSet;

/* Parse a script. Returns NULL if it contains no TEX_ANIME block. */
Dq8TexAnimSet *dq8_texanim_parse(const char *text, size_t len);

void dq8_texanim_free(Dq8TexAnimSet *set);

/* Texture lookup for apply(): return the decoded texture named `name`, or NULL. */
typedef Dq8Tim2 *(*Dq8TexLookup)(void *ctx, const char *name);

/* Apply the state at `time` (in frames) of every autoplay animation: copy each
 * one's current step's source rectangle into its destination. Triggered
 * animations (autoplay 0, e.g. a sleep face) are left alone. */
void dq8_texanim_apply(const Dq8TexAnimSet *set, float time, Dq8TexLookup lookup, void *ctx);

/* Apply one animation (any autoplay value), e.g. when the game triggers it. */
void dq8_texanim_apply_one(const Dq8TexAnimSet *set, int index, float time, Dq8TexLookup lookup,
                           void *ctx);

#endif /* DQ8_IO_TEXANIME_H */
