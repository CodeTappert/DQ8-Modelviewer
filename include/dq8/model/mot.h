#ifndef DQ8_MODEL_MOT_H
#define DQ8_MODEL_MOT_H

#include <dq8/io/archive.h>
#include <dq8/model/mds.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DQ8 skeletal motion (`.mot`, magic " MOT"). See the MOT format notes.
 *
 * A motion has two track lists. List A holds per-bone tracks with rotation
 * (unit quaternion x,y,z,w), translation, scale and visibility channels; list
 * B holds per-object tracks (opacity, a 3-float parameter). Values are float32,
 * or int16/int8 scaled per component (`raw * scale[k] / 2^(bits-1)`); key
 * times are u16 frame numbers, or one key per frame when a channel has no time
 * array (times are signed: some keys sit before frame 0). Sampling interpolates linearly between keys (normalized for
 * quaternions, along the shorter arc); visibility steps. */

enum {
    DQ8_MOT_ROTATE = 1,     /* 4 components: quaternion x,y,z,w */
    DQ8_MOT_TRANSLATE = 3,  /* 3 components: local translation */
    DQ8_MOT_SCALE = 4,      /* 3 components: local scale */
    DQ8_MOT_ALPHA = 5,      /* 1 component: opacity 0..1 (list B) */
    DQ8_MOT_PARAM3 = 6,     /* 3 components, meaning unknown (list B) */
    DQ8_MOT_VISIBLE = 7,    /* 1 component: 0/1, stepped */
};

typedef struct Dq8MotChannel {
    int      type;          /* DQ8_MOT_* */
    int      components;
    int      value_size;    /* 0x20 float32, 0x10 int16, 0x08 int8 */
    bool     scaled;        /* flag byte 0: integer values use `scale` */
    int      count;         /* keys */
    int      start;         /* first key's frame (per-frame keys run from it) */
    uint32_t times;         /* offset of s16[count] frame numbers (may be < 0), 0 = per frame from `start` */
    uint32_t values;        /* offset of value[count][components] */
    float    scale[4];
} Dq8MotChannel;

#define DQ8_MOT_MAX_CHANNELS 4

typedef struct Dq8MotTrack {
    int           list;     /* 0 = list A (bones), 1 = list B (objects) */
    int           bone;     /* bone index (list A) / object index (list B) */
    int           channel_count;
    Dq8MotChannel channels[DQ8_MOT_MAX_CHANNELS];
} Dq8MotTrack;

typedef struct Dq8Mot {
    uint8_t    *blob;       /* copy of the file (owned) */
    size_t      blob_size;
    int         version;    /* 0 or 1 (channel header layout differs) */
    int         track_count;
    int         frame_count;    /* last key frame + 1 */
    Dq8MotTrack *tracks;        /* track_count entries (owned) */
} Dq8Mot;

/* Parse a MOT blob. NULL if not a MOT. */
Dq8Mot *dq8_mot_open(const uint8_t *data, size_t size);

/* Load the first `.mot` member of `container_path` (a `.chr` pack). */
Dq8Mot *dq8_mot_load_archive(Dq8Archive *archive, const char *container_path);

void dq8_mot_free(Dq8Mot *mot);

/* Sample channel `type` of list-A track `bone` at `frame` into `out` (up to
 * 4 floats, see DQ8_MOT_*). Returns false if there is no such channel. */
bool dq8_mot_sample(const Dq8Mot *mot, int bone, int type, float frame, float out[4]);

/* Same for any track list (0 = A, 1 = B). */
bool dq8_mot_sample_list(const Dq8Mot *mot, int list, int index, int type, float frame,
                         float out[4]);

/* Pose `model` at `frame`: writes model->bone_count world matrices (16 floats
 * each, row-major, row vectors) into `worlds`, ready for dq8_mds_skin_vertex().
 * Per bone, starting from its local matrix (bone +0x20): the rotation track
 * replaces the 3x3 (quaternion), the scale track pre-multiplies it
 * (diag(s) * R), the translation track replaces row 3; then
 * World = Local * World[parent]. Bones without tracks keep their local matrix.
 * Returns false on error. See the MOT format notes section 4. */
bool dq8_mot_pose(const Dq8Mot *mot, const Dq8Mds *model, float frame, float *worlds);

/* Pose `target` with a motion authored for another skeleton `rig`, matching
 * bones by name. Some motions are made for an extended rig: the hero's dungeon
 * motions (c001_base2: 173 bones, with chn and eff helper bones) drive the 88-bone
 * body (c001_skin1), whose bones all exist in the rig by name at identical
 * bind positions. Each matched target bone gets the rig bone's motion relative
 * to its bind pose: World_t = Bind_t * inverse(Bind_rig) * World_rig. Unmatched
 * bones follow their parent. Writes target->bone_count matrices. */
bool dq8_mot_pose_retarget(const Dq8Mot *mot, const Dq8Mds *rig, const Dq8Mds *target,
                           float frame, float *worlds);

/* Fraction (0..1) of the motion's list-A tracks whose rig bone name exists in
 * `target`; 1 when rig == target. Used to decide whether a motion belongs to a
 * model. */
float dq8_mot_name_coverage(const Dq8Mot *mot, const Dq8Mds *rig, const Dq8Mds *target);

/* Is mesh `mesh_index` of `model` visible at `frame`? A visibility track
 * (DQ8_MOT_VISIBLE, stepped 0/1) on the mesh's owner bone or any ancestor
 * hides it. E.g. en141a's two arrows (ya01 in hand, ya04 flying) alternate.
 * Meshes without visibility tracks are visible. */
bool dq8_mot_mesh_visible(const Dq8Mot *mot, const Dq8Mds *model, int mesh_index, float frame);

/* Opacity of material `material` at `frame`: the list-B opacity track
 * (DQ8_MOT_ALPHA) for that material index if the motion has one, else
 * `resting` (the material's diffuse alpha). List B is indexed by material:
 * e.g. en036a's claw trail (material 10, nail0) is 0 except during the swipe. */
float dq8_mot_material_alpha(const Dq8Mot *mot, int material, float frame, float resting);

#endif /* DQ8_MODEL_MOT_H */
