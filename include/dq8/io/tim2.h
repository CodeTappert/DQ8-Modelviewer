#ifndef DQ8_IO_TIM2_H
#define DQ8_IO_TIM2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PS2 TIM2 texture (Sony). DQ8 stores these inside the IM3 (.img) member of a
 * `.chr` model pack (see dq8/io/img.h). This decodes the picture to linear
 * RGBA8888 (0xAABBGGRR), handling the GS swizzle and interlaced palettes.
 *
 * Layout (see the format notes; from the DCExtractor TM2 spec):
 *   TIM2 header (16 B): "TIM2", ver(1), fmt(1), pictureCount(2), pad(8)
 *   Picture header:
 *     +0x00 totalSize (unpacked), +0x04 paletteSize, +0x08 imageDataSize
 *     +0x0c headerSize(2), +0x0e colorEntries(2)
 *     +0x10 imageFormat, mipmapCount, clutFormat, bitsPerPixel
 *     +0x14 width(2), height(2), then gsTEX0/1/regs/clut, then userData
 *     then imageBytes[imageDataSize], then paletteBytes[paletteSize].
 *   bitsPerPixel code: 1=16b ABGR1555, 2=24b RGB, 3=32b RGBA,
 *                      4=4b indexed, 5=8b indexed. */

typedef struct Dq8Tim2 {
    int       width, height;
    int       bits;          /* bits per pixel of the image */
    uint32_t *rgba;          /* width*height, 0xAABBGGRR (owned) */
} Dq8Tim2;

/* Decode the first picture of a TIM2 blob. NULL on error. */
Dq8Tim2 *dq8_tim2_open(const uint8_t *data, size_t size);

void dq8_tim2_free(Dq8Tim2 *t);

/* Blank (transparent) canvas for a header-only TIM2: a picture header with no
 * pixel data, e.g. bingo01w or b1701a08w, which texture animation fills at
 * runtime. NULL if the blob isn't a TIM2 header. */
Dq8Tim2 *dq8_tim2_canvas(const uint8_t *data, size_t size);

/* Sample (u,v in [0,1], origin top-left) as 0xAABBGGRR; 0 if out of range. */
uint32_t dq8_tim2_sample(const Dq8Tim2 *t, float u, float v);

#endif /* DQ8_IO_TIM2_H */
