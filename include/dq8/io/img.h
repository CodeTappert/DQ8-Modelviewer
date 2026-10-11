#ifndef DQ8_IO_IMG_H
#define DQ8_IO_IMG_H

#include <dq8/io/archive.h>

#include <stddef.h>
#include <stdint.h>

/* DQ8 image archive (IM2/IM3) — the `.img` member of a model pack. It is a
 * list of embedded TIM2 textures. (The DQ8 *model* is `.mds`, a separate
 * format; see the format notes.)
 *
 * Layout (reversed; matches DCExtractor's "IMG (Version 3)" specification):
 *   0x00 "IM3\0"
 *   0x04 header size (0x10)
 *   0x08 entry count
 *   0x0c 0
 *   then `entry count` 64-byte records:
 *     name[32]; u32 recordSize; u32 dataOffset; u32 flag1; u32 zero;
 *               u32 zero;       u32 dataSize;   u32 type;  u32 zero
 *   each entry's data blob is `dataSize` bytes at `dataOffset` and begins with
 *   a TIM2 texture.
 *
 * A `.chr/.mpk/.pcp` is a PAK whose members include `name.mds`, `name.img`
 * (this), and `info.cfg`. dq8_img_load_archive() follows the chain to the IMG. */

#define DQ8_IMG_MAX_ENTRIES 256

typedef struct Dq8ImgEntry {
    char     name[32];
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t type;
} Dq8ImgEntry;

typedef struct Dq8Img {
    uint8_t    *blob;          /* copy of the IM3 bytes (owned) */
    size_t      blob_size;
    int         entry_count;
    Dq8ImgEntry entries[DQ8_IMG_MAX_ENTRIES];
} Dq8Img;

/* Parse an IM3 blob. Returns NULL if not an IM3 image. */
Dq8Img *dq8_img_open(const uint8_t *data, size_t size);

/* Load from `container_path` in `archive`, following PAK members to the IMG. */
Dq8Img *dq8_img_load_archive(Dq8Archive *archive, const char *container_path);

void dq8_img_free(Dq8Img *img);

int dq8_img_count(const Dq8Img *img);
const Dq8ImgEntry *dq8_img_entry(const Dq8Img *img, int index);

/* The IM3 bytes (for texture decoding). */
const uint8_t *dq8_img_blob(const Dq8Img *img, size_t *out_size);

#endif /* DQ8_IO_IMG_H */
