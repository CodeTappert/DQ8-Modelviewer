/* Interactive model viewer: browse every model in the ISO and play its
 * animations live. Uses the port's decoders through tools/common/scene.c, the software
 * rasterizer and the SDL3 platform layer for the window and input.
 *
 *   viewer [filter] [--iso <game iso>] [--anim N] [--autoshot out.bmp]
 *
 * The ISO comes from --iso, else $DQ8_ISO ($DQ8_DATA_DIR for an extracted
 * disc), else the last ISO opened, else a file dialog; it is remembered in the
 * per-user settings directory.
 * --autoshot renders ~20 frames, saves the last one and exits (self-test; works
 * headless with SDL_VIDEODRIVER=offscreen).
 *
 * Controls (F1 shows them in the window):
 *   Up/Down        previous/next model     PgUp/PgDn  +-20 models
 *   Left/Right     previous/next animation (first entry = T-pose)
 *   Space          play/pause              , .        step one frame
 *   mouse drag     orbit                   wheel      zoom
 *   1 2 3          front / side / 3-4 view R          reset camera
 *   /              search (type, Enter)    T          texture animation on/off
 *   S              screenshot (BMP in ./screenshots)
 *   Esc            close search / quit */
#include "../common/scene.h"
#include "../common/softrast.h"

#include <dq8/io/vfs.h>
#include <dq8/platform.h>

#ifdef DQ8_SDL_MAIN   /* SDL provides WinMain for a console-less Windows build */
#include <SDL3/SDL_main.h>
#endif
#ifndef DQ8_VERSION
#define DQ8_VERSION "dev"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#include <iconv.h>
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

typedef struct {
    Dq8Archive *archive;
    char      **units;
    int         unit_count;
    char        filter[128];
    int         cur;            /* unit index */
    Scene      *scene;
    int         anim;           /* -1 = T-pose, else key index */
    float       frame;
    int         playing;
    int         texanim;
    float       yaw, pitch, zoom;
    float       center[3], base_scale;
    int         searching;
    char        search[128];
    int         help;
    char        status[256];
    float      *pose;
    SrTriList   tris;
    float      *zbuf;
    int         zw, zh;
} Viewer;

/* Shift-JIS -> UTF-8 for the window title. */
static void sjis_to_utf8(const char *in, char *out, size_t len)
{
#ifdef _WIN32
    wchar_t w[256];
    int n = MultiByteToWideChar(932, 0, in, -1, w, 256);
    if (n > 0 && WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)len, NULL, NULL) > 0) {
        return;
    }
#else
    iconv_t cd = iconv_open("UTF-8", "CP932");
    if (cd != (iconv_t)-1) {
        char *ip = (char *)in, *op = out;
        size_t il = strlen(in), ol = len - 1;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        iconv_close(cd);
        if (r != (size_t)-1) {
            *op = 0;
            return;
        }
    }
#endif
    snprintf(out, len, "%s", in);
}

static void relist(Viewer *v)
{
    scene_free_list(v->units, v->unit_count);
    v->units = scene_list_units(v->archive, v->filter[0] ? v->filter : NULL, &v->unit_count);
    v->cur = 0;
}

static void fit_camera(Viewer *v)
{
    Scene *s = v->scene;
    if (!s || !s->m) {
        return;
    }
    SrTriList lists[2] = { { 0 }, { 0 } };
    scene_pose(s, -1, v->pose);
    scene_tris(s, v->pose, 0, 0, &lists[0]);
    if (s->key_count && scene_pose(s, (float)s->keys[0].start, v->pose)) {
        scene_tris(s, v->pose, (float)s->keys[0].start, 1, &lists[1]);
    }
    SrCamera c = sr_fit(lists, 2, 1000, 0, 0);
    memcpy(v->center, c.center, sizeof(v->center));
    v->base_scale = c.scale / 1000.0f;   /* per pixel of the smaller window side */
    sr_list_free(&lists[0]);
    sr_list_free(&lists[1]);
}

static void set_title(Viewer *v)
{
    char title[512], name[256] = "", anim[128] = "";
    if (v->scene && v->scene->title[0]) {
        sjis_to_utf8(v->scene->title, name, sizeof(name));
    }
    if (v->scene && v->anim >= 0) {
        sjis_to_utf8(v->scene->keys[v->anim].name, anim, sizeof(anim));
    }
    snprintf(title, sizeof(title), "DQ8 Model Viewer " DQ8_VERSION " - %s %s%s%s", v->unit_count ? v->units[v->cur] : "(no models)",
             name, anim[0] ? " - " : "", anim);
    dq8_platform_set_title(title);
}

static void load_current(Viewer *v)
{
    scene_free(v->scene);
    v->scene = NULL;
    free(v->pose);
    v->pose = NULL;
    if (!v->unit_count) {
        set_title(v);
        return;
    }
    v->scene = scene_load(v->archive, v->units[v->cur]);
    Scene *s = v->scene;
    int bones = s->m ? (int)s->m->bone_count : 0;
    v->pose = malloc(sizeof(float) * 16 * (size_t)(bones ? bones : 1));
    v->anim = s->key_count ? 0 : -1;
    v->frame = v->anim >= 0 ? (float)s->keys[0].start : 0;
    fit_camera(v);
    set_title(v);
}

static void set_anim(Viewer *v, int a)
{
    if (!v->scene) {
        return;
    }
    int n = v->scene->key_count;
    v->anim = a < -1 ? n - 1 : a >= n ? -1 : a;
    v->frame = v->anim >= 0 ? (float)v->scene->keys[v->anim].start : 0;
    set_title(v);
}

static void save_bmp(const Dq8Framebuffer *fb, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    int row = fb->width * 3 + (4 - fb->width * 3 % 4) % 4;
    uint32_t size = 54 + (uint32_t)row * fb->height;
    uint8_t h[54] = { 'B', 'M' };
    memcpy(h + 2, &size, 4);
    h[10] = 54;
    h[14] = 40;
    int32_t w = fb->width, hh = fb->height;
    memcpy(h + 18, &w, 4);
    memcpy(h + 22, &hh, 4);
    h[26] = 1;
    h[28] = 24;
    fwrite(h, 1, 54, f);
    uint8_t *line = calloc(1, (size_t)row);
    for (int y = fb->height - 1; y >= 0; y--) {
        for (int x = 0; x < fb->width; x++) {
            uint32_t p = fb->pixels[(size_t)y * fb->pitch + x];
            line[x * 3] = (uint8_t)p;
            line[x * 3 + 1] = (uint8_t)(p >> 8);
            line[x * 3 + 2] = (uint8_t)(p >> 16);
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
}

static void handle_input(Viewer *v, float dt)
{
    if (v->searching) {
        const char *t = dq8_platform_text();
        if (*t && strlen(v->search) + strlen(t) < sizeof(v->search) - 1) {
            strcat(v->search, t);
        }
        if (dq8_platform_key_pressed(DQ8_KEY_BACKSPACE) && v->search[0]) {
            v->search[strlen(v->search) - 1] = 0;
        }
        if (dq8_platform_key_pressed(DQ8_KEY_ENTER)) {
            snprintf(v->filter, sizeof(v->filter), "%s", v->search);
            v->searching = 0;
            relist(v);
            load_current(v);
        }
        if (dq8_platform_key_pressed(DQ8_KEY_ESCAPE)) {
            v->searching = 0;
        }
        return;
    }
    if (dq8_platform_key_pressed('/')) {
        v->searching = 1;
        snprintf(v->search, sizeof(v->search), "%s", v->filter);
        return;
    }
    int step = dq8_platform_key_pressed(DQ8_KEY_DOWN) - dq8_platform_key_pressed(DQ8_KEY_UP)
             + 20 * (dq8_platform_key_pressed(DQ8_KEY_PAGEDOWN) - dq8_platform_key_pressed(DQ8_KEY_PAGEUP));
    if (step && v->unit_count) {
        v->cur = ((v->cur + step) % v->unit_count + v->unit_count) % v->unit_count;
        load_current(v);
    }
    if (dq8_platform_key_pressed(DQ8_KEY_HOME) && v->unit_count) {
        v->cur = 0;
        load_current(v);
    }
    if (dq8_platform_key_pressed(DQ8_KEY_END) && v->unit_count) {
        v->cur = v->unit_count - 1;
        load_current(v);
    }
    if (dq8_platform_key_pressed(DQ8_KEY_RIGHT)) {
        set_anim(v, v->anim + 1);
    }
    if (dq8_platform_key_pressed(DQ8_KEY_LEFT)) {
        set_anim(v, v->anim - 1);
    }
    if (dq8_platform_key_pressed(' ')) {
        v->playing = !v->playing;
    }
    Scene *s = v->scene;
    if (s && v->anim >= 0) {
        const SceneKey *k = &s->keys[v->anim];
        float len = (float)(k->end - k->start + 1);
        if (v->playing) {
            v->frame += dt * 30.0f;
        }
        if (dq8_platform_key_pressed('.')) {
            v->playing = 0;
            v->frame = floorf(v->frame) + 1;
        }
        if (dq8_platform_key_pressed(',')) {
            v->playing = 0;
            v->frame = floorf(v->frame) - 1;
        }
        while (v->frame > k->end + 0.999f) {
            v->frame -= len;
        }
        while (v->frame < k->start) {
            v->frame += len;
        }
    }
    if (dq8_platform_key_pressed('t')) {
        v->texanim = !v->texanim;
    }
    if (dq8_platform_key_pressed('1')) {
        v->yaw = 0;
        v->pitch = 0;
    }
    if (dq8_platform_key_pressed('2')) {
        v->yaw = 1.5708f;
        v->pitch = 0;
    }
    if (dq8_platform_key_pressed('3') || dq8_platform_key_pressed('r')) {
        v->yaw = 0.6109f;
        v->pitch = 0.2618f;
        v->zoom = 1;
        fit_camera(v);
    }
    if (dq8_platform_key_pressed(DQ8_KEY_F1)) {
        v->help = !v->help;
    }
    int mx, my, buttons;
    float dx, dy;
    dq8_platform_mouse(&mx, &my, &buttons);
    dq8_platform_mouse_motion(&dx, &dy);
    if (buttons & 1) {
        v->yaw += dx * 0.01f;
        v->pitch = fmaxf(-1.5f, fminf(1.5f, v->pitch + dy * 0.01f));
    }
    float wheel = dq8_platform_mouse_wheel();
    if (wheel != 0) {
        v->zoom *= powf(1.15f, wheel);
    }
}

static void draw_text(Dq8Framebuffer *fb, int x, int *y, uint32_t rgb, const char *text)
{
    (void)fb;
    dq8_platform_debug_text(x + 2, *y + 2, 0x000000, text);   /* shadow */
    dq8_platform_debug_text(x, *y, rgb, text);
    *y += 20;
}

static void draw(Viewer *v, Dq8Framebuffer *fb)
{
    if (v->zw != fb->width || v->zh != fb->height) {
        free(v->zbuf);
        v->zbuf = malloc(sizeof(float) * (size_t)fb->width * fb->height);
        v->zw = fb->width;
        v->zh = fb->height;
    }
    SrImage im = { fb->width, fb->height, fb->pixels, v->zbuf };
    sr_clear(&im, 0, 0, fb->width, fb->height, 0xFF282828u);
    Scene *s = v->scene;
    if (s && s->m) {
        float frame = v->anim >= 0 ? v->frame : (s->key_count ? (float)s->keys[0].start : 0);
        if (v->anim >= 0) {
            scene_pose(s, v->frame, v->pose);
        } else {
            scene_pose(s, -1, v->pose);
        }
        scene_tris(s, v->pose, frame, 1, &v->tris);
        if (v->texanim) {
            scene_animate_textures(frame);
        }
        int side = fb->width < fb->height ? fb->width : fb->height;
        SrCamera cam = { { v->center[0], v->center[1], v->center[2] }, v->base_scale * side * v->zoom,
                         v->yaw, v->pitch };
        sr_draw(&im, 0, 0, fb->width, fb->height, &v->tris, &cam);
    }

    int y = 8;
    char line[256];
    snprintf(line, sizeof(line), "[%d/%d] %s", v->unit_count ? v->cur + 1 : 0, v->unit_count,
             v->unit_count ? v->units[v->cur] : "(no models match)");
    draw_text(fb, 8, &y, 0xFFE070, line);
    if (s && s->m) {
        if (v->anim >= 0) {
            const SceneKey *k = &s->keys[v->anim];
            snprintf(line, sizeof(line), "animation %d/%d  frames %d-%d  frame %.0f  %s", v->anim + 1,
                     s->key_count, k->start, k->end, floorf(v->frame), v->playing ? "playing" : "paused");
        } else {
            snprintf(line, sizeof(line), "T-pose (export pose)  %d animation(s)", s->key_count);
        }
        draw_text(fb, 8, &y, 0xFFFFFF, line);
        snprintf(line, sizeof(line), "%u bones, %d meshes, %d triangles drawn%s", s->m->bone_count,
                 dq8_mds_mesh_count(s->m), v->tris.n, v->texanim ? "" : "  (texture animation off)");
        draw_text(fb, 8, &y, 0xB0B0B0, line);
    }
    if (s && s->notes) {   /* the loader's notes (ASCII part) */
        const char *p = s->notes;
        int shown = 0;
        while (*p && shown < 6) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            if ((n > 5 && !strncmp(p, "note=", 5)) || (n > 5 && !strncmp(p, "skip=", 5))
                || (n > 5 && !strncmp(p, "info=", 5) && strncmp(p, "info=texture animation", 22))) {
                snprintf(line, sizeof(line), "%.*s", (int)(n < 120 ? n : 120), p + 5);
                for (char *c = line; *c; c++) {
                    if ((unsigned char)*c >= 128) {
                        *c = '?';
                    }
                }
                draw_text(fb, 8, &y, 0xFFB050, line);
                shown++;
            }
            p += n + (nl ? 1 : 0);
        }
    }
    if (v->searching) {
        snprintf(line, sizeof(line), "search: %s_   (Enter = apply, Esc = cancel)", v->search);
        draw_text(fb, 8, &y, 0x70FF70, line);
    }
    if (v->status[0]) {
        draw_text(fb, 8, &y, 0x70D0FF, v->status);
    }
    int hy = fb->height - (v->help ? 200 : 28);
    if (v->help) {
        static const char *help[] = {
            "Up/Down: model   PgUp/PgDn: +-20   Home/End: first/last",
            "Left/Right: animation (first = T-pose)   Space: play/pause   , . : step frame",
            "Mouse drag: orbit   Wheel: zoom   1/2/3: front/side/3-4   R: reset camera",
            "/: search models   T: texture animation   S: screenshot   Esc: quit",
        };
        for (int i = 0; i < 4; i++) {
            draw_text(fb, 8, &hy, 0xFFFFFF, help[i]);
        }
    } else {
        draw_text(fb, 8, &hy, 0x909090, "F1: help");
    }
}

/* Mounts `path` (an ISO, or a directory when is_dir) and opens its DATA archive. */
static Dq8Archive *open_disc(const char *path, int is_dir)
{
    if (!path || !path[0]) {
        return NULL;
    }
    if (!(is_dir ? dq8_vfs_mount_dir(path) : dq8_vfs_mount_iso(path))) {
        return NULL;
    }
    return dq8_archive_open("DATA");
}

static void last_iso_path(char *out, size_t len)
{
    snprintf(out, len, "%slast_iso.txt", dq8_platform_pref_dir());
}

/* --iso, then $DQ8_DATA_DIR / $DQ8_ISO, then the remembered ISO, then a file
 * dialog until a valid ISO is picked or the user cancels. */
static Dq8Archive *find_disc(const char *iso_arg)
{
    char path[1024] = "", cfg[1024];
    last_iso_path(cfg, sizeof(cfg));
    Dq8Archive *a = NULL;
    if (iso_arg) {
        snprintf(path, sizeof(path), "%s", iso_arg);
        if (!(a = open_disc(path, 0))) {
            fprintf(stderr, "cannot open %s as a DQ8 ISO\n", path);
            return NULL;
        }
    } else if (getenv("DQ8_DATA_DIR")) {
        return open_disc(getenv("DQ8_DATA_DIR"), 1);
    } else if (getenv("DQ8_ISO")) {
        return open_disc(getenv("DQ8_ISO"), 0);
    } else {
        FILE *f = fopen(cfg, "rb");
        if (f) {
            size_t n = fread(path, 1, sizeof(path) - 1, f);
            path[n] = 0;
            path[strcspn(path, "\r\n")] = 0;
            fclose(f);
            a = open_disc(path, 0);
        }
        while (!a) {
            if (!dq8_platform_pick_file("Dragon Quest VIII ISO", "iso", path, sizeof(path))) {
                return NULL;
            }
            if (!(a = open_disc(path, 0))) {
                dq8_platform_message("DQ8 Model Viewer", "That file is not a Dragon Quest VIII (PS2) ISO. Pick another one.");
            }
        }
    }
    FILE *f = fopen(cfg, "wb");
    if (f && dq8_platform_pref_dir()[0]) {
        fputs(path, f);
    }
    if (f) {
        fclose(f);
    }
    return a;
}

int main(int argc, char **argv)
{
    Viewer v;
    memset(&v, 0, sizeof(v));
    const char *autoshot = NULL, *iso = NULL;
    int start_anim = -2;
    snprintf(v.filter, sizeof(v.filter), "chara/");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--autoshot") && i + 1 < argc) {
            autoshot = argv[++i];
        } else if (!strcmp(argv[i], "--anim") && i + 1 < argc) {
            start_anim = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--iso") && i + 1 < argc) {
            iso = argv[++i];
        } else {
            snprintf(v.filter, sizeof(v.filter), "%s", argv[i]);
        }
    }
    if (dq8_platform_init("DQ8 Model Viewer " DQ8_VERSION, 1280, 800) != 0) {
        return 1;
    }
    dq8_vfs_init();
    v.archive = find_disc(iso);
    if (!v.archive) {
        fprintf(stderr, "no game disc: pass --iso <Dragon Quest VIII ISO> or pick one in the dialog\n");
        dq8_platform_shutdown();
        return 1;
    }
    v.units = scene_list_units(v.archive, v.filter, &v.unit_count);
    v.playing = 1;
    v.texanim = 1;
    v.yaw = 0.6109f;
    v.pitch = 0.2618f;
    v.zoom = 1;
    load_current(&v);
    if (start_anim >= -1) {
        set_anim(&v, start_anim);
    }
    dq8_platform_dt();
    int frames_run = 0;
    while (dq8_platform_poll()) {
        float dt = autoshot ? 1.0f / 30 : dq8_platform_dt();
        if (!v.searching && dq8_platform_key_pressed(DQ8_KEY_ESCAPE)) {
            break;
        }
        handle_input(&v, dt);
        Dq8Framebuffer *fb = dq8_platform_begin_frame();
        draw(&v, fb);
        if (!v.searching && dq8_platform_key_pressed('s') && v.unit_count) {
            char path[512], safe[256];
            snprintf(safe, sizeof(safe), "%s", v.units[v.cur]);
            for (char *p = safe; *p; p++) {
                if (*p == '/' || *p == ':' || *p == '\\') {
                    *p = '_';
                }
            }
            MKDIR("screenshots");
            snprintf(path, sizeof(path), "screenshots/%s_a%d_f%.0f.bmp", safe, v.anim, floorf(v.frame));
            save_bmp(fb, path);
            snprintf(v.status, sizeof(v.status), "saved %s", path);
        }
        if (autoshot && ++frames_run == 20) {
            save_bmp(fb, autoshot);
            dq8_platform_end_frame();
            break;
        }
        dq8_platform_end_frame();
    }
    scene_free(v.scene);
    sr_list_free(&v.tris);
    free(v.pose);
    free(v.zbuf);
    scene_free_list(v.units, v.unit_count);
    dq8_archive_close(v.archive);
    dq8_platform_shutdown();
    return 0;
}
