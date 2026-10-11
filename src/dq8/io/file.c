#include <dq8/io/file.h>

#include <stdio.h>
#include <stdlib.h>

/* 64-bit seeking: MinGW's off_t is 32-bit by default and overflows on the
 * multi-GB .DAT archives. */
#if defined(_WIN32)
#include <windows.h>
#define DQ8_FSEEK _fseeki64
#define DQ8_FTELL _ftelli64
#else
#define DQ8_FSEEK fseeko
#define DQ8_FTELL ftello
#endif

struct Dq8File {
    FILE *fp;
};

/* Paths are UTF-8; Windows' fopen expects the ANSI code page. */
static FILE *open_utf8(const char *path, const char *mode)
{
#if defined(_WIN32)
    wchar_t wpath[1024], wmode[8];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) > 0
        && MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8) > 0) {
        return _wfopen(wpath, wmode);
    }
#endif
    return fopen(path, mode);
}

Dq8File *dq8_file_open(const char *path, const char *mode)
{
    FILE *fp = open_utf8(path, mode);
    if (!fp) {
        return NULL;
    }
    Dq8File *f = malloc(sizeof(*f));
    if (!f) {
        fclose(fp);
        return NULL;
    }
    f->fp = fp;
    return f;
}

void dq8_file_close(Dq8File *f)
{
    if (f) {
        fclose(f->fp);
        free(f);
    }
}

size_t dq8_file_read(Dq8File *f, void *buf, size_t n)
{
    return f ? fread(buf, 1, n, f->fp) : 0;
}

size_t dq8_file_write(Dq8File *f, const void *buf, size_t n)
{
    return f ? fwrite(buf, 1, n, f->fp) : 0;
}

int dq8_file_seek(Dq8File *f, int64_t offset, int whence)
{
    if (!f) {
        return -1;
    }
    int w = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
    return DQ8_FSEEK(f->fp, offset, w);
}

int64_t dq8_file_tell(Dq8File *f)
{
    return f ? (int64_t)DQ8_FTELL(f->fp) : -1;
}

int64_t dq8_file_size(Dq8File *f)
{
    if (!f) {
        return -1;
    }
    int64_t cur = (int64_t)DQ8_FTELL(f->fp);
    if (DQ8_FSEEK(f->fp, 0, SEEK_END) != 0) {
        return -1;
    }
    int64_t end = (int64_t)DQ8_FTELL(f->fp);
    DQ8_FSEEK(f->fp, cur, SEEK_SET);
    return end;
}

void *dq8_file_load(const char *path, size_t *out_size)
{
    Dq8File *f = dq8_file_open(path, "rb");
    if (!f) {
        return NULL;
    }
    int64_t sz = dq8_file_size(f);
    if (sz < 0) {
        dq8_file_close(f);
        return NULL;
    }
    void *buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) {
        dq8_file_close(f);
        return NULL;
    }
    dq8_file_seek(f, 0, 0);
    size_t got = dq8_file_read(f, buf, (size_t)sz);
    dq8_file_close(f);
    if (out_size) {
        *out_size = got;
    }
    return buf;
}
