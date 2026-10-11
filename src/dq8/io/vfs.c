#include <dq8/io/file.h>
#include <dq8/io/vfs.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { VFS_NONE, VFS_DIR, VFS_ISO };

static int s_kind = VFS_NONE;
static char s_root[1024];
static Dq8File *s_iso;

#define ISO_SECTOR 2048

/* ------------------------------------------------------------- helpers */

char *dq8_vfs_normalize(const char *game_path, char *out, size_t outsz)
{
    if (!out || outsz == 0) {
        return out;
    }
    size_t n = 0;
    const char *p = game_path ? game_path : "";

    /* drop a leading "cdrom0:" (case-insensitive) and slashes */
    if (strlen(p) >= 7 && (tolower((unsigned char)p[0]) == 'c')) {
        char tag[8];
        for (int i = 0; i < 7; i++) {
            tag[i] = (char)tolower((unsigned char)p[i]);
        }
        tag[7] = 0;
        if (strcmp(tag, "cdrom0:") == 0) {
            p += 7;
        }
    }
    while (*p == '\\' || *p == '/') {
        p++;
    }

    /* copy, normalising separators, into out */
    for (; *p && n + 1 < outsz; p++) {
        if (*p == '\\') {
            out[n++] = '/';
        } else {
            out[n++] = *p;
        }
    }
    out[n] = 0;

    /* strip a trailing ";N" version suffix */
    char *semi = strrchr(out, ';');
    if (semi && semi[1] >= '0' && semi[1] <= '9') {
        *semi = 0;
    }
    return out;
}

static void join_dir(const char *rel, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s/%s", s_root, rel);
}

static int ci_equal(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------ ISO9660 */

static int iso_read_sector(uint32_t lba, void *buf)
{
    if (!s_iso) {
        return 0;
    }
    if (dq8_file_seek(s_iso, (int64_t)lba * ISO_SECTOR, 0) != 0) {
        return 0;
    }
    return dq8_file_read(s_iso, buf, ISO_SECTOR) == ISO_SECTOR;
}

/* Find a path (slash-separated, normalized) in an ISO9660 image.
 * Fills resolved extent LBA + byte size. Returns 1 on success. */
static int iso_find(const char *path, uint32_t *out_lba, uint32_t *out_size, int *out_isdir)
{
    uint8_t pvd[ISO_SECTOR];
    if (!iso_read_sector(16, pvd)) {
        return 0;
    }
    if (memcmp(pvd + 1, "CD001", 5) != 0) {
        return 0;
    }

    /* root directory record lives at offset 156 in the PVD */
    uint8_t root[256];
    memcpy(root, pvd + 156, 34);
    uint32_t cur_lba = root[2] | (root[3] << 8) | (root[4] << 16) | (root[5] << 24);
    uint32_t cur_size = root[10] | (root[11] << 8) | (root[12] << 16) | (root[13] << 24);

    char comp[256];
    const char *p = path;

    while (*p) {
        const char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        if (clen >= sizeof(comp)) {
            return 0;
        }
        memcpy(comp, p, clen);
        comp[clen] = 0;
        p = slash ? slash + 1 : p + clen;

        if (clen == 0) {
            continue;
        }

        /* scan the current directory's records for `comp` */
        uint8_t sec[ISO_SECTOR];
        uint32_t bytes_left = cur_size;
        int found = 0;
        for (uint32_t lba = cur_lba; bytes_left > 0; lba++) {
            if (!iso_read_sector(lba, sec)) {
                return 0;
            }
            uint32_t off = 0;
            while (off < ISO_SECTOR && bytes_left > 0) {
                uint8_t rlen = sec[off];
                if (rlen == 0) {
                    break;   /* rest of sector is padding */
                }
                uint8_t name_len = sec[off + 32];
                const char *name = (const char *)(sec + off + 33);
                /* strip a ";N" version suffix from the ISO record name */
                size_t eff = name_len;
                for (size_t i = 0; i < name_len; i++) {
                    if (name[i] == ';') {
                        eff = i;
                        break;
                    }
                }
                /* skip "." and ".." */
                int is_dot = (name_len == 1 && (name[0] == 0 || name[0] == 1));
                if (!is_dot && eff == clen && ci_equal(name, comp, clen)) {
                    cur_lba = sec[off + 2] | (sec[off + 3] << 8)
                            | (sec[off + 4] << 16) | (sec[off + 5] << 24);
                    cur_size = sec[off + 10] | (sec[off + 11] << 8)
                             | (sec[off + 12] << 16) | (sec[off + 13] << 24);
                    found = 1;
                    if (out_isdir) {
                        *out_isdir = (sec[off + 25] & 2) != 0;
                    }
                    break;
                }
                off += rlen;
                bytes_left = bytes_left > rlen ? bytes_left - rlen : 0;
            }
            if (found) {
                break;
            }
        }
        if (!found) {
            return 0;
        }
    }

    *out_lba = cur_lba;
    *out_size = cur_size;
    if (out_isdir) {
        *out_isdir = *out_isdir;   /* last component's flag */
    }
    return 1;
}

/* -------------------------------------------------------------- public */

void dq8_vfs_init(void)
{
    /* nothing yet */
}

int dq8_vfs_mount_dir(const char *dir)
{
    dq8_vfs_unmount();
    if (!dir || strlen(dir) >= sizeof(s_root)) {
        return 0;
    }
    strcpy(s_root, dir);
    s_kind = VFS_DIR;
    return 1;
}

int dq8_vfs_mount_iso(const char *iso_path)
{
    dq8_vfs_unmount();
    Dq8File *f = dq8_file_open(iso_path, "rb");
    if (!f) {
        return 0;
    }
    s_iso = f;
    s_kind = VFS_ISO;

    uint8_t pvd[ISO_SECTOR];
    if (!iso_read_sector(16, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) {
        dq8_vfs_unmount();
        return 0;
    }
    return 1;
}

void dq8_vfs_unmount(void)
{
    if (s_iso) {
        dq8_file_close(s_iso);
        s_iso = NULL;
    }
    s_kind = VFS_NONE;
    s_root[0] = 0;
}

int dq8_vfs_exists(const char *game_path)
{
    char rel[1024], full[1100];
    dq8_vfs_normalize(game_path, rel, sizeof(rel));
    if (s_kind == VFS_DIR) {
        struct stat st;
        join_dir(rel, full, sizeof(full));
        return stat(full, &st) == 0;
    }
    if (s_kind == VFS_ISO) {
        uint32_t lba, size;
        int isdir;
        return iso_find(rel, &lba, &size, &isdir);
    }
    return 0;
}

int64_t dq8_vfs_size(const char *game_path)
{
    char rel[1024], full[1100];
    dq8_vfs_normalize(game_path, rel, sizeof(rel));
    if (s_kind == VFS_DIR) {
        struct stat st;
        join_dir(rel, full, sizeof(full));
        if (stat(full, &st) != 0) {
            return -1;
        }
        return (int64_t)st.st_size;
    }
    if (s_kind == VFS_ISO) {
        uint32_t lba, size;
        if (!iso_find(rel, &lba, &size, NULL)) {
            return -1;
        }
        return (int64_t)size;
    }
    return -1;
}

void *dq8_vfs_load(const char *game_path, size_t *out_size)
{
    char rel[1024], full[1100];
    dq8_vfs_normalize(game_path, rel, sizeof(rel));

    if (s_kind == VFS_DIR) {
        join_dir(rel, full, sizeof(full));
        return dq8_file_load(full, out_size);
    }
    if (s_kind == VFS_ISO) {
        uint32_t lba, size;
        int isdir;
        if (!iso_find(rel, &lba, &size, &isdir) || isdir) {
            return NULL;
        }
        uint8_t *buf = malloc(size ? size : 1);
        if (!buf) {
            return NULL;
        }
        if (dq8_file_seek(s_iso, (int64_t)lba * ISO_SECTOR, 0) != 0
            || dq8_file_read(s_iso, buf, size) != size) {
            free(buf);
            return NULL;
        }
        if (out_size) {
            *out_size = size;
        }
        return buf;
    }
    return NULL;
}

/* ------------------------------------------------------ streaming handles */

struct Dq8VfsFile {
    Dq8File *dir;      /* directory backend handle (or NULL) */
    uint32_t base;     /* ISO: byte offset of the file's extent */
    uint32_t size;     /* ISO: file size */
    uint32_t pos;      /* ISO: read cursor */
};

Dq8VfsFile *dq8_vfs_open(const char *game_path)
{
    char rel[1024], full[1100];
    dq8_vfs_normalize(game_path, rel, sizeof(rel));

    if (s_kind == VFS_DIR) {
        join_dir(rel, full, sizeof(full));
        Dq8File *f = dq8_file_open(full, "rb");
        if (!f) {
            return NULL;
        }
        Dq8VfsFile *h = malloc(sizeof(*h));
        h->dir = f;
        h->base = h->size = h->pos = 0;
        return h;
    }
    if (s_kind == VFS_ISO) {
        uint32_t lba, size;
        int isdir;
        if (!iso_find(rel, &lba, &size, &isdir) || isdir) {
            return NULL;
        }
        Dq8VfsFile *h = malloc(sizeof(*h));
        h->dir = NULL;
        h->base = lba * ISO_SECTOR;
        h->size = size;
        h->pos = 0;
        return h;
    }
    return NULL;
}

int64_t dq8_vfs_file_size(Dq8VfsFile *f)
{
    if (!f) {
        return -1;
    }
    return f->dir ? dq8_file_size(f->dir) : (int64_t)f->size;
}

int dq8_vfs_read(Dq8VfsFile *f, void *buf, size_t n)
{
    if (!f) {
        return -1;
    }
    if (f->dir) {
        return (int)dq8_file_read(f->dir, buf, n);
    }
    if (f->pos >= f->size) {
        return 0;
    }
    if (n > f->size - f->pos) {
        n = f->size - f->pos;
    }
    if (dq8_file_seek(s_iso, (int64_t)f->base + f->pos, 0) != 0) {
        return -1;
    }
    size_t got = dq8_file_read(s_iso, buf, n);
    f->pos += (uint32_t)got;
    return (int)got;
}

int dq8_vfs_seek(Dq8VfsFile *f, int64_t offset)
{
    if (!f) {
        return -1;
    }
    if (f->dir) {
        return dq8_file_seek(f->dir, offset, 0);
    }
    f->pos = (uint32_t)offset;
    return 0;
}

void dq8_vfs_close(Dq8VfsFile *f)
{
    if (!f) {
        return;
    }
    if (f->dir) {
        dq8_file_close(f->dir);
    }
    free(f);
}
