#ifndef DQ8_TOOLS_SCENE_H
#define DQ8_TOOLS_SCENE_H

/* Loading one model pack ("unit") from the ISO into something drawable, for the
 * debug tools (model_sheet, viewer). Wraps the port's decoders with the pairing
 * rules found during the model review (the MDS format notes, the MOT format notes):
 *  - the main model is info.cfg's MODEL (never the shadow model);
 *  - motion from the model's own nesting level, else paired packs
 *    (<id>_skinN -> <id>_base1, <id>_highN -> <id>_htype);
 *  - motion-only / weapon / rig packs render on a matching body (item/ weapon
 *    codes, <id>_skin1, chara/<token>), retargeted by bone name when the rig
 *    differs; partial prop motions are not forced onto a body;
 *  - textures from the pack, else borrowed from another pack; #texanime
 *    scripts animate them.
 * One scene is loaded at a time (textures and texture animations are module
 * state). */

#include "softrast.h"

#include <dq8/io/archive.h>
#include <dq8/model/mds.h>
#include <dq8/model/mot.h>

#include <stddef.h>

#define SCENE_MAX_KEYS 64

typedef struct SceneKey {
    char name[64];   /* raw Shift-JIS, from info.cfg KEY */
    int  start, end;
} SceneKey;

typedef struct Scene {
    char     unit[256];
    char     title[128];       /* raw Shift-JIS NAME from info.cfg, may be empty */
    Dq8Mds  *m;                /* model to draw (NULL: nothing to draw, see notes) */
    Dq8Mds  *rig;              /* the motion's own skeleton when retargeting */
    Dq8Mot  *mot;
    SceneKey keys[SCENE_MAX_KEYS];
    int      key_count;
    char    *notes;            /* "key=value\n" lines */
    size_t   notes_len, notes_cap;
} Scene;

/* Enumerate units: every .chr in the archive, also inside .pac/.pak bundles
 * ("bundle.pac:inner.chr"). `filter` (substring) may be NULL. Returns a
 * malloc'd array of malloc'd strings; *count receives its length. */
char **scene_list_units(Dq8Archive *a, const char *filter, int *count);
void scene_free_list(char **units, int count);

Scene *scene_load(Dq8Archive *a, const char *unit);
void scene_free(Scene *s);

/* Append a "key=value" note (printf style). `s` may be NULL. */
void scene_note(Scene *s, const char *fmt, ...);

/* Bone world matrices at `frame` (motion pose), or the export pose when the
 * scene has no motion or frame < 0. `pose` holds m->bone_count * 16 floats. */
int scene_pose(const Scene *s, float frame, float *pose);

/* Triangles of the model in `pose`. With `state` set, mesh visibility and
 * material opacity come from the motion at `frame` (not when retargeting). */
void scene_tris(const Scene *s, const float *pose, float frame, int state, SrTriList *out);

/* Apply texture animation (#texanime) at `time` (frames). */
void scene_animate_textures(float time);

#endif /* DQ8_TOOLS_SCENE_H */
