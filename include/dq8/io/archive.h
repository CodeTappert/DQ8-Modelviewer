#ifndef DQ8_IO_ARCHIVE_H
#define DQ8_IO_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>

/* DQ8 data archive (`DATA.HD6` index + `DATA.DAT` bodies, or `DATA2.*`).
 *
 * Format reversed from the PAL executable — see the format notes. Names are
 * stored as component-dictionary indices; members are located by a sector
 * offset and size. All data is streamed from the disc (ISO or extracted dir)
 * via the VFS; nothing is embedded. */

typedef struct Dq8Archive Dq8Archive;

/* Open by base name ("DATA" or "DATA2"). Loads `<base>.HD6` and opens
 * `<base>.DAT` through the mounted VFS. Returns NULL on failure. */
Dq8Archive *dq8_archive_open(const char *base);
void dq8_archive_close(Dq8Archive *a);

int dq8_archive_count(const Dq8Archive *a);

/* Member name at `index` (NULL if out of range). */
const char *dq8_archive_name(const Dq8Archive *a, int index);

/* Find a member by path (case-insensitive; '\' and '/' are equivalent).
 * On success fills *sector (2048-byte units) and *size (bytes), returns 1. */
int dq8_archive_find(const Dq8Archive *a, const char *path,
                     uint32_t *sector, uint32_t *size);

/* Load a member fully into a malloc'd buffer (caller frees). */
void *dq8_archive_load(Dq8Archive *a, const char *path, size_t *out_size);

#endif /* DQ8_IO_ARCHIVE_H */
