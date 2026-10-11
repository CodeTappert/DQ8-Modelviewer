#ifndef DQ8_PLATFORM_H
#define DQ8_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Minimal platform abstraction: window, input, timing and host dialogs.
 * Backend: src/platform/platform_sdl3.c (SDL3). */

typedef struct Dq8Framebuffer {
    uint32_t *pixels;   /* 0xAARRGGBB */
    int width;
    int height;
    int pitch;          /* in pixels */
} Dq8Framebuffer;

/* Returns 0 on success. The window is resizable; the framebuffer follows it. */
int dq8_platform_init(const char *title, int width, int height);
void dq8_platform_shutdown(void);

/* Pumps input/events; returns 0 when the user asked to quit. */
int dq8_platform_poll(void);

Dq8Framebuffer *dq8_platform_begin_frame(void);
void dq8_platform_end_frame(void);

/* Monotonic time in seconds, and the delta since the previous call. */
double dq8_platform_time(void);
float dq8_platform_dt(void);

/* --- input (state as of the last dq8_platform_poll) --- */

/* Keys: printable ASCII keys use their lowercase character ('a', '1', ',');
 * the rest use these codes. */
enum {
    DQ8_KEY_ESCAPE = 256,
    DQ8_KEY_ENTER,
    DQ8_KEY_BACKSPACE,
    DQ8_KEY_TAB,
    DQ8_KEY_LEFT,
    DQ8_KEY_RIGHT,
    DQ8_KEY_UP,
    DQ8_KEY_DOWN,
    DQ8_KEY_PAGEUP,
    DQ8_KEY_PAGEDOWN,
    DQ8_KEY_HOME,
    DQ8_KEY_END,
    DQ8_KEY_F1,
    DQ8_KEY_SHIFT,
    DQ8_KEY_CTRL,
    DQ8_KEY_COUNT
};

int dq8_platform_key_down(int key);
/* Pressed since the previous poll (key repeat counts as a press). */
int dq8_platform_key_pressed(int key);

/* Mouse position in framebuffer pixels, buttons bit 0 = left, 1 = right,
 * 2 = middle; motion and wheel are accumulated since the previous poll. */
void dq8_platform_mouse(int *x, int *y, int *buttons);
void dq8_platform_mouse_motion(float *dx, float *dy);
float dq8_platform_mouse_wheel(void);

/* UTF-8 text typed since the previous poll ("" if none). */
const char *dq8_platform_text(void);

/* --- debug output --- */

/* Window title (UTF-8). */
void dq8_platform_set_title(const char *utf8);

/* Queue ASCII debug text at framebuffer pixel (x, y), drawn over the frame
 * at end_frame (16 px glyphs). */
void dq8_platform_debug_text(int x, int y, uint32_t rgb, const char *text);

/* --- host dialogs / storage --- */

/* Native open-file dialog listing files with extension `ext` (e.g. "iso").
 * Blocks until the user picks a file (returns 1, UTF-8 path in out) or
 * cancels (0). */
int dq8_platform_pick_file(const char *filter_name, const char *ext, char *out, size_t outsz);

/* Per-user writable settings directory ending in a path separator, or "". */
const char *dq8_platform_pref_dir(void);

/* Modal message box (falls back to stderr). */
void dq8_platform_message(const char *title, const char *text);

#endif /* DQ8_PLATFORM_H */
