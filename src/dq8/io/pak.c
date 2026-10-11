#include <dq8/io/pak.h>

#include <stdlib.h>
#include <string.h>

#define PAK_HEADER 0x50   /* name[64] + 4 * i32 */

struct Dq8Pak {
    Dq8PakEntry *entries;
    int          count;
};

static int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)
                   | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

Dq8Pak *dq8_pak_open(const uint8_t *data, size_t size)
{
    if (!data || size < PAK_HEADER) {
        return NULL;
    }

    Dq8Pak *p = calloc(1, sizeof(*p));
    int cap = 0;

    size_t pos = 0;
    while (pos + PAK_HEADER <= size) {
        const uint8_t *h = data + pos;
        if (h[0] == 0) {
            break;   /* empty name terminates the chain */
        }
        int32_t rel = rd_i32(h + 64);
        int32_t fsz = rd_i32(h + 68);
        int32_t nxt = rd_i32(h + 72);
        int32_t ver = rd_i32(h + 76);
        if (nxt <= 0) {
            break;
        }
        /* bounds: member data must lie within the image */
        if (rel < 0 || fsz < 0 || pos + (size_t)rel + (size_t)fsz > size) {
            break;
        }

        if (p->count == cap) {
            cap = cap ? cap * 2 : 16;
            p->entries = realloc(p->entries, (size_t)cap * sizeof(Dq8PakEntry));
        }
        Dq8PakEntry *e = &p->entries[p->count++];
        memset(e, 0, sizeof(*e));
        memcpy(e->name, h, 64);
        e->name[63] = 0;
        e->data = data + pos + rel;
        e->size = (uint32_t)fsz;
        e->version = ver;

        pos += (size_t)nxt;
    }

    if (p->count == 0) {
        free(p->entries);
        free(p);
        return NULL;
    }
    return p;
}

void dq8_pak_close(Dq8Pak *p)
{
    if (p) {
        free(p->entries);
        free(p);
    }
}

int dq8_pak_count(const Dq8Pak *p)
{
    return p ? p->count : 0;
}

const Dq8PakEntry *dq8_pak_entry(const Dq8Pak *p, int index)
{
    if (!p || index < 0 || index >= p->count) {
        return NULL;
    }
    return &p->entries[index];
}

const Dq8PakEntry *dq8_pak_find(const Dq8Pak *p, const char *name)
{
    if (!p || !name) {
        return NULL;
    }
    for (int i = 0; i < p->count; i++) {
        if (strcmp(p->entries[i].name, name) == 0) {
            return &p->entries[i];
        }
    }
    return NULL;
}
