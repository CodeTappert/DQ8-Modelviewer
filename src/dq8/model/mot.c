#include <dq8/model/mot.h>
#include <dq8/io/pak.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int components(int type)
{
    switch (type) {
    case DQ8_MOT_ROTATE: return 4;
    case DQ8_MOT_TRANSLATE:
    case DQ8_MOT_SCALE:
    case DQ8_MOT_PARAM3: return 3;
    case DQ8_MOT_ALPHA:
    case DQ8_MOT_VISIBLE: return 1;
    default: return 0;
    }
}

/* Channel header at `ch` (offsets inside are relative to `ch`). Version 1 is
 * 0x20 bytes: u8 flags[4] (scaled, value size, time size, 0), s16, u16 count,
 * u32 times, u32 values, f32[4] scale. Version 0 (converted in place by
 * FUN_0014c310) is 0x40 bytes of u32 words: flags at +0x04/+0x08/+0x0C, count
 * +0x18, times +0x1C, values +0x20, scale at +0x30. Value size codes match the
 * MDS format word: 0x20 float32, 0x10 int16, 0x08 int8. */
static bool read_channel(Dq8Mot *mot, int type, size_t ch, Dq8MotChannel *out)
{
    size_t hdr = mot->version == 0 ? 0x40 : 0x20;
    if (ch + hdr > mot->blob_size) {
        return false;
    }
    const uint8_t *p = mot->blob + ch;
    uint32_t count, times, values, scaled, vsize;
    const uint8_t *sc;
    if (mot->version == 0) {
        scaled = rd32(p + 0x04);
        vsize = rd32(p + 0x08);
        count = rd32(p + 0x18);
        times = rd32(p + 0x1c);
        values = rd32(p + 0x20);
        sc = p + 0x30;
    } else {
        scaled = p[0];
        vsize = p[1];
        count = rd16(p + 6);
        times = rd32(p + 8);
        values = rd32(p + 12);
        sc = p + 0x10;
    }
    if (vsize != 0x20 && vsize != 0x10 && vsize != 0x08) {
        return false;
    }
    size_t vbytes = (size_t)count * components(type) * (vsize / 8);
    if (count == 0 || values == 0 || ch + values + vbytes > mot->blob_size
        || (times && ch + times + (size_t)count * 2 > mot->blob_size)) {
        return false;
    }
    out->type = type;
    out->components = components(type);
    out->value_size = (int)vsize;
    out->scaled = scaled != 0;
    out->count = (int)count;
    out->times = times ? (uint32_t)(ch + times) : 0;
    out->values = (uint32_t)(ch + values);
    memcpy(out->scale, sc, sizeof(out->scale));
    int last = times ? (int16_t)rd16(mot->blob + out->times + (count - 1) * 2) : (int)count - 1;
    if (last + 1 > mot->frame_count) {
        mot->frame_count = last + 1;
    }
    return true;
}

Dq8Mot *dq8_mot_open(const uint8_t *data, size_t size)
{
    if (!data || size < 0x30 || memcmp(data, " MOT", 4) != 0) {
        return NULL;
    }
    Dq8Mot *mot = calloc(1, sizeof(*mot));
    if (!mot) {
        return NULL;
    }
    mot->blob = malloc(size);
    if (!mot->blob) {
        free(mot);
        return NULL;
    }
    memcpy(mot->blob, data, size);
    mot->blob_size = size;
    mot->version = (int)rd32(data + 4);

    /* Two track lists (header +0x20 = A, bones; +0x28 = B, objects); each is a
     * linked list of nodes: u32 next (rel), u32 index, u32 name/0,
     * u32 channelCount, then channelCount × {u32 type, u32 rel offset}. */
    static const size_t lists[2] = { 0x20, 0x28 };
    int cap = 0;
    for (int l = 0; l < 2; l++) {
        size_t node = rd32(data + lists[l]);
        int guard = 0;
        while (node && node + 16 <= size && guard++ < 65536) {
            uint32_t next = rd32(data + node);
            uint32_t nch = rd32(data + node + 12);
            if (mot->track_count == cap) {
                cap = cap ? cap * 2 : 64;
                Dq8MotTrack *grown = realloc(mot->tracks, (size_t)cap * sizeof(*grown));
                if (!grown) {
                    dq8_mot_free(mot);
                    return NULL;
                }
                mot->tracks = grown;
            }
            Dq8MotTrack *t = &mot->tracks[mot->track_count];
            t->list = l;
            t->bone = (int)rd32(data + node + 4);
            t->channel_count = 0;
            for (uint32_t c = 0; c < nch && node + 16 + c * 8 + 8 <= size; c++) {
                int type = (int)rd32(data + node + 16 + c * 8);
                uint32_t rel = rd32(data + node + 16 + c * 8 + 4);
                if (!rel || components(type) == 0
                    || t->channel_count >= DQ8_MOT_MAX_CHANNELS) {
                    continue;
                }
                if (read_channel(mot, type, node + rel, &t->channels[t->channel_count])) {
                    t->channel_count++;
                }
            }
            mot->track_count++;
            node = next ? node + next : 0;
        }
    }
    return mot;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && strcmp(s + n - k, suffix) == 0;
}

/* Descend only into `.chr` members: the PAK parser will happily "open" a MOT
 * blob as a PAK. */
static Dq8Mot *find_mot(const uint8_t *data, size_t size, int depth)
{
    Dq8Pak *pak = depth < 4 ? dq8_pak_open(data, size) : NULL;
    Dq8Mot *found = NULL;
    for (int i = 0; pak && i < dq8_pak_count(pak) && !found; i++) {
        const Dq8PakEntry *e = dq8_pak_entry(pak, i);
        if (ends_with(e->name, ".mot")) {
            found = dq8_mot_open(e->data, e->size);
        } else if (ends_with(e->name, ".chr")) {
            found = find_mot(e->data, e->size, depth + 1);
        }
    }
    dq8_pak_close(pak);
    return found;
}

Dq8Mot *dq8_mot_load_archive(Dq8Archive *archive, const char *container_path)
{
    size_t size = 0;
    uint8_t *data = dq8_archive_load(archive, container_path, &size);
    if (!data) {
        return NULL;
    }
    Dq8Mot *mot = find_mot(data, size, 0);
    free(data);
    return mot;
}

void dq8_mot_free(Dq8Mot *mot)
{
    if (mot) {
        free(mot->tracks);
        free(mot->blob);
        free(mot);
    }
}

static void key_value(const Dq8Mot *mot, const Dq8MotChannel *c, int k, float out[4])
{
    int n = c->components, bytes = c->value_size / 8;
    const uint8_t *p = mot->blob + c->values + (size_t)k * n * bytes;
    for (int i = 0; i < n; i++, p += bytes) {
        if (c->value_size == 0x20) {
            memcpy(&out[i], p, 4);
        } else {
            float raw = c->value_size == 0x10 ? (float)(int16_t)rd16(p) : (float)(int8_t)p[0];
            float unit = c->value_size == 0x10 ? 32768.0f : 128.0f;
            out[i] = c->scaled ? raw * c->scale[i] / unit : raw;
        }
    }
}

static float key_time(const Dq8Mot *mot, const Dq8MotChannel *c, int k)
{
    return c->times ? (float)(int16_t)rd16(mot->blob + c->times + (size_t)k * 2) : (float)k;
}

bool dq8_mot_sample_list(const Dq8Mot *mot, int list, int index, int type, float frame,
                         float out[4])
{
    if (!mot || !out) {
        return false;
    }
    const Dq8MotChannel *c = NULL;
    for (int t = 0; t < mot->track_count && !c; t++) {
        if (mot->tracks[t].list != list || mot->tracks[t].bone != index) {
            continue;
        }
        for (int i = 0; i < mot->tracks[t].channel_count; i++) {
            if (mot->tracks[t].channels[i].type == type) {
                c = &mot->tracks[t].channels[i];
            }
        }
    }
    if (!c) {
        return false;
    }
    int n = c->components;
    /* last key with time <= frame */
    int j = 0;
    while (j + 1 < c->count && key_time(mot, c, j + 1) <= frame) {
        j++;
    }
    float a[4] = { 0 }, b[4] = { 0 };
    key_value(mot, c, j, a);
    if (j + 1 >= c->count || frame <= key_time(mot, c, j) || type == DQ8_MOT_VISIBLE) {
        memcpy(out, a, sizeof(float) * n);
    } else {
        float t0 = key_time(mot, c, j), t1 = key_time(mot, c, j + 1);
        float w = (frame - t0) / (t1 - t0);
        key_value(mot, c, j + 1, b);
        float dot = 0;
        for (int i = 0; i < n; i++) {
            dot += a[i] * b[i];
        }
        float sign = (type == DQ8_MOT_ROTATE && dot < 0) ? -1.0f : 1.0f;
        for (int i = 0; i < n; i++) {
            out[i] = a[i] + (sign * b[i] - a[i]) * w;
        }
    }
    if (type == DQ8_MOT_ROTATE) {
        float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
        if (len > 0) {
            for (int i = 0; i < 4; i++) {
                out[i] /= len;
            }
        }
    }
    return true;
}

bool dq8_mot_sample(const Dq8Mot *mot, int bone, int type, float frame, float out[4])
{
    return dq8_mot_sample_list(mot, 0, bone, type, frame, out);
}

static void mat_mul(const float *a, const float *b, float *out)
{
    float r[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j]
                         + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        }
    }
    memcpy(out, r, sizeof(r));
}

bool dq8_mot_pose(const Dq8Mot *mot, const Dq8Mds *model, float frame, float *worlds)
{
    if (!mot || !model || !worlds) {
        return false;
    }
    int n = (int)model->bone_count;
    for (int b = 0; b < n; b++) {
        float *L = worlds + (size_t)b * 16, v[4];
        if (!dq8_mds_bone_local(model, b, L)) {
            return false;
        }
        if (dq8_mot_sample(mot, b, DQ8_MOT_ROTATE, frame, v)) {
            float x = v[0], y = v[1], z = v[2], w = v[3];
            float R[9] = {
                1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y),
            };
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) {
                    L[r * 4 + c] = R[r * 3 + c];
                }
            }
        }
        if (dq8_mot_sample(mot, b, DQ8_MOT_SCALE, frame, v)) {
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) {
                    L[r * 4 + c] *= v[r];
                }
            }
        }
        if (dq8_mot_sample(mot, b, DQ8_MOT_TRANSLATE, frame, v)) {
            L[12] = v[0];
            L[13] = v[1];
            L[14] = v[2];
        }
        /* parents precede children in every file seen; compose in place */
        int p = dq8_mds_bone_parent(model, b);
        if (p >= 0 && p < b) {
            mat_mul(L, worlds + (size_t)p * 16, L);
        } else if (p >= b) {
            return false;
        }
    }
    return true;
}

bool dq8_mot_mesh_visible(const Dq8Mot *mot, const Dq8Mds *model, int mesh_index, float frame)
{
    if (!mot || !model) {
        return true;
    }
    int b = dq8_mds_mesh_owner(model, mesh_index);
    for (int guard = 0; b >= 0 && guard < 1024; guard++) {
        float v[4];
        if (dq8_mot_sample(mot, b, DQ8_MOT_VISIBLE, frame, v) && v[0] < 0.5f) {
            return false;
        }
        b = dq8_mds_bone_parent(model, b);
    }
    return true;
}

float dq8_mot_material_alpha(const Dq8Mot *mot, int material, float frame, float resting)
{
    float v[4];
    if (mot && dq8_mot_sample_list(mot, 1, material, DQ8_MOT_ALPHA, frame, v)) {
        return v[0];
    }
    return resting;
}

/* inverse of an affine row-vector matrix */
static void inv_affine(const float *M, float *out)
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

bool dq8_mot_pose_retarget(const Dq8Mot *mot, const Dq8Mds *rig, const Dq8Mds *target,
                           float frame, float *worlds)
{
    if (!mot || !rig || !target || !worlds) {
        return false;
    }
    float *rw = malloc(sizeof(float) * 16 * (rig->bone_count ? rig->bone_count : 1));
    if (!rw || !dq8_mot_pose(mot, rig, frame, rw)) {
        free(rw);
        return false;
    }
    for (int b = 0; b < (int)target->bone_count; b++) {
        float *W = worlds + (size_t)b * 16;
        int r = dq8_mds_bone_find(rig, dq8_mds_bone_name(target, b));
        if (r >= 0) {
            float bt[16], br[16], inv[16];
            dq8_mds_bone_world(target, b, bt);
            dq8_mds_bone_world(rig, r, br);
            inv_affine(br, inv);
            mat_mul(bt, inv, W);
            mat_mul(W, rw + (size_t)r * 16, W);
        } else {
            int p = dq8_mds_bone_parent(target, b);
            dq8_mds_bone_local(target, b, W);
            if (p >= 0 && p < b) {
                mat_mul(W, worlds + (size_t)p * 16, W);
            }
        }
    }
    free(rw);
    return true;
}

float dq8_mot_name_coverage(const Dq8Mot *mot, const Dq8Mds *rig, const Dq8Mds *target)
{
    if (!mot || !rig || !target) {
        return 0;
    }
    int n = 0, hit = 0;
    for (int t = 0; t < mot->track_count; t++) {
        if (mot->tracks[t].list != 0) {
            continue;
        }
        n++;
        hit += dq8_mds_bone_find(target, dq8_mds_bone_name(rig, mot->tracks[t].bone)) >= 0;
    }
    return n ? (float)hit / n : 0;
}
