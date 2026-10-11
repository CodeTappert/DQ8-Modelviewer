#include <dq8/io/img.h>
#include <dq8/io/pak.h>

#include <stdlib.h>
#include <string.h>

#define IM3_HEADER 0x10
#define IM3_ENTRY  0x40

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_im3(const uint8_t *d, size_t n)
{
    return n >= 4 && d[0] == 'I' && d[1] == 'M' && d[2] == '3';
}

Dq8Img *dq8_img_open(const uint8_t *data, size_t size)
{
    if (!is_im3(data, size) || size < IM3_HEADER) {
        return NULL;
    }
    uint32_t count = rd32(data + 0x08);
    if (count > DQ8_IMG_MAX_ENTRIES
        || (size_t)IM3_HEADER + (size_t)count * IM3_ENTRY > size) {
        return NULL;
    }

    Dq8Img *img = calloc(1, sizeof(*img));
    img->blob = malloc(size);
    memcpy(img->blob, data, size);
    img->blob_size = size;
    img->entry_count = (int)count;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *rec = img->blob + IM3_HEADER + i * IM3_ENTRY;
        Dq8ImgEntry *e = &img->entries[i];
        memcpy(e->name, rec, 32);
        e->name[31] = 0;
        e->data_offset = rd32(rec + 0x24);
        e->data_size = rd32(rec + 0x34);
        e->type = rd32(rec + 0x38);
    }
    return img;
}

/* Recursively descend PAK members looking for an IM3 image. */
static const uint8_t *find_img(const uint8_t *data, size_t size, size_t *out_size, int depth)
{
    if (depth > 4 || size < 4) {
        return NULL;
    }
    if (is_im3(data, size)) {
        *out_size = size;
        return data;
    }
    Dq8Pak *pak = dq8_pak_open(data, size);
    if (!pak) {
        return NULL;
    }
    const uint8_t *found = NULL;
    for (int i = 0; i < dq8_pak_count(pak) && !found; i++) {
        const Dq8PakEntry *e = dq8_pak_entry(pak, i);
        found = find_img(e->data, e->size, out_size, depth + 1);
    }
    dq8_pak_close(pak);
    return found;
}

Dq8Img *dq8_img_load_archive(Dq8Archive *archive, const char *container_path)
{
    size_t size = 0;
    uint8_t *data = dq8_archive_load(archive, container_path, &size);
    if (!data) {
        return NULL;
    }
    size_t im3_size = 0;
    const uint8_t *im3 = find_img(data, size, &im3_size, 0);
    Dq8Img *img = im3 ? dq8_img_open(im3, im3_size) : NULL;
    free(data);
    return img;
}

void dq8_img_free(Dq8Img *img)
{
    if (img) {
        free(img->blob);
        free(img);
    }
}

int dq8_img_count(const Dq8Img *img)
{
    return img ? img->entry_count : 0;
}

const Dq8ImgEntry *dq8_img_entry(const Dq8Img *img, int index)
{
    if (!img || index < 0 || index >= img->entry_count) {
        return NULL;
    }
    return &img->entries[index];
}

const uint8_t *dq8_img_blob(const Dq8Img *img, size_t *out_size)
{
    if (!img) {
        return NULL;
    }
    if (out_size) {
        *out_size = img->blob_size;
    }
    return img->blob;
}
