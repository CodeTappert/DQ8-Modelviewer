#include <dq8/io/archive.h>
#include <dq8/io/vfs.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARCHIVE_SECTOR 2048

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

typedef struct {
    char    *name;    /* decoded path, '/' separators */
    uint32_t sector;  /* 2048-byte units into the .DAT */
    uint32_t size;    /* bytes */
} Member;

struct Dq8Archive {
    uint8_t    *hd6;
    size_t      hd6_size;
    Dq8VfsFile *dat;
    uint32_t    pool_off;
    uint32_t    pool_count;
    uint32_t    table_a;
    uint32_t    count;
    uint32_t    ent_off;
    Member     *members;
    int         nmembers;
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Append the dictionary token at index `idx` to out (bounded). */
static void append_token(const uint8_t *hd6, size_t size, uint32_t pool_off,
                         uint32_t pool_count, uint32_t idx, char *out, size_t cap)
{
    if (idx >= pool_count) {
        return;
    }
    /* walk the pool to the idx-th string (pool is small: ~4.5k tokens) */
    uint32_t p = pool_off;
    for (uint32_t i = 0; i < idx; i++) {
        while (p < size && hd6[p] != 0) {
            p++;
        }
        p++;   /* skip the NUL */
    }
    size_t n = strlen(out);
    for (; p < size && hd6[p] != 0 && n + 1 < cap; p++) {
        out[n++] = (char)hd6[p];
    }
    out[n] = 0;
}

/* Decode a name (component-index byte string) at table_a + name_off into out.
 * '\' is normalised to '/'. */
static void decode_name(const uint8_t *hd6, size_t size, uint32_t table_a,
                        uint32_t name_off, uint32_t pool_off, uint32_t pool_count,
                        char *out, size_t cap)
{
    out[0] = 0;
    uint32_t p = table_a + name_off;
    while (p < size) {
        uint8_t b = hd6[p];
        uint32_t idx;
        if (b == 0) {
            break;
        }
        if (b < 0x80) {
            idx = b;
            p += 1;
        } else {
            if (p + 1 >= size) {
                break;
            }
            idx = (b & 0x7F) | ((uint32_t)hd6[p + 1] << 7);
            p += 2;
        }
        append_token(hd6, size, pool_off, pool_count, idx, out, cap);
    }
    for (char *q = out; *q; q++) {
        if (*q == '\\') {
            *q = '/';
        }
    }
}

/* Normalise a query path: strip "cdrom0:", leading slashes, ";N", and map
 * '\' to '/'. */
static void normalize_query(const char *path, char *out, size_t cap)
{
    const char *p = path ? path : "";
    if (strlen(p) >= 7 && tolower((unsigned char)p[0]) == 'c'
        && tolower((unsigned char)p[1]) == 'd' && tolower((unsigned char)p[2]) == 'r'
        && tolower((unsigned char)p[3]) == 'o' && tolower((unsigned char)p[4]) == 'm'
        && tolower((unsigned char)p[5]) == '0' && p[6] == ':') {
        p += 7;
    }
    while (*p == '/' || *p == '\\') {
        p++;
    }
    size_t n = 0;
    for (; *p && n + 1 < cap; p++) {
        out[n++] = (*p == '\\') ? '/' : *p;
    }
    out[n] = 0;
    char *semi = strrchr(out, ';');
    if (semi && semi[1] >= '0' && semi[1] <= '9') {
        *semi = 0;
    }
}

static int ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

Dq8Archive *dq8_archive_open(const char *base)
{
    char path[64];
    size_t size = 0;

    snprintf(path, sizeof(path), "%s.HD6;1", base);
    uint8_t *hd6 = dq8_vfs_load(path, &size);
    if (!hd6) {
        return NULL;
    }

    Dq8Archive *a = calloc(1, sizeof(*a));
    a->hd6 = hd6;
    a->hd6_size = size;
    if (size < 0x2c || memcmp(hd6, "HD6\0", 4) != 0) {
        dq8_archive_close(a);
        return NULL;
    }

    a->pool_off = rd32(hd6 + 0x04);
    a->pool_count = rd32(hd6 + 0x0C);
    a->table_a = rd32(hd6 + 0x14);
    a->count = rd32(hd6 + 0x24);
    a->ent_off = rd32(hd6 + 0x28);

    snprintf(path, sizeof(path), "%s.DAT;1", base);
    a->dat = dq8_vfs_open(path);
    if (!a->dat) {
        dq8_archive_close(a);
        return NULL;
    }

    a->members = calloc(a->count ? a->count : 1, sizeof(Member));
    a->nmembers = (int)a->count;
    for (uint32_t k = 0; k < a->count; k++) {
        uint32_t e0 = rd32(hd6 + a->ent_off + k * 8 + 0);
        uint32_t e1 = rd32(hd6 + a->ent_off + k * 8 + 4);
        char name[512];
        decode_name(hd6, size, a->table_a, e0 & 0x3FFFF,
                    a->pool_off, a->pool_count, name, sizeof(name));
        a->members[k].name = dup_str(name);
        a->members[k].sector = ((e1 & 0xFF) << 14) | (e0 >> 18);
        a->members[k].size = (e1 >> 8) * 16;
    }
    return a;
}

void dq8_archive_close(Dq8Archive *a)
{
    if (!a) {
        return;
    }
    if (a->members) {
        for (int i = 0; i < a->nmembers; i++) {
            free(a->members[i].name);
        }
        free(a->members);
    }
    if (a->dat) {
        dq8_vfs_close(a->dat);
    }
    free(a->hd6);
    free(a);
}

int dq8_archive_count(const Dq8Archive *a)
{
    return a ? a->nmembers : 0;
}

const char *dq8_archive_name(const Dq8Archive *a, int index)
{
    if (!a || index < 0 || index >= a->nmembers) {
        return NULL;
    }
    return a->members[index].name;
}

int dq8_archive_find(const Dq8Archive *a, const char *path,
                     uint32_t *sector, uint32_t *size)
{
    if (!a) {
        return 0;
    }
    char want[512];
    normalize_query(path, want, sizeof(want));
    for (int i = 0; i < a->nmembers; i++) {
        if (ci_equal(a->members[i].name, want)) {
            if (sector) {
                *sector = a->members[i].sector;
            }
            if (size) {
                *size = a->members[i].size;
            }
            return 1;
        }
    }
    return 0;
}

void *dq8_archive_load(Dq8Archive *a, const char *path, size_t *out_size)
{
    uint32_t sector, size;
    if (!dq8_archive_find(a, path, &sector, &size)) {
        return NULL;
    }
    void *buf = malloc(size ? size : 1);
    if (!buf) {
        return NULL;
    }
    if (dq8_vfs_seek(a->dat, (int64_t)sector * ARCHIVE_SECTOR) != 0
        || dq8_vfs_read(a->dat, buf, size) != (int)size) {
        free(buf);
        return NULL;
    }
    if (out_size) {
        *out_size = size;
    }
    return buf;
}
