#ifndef DQ8_IO_PAK_H
#define DQ8_IO_PAK_H

#include <stddef.h>
#include <stdint.h>

/* DQ8 PAK-family container (`.pak .mpk .pcp .ipk .nav .sky .snd .pac .chr`).
 *
 * Cross-checked against Fire-Cube/PakTools.py (see the REFERENCES format notes). A PAK
 * is a chain of 80-byte headers:
 *     name[64], i32 relative_offset, i32 size, i32 next_header_offset, i32 version
 * A member's bytes are at `headerStart + relative_offset`; the next header is at
 * `headerStart + next_header_offset`. Iterate until an empty name or
 * `next_header_offset <= 0`. Members can nest (an `.mpk` holds `.pcp`s).
 *
 * The parser works on a memory image (load the container from the archive); it
 * does not copy or own the bytes. */

typedef struct {
    char           name[64];
    const uint8_t *data;
    uint32_t       size;
    int32_t        version;
} Dq8PakEntry;

typedef struct Dq8Pak Dq8Pak;

/* Parse `data`/`size` (both must stay valid while the Dq8Pak is used). */
Dq8Pak *dq8_pak_open(const uint8_t *data, size_t size);
void dq8_pak_close(Dq8Pak *p);

int dq8_pak_count(const Dq8Pak *p);
const Dq8PakEntry *dq8_pak_entry(const Dq8Pak *p, int index);

/* Find a member by name (exact). NULL if absent. */
const Dq8PakEntry *dq8_pak_find(const Dq8Pak *p, const char *name);

#endif /* DQ8_IO_PAK_H */
