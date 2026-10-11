#ifndef DQ8_IO_FILE_H
#define DQ8_IO_FILE_H

#include <stddef.h>
#include <stdint.h>

/* Host file I/O for the reimplementation.
 *
 * The original game reaches the disc through the PS2 CDVD driver and the SDK
 * file wrappers (`sceCdSearchFile`/`sceCdRead`/…). On the host these become
 * ordinary stdio-backed operations. The archive layer (see the format notes)
 * uses these to read `DATA.DAT` members. */

typedef struct Dq8File Dq8File;

/* mode follows fopen ("rb", "wb", ...). Returns NULL on failure. */
Dq8File *dq8_file_open(const char *path, const char *mode);
void dq8_file_close(Dq8File *f);

size_t dq8_file_read(Dq8File *f, void *buf, size_t n);
size_t dq8_file_write(Dq8File *f, const void *buf, size_t n);

/* whence: 0 = SET, 1 = CUR, 2 = END. Returns 0 on success. */
int dq8_file_seek(Dq8File *f, int64_t offset, int whence);
int64_t dq8_file_tell(Dq8File *f);
int64_t dq8_file_size(Dq8File *f);

/* Read an entire file into a malloc'd buffer (caller frees). Sets *out_size. */
void *dq8_file_load(const char *path, size_t *out_size);

#endif /* DQ8_IO_FILE_H */
