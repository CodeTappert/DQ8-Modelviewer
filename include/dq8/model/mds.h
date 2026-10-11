#ifndef DQ8_MODEL_MDS_H
#define DQ8_MODEL_MDS_H

#include <dq8/io/archive.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* DQ8 model (`.mds`) — Level-5 engine format (a variant of Dark Cloud's "MDS0";
 * here magic is `" MDS"`). See the format notes and the REFERENCES format notes.
 *
 * Header (reversed):
 *   0x00 " MDS\0"
 *   0x08 bone count
 *   0x20 mesh count
 *   0x24 mesh-table offset
 * Each mesh is a `" MDT"` block:
 *   0x00 " MDT\0"
 *   0x08 header size
 *   0x0c mesh size (stride to the next mesh)
 *   0x10 format flags (e.g. 0x10080810)
 *
 * Full layout (bones, skinning, materials): the MDS format notes. */

#define DQ8_MDS_MAX_MESHES 64

typedef struct Dq8MdsMesh {
    uint32_t offset;        /* within the MDS blob */
    uint32_t header_size;
    uint32_t data_size;     /* size of this mesh block (stride) */
    uint32_t format;        /* flags at MDT+0x10 */
    uint32_t vertex_count;  /* descriptor at MDT+header_size */
    uint32_t vertex_offset;
    uint32_t normal_count;
    uint32_t normal_offset;
    uint32_t uv_count;
    uint32_t uv_offset;
    uint32_t influences;        /* descriptor +0x20: weights per vertex, 0 = rigid */
    uint32_t weight_offset;     /* +0x24: u16[vertex_count][influences], 0x8000 = 1.0 */
    uint32_t bone_index_offset; /* +0x28: u16[vertex_count][influences], 0xFFFF = none */
    float    pos_scale[3];  /* MDT+0x20: position = int16 * pos_scale / 32768 */
    float    nrm_scale[3];  /* MDT+0x40: normal = int8 * nrm_scale / 128 (128/127) */
    float    uv_scale[2];   /* MDT+0x50: uv = u8 * uv_scale / 128 */
    uint8_t  pos_size;      /* element size from the format word (0x20/0x10/0x08) */
    uint8_t  normal_size;
    uint8_t  uv_size;
    uint8_t  color_size;
    const uint8_t *data;    /* mesh block bytes (header_size .. data_size) */
} Dq8MdsMesh;

typedef struct Dq8Mds {
    uint8_t    *blob;       /* copy of the MDS bytes (owned) */
    size_t      blob_size;
    uint32_t    bone_count;
    int         mesh_count;
    Dq8MdsMesh  meshes[DQ8_MDS_MAX_MESHES];
} Dq8Mds;

/* Parse an MDS blob. NULL if not an MDS. */
Dq8Mds *dq8_mds_open(const uint8_t *data, size_t size);

/* Load from `container_path` (a `.chr/.mpk/.pcp` PAK), following members to the
 * `.mds`. */
Dq8Mds *dq8_mds_load_archive(Dq8Archive *archive, const char *container_path);

void dq8_mds_free(Dq8Mds *m);

int dq8_mds_mesh_count(const Dq8Mds *m);
const Dq8MdsMesh *dq8_mds_mesh(const Dq8Mds *m, int index);

/* The MDS bytes (for downstream geometry decoding). */
const uint8_t *dq8_mds_blob(const Dq8Mds *m, size_t *out_size);

/* Vertex `i` of `mesh` written into `out` as (x,y,z,w=1). Returns false if out
 * of range. DQ8 stores positions as 3×int16 (stride 6 bytes) at
 * `descriptor + vertex_offset`, dequantized as `raw * pos_scale[k] / 32768`.
 * Positions are in the owner bone's space (the bone whose +0x0C equals the
 * mesh index); use dq8_mds_skin_vertex() to place them. */
bool dq8_mds_vertex(const Dq8Mds *m, const Dq8MdsMesh *mesh, int i, float out[4]);

/* World matrix (local chain, i.e. the exported pose) of mesh `mesh_index`'s
 * owner bone (the bone whose +0x0C equals the index), as 16 floats, row-major,
 * row vectors: placed = (x,y,z,1) · M. This places rigid meshes. Skinned meshes
 * are bound in the pose stored at bone +0x60, which can differ from the chain
 * (the MDS format notes section 4). Returns false if no bone owns the mesh. */
bool dq8_mds_mesh_world(const Dq8Mds *m, int mesh_index, float out[16]);

/* --- Skeleton and skinning (the MDS format notes section 4) ---
 * Matrices are 16 floats, row-major, row vectors (p' = (x,y,z,1) * M).
 * An .mds holds two poses: the export pose (bone +0x20 local matrices composed
 * up the parents; motion frame 0 matches it) and the skin bind pose (world
 * matrices stored at bone +0x60). They usually agree, but not always (the
 * bingo crane differs by 15.4 units). */

/* Name of `bone` (string table), "" if unavailable. */
const char *dq8_mds_bone_name(const Dq8Mds *m, int bone);

/* Index of the bone named `name`, or -1. */
int dq8_mds_bone_find(const Dq8Mds *m, const char *name);

/* Parent of `bone` (-1 = root or error). */
int dq8_mds_bone_parent(const Dq8Mds *m, int bone);

/* Bone flags (bone +0x14, copied to the game's mgCFrame +0x26). */
enum {
    DQ8_MDS_BONE_BILLBOARD = 0x06,       /* 2/4: faces the camera [not handled] */
    DQ8_MDS_BONE_SCALE_COMPENSATE = 0x10 /* scale does not shear children, see dq8_mot_pose */
};
unsigned dq8_mds_bone_flags(const Dq8Mds *m, int bone);

/* Local (parent-relative) matrix of `bone`, bone +0x20. */
bool dq8_mds_bone_local(const Dq8Mds *m, int bone, float out[16]);

/* Export-pose world matrix of `bone`. */
bool dq8_mds_bone_world(const Dq8Mds *m, int bone, float out[16]);

/* Skin bind matrix of `bone`: the stored +0x60 world matrix where present
 * (+0x9C == 1.0), else the export-pose world. */
bool dq8_mds_bone_skin_bind(const Dq8Mds *m, int bone, float out[16]);

/* Owner bone of mesh `mesh_index`, or -1. */
int dq8_mds_mesh_owner(const Dq8Mds *m, int mesh_index);

/* Skin influences of vertex `i`: up to 4 (bone, weight) pairs with weight > 0.
 * Returns the count (0 for rigid meshes, -1 on error). */
int dq8_mds_vertex_weights(const Dq8Mds *m, const Dq8MdsMesh *mesh, int i,
                           int bones[4], float weights[4]);

/* Place vertex `i` of mesh `mesh_index` in a pose. `pose` holds bone_count
 * world matrices (e.g. from a motion), or NULL for the export pose.
 *   rigid:   p * Pose[owner]
 *   skinned: q = p * SkinBind[owner];  sum of w * q * inverse(SkinBind[b]) * Pose[b] */
bool dq8_mds_skin_vertex(const Dq8Mds *m, int mesh_index, int i, const float *pose,
                         float out[3]);

/* UV `i` of `mesh` written into `out` as (u,v) in [0,1]. DQ8 stores each UV as
 * 2×u8 at `descriptor + uv_offset`, dequantized as `raw * uv_scale[k] / 128`.
 * Returns false if out of range. */
bool dq8_mds_uv(const Dq8Mds *m, const Dq8MdsMesh *mesh, int i, float out[2]);

/* Primitive decode. The mesh's primitive list (at `mesh->offset + 0x74`) is a
 * linked list of nodes; each node's u16 index array (`node + 0x10`, count
 * `u16@node+6`) is a triangle strip over the mesh's vertices. */
int dq8_mds_triangle_count(const Dq8Mds *m, const Dq8MdsMesh *mesh);

/* Fill up to `max` triangles (3 u16 vertex indices each, stacked) into `out`.
 * Returns the number of triangles written. */
int dq8_mds_triangles(const Dq8Mds *m, const Dq8MdsMesh *mesh,
                      uint16_t *out, int max);

/* Like dq8_mds_triangles but also yields UV indices: `out` holds 6 u16 per
 * triangle (v0,v1,v2, uv0,uv1,uv2). UVs come from the primitive node's entry
 * array (`entry[1] = node + *(entry+4)`); `entry[2]` is normals. */
int dq8_mds_triangles_uv(const Dq8Mds *m, const Dq8MdsMesh *mesh,
                         uint16_t *out, int max);

/* Surface triangle of a mesh: vertex, UV and normal indices, the material of
 * the UV-mapped pass, and the node's environment-mapped pass (a second entry
 * with no UVs whose material has mapping mode 2: metal shine, weapon glints),
 * or -1. */
typedef struct Dq8MdsFace {
    uint16_t v[3];
    uint16_t uv[3];
    uint16_t n[3];
    int      material;
    int      env_material;
} Dq8MdsFace;

/* Surface triangles of `mesh` (UV-mapped pass only: reflection passes without
 * UVs and the 0x10 outline styles are skipped; strip winding flips on odd
 * triangles; degenerate triangles are dropped). Writes up to `max` faces to
 * `out` (may be NULL to count) and returns the total. See the MDS format notes 3.4. */
int dq8_mds_faces(const Dq8Mds *m, const Dq8MdsMesh *mesh, Dq8MdsFace *out, int max);

/* Normal `i` of `mesh` (unit-ish vector; int8 * scale / 128, or float32). */
bool dq8_mds_normal(const Dq8Mds *m, const Dq8MdsMesh *mesh, int i, float out[3]);

/* Rotate direction `dir` (e.g. a normal) of vertex `i` of mesh `mesh_index` into
 * a pose with the same bone blend as dq8_mds_skin_vertex (no translation).
 * Not renormalized. */
bool dq8_mds_skin_direction(const Dq8Mds *m, int mesh_index, int i, const float dir[3],
                            const float *pose, float out[3]);

/* Mapping mode of material `index` (material +0x40): 0 = UV-mapped,
 * DQ8_MDS_MAP_ENV (2) = environment map (UVs from the view-space normal),
 * 1 = unnamed untextured materials. */
#define DQ8_MDS_MAP_ENV 2
int dq8_mds_material_mapping(const Dq8Mds *m, int index);

/* Diffuse color of material `index` (RGBA floats, material +0x00). The alpha
 * is the material's resting opacity (motions can animate it, see
 * dq8_mot_material_alpha). Returns false if out of range. */
bool dq8_mds_material_color(const Dq8Mds *m, int index, float rgba[4]);

/* Render-flag word of material `index` (material +0x44), 0 if out of range.
 * Bit DQ8_MDS_MAT_ADDITIVE (0x10) marks additive-blended materials: swing
 * trails (b1000a_nail 0xba, en041k00 0x3a), flares and auras (0x43a); their
 * textures are glows/streaks on black. 0x400 without 0x10 marks translucent
 * blur copies (monsini2_blur 0x42b). Other bits are not decoded yet (common
 * plain values 0x2e, 0x20, 0x2c). */
#define DQ8_MDS_MAT_ADDITIVE 0x10u
uint32_t dq8_mds_material_flags(const Dq8Mds *m, int index);

/* Texture name of material `index` ("" if untextured), or NULL if out of range.
 * Points into the model's string table. */
const char *dq8_mds_material_texture(const Dq8Mds *m, int index);

#endif /* DQ8_MODEL_MDS_H */
