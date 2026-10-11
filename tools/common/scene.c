/* Model-pack loading for the debug tools; see scene.h. */
#include "scene.h"

#include <dq8/io/img.h>
#include <dq8/io/pak.h>
#include <dq8/io/texanime.h>
#include <dq8/io/tim2.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MEMBERS 64
#define MAX_TEX 128

void scene_note(Scene *s, const char *fmt, ...)
{
    if (!s) {
        return;
    }
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    size_t len = strlen(line);
    if (len == 0 || line[len - 1] != '\n') {
        line[len < sizeof(line) - 1 ? len : sizeof(line) - 2] = '\n';
        line[len < sizeof(line) - 1 ? len + 1 : sizeof(line) - 1] = 0;
        len = strlen(line);
    }
    if (s->notes_len + len + 1 > s->notes_cap) {
        s->notes_cap = (s->notes_len + len + 1) * 2;
        s->notes = realloc(s->notes, s->notes_cap);
    }
    memcpy(s->notes + s->notes_len, line, len + 1);
    s->notes_len += len;
}

/* ---------------------------------------------------------------- units */

typedef struct {
    char           name[96];
    const uint8_t *data;
    size_t         size;
    int            depth;   /* nesting level: 0 = the pack itself, 1 = inside a nested .chr */
} Member;

typedef struct {
    Member mds[MAX_MEMBERS], img[MAX_MEMBERS], mot[MAX_MEMBERS], cfg[MAX_MEMBERS];
    int    n_mds, n_img, n_mot, n_cfg;
} Members;

static int ends_with(const char *s, const char *x)
{
    size_t n = strlen(s), k = strlen(x);
    return n >= k && strcmp(s + n - k, x) == 0;
}

static int g_collect_depth;

static void add_member(Member *list, int *n, const char *name, const uint8_t *d, size_t s)
{
    if (*n < MAX_MEMBERS) {
        snprintf(list[*n].name, sizeof(list[*n].name), "%s", name);
        list[*n].data = d;
        list[*n].size = s;
        list[*n].depth = g_collect_depth;
        (*n)++;
    }
}

/* Collect a pack's members; descend only into nested .chr (the PAK parser
 * accepts MOT/IM3 blobs too). */
static void collect(const uint8_t *d, size_t n, Members *out, int depth)
{
    Dq8Pak *p = depth < 4 ? dq8_pak_open(d, n) : NULL;
    g_collect_depth = depth;
    for (int i = 0; p && i < dq8_pak_count(p); i++) {
        const Dq8PakEntry *e = dq8_pak_entry(p, i);
        if (ends_with(e->name, ".mds")) {
            add_member(out->mds, &out->n_mds, e->name, e->data, e->size);
        } else if (ends_with(e->name, ".img")) {
            add_member(out->img, &out->n_img, e->name, e->data, e->size);
        } else if (ends_with(e->name, ".mot")) {
            add_member(out->mot, &out->n_mot, e->name, e->data, e->size);
        } else if (ends_with(e->name, ".cfg")) {
            add_member(out->cfg, &out->n_cfg, e->name, e->data, e->size);
        } else if (ends_with(e->name, ".chr")) {
            collect(e->data, e->size, out, depth + 1);
            g_collect_depth = depth;
        }
    }
    dq8_pak_close(p);
}

/* Load a unit's bytes (caller frees *owner). */
static const uint8_t *load_unit(Dq8Archive *a, const char *name, uint8_t **owner, size_t *size)
{
    char path[256];
    snprintf(path, sizeof(path), "%s", name);
    char *colon = strchr(path, ':');
    if (colon) {
        *colon = 0;
    }
    size_t n;
    uint8_t *d = dq8_archive_load(a, path, &n);
    *owner = d;
    if (!d) {
        return NULL;
    }
    if (!colon) {
        *size = n;
        return d;
    }
    Dq8Pak *p = dq8_pak_open(d, n);
    const Dq8PakEntry *e = p ? dq8_pak_find(p, colon + 1) : NULL;
    const uint8_t *r = e ? e->data : NULL;
    if (e) {
        *size = e->size;
    }
    dq8_pak_close(p);
    return r;
}

/* ------------------------------------------------------------- textures */

typedef struct {
    char     name[32];
    Dq8Tim2 *tim;      /* NULL = runtime canvas no script fills (drawn cyan) */
} Tex;

static Tex g_tex[MAX_TEX];
static int g_tex_count;
static Dq8TexAnimSet *g_anims[16];   /* "#texanime" scripts of the pack's images */
static int g_anim_count;

static Dq8Tim2 *tex_lookup(void *ctx, const char *name)
{
    (void)ctx;
    for (int i = 0; i < g_tex_count; i++) {
        if (strcmp(g_tex[i].name, name) == 0) {
            return g_tex[i].tim;
        }
    }
    return NULL;
}

/* Is `name` the destination of some texture animation? */
static int animated_canvas(const char *name)
{
    for (int s = 0; s < g_anim_count; s++) {
        for (int i = 0; i < g_anims[s]->count; i++) {
            for (int k = 0; k < g_anims[s]->anims[i].frame_count; k++) {
                if (strcmp(g_anims[s]->anims[i].frames[k].dst, name) == 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void animate_textures(float time)
{
    for (int s = 0; s < g_anim_count; s++) {
        dq8_texanim_apply(g_anims[s], time, tex_lookup, NULL);
    }
}

static void load_textures(const Members *mem)
{
    for (int i = 0; i < mem->n_img; i++) {
        Dq8Img *im = dq8_img_open(mem->img[i].data, mem->img[i].size);
        size_t bn;
        const uint8_t *blob = im ? dq8_img_blob(im, &bn) : NULL;
        for (int k = 0; im && k < dq8_img_count(im) && g_tex_count < MAX_TEX; k++) {
            const Dq8ImgEntry *e = dq8_img_entry(im, k);
            if (strcmp(e->name, "#texanime") == 0) {   /* animation script, not a picture */
                Dq8TexAnimSet *set = dq8_texanim_parse((const char *)blob + e->data_offset,
                                                       e->data_size);
                if (set && g_anim_count < 16) {
                    g_anims[g_anim_count++] = set;
                } else {
                    dq8_texanim_free(set);
                }
                continue;
            }
            Tex *t = &g_tex[g_tex_count++];
            snprintf(t->name, sizeof(t->name), "%s", e->name);
            t->tim = dq8_tim2_open(blob + e->data_offset, e->data_size);
            if (!t->tim) {
                t->tim = dq8_tim2_canvas(blob + e->data_offset, e->data_size);
            }
        }
        dq8_img_free(im);
    }
}

/* Canvases that no script fills stay cyan (tim = NULL). */
static void finish_textures(void)
{
    for (int i = 0; i < g_tex_count; i++) {
        if (g_tex[i].tim && g_tex[i].tim->bits == 32 && !animated_canvas(g_tex[i].name)) {
            int blank = 1;
            for (int p = 0; p < g_tex[i].tim->width * g_tex[i].tim->height && blank; p++) {
                blank = g_tex[i].tim->rgba[p] == 0;
            }
            if (blank) {
                dq8_tim2_free(g_tex[i].tim);
                g_tex[i].tim = NULL;
            }
        }
    }
}

static void free_textures(void)
{
    for (int i = 0; i < g_tex_count; i++) {
        dq8_tim2_free(g_tex[i].tim);
    }
    g_tex_count = 0;
    for (int s = 0; s < g_anim_count; s++) {
        dq8_texanim_free(g_anims[s]);
    }
    g_anim_count = 0;
}

/* -1 = missing, -2 = untextured, else index (tim may be NULL = runtime canvas) */
static int find_tex(const char *name)
{
    if (!name || !*name) {
        return -2;
    }
    for (int i = 0; i < g_tex_count; i++) {
        if (strcmp(g_tex[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------- borrowed textures */
/* Some packs reference textures stored in another pack (blur copies use the
 * base monster's, alternate outfits share the body texture). Built lazily on
 * the first miss: an index of every TIM2 name in every .img in the archive. */

typedef struct {
    char name[32];
    char path[256];   /* archive path, nested members separated by ':' */
} TexRef;

static TexRef *g_index;
static int g_index_count, g_index_cap, g_index_built;

static void index_img(const char *where, const uint8_t *d, size_t n)
{
    Dq8Img *im = dq8_img_open(d, n);
    for (int i = 0; im && i < dq8_img_count(im); i++) {
        const Dq8ImgEntry *e = dq8_img_entry(im, i);
        if (e->data_size <= 64) {
            continue;   /* header-only runtime canvas */
        }
        if (g_index_count == g_index_cap) {
            g_index_cap = g_index_cap ? g_index_cap * 2 : 4096;
            g_index = realloc(g_index, sizeof(TexRef) * (size_t)g_index_cap);
        }
        TexRef *r = &g_index[g_index_count++];
        snprintf(r->name, sizeof(r->name), "%s", e->name);
        snprintf(r->path, sizeof(r->path), "%s", where);
    }
    dq8_img_free(im);
}

static void index_pack(const char *where, const uint8_t *d, size_t n, int depth)
{
    Dq8Pak *p = depth < 4 ? dq8_pak_open(d, n) : NULL;
    for (int i = 0; p && i < dq8_pak_count(p); i++) {
        const Dq8PakEntry *e = dq8_pak_entry(p, i);
        char w[256];
        snprintf(w, sizeof(w), "%s:%s", where, e->name);
        if (ends_with(e->name, ".img")) {
            index_img(w, e->data, e->size);
        } else if (ends_with(e->name, ".chr") || ends_with(e->name, ".pac")
                   || ends_with(e->name, ".pak")) {
            index_pack(w, e->data, e->size, depth + 1);
        }
    }
    dq8_pak_close(p);
}

static void build_index(Dq8Archive *a)
{
    g_index_built = 1;
    for (int i = 0; i < dq8_archive_count(a); i++) {
        const char *nm = dq8_archive_name(a, i);
        int img = nm && ends_with(nm, ".img");
        int pack = nm && (ends_with(nm, ".chr") || ends_with(nm, ".pac") || ends_with(nm, ".pak"));
        if (!img && !pack) {
            continue;
        }
        size_t n;
        uint8_t *d = dq8_archive_load(a, nm, &n);
        if (d) {
            if (img) {
                index_img(nm, d, n);
            } else {
                index_pack(nm, d, n, 0);
            }
            free(d);
        }
    }
}

/* Load "a.chr:b.chr:c.img" (archive member, then nested PAK members). */
static uint8_t *load_path(Dq8Archive *a, const char *path, const uint8_t **data, size_t *size)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *seg = strtok(tmp, ":");
    size_t n;
    uint8_t *own = dq8_archive_load(a, seg, &n);
    const uint8_t *d = own;
    for (seg = strtok(NULL, ":"); d && seg; seg = strtok(NULL, ":")) {
        Dq8Pak *p = dq8_pak_open(d, n);
        const Dq8PakEntry *e = p ? dq8_pak_find(p, seg) : NULL;
        d = e ? e->data : NULL;
        n = e ? e->size : 0;
        dq8_pak_close(p);
    }
    *data = d;
    *size = n;
    return own;
}

/* Borrow texture `name` from another pack; prefers packs whose path shares
 * the unit's id (text before the first '_' of its file name). Returns the
 * source path or NULL. */
static const char *borrow_texture(Dq8Archive *a, const char *unit, const char *name)
{
    if (g_tex_count >= MAX_TEX) {
        return NULL;
    }
    if (!g_index_built) {
        build_index(a);
    }
    const char *base = strrchr(unit, '/');
    base = base ? base + 1 : unit;
    char id[64];
    snprintf(id, sizeof(id), "%s", base);
    id[strcspn(id, "_.:")] = 0;
    const TexRef *best = NULL;
    for (int i = 0; i < g_index_count; i++) {
        if (strcmp(g_index[i].name, name) == 0) {
            if (!best || (strstr(g_index[i].path, id) && !strstr(best->path, id))) {
                best = &g_index[i];
            }
        }
    }
    if (!best) {
        return NULL;
    }
    const uint8_t *d;
    size_t n;
    uint8_t *own = load_path(a, best->path, &d, &n);
    Dq8Img *im = d ? dq8_img_open(d, n) : NULL;
    size_t bn;
    const uint8_t *blob = im ? dq8_img_blob(im, &bn) : NULL;
    for (int k = 0; im && k < dq8_img_count(im); k++) {
        const Dq8ImgEntry *e = dq8_img_entry(im, k);
        if (strcmp(e->name, name) == 0) {
            Tex *t = &g_tex[g_tex_count++];
            snprintf(t->name, sizeof(t->name), "%s", name);
            t->tim = dq8_tim2_open(blob + e->data_offset, e->data_size);
            break;
        }
    }
    dq8_img_free(im);
    free(own);
    return best->path;
}


/* `mot`/`frame` (optional) drive mesh visibility (bone type-7 tracks) and
 * material opacity (list-B type-5 tracks); without a motion, materials use
 * their diffuse alpha and every mesh is shown. */
static void build_tris(const Dq8Mds *m, const float *pose, const Dq8Mot *mot, float frame,
                       SrTriList *out)
{
    for (int k = 0; k < dq8_mds_mesh_count(m); k++) {
        const Dq8MdsMesh *me = dq8_mds_mesh(m, k);
        int nf = dq8_mds_faces(m, me, NULL, 0);
        if (nf <= 0 || (mot && !dq8_mot_mesh_visible(mot, m, k, frame))) {
            continue;
        }
        Dq8MdsFace *f = malloc(sizeof(*f) * (size_t)nf);
        float (*pos)[3] = malloc(sizeof(float[3]) * (me->vertex_count ? me->vertex_count : 1));
        dq8_mds_faces(m, me, f, nf);
        for (uint32_t i = 0; i < me->vertex_count; i++) {
            if (!dq8_mds_skin_vertex(m, k, (int)i, pose, pos[i])) {
                pos[i][0] = pos[i][1] = pos[i][2] = 0;
            }
        }
        for (int j = 0; j < nf; j++) {
            float rgba[4] = { 1, 1, 1, 1 };
            dq8_mds_material_color(m, f[j].material, rgba);
            float alpha = dq8_mot_material_alpha(mot, f[j].material, frame, rgba[3]);
            if (alpha < 0.02f) {
                continue;   /* faded out (e.g. claw trails outside the swipe) */
            }
            SrTri *t = sr_push(out);
            for (int e = 0; e < 3; e++) {
                memcpy(t->p[e], pos[f[j].v[e]], sizeof(t->p[e]));
                float uv[2] = { 0, 0 };
                dq8_mds_uv(m, me, f[j].uv[e], uv);
                t->uv[e][0] = uv[0];
                t->uv[e][1] = uv[1];
            }
            int ti = find_tex(dq8_mds_material_texture(m, f[j].material));
            t->tex_state = ti == -2 ? SR_UNTEXTURED : ti == -1 ? SR_MISSING
                         : g_tex[ti].tim ? SR_TEXTURED : SR_CANVAS;
            t->tex = ti >= 0 ? g_tex[ti].tim : NULL;
            t->alpha = alpha > 1 ? 1 : alpha;
            t->additive = (dq8_mds_material_flags(m, f[j].material) & DQ8_MDS_MAT_ADDITIVE) != 0;
            if (f[j].env_material >= 0) {
                int et = find_tex(dq8_mds_material_texture(m, f[j].env_material));
                if (et >= 0 && g_tex[et].tim) {
                    float er[4] = { 1, 1, 1, 1 };
                    dq8_mds_material_color(m, f[j].env_material, er);
                    t->env = g_tex[et].tim;
                    memcpy(t->env_rgb, er, sizeof(t->env_rgb));
                    for (int e = 0; e < 3; e++) {
                        float nn[3];
                        t->nrm[e][1] = 1;
                        if (f[j].n[e] != 0xFFFF && dq8_mds_normal(m, me, f[j].n[e], nn)) {
                            dq8_mds_skin_direction(m, k, f[j].v[e], nn, pose, t->nrm[e]);
                        }
                    }
                }
            }
        }
        free(f);
        free(pos);
    }
}

/* ---------------------------------------------------------- info.cfg KEYs */

static int parse_keys(const Members *mem, int depth, SceneKey *keys, int max)
{
    int n = 0;
    for (int c = 0; c < mem->n_cfg && n == 0; c++) {
        if (depth >= 0 && mem->cfg[c].depth != depth) {
            continue;
        }
        const char *s = (const char *)mem->cfg[c].data, *end = s + mem->cfg[c].size;
        while (s < end && n < max) {
            const char *k = NULL;
            for (const char *p = s; p + 5 < end; p++) {
                if (memcmp(p, "KEY \"", 5) == 0 && (p == s || p[-1] != '_')) {
                    k = p;
                    break;
                }
            }
            if (!k) {
                break;
            }
            const char *q0 = k + 5, *q1 = memchr(q0, '"', (size_t)(end - q0));
            if (!q1) {
                break;
            }
            size_t len = (size_t)(q1 - q0) < sizeof(keys[n].name) - 1 ? (size_t)(q1 - q0)
                                                                     : sizeof(keys[n].name) - 1;
            memcpy(keys[n].name, q0, len);
            keys[n].name[len] = 0;
            if (sscanf(q1 + 1, " , %d , %d", &keys[n].start, &keys[n].end) == 2) {
                n++;
            }
            s = q1 + 1;
        }
    }
    return n;
}

/* ------------------------------------------------------------- one unit */

static int pose_at(const Dq8Mot *mot, const Dq8Mds *rig, const Dq8Mds *m, float f, float *pose)
{
    return rig ? dq8_mot_pose_retarget(mot, rig, m, f, pose) : dq8_mot_pose(mot, m, f, pose);
}

static int mot_fits(const Dq8Mot *mot, const Dq8Mds *m)
{
    for (int t = 0; t < mot->track_count; t++) {
        if (mot->tracks[t].list == 0 && (uint32_t)mot->tracks[t].bone >= m->bone_count) {
            return 0;
        }
    }
    return 1;
}

/* Mesh-only character packs (c001_skin1) take their motion from <id>_base1. */
static Dq8Mot *paired_motion(Dq8Archive *a, const char *unit, char *src, size_t srclen)
{
    /* <id>_skinN takes <id>_base1's motion; <id>_highN (high tension) <id>_htype's */
    const char *skin = strstr(unit, "_skin"), *high = strstr(unit, "_high");
    const char *cut = skin ? skin : high;
    if (!cut || strchr(unit, ':')) {
        return NULL;
    }
    char path[256];
    snprintf(path, sizeof(path), "%.*s%s", (int)(cut - unit), unit, skin ? "_base1.chr" : "_htype.chr");
    snprintf(src, srclen, "%s", path);
    return dq8_mot_load_archive(a, path);
}

/* Body pack for motion-only / effect packs: chara/c001_bt1.chr -> chara/c001_skin1.chr */
static int body_path(const char *unit, char *out, size_t len)
{
    /* nested units (bundle.pac:inner.chr) use the bundle's path */
    const char *colon = strchr(unit, ':');
    const char *slash = strrchr(unit, '/');
    while (colon && slash && slash > colon) {   /* last '/' before the ':' */
        const char *s = unit, *last = NULL;
        for (; s < colon; s++) {
            if (*s == '/') {
                last = s;
            }
        }
        slash = last;
    }
    const char *us = slash ? strchr(slash, '_') : NULL;
    if (!us || (colon && us > colon)) {
        return 0;
    }
    snprintf(out, len, "%.*s_skin1.chr", (int)(us - unit), unit);
    return strcmp(out, unit) != 0;
}

/* Value of a `KEYWORD "value"` directive in the pack's .cfg files. `exact`
 * rejects matches preceded by '_' (MODEL vs SHADOW_MODEL). */
static int cfg_value(const Members *mem, const char *keyword, char *out, size_t len)
{
    size_t kl = strlen(keyword);
    for (int c = 0; c < mem->n_cfg; c++) {
        const char *s = (const char *)mem->cfg[c].data, *end = s + mem->cfg[c].size;
        for (const char *p = s; p + kl + 2 < end; p++) {
            if (memcmp(p, keyword, kl) == 0 && p[kl] == ' ' && p[kl + 1] == '"'
                && (p == s || (p[-1] != '_' && p[-1] != 'W'))) {
                const char *q0 = p + kl + 2, *q1 = memchr(q0, '"', (size_t)(end - q0));
                if (q1) {
                    snprintf(out, len, "%.*s", (int)(q1 - q0), q0);
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* The pack's main model: the one info.cfg names in MODEL "x.mds"; otherwise
 * the .mds with the most surface triangles that isn't the SHADOW_MODEL
 * (shadow casters like en049s.mds have no textured surface). */
static int g_model_depth;   /* depth of the member choose_model() picked */

static Dq8Mds *choose_model(const Members *mem, char *name, size_t len, Scene *meta)
{
    char want[96] = "", shadow[96] = "";
    cfg_value(mem, "MODEL", want, sizeof(want));
    cfg_value(mem, "SHADOW_MODEL", shadow, sizeof(shadow));
    int best = -1, best_tris = -1;
    for (int i = 0; i < mem->n_mds; i++) {
        Dq8Mds *c = dq8_mds_open(mem->mds[i].data, mem->mds[i].size);
        if (!c || dq8_mds_mesh_count(c) == 0) {
            dq8_mds_free(c);
            continue;
        }
        int tris = 0;
        for (int k = 0; k < dq8_mds_mesh_count(c); k++) {
            tris += dq8_mds_faces(c, dq8_mds_mesh(c, k), NULL, 0);
        }
        int score = tris;
        if (want[0] && strcmp(mem->mds[i].name, want) == 0) {
            score = 1 << 29;
        } else if (shadow[0] && strcmp(mem->mds[i].name, shadow) == 0) {
            score = -1;
        }
        if (score > best_tris) {
            best_tris = score;
            best = i;
        }
        dq8_mds_free(c);
    }
    for (int i = 0; meta && i < mem->n_mds; i++) {
        if (i != best) {
            scene_note(meta, "other_mds=%s%s\n", mem->mds[i].name,
                    shadow[0] && strcmp(mem->mds[i].name, shadow) == 0 ? " (shadow model)" : "");
        }
    }
    if (best < 0) {
        return NULL;
    }
    snprintf(name, len, "%s", mem->mds[best].name);
    g_model_depth = mem->mds[best].depth;
    return dq8_mds_open(mem->mds[best].data, mem->mds[best].size);
}

/* Find a body for a motion: first <bundle-id>_skin1.chr (body_path), then for
 * each '_'-separated token of the unit's file name (cutscene packs are named
 * <scene>_<character>.chr, e.g. e1004-1_c001, e1602_4_p012a, p032a_mk):
 * chara/<token>_skin1.chr and chara/<token>.chr. First pack whose model the
 * motion fits wins. */
static int model_tris(const Dq8Mds *m)
{
    int n = 0;
    for (int k = 0; m && k < dq8_mds_mesh_count(m); k++) {
        n += dq8_mds_faces(m, dq8_mds_mesh(m, k), NULL, 0);
    }
    return n;
}

/* Item code token (c1s001, c4w, c4w001b2): weapon motions pair with item/ models. */
static int is_item_code(const char *tok)
{
    return tok[0] == 'c' && tok[1] >= '0' && tok[1] <= '9' && tok[2] >= 'a' && tok[2] <= 'z';
}

/* Does the motion belong to model `m`? With the motion's own rig: by bone name
 * (at least half the tracks); without: every track's bone index exists. */
static int motion_matches(const Dq8Mot *mot, const Dq8Mds *rig, const Dq8Mds *m)
{
    return rig ? dq8_mot_name_coverage(mot, rig, m) >= 0.5f : mot_fits(mot, m);
}

static Dq8Mds *find_body(Dq8Archive *a, const char *unit, const Dq8Mot *mot, const Dq8Mds *rig,
                         Members *mem, uint8_t **owner, char *path, size_t plen, char *mname,
                         size_t mlen)
{
    char cand[40][256];
    int nc = 0;
    {   /* weapons first: tokens like c4w -> item/c4w..., c4w001b2 -> item/c4w001b2.chr */
        const char *b0 = strrchr(unit, ':');
        b0 = b0 ? b0 + 1 : (strrchr(unit, '/') ? strrchr(unit, '/') + 1 : unit);
        char nm0[128];
        snprintf(nm0, sizeof(nm0), "%s", b0);
        char *d0 = strrchr(nm0, '.');
        if (d0) {
            *d0 = 0;
        }
        for (char *tok = strtok(nm0, "_"); tok && nc < 8; tok = strtok(NULL, "_")) {
            if (!is_item_code(tok)) {
                continue;
            }
            char pre[64];
            snprintf(pre, sizeof(pre), "item/%s", tok);
            for (int i = 0; i < dq8_archive_count(a) && nc < 8; i++) {
                const char *nm = dq8_archive_name(a, i);
                if (nm && strncmp(nm, pre, strlen(pre)) == 0 && ends_with(nm, ".chr")) {
                    snprintf(cand[nc++], sizeof(cand[0]), "%s", nm);
                    break;   /* first variant */
                }
            }
        }
    }
    if (body_path(unit, cand[nc], sizeof(cand[0]))) {
        nc++;
    }
    const char *base = strrchr(unit, ':');
    base = base ? base + 1 : (strrchr(unit, '/') ? strrchr(unit, '/') + 1 : unit);
    char name[128];
    snprintf(name, sizeof(name), "%s", base);
    char *dot = strrchr(name, '.');
    if (dot) {
        *dot = 0;
    }
    for (char *tok = strtok(name, "_"); tok && nc + 4 <= 40; tok = strtok(NULL, "_")) {
        snprintf(cand[nc++], sizeof(cand[0]), "chara/%s_skin1.chr", tok);
        snprintf(cand[nc++], sizeof(cand[0]), "chara/%s.chr", tok);
        size_t tl = strlen(tok);   /* variant letter: c002a -> c002 */
        if (tl > 2 && tok[tl - 1] >= 'a' && tok[tl - 1] <= 'z' && tok[tl - 2] >= '0' && tok[tl - 2] <= '9') {
            snprintf(cand[nc++], sizeof(cand[0]), "chara/%.*s_skin1.chr", (int)(tl - 1), tok);
            snprintf(cand[nc++], sizeof(cand[0]), "chara/%.*s.chr", (int)(tl - 1), tok);
        }
    }
    for (int i = 0; i < nc; i++) {
        uint32_t sector, bytes;
        if (strcmp(cand[i], unit) == 0 || !dq8_archive_find(a, cand[i], &sector, &bytes)) {
            continue;
        }
        size_t bs;
        uint8_t *own = NULL;
        const uint8_t *bd = load_unit(a, cand[i], &own, &bs);
        Members bm;
        memset(&bm, 0, sizeof(bm));
        if (bd) {
            collect(bd, bs, &bm, 0);
        }
        char nm[96];
        Dq8Mds *m = choose_model(&bm, nm, sizeof(nm), NULL);
        int body_tracks = 0;   /* a body needs a body motion, not a partial prop motion */
        for (int t2 = 0; t2 < mot->track_count; t2++) {
            body_tracks += mot->tracks[t2].list == 0;
        }
        int is_item = strncmp(cand[i], "item/", 5) == 0;
        if (m && model_tris(m) >= 64 && (is_item || rig || body_tracks >= 16)
            && motion_matches(mot, rig, m)) {
            *mem = bm;
            *owner = own;
            snprintf(path, plen, "%s", cand[i]);
            snprintf(mname, mlen, "%s", nm);
            return m;
        }
        dq8_mds_free(m);
        free(own);
    }
    return NULL;
}

Scene *scene_load(Dq8Archive *a, const char *unit)
{
    uint8_t *owner = NULL;
    size_t size = 0;
    const uint8_t *data = load_unit(a, unit, &owner, &size);
    Members mem;
    memset(&mem, 0, sizeof(mem));
    if (data) {
        collect(data, size, &mem, 0);
    }

    Scene *meta = calloc(1, sizeof(*meta));
    scene_set_view(meta, 0.6109f, 0.2618f);   /* 3/4 view until the caller sets one */
    snprintf(meta->unit, sizeof(meta->unit), "%s", unit);
    scene_note(meta, "unit=%s", unit);

    /* main model: info.cfg MODEL, else most triangles, never the shadow model */
    char mname[96];
    Dq8Mds *m = choose_model(&mem, mname, sizeof(mname), meta);
    if (m) {
        scene_note(meta, "mds=%s\n", mname);
    }
    for (int c = 0; c < mem.n_cfg; c++) {   /* display name */
        const char *s = (const char *)mem.cfg[c].data, *end = s + mem.cfg[c].size;
        for (const char *p = s; p + 6 < end; p++) {
            if (memcmp(p, "NAME \"", 6) == 0) {
                const char *q = memchr(p + 6, '"', (size_t)(end - p - 6));
                if (q) {
                    scene_note(meta, "title=%.*s\n", (int)(q - p - 6), p + 6);
                    snprintf(meta->title, sizeof(meta->title), "%.*s", (int)(q - p - 6), p + 6);
                }
                break;
            }
        }
    }
    /* motion: own, else paired (<id>_skinN takes <id>_base1's) */
    Dq8Mot *mot = NULL;
    char mot_src[256] = "";
    int own_motion = 0;
    int want_depth = m ? g_model_depth : 0;
    for (int pass = 0; pass < 2 && !mot; pass++) {
        for (int i = 0; i < mem.n_mot && !mot; i++) {
            /* pass 0: same level as the model; pass 1 (no model): any level */
            if (pass == 0 ? mem.mot[i].depth != want_depth : (m != NULL)) {
                continue;
            }
            mot = dq8_mot_open(mem.mot[i].data, mem.mot[i].size);
            snprintf(mot_src, sizeof(mot_src), "%s", mem.mot[i].name);
            own_motion = mot != NULL;
            want_depth = mem.mot[i].depth;
        }
    }
    for (int i = 0; i < mem.n_mot; i++) {
        if (mem.mot[i].depth != want_depth) {
            scene_note(meta, "info=nested %s animates an attachment (own model); not applied to the body\n",
                    mem.mot[i].name);
        }
    }
    Members pair_mem, body_mem;
    uint8_t *pair_owner = NULL, *body_owner = NULL;
    memset(&pair_mem, 0, sizeof(pair_mem));
    memset(&body_mem, 0, sizeof(body_mem));
    if (!mot && m) {
        mot = paired_motion(a, unit, mot_src, sizeof(mot_src));
        if (mot) {
            size_t ps;
            const uint8_t *pd = load_unit(a, mot_src, &pair_owner, &ps);
            if (pd) {
                collect(pd, ps, &pair_mem, 0);
            }
        }
    }

    /* Motions whose own pack has no real mesh render on a body:
     *  - motion-only packs (c001_atype) and weapon motions (c004_btsp006_c4w -> item/c4w...)
     *  - packs whose mesh doesn't fit the motion (c001_bt1 = trail effect + body motion)
     *  - packs whose model is only a rig (c001_base2: 173-bone dungeon skeleton with
     *    placeholder triangles) -> retarget onto the 88-bone body by bone name. */
    char body[256], bname[96];
    Dq8Mds *rig = NULL;          /* the motion's own skeleton, when it differs from m */
    int retarget = 0;
    int own_tris = model_tris(m);
    if (mot && own_motion && (!m || own_tris < 64 || !mot_fits(mot, m))) {
        const Dq8Mds *own_rig = (m && mot_fits(mot, m)) ? m : NULL;
        Dq8Mds *skel = NULL;   /* skeleton-only .mds (0 meshes) = the motion's rig */
        for (int i = 0; !m && i < mem.n_mds && !skel; i++) {
            Dq8Mds *c = dq8_mds_open(mem.mds[i].data, mem.mds[i].size);
            if (c && c->bone_count > 0 && mot_fits(mot, c)) {
                skel = c;
            } else {
                dq8_mds_free(c);
            }
        }
        if (skel) {
            own_rig = skel;
        }
        Dq8Mds *bm = find_body(a, unit, mot, own_rig, &body_mem, &body_owner, body, sizeof(body),
                               bname, sizeof(bname));
        if (bm) {
            if (own_rig) {
                int same = 1;   /* names line up index for index -> plain posing */
                for (int t2 = 0; t2 < mot->track_count && same; t2++) {
                    int b = mot->tracks[t2].bone;
                    same = mot->tracks[t2].list != 0
                        || strcmp(dq8_mds_bone_name(own_rig, b), dq8_mds_bone_name(bm, b)) == 0;
                }
                retarget = !same;
            }
            if (skel && retarget) {
                scene_note(meta, "note=skeleton-only pack (%u bones); motion retargeted by bone name onto %s\n",
                        skel->bone_count, body);
                rig = skel;
                skel = NULL;
            } else if (m && retarget) {
                scene_note(meta, "note=pack model is a %u-bone rig (%d triangles); motion retargeted by bone name onto %s\n",
                        m->bone_count, own_tris, body);
                rig = m;
            } else if (m) {
                scene_note(meta, "note=own mesh not shown; motion rendered on %s\n", body);
                dq8_mds_free(m);
            } else {
                scene_note(meta, "note=motion-only pack; rendered on %s\n", body);
            }
            m = bm;
            scene_note(meta, "mds=%s (from %s)\n", bname, body);
            load_textures(&body_mem);
        } else if (skel) {
            scene_note(meta, "note=skeleton-only pack whose bones match no body (e.g. effect position markers); nothing to draw\n");
        }
        dq8_mds_free(skel);
    }
    if (!m) {
        scene_note(meta, "skip=no mesh%s\n", mot ? " (motion fits no body)" : "");
        dq8_mot_free(mot);
        free(pair_owner);
        free(body_owner);
        free(owner);
        return meta;
    }
    load_textures(&mem);
    /* Character motions are indexed by the <id>_base1 skeleton; a skin model
     * may order the same bones differently (c002: the head track would turn
     * the helmet bone). Re-point tracks by name when the orders differ. */
    if (mot && !retarget && !strchr(unit, ':')) {
        const char *base = strrchr(unit, '/');
        base = base ? base + 1 : unit;
        const char *us = strchr(base, '_');
        char rig_path[256];
        snprintf(rig_path, sizeof(rig_path), "%.*s_base1.chr", (int)((us ? us : strchr(base, '.')) - unit), unit);
        Dq8Mds *skel = (us || strstr(unit, ".chr")) && strcmp(rig_path, unit) != 0 ? dq8_mds_load_archive(a, rig_path) : NULL;
        if (skel && mot_fits(mot, skel) && dq8_mot_name_coverage(mot, skel, m) >= 0.9f) {
            int moved = dq8_mot_remap_bones(mot, skel, m);
            if (moved) {
                scene_note(meta, "note=motion indexed by %s; %d track(s) re-pointed by bone name\n", rig_path, moved);
            }
        }
        dq8_mds_free(skel);
    }
    if (mot && !retarget && !mot_fits(mot, m)) {
        scene_note(meta, "note=motion %s references bones beyond the model; ignored\n", mot_src);
        dq8_mot_free(mot);
        mot = NULL;
    }

    finish_textures();
    if (g_anim_count) {
        scene_note(meta, "info=texture animation: %d script(s) (#texanime) applied per frame\n", g_anim_count);
    }
    int tris = 0;
    for (int k = 0; k < dq8_mds_mesh_count(m); k++) {
        tris += dq8_mds_faces(m, dq8_mds_mesh(m, k), NULL, 0);
    }
    scene_note(meta, "stats=%u bones, %d meshes, %d triangles\n", m->bone_count,
            dq8_mds_mesh_count(m), tris);

    /* texture problems: borrow from other packs, report what's left (once per
     * texture, with how many faces use it) */
    for (int i = 0; i < 256; i++) {
        const char *tn = dq8_mds_material_texture(m, i);
        if (!tn) {
            break;
        }
        int seen = 0;
        for (int j = 0; j < i; j++) {
            const char *pj = dq8_mds_material_texture(m, j);
            seen |= pj && strcmp(pj, tn) == 0;
        }
        if (seen || !*tn) {
            continue;
        }
        int faces = 0;
        for (int k = 0; k < dq8_mds_mesh_count(m); k++) {
            const Dq8MdsMesh *me = dq8_mds_mesh(m, k);
            int nf = dq8_mds_faces(m, me, NULL, 0);
            Dq8MdsFace *f = nf > 0 ? malloc(sizeof(*f) * (size_t)nf) : NULL;
            if (f) {
                dq8_mds_faces(m, me, f, nf);
                for (int j = 0; j < nf; j++) {
                    const char *ft = dq8_mds_material_texture(m, f[j].material);
                    faces += ft && strcmp(ft, tn) == 0;
                }
                free(f);
            }
        }
        if (faces == 0) {
            continue;   /* material not used by any surface face */
        }
        int ti = find_tex(tn);
        if (ti == -1) {
            const char *src = borrow_texture(a, unit, tn);
            if (src) {
                scene_note(meta, "info=texture '%s' borrowed from %s\n", tn, src);
            } else {
                scene_note(meta, "note=texture '%s' not on the disc (%d faces, magenta)\n", tn, faces);
            }
        } else if (ti >= 0 && !g_tex[ti].tim) {
            scene_note(meta, "note=texture '%s' is built at runtime (texanime; %d faces, cyan)\n", tn,
                    faces);
        }
    }

    /* animation ranges */
    SceneKey *keys = meta->keys;
    int nk = 0;
    if (mot) {
        scene_note(meta, "motion=%s (%d frames, %d tracks)\n", mot_src, mot->frame_count,
                mot->track_count);
        nk = parse_keys(own_motion ? &mem : &pair_mem, own_motion ? want_depth : -1, keys, SCENE_MAX_KEYS);
        if (nk == 0 && mot->frame_count > 1) {
            snprintf(keys[0].name, sizeof(keys[0].name), "(whole motion)");
            keys[0].start = 0;
            keys[0].end = mot->frame_count - 1;
            nk = 1;
        }
    }

    meta->key_count = nk;
    meta->m = m;
    meta->rig = rig;
    meta->mot = mot;
    free(pair_owner);
    free(body_owner);
    free(owner);
    return meta;
}

/* ---------------------------------------------------------------- public */

char **scene_list_units(Dq8Archive *a, const char *filter, int *count)
{
    char **out = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < dq8_archive_count(a); i++) {
        const char *nm = dq8_archive_name(a, i);
        if (!nm) {
            continue;
        }
        char names[256][256];
        int k = 0;
        if (ends_with(nm, ".chr")) {
            snprintf(names[k++], sizeof(names[0]), "%s", nm);
        } else if (ends_with(nm, ".pac") || ends_with(nm, ".pak")) {
            size_t sz;
            uint8_t *d = dq8_archive_load(a, nm, &sz);
            Dq8Pak *p = d ? dq8_pak_open(d, sz) : NULL;
            for (int j = 0; p && j < dq8_pak_count(p) && k < 256; j++) {
                const Dq8PakEntry *e = dq8_pak_entry(p, j);
                if (ends_with(e->name, ".chr")) {
                    snprintf(names[k++], sizeof(names[0]), "%s:%s", nm, e->name);
                }
            }
            dq8_pak_close(p);
            free(d);
        }
        for (int j = 0; j < k; j++) {
            if (filter && !strstr(names[j], filter)) {
                continue;
            }
            if (n == cap) {
                cap = cap ? cap * 2 : 1024;
                out = realloc(out, sizeof(char *) * (size_t)cap);
            }
            out[n++] = strdup(names[j]);
        }
    }
    *count = n;
    return out;
}

void scene_free_list(char **units, int count)
{
    for (int i = 0; i < count; i++) {
        free(units[i]);
    }
    free(units);
}

void scene_free(Scene *s)
{
    if (s) {
        dq8_mot_free(s->mot);
        dq8_mds_free(s->m);
        dq8_mds_free(s->rig);
        free(s->notes);
        free(s);
        free_textures();
    }
}

int scene_pose(const Scene *s, float frame, float *pose)
{
    if (!s || !s->m) {
        return 0;
    }
    int ok = 1;
    if (!s->mot || frame < 0) {
        for (uint32_t b = 0; b < s->m->bone_count; b++) {
            dq8_mds_bone_world(s->m, (int)b, pose + (size_t)b * 16);
        }
    } else {
        ok = pose_at(s->mot, s->rig, s->m, frame, pose);
    }
    if (ok) {   /* billboard bones face the (orthographic) camera: an eye far along the view axis */
        float cy = cosf(s->view_yaw), sy = sinf(s->view_yaw), cp = cosf(s->view_pitch), sp = sinf(s->view_pitch);
        float eye[3] = { cp * sy * 1e6f, sp * 1e6f, cp * cy * 1e6f };
        dq8_mot_billboard(s->m, pose, eye);
    }
    return ok;
}

void scene_set_view(Scene *s, float yaw, float pitch)
{
    if (s) {
        s->view_yaw = yaw;
        s->view_pitch = pitch;
    }
}

void scene_tris(const Scene *s, const float *pose, float frame, int state, SrTriList *out)
{
    out->n = 0;
    if (s && s->m) {
        build_tris(s->m, pose, (state && !s->rig) ? s->mot : NULL, frame, out);
    }
}

void scene_animate_textures(float time)
{
    animate_textures(time);
}
