#ifndef DQ8_IO_VFS_H
#define DQ8_IO_VFS_H

#include <stddef.h>
#include <stdint.h>

/* Virtual filesystem over the game disc.
 *
 * Two backings:
 *   - a host directory (extracted data / development),
 *   - an ISO9660 image (the player only supplies the ISO).
 *
 * Game paths are PS2-style (`DATA.HD6;1`, `MODULES\IOPRP300.IMG;1`,
 * `cdrom0:\BIN\TITLE.BIN;1`); dq8_vfs_normalize() turns them into plain
 * `A/B/C` paths. */

void dq8_vfs_init(void);
int  dq8_vfs_mount_dir(const char *dir);
int  dq8_vfs_mount_iso(const char *iso_path);
void dq8_vfs_unmount(void);

/* Normalise a PS2 path (strip cdrom0:/leading slashes/`;N`, `\` -> `/`). */
char *dq8_vfs_normalize(const char *game_path, char *out, size_t outsz);

int     dq8_vfs_exists(const char *game_path);
int64_t dq8_vfs_size(const char *game_path);
void   *dq8_vfs_load(const char *game_path, size_t *out_size);

/* Streaming handle (reads without loading the whole file — needed for the
 * multi-GB .DAT archives). */
typedef struct Dq8VfsFile Dq8VfsFile;
Dq8VfsFile *dq8_vfs_open(const char *game_path);
int64_t     dq8_vfs_file_size(Dq8VfsFile *f);
int         dq8_vfs_read(Dq8VfsFile *f, void *buf, size_t n);   /* bytes read */
int         dq8_vfs_seek(Dq8VfsFile *f, int64_t offset);        /* absolute */
void        dq8_vfs_close(Dq8VfsFile *f);

#endif /* DQ8_IO_VFS_H */
