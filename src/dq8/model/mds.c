#include <dq8/model/mds.h>
#include <dq8/io/pak.h>

#include <stdlib.h>
#include <string.h>

#define MDT_HEADER 0x10

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_mds(const uint8_t *d, size_t n)
{
    return n >= 4 && d[0] == ' ' && d[1] == 'M' && d[2] == 'D' && d[3] == 'S';
}

static int is_mdt(const uint8_t *d, size_t n)
{
    return n >= 4 && d[0] == ' ' && d[1] == 'M' && d[2] == 'D' && d[3] == 'T';
}

Dq8Mds *dq8_mds_open(const uint8_t *data, size_t size)
{
    if (!is_mds(data, size) || size < 0x28) {
        return NULL;
    }
    uint32_t bone_count = rd32(data + 0x0c);
    uint32_t mesh_count = rd32(data + 0x20);
    uint32_t mesh_off = rd32(data + 0x24);
    if (mesh_count > DQ8_MDS_MAX_MESHES || (mesh_count && mesh_off >= size)) {  /* *_base1.mds: skeleton only */
        return NULL;
    }

    Dq8Mds *m = calloc(1, sizeof(*m));
    m->blob = malloc(size);
    memcpy(m->blob, data, size);
    m->blob_size = size;
    m->bone_count = bone_count;
    m->mesh_count = (int)mesh_count;

    uint32_t off = mesh_off;
    for (uint32_t i = 0; i < mesh_count; i++) {
        if (off + MDT_HEADER > size || !is_mdt(m->blob + off, size - off)) {
            m->mesh_count = (int)i;
            break;
        }
        Dq8MdsMesh *mesh = &m->meshes[i];
        mesh->offset = off;
        mesh->header_size = rd32(m->blob + off + 0x08);
        mesh->data_size = rd32(m->blob + off + 0x0c);
        mesh->format = rd32(m->blob + off + 0x10);
        mesh->pos_size = (uint8_t)(mesh->format & 0xFF);
        mesh->normal_size = (uint8_t)((mesh->format >> 8) & 0xFF);
        mesh->uv_size = (uint8_t)((mesh->format >> 16) & 0xFF);
        mesh->color_size = (uint8_t)((mesh->format >> 24) & 0xFF);
        mesh->data = m->blob + off;
        /* descriptor follows the header: counts + offsets (relative to the
         * descriptor). */
        /* per-axis dequantization scales (see the MDS format notes section 3.3) */
        if (off + 0x58 <= size) {
            memcpy(mesh->pos_scale, m->blob + off + 0x20, sizeof(mesh->pos_scale));
            memcpy(mesh->uv_scale, m->blob + off + 0x50, sizeof(mesh->uv_scale));
            memcpy(mesh->nrm_scale, m->blob + off + 0x40, sizeof(mesh->nrm_scale));
        }
        if (off + mesh->header_size + 0x18 <= size) {
            const uint8_t *d = m->blob + off + mesh->header_size;
            mesh->vertex_count = rd32(d + 0x00);
            mesh->vertex_offset = rd32(d + 0x04);
            mesh->normal_count = rd32(d + 0x08);
            mesh->normal_offset = rd32(d + 0x0c);
            mesh->uv_count = rd32(d + 0x10);
            mesh->uv_offset = rd32(d + 0x14);
        }
        if (off + mesh->header_size + 0x2c <= size) {
            const uint8_t *d = m->blob + off + mesh->header_size;
            size_t base = (size_t)off + mesh->header_size;
            size_t bytes = (size_t)mesh->vertex_count * rd32(d + 0x20) * 2;
            /* skin arrays (offsets relative to the descriptor), validated */
            if (rd32(d + 0x20) <= 4 && base + rd32(d + 0x24) + bytes <= size
                && base + rd32(d + 0x28) + bytes <= size) {
                mesh->influences = rd32(d + 0x20);
                mesh->weight_offset = rd32(d + 0x24);
                mesh->bone_index_offset = rd32(d + 0x28);
            }
        }
        if (mesh->data_size == 0) {
            m->mesh_count = (int)i + 1;
            break;
        }
        off += mesh->data_size;
    }
    return m;
}

/* Recursively descend PAK members looking for a `.mds`. */
static const uint8_t *find_mds(const uint8_t *data, size_t size, size_t *out_size, int depth)
{
    if (depth > 4 || size < 4) {
        return NULL;
    }
    if (is_mds(data, size)) {
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
        found = find_mds(e->data, e->size, out_size, depth + 1);
    }
    dq8_pak_close(pak);
    return found;
}

Dq8Mds *dq8_mds_load_archive(Dq8Archive *archive, const char *container_path)
{
    size_t size = 0;
    uint8_t *data = dq8_archive_load(archive, container_path, &size);
    if (!data) {
        return NULL;
    }
    size_t mds_size = 0;
    const uint8_t *mds = find_mds(data, size, &mds_size, 0);
    Dq8Mds *m = mds ? dq8_mds_open(mds, mds_size) : NULL;
    free(data);
    return m;
}

void dq8_mds_free(Dq8Mds *m)
{
    if (m) {
        free(m->blob);
        free(m);
    }
}

int dq8_mds_mesh_count(const Dq8Mds *m)
{
    return m ? m->mesh_count : 0;
}

const Dq8MdsMesh *dq8_mds_mesh(const Dq8Mds *m, int index)
{
    if (!m || index < 0 || index >= m->mesh_count) {
        return NULL;
    }
    return &m->meshes[index];
}

const uint8_t *dq8_mds_blob(const Dq8Mds *m, size_t *out_size)
{
    if (!m) {
        return NULL;
    }
    if (out_size) {
        *out_size = m->blob_size;
    }
    return m->blob;
}

/* The geometry descriptor sits at `mesh + *(mesh+0x60)` (== mesh+0x90); its
 * offset fields are relative to the descriptor. Component sizes come from the
 * format word: positions 3x (int16 | float32 | int8), UVs 2x (u8 | float32). */

static size_t mesh_descriptor(const Dq8Mds *m, const Dq8MdsMesh *mesh)
{
    return (size_t)mesh->offset + rd32(m->blob + mesh->offset + 0x60);
}

/* One quantized component per the format word's size code (the MDS format notes 3.1):
 * 0x20 float32 (as stored), 0x10 int16 (raw * scale / 32768), 0x08 int8
 * (raw * scale / 128). `is_unsigned` for u8 UVs. */
static int comp_bytes(uint8_t code)
{
    return code == 0x20 ? 4 : code == 0x10 ? 2 : code == 0x08 ? 1 : 0;
}

static float read_comp(const uint8_t *p, uint8_t code, float scale, int is_unsigned)
{
    switch (code) {
    case 0x20: {
        float f;
        memcpy(&f, p, 4);
        return f;
    }
    case 0x10:
        return (is_unsigned ? (float)(uint16_t)(p[0] | (p[1] << 8))
                            : (float)(int16_t)(p[0] | (p[1] << 8))) * scale / 32768.0f;
    default:
        return (is_unsigned ? (float)p[0] : (float)(int8_t)p[0]) * scale / 128.0f;
    }
}

bool dq8_mds_vertex(const Dq8Mds *m, const Dq8MdsMesh *mesh, int i, float out[4])
{
    int cb = mesh ? comp_bytes(mesh->pos_size) : 0;
    if (!m || !mesh || !out || i < 0 || (uint32_t)i >= mesh->vertex_count || !cb) {
        return false;
    }
    size_t off = mesh_descriptor(m, mesh) + mesh->vertex_offset + (size_t)i * 3 * cb;
    if (off + 3 * (size_t)cb > m->blob_size) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        out[k] = read_comp(m->blob + off + k * cb, mesh->pos_size, mesh->pos_scale[k], 0);
    }
    out[3] = 1.0f;
    return true;
}

/* Bone record (0xA0): +0x0C owned mesh index, +0x10 parent, +0x20 local 4×4
 * (row-major, row vectors). World = Local · World[parent]. */
static void bone_world(const Dq8Mds *m, int32_t b, float out[16], int depth)
{
    uint32_t stride = rd32(m->blob + 0x14), base = rd32(m->blob + 0x18);
    size_t o = base + (size_t)b * stride;
    float local[16];
    memcpy(local, m->blob + o + 0x20, sizeof(local));
    int32_t parent = (int32_t)rd32(m->blob + o + 0x10);
    if (parent < 0 || (uint32_t)parent >= m->bone_count || depth > 256) {
        memcpy(out, local, sizeof(local));
        return;
    }
    float pw[16];
    bone_world(m, parent, pw, depth + 1);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) {
                s += local[r * 4 + k] * pw[k * 4 + c];
            }
            out[r * 4 + c] = s;
        }
    }
}

static bool bones_valid(const Dq8Mds *m)
{
    uint32_t stride = rd32(m->blob + 0x14), base = rd32(m->blob + 0x18);
    return stride >= 0xa0 && (size_t)base + (size_t)m->bone_count * stride <= m->blob_size;
}

static const uint8_t *bone_rec(const Dq8Mds *m, int b)
{
    return m->blob + rd32(m->blob + 0x18) + (size_t)b * rd32(m->blob + 0x14);
}

const char *dq8_mds_bone_name(const Dq8Mds *m, int bone)
{
    if (!m || bone < 0 || (uint32_t)bone >= m->bone_count || !bones_valid(m)) {
        return "";
    }
    size_t st = rd32(m->blob + 0x38), sz = rd32(m->blob + 0x3c);
    uint32_t off = rd32(bone_rec(m, bone));
    if (st + sz > m->blob_size || off >= sz || !memchr(m->blob + st + off, 0, sz - off)) {
        return "";
    }
    return (const char *)m->blob + st + off;
}

int dq8_mds_bone_find(const Dq8Mds *m, const char *name)
{
    for (uint32_t b = 0; m && name && *name && b < m->bone_count; b++) {
        if (strcmp(dq8_mds_bone_name(m, (int)b), name) == 0) {
            return (int)b;
        }
    }
    return -1;
}

int dq8_mds_bone_parent(const Dq8Mds *m, int bone)
{
    if (!m || bone < 0 || (uint32_t)bone >= m->bone_count || !bones_valid(m)) {
        return -1;
    }
    int32_t p = (int32_t)rd32(bone_rec(m, bone) + 0x10);
    return (p >= 0 && (uint32_t)p < m->bone_count) ? p : -1;
}

unsigned dq8_mds_bone_flags(const Dq8Mds *m, int bone)
{
    if (!m || bone < 0 || (uint32_t)bone >= m->bone_count || !bones_valid(m)) {
        return 0;
    }
    return rd32(bone_rec(m, bone) + 0x14) & 0xffff;
}

bool dq8_mds_bone_local(const Dq8Mds *m, int bone, float out[16])
{
    if (!m || !out || bone < 0 || (uint32_t)bone >= m->bone_count || !bones_valid(m)) {
        return false;
    }
    memcpy(out, bone_rec(m, bone) + 0x20, 16 * sizeof(float));
    return true;
}

bool dq8_mds_bone_world(const Dq8Mds *m, int bone, float out[16])
{
    if (!m || !out || bone < 0 || (uint32_t)bone >= m->bone_count || !bones_valid(m)) {
        return false;
    }
    bone_world(m, bone, out, 0);
    return true;
}

bool dq8_mds_bone_skin_bind(const Dq8Mds *m, int bone, float out[16])
{
    if (!dq8_mds_bone_world(m, bone, out)) {
        return false;
    }
    float w;
    memcpy(&w, bone_rec(m, bone) + 0x9c, 4);
    if (w == 1.0f) {
        memcpy(out, bone_rec(m, bone) + 0x60, 16 * sizeof(float));
    }
    return true;
}

int dq8_mds_mesh_owner(const Dq8Mds *m, int mesh_index)
{
    if (!m || mesh_index < 0 || mesh_index >= m->mesh_count || !bones_valid(m)) {
        return -1;
    }
    for (uint32_t b = 0; b < m->bone_count; b++) {
        if ((int32_t)rd32(bone_rec(m, (int)b) + 0x0c) == mesh_index) {
            return (int)b;
        }
    }
    return -1;
}

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int dq8_mds_vertex_weights(const Dq8Mds *m, const Dq8MdsMesh *mesh, int i,
                           int bones[4], float weights[4])
{
    if (!m || !mesh || i < 0 || (uint32_t)i >= mesh->vertex_count) {
        return -1;
    }
    size_t desc = (size_t)mesh->offset + mesh->header_size;
    int n = 0;
    for (uint32_t k = 0; k < mesh->influences; k++) {
        size_t at = ((size_t)i * mesh->influences + k) * 2;
        uint16_t b = rd16le(m->blob + desc + mesh->bone_index_offset + at);
        uint16_t w = rd16le(m->blob + desc + mesh->weight_offset + at);
        if (b != 0xFFFF && w != 0 && b < m->bone_count) {
            bones[n] = b;
            weights[n] = w / 32768.0f;
            n++;
        }
    }
    return n;
}

static void apply(const float p[3], const float *M, float out[3])
{
    for (int c = 0; c < 3; c++) {
        out[c] = p[0] * M[c] + p[1] * M[4 + c] + p[2] * M[8 + c] + M[12 + c];
    }
}

/* Inverse of an affine row-vector matrix (last column 0,0,0,1). */
static void inverse_affine(const float *M, float *out)
{
    float a = M[0], b = M[1], c = M[2], d = M[4], e = M[5], f = M[6], g = M[8], h = M[9], k = M[10];
    float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    float s = det != 0 ? 1.0f / det : 0;
    float R[9] = {
        (e * k - f * h) * s, (c * h - b * k) * s, (b * f - c * e) * s,
        (f * g - d * k) * s, (a * k - c * g) * s, (c * d - a * f) * s,
        (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s,
    };
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            out[r * 4 + col] = R[r * 3 + col];
        }
        out[r * 4 + 3] = 0;
    }
    for (int col = 0; col < 3; col++) {
        out[12 + col] = -(M[12] * R[col] + M[13] * R[3 + col] + M[14] * R[6 + col]);
    }
    out[15] = 1;
}

static void pose_of(const Dq8Mds *m, const float *pose, int b, float out[16])
{
    if (pose) {
        memcpy(out, pose + (size_t)b * 16, 16 * sizeof(float));
    } else {
        dq8_mds_bone_world(m, b, out);
    }
}

bool dq8_mds_skin_vertex(const Dq8Mds *m, int mesh_index, int i, const float *pose,
                         float out[3])
{
    const Dq8MdsMesh *mesh = dq8_mds_mesh(m, mesh_index);
    float v[4], M[16];
    if (!out || !mesh || !dq8_mds_vertex(m, mesh, i, v)) {
        return false;
    }
    int owner = dq8_mds_mesh_owner(m, mesh_index);
    if (mesh->influences == 0) {
        if (owner < 0) {
            memcpy(out, v, 3 * sizeof(float));
            return true;
        }
        pose_of(m, pose, owner, M);
        apply(v, M, out);
        return true;
    }
    /* skinned: owner space at skin-bind time, then blend bone deltas */
    float q[3];
    if (owner >= 0) {
        dq8_mds_bone_skin_bind(m, owner, M);
        apply(v, M, q);
    } else {
        memcpy(q, v, sizeof(q));
    }
    int bones[4];
    float w[4];
    int n = dq8_mds_vertex_weights(m, mesh, i, bones, w);
    out[0] = out[1] = out[2] = 0;
    for (int k = 0; k < n; k++) {
        float bind[16], inv[16], P[16], local[3], posed[3];
        dq8_mds_bone_skin_bind(m, bones[k], bind);
        inverse_affine(bind, inv);
        apply(q, inv, local);
        pose_of(m, pose, bones[k], P);
        apply(local, P, posed);
        for (int c = 0; c < 3; c++) {
            out[c] += w[k] * posed[c];
        }
    }
    return n > 0;
}

bool dq8_mds_mesh_world(const Dq8Mds *m, int mesh_index, float out[16])
{
    if (!m || !out || mesh_index < 0 || mesh_index >= m->mesh_count) {
        return false;
    }
    uint32_t stride = rd32(m->blob + 0x14), base = rd32(m->blob + 0x18);
    if ((size_t)base + (size_t)m->bone_count * stride > m->blob_size || stride < 0x60) {
        return false;
    }
    for (uint32_t b = 0; b < m->bone_count; b++) {
        if ((int32_t)rd32(m->blob + base + (size_t)b * stride + 0x0c) == mesh_index) {
            bone_world(m, (int32_t)b, out, 0);
            return true;
        }
    }
    return false;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

bool dq8_mds_uv(const Dq8Mds *m, const Dq8MdsMesh *mesh, int i, float out[2])
{
    if (!m || !mesh || !out || i < 0 || (uint32_t)i >= mesh->uv_count) {
        return false;
    }
    int cb = comp_bytes(mesh->uv_size);
    size_t off = mesh_descriptor(m, mesh) + mesh->uv_offset + (size_t)i * 2 * cb;
    if (!cb || off + 2 * (size_t)cb > m->blob_size) {
        return false;
    }
    out[0] = read_comp(m->blob + off, mesh->uv_size, mesh->uv_scale[0], 1);
    out[1] = read_comp(m->blob + off + cb, mesh->uv_size, mesh->uv_scale[1], 1);
    return true;
}

/* Primitive styles (node+4, low u16), matching GL/Dark-Cloud: 3 = triangle
 * list, 4 = triangle strip. */
#define MDS_PRIM_TRIANGLES 3
#define MDS_PRIM_STRIP     4

/* Walk the mesh's primitive list. `*state` starts at MDS_IT_INIT; each call
 * yields the next node's (style, indices, count) and advances `*state`. */
#define MDS_IT_INIT ((size_t)-1)

static int each_strip(const Dq8Mds *m, const Dq8MdsMesh *mesh, size_t *state,
                      uint16_t *style, const uint8_t **indices, int *count,
                      const uint8_t **uv)
{
    if (!m || !mesh || !state) {
        return 0;
    }
    size_t node;
    if (*state == MDS_IT_INIT) {
        uint32_t head = rd32(m->blob + mesh->offset + 0x74);
        node = mesh->offset + head;
    } else {
        node = *state;
    }
    if (node == 0 || node + 0x14 > m->blob_size) {
        *state = 0;
        return 0;
    }
    uint32_t next = rd32(m->blob + node + 0x00);
    uint32_t idxrel = rd32(m->blob + node + 0x10);
    *style = rd16(m->blob + node + 0x04);
    uint16_t cnt = rd16(m->blob + node + 0x06);
    size_t ip = node + idxrel;
    /* Only plain lists/strips are mesh surface. Styles with bit 0x10 (19 =
     * 0x10|3 triangle list, 17 = 0x10|1 line list) are the toon-outline pass
     * (no UVs/normals, mode-1 materials); see the MDS format notes section 3.4. */
    if (ip + (size_t)cnt * 2 > m->blob_size
        || (*style != MDS_PRIM_TRIANGLES && *style != MDS_PRIM_STRIP)) {
        cnt = 0;
    }
    *indices = m->blob + ip;
    *count = (int)cnt;
    /* uv indices live in the node's entry array: entry = node + *(node+0x0c),
     * uv array = node + *(entry+4). */
    if (uv) {
        size_t ent = node + rd32(m->blob + node + 0x0c);
        uint32_t rel = rd32(m->blob + ent + 4);
        *uv = (rel && node + rel + (size_t)cnt * 2 <= m->blob_size)
            ? m->blob + node + rel : NULL;
    }
    *state = next ? node + next : 0;
    return 1;
}

static int prim_triangle_count(uint16_t style, int cnt)
{
    if (cnt < 3) {
        return 0;
    }
    if (style == MDS_PRIM_TRIANGLES) {
        return cnt / 3;
    }
    return cnt - 2;   /* strip (and default) */
}

int dq8_mds_triangle_count(const Dq8Mds *m, const Dq8MdsMesh *mesh)
{
    size_t state = MDS_IT_INIT;
    uint16_t style;
    const uint8_t *idx;
    int cnt;
    int tris = 0;
    int guard = 0;
    while (guard++ < 256 && each_strip(m, mesh, &state, &style, &idx, &cnt, NULL)) {
        tris += prim_triangle_count(style, cnt);
    }
    return tris;
}

int dq8_mds_triangles(const Dq8Mds *m, const Dq8MdsMesh *mesh,
                      uint16_t *out, int max)
{
    size_t state = MDS_IT_INIT;
    uint16_t style;
    const uint8_t *idx;
    int cnt;
    int n = 0;
    int guard = 0;
    while (guard++ < 256 && each_strip(m, mesh, &state, &style, &idx, &cnt, NULL)) {
        if (cnt < 3) {
            continue;
        }
        if (style == MDS_PRIM_TRIANGLES) {
            for (int k = 0; k + 2 < cnt; k += 3) {
                if (n >= max) {
                    return n;
                }
                out[n * 3 + 0] = rd16(idx + (size_t)k * 2);
                out[n * 3 + 1] = rd16(idx + (size_t)(k + 1) * 2);
                out[n * 3 + 2] = rd16(idx + (size_t)(k + 2) * 2);
                n++;
            }
        } else {
            for (int k = 0; k + 2 < cnt; k++) {
                if (n >= max) {
                    return n;
                }
                out[n * 3 + 0] = rd16(idx + (size_t)k * 2);
                out[n * 3 + 1] = rd16(idx + (size_t)(k + 1) * 2);
                out[n * 3 + 2] = rd16(idx + (size_t)(k + 2) * 2);
                n++;
            }
        }
    }
    return n;
}

int dq8_mds_triangles_uv(const Dq8Mds *m, const Dq8MdsMesh *mesh,
                         uint16_t *out, int max)
{
    size_t state = MDS_IT_INIT;
    uint16_t style;
    const uint8_t *idx, *uv;
    int cnt;
    int n = 0;
    int guard = 0;
    while (guard++ < 256
           && each_strip(m, mesh, &state, &style, &idx, &cnt, &uv)) {
        if (cnt < 3 || !uv) {
            continue;
        }
        int step = (style == MDS_PRIM_TRIANGLES) ? 3 : 1;
        for (int k = 0; k + 2 < cnt; k += step) {
            if (n >= max) {
                return n;
            }
            for (int e = 0; e < 3; e++) {
                out[n * 6 + e] = rd16(idx + (size_t)(k + e) * 2);
                out[n * 6 + 3 + e] = rd16(uv + (size_t)(k + e) * 2);
            }
            n++;
        }
    }
    return n;
}

int dq8_mds_faces(const Dq8Mds *m, const Dq8MdsMesh *mesh, Dq8MdsFace *out, int max)
{
    if (!m || !mesh) {
        return 0;
    }
    size_t node = (size_t)mesh->offset + rd32(m->blob + mesh->offset + 0x74);
    int n = 0;
    for (int guard = 0; node && node + 0x14 <= m->blob_size && guard < 256; guard++) {
        uint32_t next = rd32(m->blob + node);
        uint16_t style = rd16(m->blob + node + 4), cnt = rd16(m->blob + node + 6);
        uint8_t entries = m->blob[node + 8];
        size_t ent = node + rd32(m->blob + node + 0x0c);
        size_t vi = node + rd32(m->blob + node + 0x10);
        uint32_t uvrel = ent + 16 <= m->blob_size ? rd32(m->blob + ent + 4) : 0;
        uint32_t nrel = ent + 16 <= m->blob_size ? rd32(m->blob + ent + 8) : 0;
        size_t ui = node + uvrel, ni = node + nrel;
        int env_material = -1;   /* second entry: no UVs, environment-mapped */
        for (int e = 1; e < entries && ent + 16 * (size_t)(e + 1) <= m->blob_size; e++) {
            int mat = (int)rd32(m->blob + ent + 16 * (size_t)e);
            if (rd32(m->blob + ent + 16 * (size_t)e + 4) == 0
                && dq8_mds_material_mapping(m, mat) == DQ8_MDS_MAP_ENV) {
                env_material = mat;
                break;
            }
        }
        int has_n = nrel && ni + (size_t)cnt * 2 <= m->blob_size;
        if ((style == MDS_PRIM_TRIANGLES || style == MDS_PRIM_STRIP) && entries && uvrel
            && cnt >= 3 && vi + (size_t)cnt * 2 <= m->blob_size
            && ui + (size_t)cnt * 2 <= m->blob_size) {
            int material = (int)rd32(m->blob + ent);
            int step = style == MDS_PRIM_TRIANGLES ? 3 : 1;
            for (int k = 0; k + 2 < cnt; k += step) {
                int c[3] = { k, k + 1, k + 2 };
                if (style == MDS_PRIM_STRIP && (k & 1)) {
                    c[1] = k + 2;
                    c[2] = k + 1;
                }
                uint16_t a = rd16(m->blob + vi + c[0] * 2), b = rd16(m->blob + vi + c[1] * 2),
                         d = rd16(m->blob + vi + c[2] * 2);
                if (a == b || b == d || a == d || a >= mesh->vertex_count
                    || b >= mesh->vertex_count || d >= mesh->vertex_count) {
                    continue;
                }
                if (out && n < max) {
                    Dq8MdsFace *f = &out[n];
                    f->v[0] = a;
                    f->v[1] = b;
                    f->v[2] = d;
                    for (int e = 0; e < 3; e++) {
                        f->uv[e] = rd16(m->blob + ui + c[e] * 2);
                        f->n[e] = has_n ? rd16(m->blob + ni + c[e] * 2) : 0xFFFF;
                    }
                    f->material = material;
                    f->env_material = env_material;
                }
                n++;
            }
        }
        node = next ? node + next : 0;
    }
    return n;
}

const char *dq8_mds_material_texture(const Dq8Mds *m, int index)
{
    if (!m || index < 0 || (uint32_t)index >= rd32(m->blob + 0x2c)) {
        return NULL;
    }
    size_t rec = rd32(m->blob + 0x34) + (size_t)index * rd32(m->blob + 0x30);
    size_t st = rd32(m->blob + 0x38), sz = rd32(m->blob + 0x3c);
    if (rec + 0x38 > m->blob_size || st + sz > m->blob_size) {
        return NULL;
    }
    uint32_t off = rd32(m->blob + rec + 0x34);
    if (off >= sz || !memchr(m->blob + st + off, 0, sz - off)) {
        return "";
    }
    return (const char *)m->blob + st + off;
}

bool dq8_mds_material_color(const Dq8Mds *m, int index, float rgba[4])
{
    if (!m || !rgba || index < 0 || (uint32_t)index >= rd32(m->blob + 0x2c)) {
        return false;
    }
    size_t rec = rd32(m->blob + 0x34) + (size_t)index * rd32(m->blob + 0x30);
    if (rec + 16 > m->blob_size) {
        return false;
    }
    memcpy(rgba, m->blob + rec, 4 * sizeof(float));
    return true;
}

uint32_t dq8_mds_material_flags(const Dq8Mds *m, int index)
{
    if (!m || index < 0 || (uint32_t)index >= rd32(m->blob + 0x2c)) {
        return 0;
    }
    size_t rec = rd32(m->blob + 0x34) + (size_t)index * rd32(m->blob + 0x30);
    return rec + 0x48 <= m->blob_size ? rd32(m->blob + rec + 0x44) : 0;
}

int dq8_mds_material_mapping(const Dq8Mds *m, int index)
{
    if (!m || index < 0 || (uint32_t)index >= rd32(m->blob + 0x2c)) {
        return -1;
    }
    size_t rec = rd32(m->blob + 0x34) + (size_t)index * rd32(m->blob + 0x30);
    return rec + 0x44 <= m->blob_size ? (int)rd32(m->blob + rec + 0x40) : -1;
}

bool dq8_mds_normal(const Dq8Mds *m, const Dq8MdsMesh *mesh, int i, float out[3])
{
    int cb = mesh ? comp_bytes(mesh->normal_size) : 0;
    if (!m || !mesh || !out || i < 0 || (uint32_t)i >= mesh->normal_count || !cb) {
        return false;
    }
    size_t off = mesh_descriptor(m, mesh) + mesh->normal_offset + (size_t)i * 3 * cb;
    if (off + 3 * (size_t)cb > m->blob_size) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        out[k] = read_comp(m->blob + off + k * cb, mesh->normal_size, mesh->nrm_scale[k], 0);
    }
    return true;
}

static void apply_dir(const float d[3], const float *M, float out[3])
{
    for (int c = 0; c < 3; c++) {
        out[c] = d[0] * M[c] + d[1] * M[4 + c] + d[2] * M[8 + c];
    }
}

bool dq8_mds_skin_direction(const Dq8Mds *m, int mesh_index, int i, const float dir[3],
                            const float *pose, float out[3])
{
    const Dq8MdsMesh *mesh = dq8_mds_mesh(m, mesh_index);
    if (!mesh || !dir || !out || i < 0 || (uint32_t)i >= mesh->vertex_count) {
        return false;
    }
    float M[16];
    int owner = dq8_mds_mesh_owner(m, mesh_index);
    if (mesh->influences == 0) {
        if (owner < 0) {
            memcpy(out, dir, 3 * sizeof(float));
            return true;
        }
        pose_of(m, pose, owner, M);
        apply_dir(dir, M, out);
        return true;
    }
    float q[3];
    if (owner >= 0) {
        dq8_mds_bone_skin_bind(m, owner, M);
        apply_dir(dir, M, q);
    } else {
        memcpy(q, dir, sizeof(q));
    }
    int bones[4];
    float w[4];
    int n = dq8_mds_vertex_weights(m, mesh, i, bones, w);
    out[0] = out[1] = out[2] = 0;
    for (int k = 0; k < n; k++) {
        float bind[16], inv[16], P[16], local[3], posed[3];
        dq8_mds_bone_skin_bind(m, bones[k], bind);
        inverse_affine(bind, inv);
        apply_dir(q, inv, local);
        pose_of(m, pose, bones[k], P);
        apply_dir(local, P, posed);
        for (int c = 0; c < 3; c++) {
            out[c] += w[k] * posed[c];
        }
    }
    return n > 0;
}
