#include <dq8/platform.h>

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SDL3 platform backend: resizable window presenting a software framebuffer,
 * keyboard/mouse/text input, timing, debug text (SDL's built-in 8x8 font,
 * drawn at 2x). */

#define MAX_TEXT_ITEMS 256

typedef struct {
    int      x, y;
    uint32_t rgb;
    char     text[128];
} TextItem;

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static Dq8Framebuffer g_fb;
static double g_prev;

static uint8_t g_down[DQ8_KEY_COUNT], g_pressed[DQ8_KEY_COUNT];
static float g_mx, g_my, g_mdx, g_mdy, g_wheel;
static uint32_t g_buttons;
static char g_text[256];
static TextItem g_items[MAX_TEXT_ITEMS];
static int g_item_count;

static int resize_framebuffer(int w, int h)
{
    if (w < 1 || h < 1) {
        return 0;
    }
    if (g_fb.pixels && g_fb.width == w && g_fb.height == h) {
        return 1;
    }
    uint32_t *px = realloc(g_fb.pixels, sizeof(uint32_t) * (size_t)w * h);
    if (!px) {
        return 0;
    }
    g_fb.pixels = px;
    g_fb.width = w;
    g_fb.height = h;
    g_fb.pitch = w;
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
    }
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    return g_texture != NULL;
}

int dq8_platform_init(const char *title, int width, int height)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_CreateWindowAndRenderer(title, width, height, SDL_WINDOW_RESIZABLE, &g_window, &g_renderer)) {
        fprintf(stderr, "SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(g_renderer, 1);
    int w, h;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    if (!resize_framebuffer(w, h)) {
        fprintf(stderr, "framebuffer allocation failed\n");
        return 1;
    }
    SDL_StartTextInput(g_window);
    g_prev = dq8_platform_time();
    return 0;
}

void dq8_platform_shutdown(void)
{
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
    }
    if (g_renderer) {
        SDL_DestroyRenderer(g_renderer);
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
    }
    free(g_fb.pixels);
    memset(&g_fb, 0, sizeof(g_fb));
    SDL_Quit();
}

static int map_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_ESCAPE: return DQ8_KEY_ESCAPE;
    case SDLK_RETURN: case SDLK_KP_ENTER: return DQ8_KEY_ENTER;
    case SDLK_BACKSPACE: return DQ8_KEY_BACKSPACE;
    case SDLK_TAB: return DQ8_KEY_TAB;
    case SDLK_LEFT: return DQ8_KEY_LEFT;
    case SDLK_RIGHT: return DQ8_KEY_RIGHT;
    case SDLK_UP: return DQ8_KEY_UP;
    case SDLK_DOWN: return DQ8_KEY_DOWN;
    case SDLK_PAGEUP: return DQ8_KEY_PAGEUP;
    case SDLK_PAGEDOWN: return DQ8_KEY_PAGEDOWN;
    case SDLK_HOME: return DQ8_KEY_HOME;
    case SDLK_END: return DQ8_KEY_END;
    case SDLK_F1: return DQ8_KEY_F1;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return DQ8_KEY_SHIFT;
    case SDLK_LCTRL: case SDLK_RCTRL: return DQ8_KEY_CTRL;
    default:
        return (k >= 32 && k < 127) ? (int)k : 0;   /* SDL keycodes are lowercase ASCII */
    }
}

int dq8_platform_poll(void)
{
    memset(g_pressed, 0, sizeof(g_pressed));
    g_mdx = g_mdy = g_wheel = 0;
    g_text[0] = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            return 0;
        case SDL_EVENT_KEY_DOWN: {
            int k = map_key(e.key.key);
            if (k > 0 && k < DQ8_KEY_COUNT) {
                g_down[k] = 1;
                g_pressed[k] = 1;
            }
            break;
        }
        case SDL_EVENT_KEY_UP: {
            int k = map_key(e.key.key);
            if (k > 0 && k < DQ8_KEY_COUNT) {
                g_down[k] = 0;
            }
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
            strncat(g_text, e.text.text, sizeof(g_text) - strlen(g_text) - 1);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            g_mdx += e.motion.xrel;
            g_mdy += e.motion.yrel;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            g_wheel += e.wheel.y;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            resize_framebuffer(e.window.data1, e.window.data2);
            break;
        default:
            break;
        }
    }
    SDL_MouseButtonFlags b = SDL_GetMouseState(&g_mx, &g_my);
    g_buttons = ((b & SDL_BUTTON_LMASK) ? 1u : 0u) | ((b & SDL_BUTTON_RMASK) ? 2u : 0u)
              | ((b & SDL_BUTTON_MMASK) ? 4u : 0u);
    return 1;
}

Dq8Framebuffer *dq8_platform_begin_frame(void)
{
    g_item_count = 0;
    return &g_fb;
}

void dq8_platform_end_frame(void)
{
    SDL_UpdateTexture(g_texture, NULL, g_fb.pixels, g_fb.pitch * 4);
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, g_texture, NULL, NULL);
    SDL_SetRenderScale(g_renderer, 2.0f, 2.0f);
    for (int i = 0; i < g_item_count; i++) {
        const TextItem *t = &g_items[i];
        SDL_SetRenderDrawColor(g_renderer, (Uint8)(t->rgb >> 16), (Uint8)(t->rgb >> 8), (Uint8)t->rgb, 255);
        SDL_RenderDebugText(g_renderer, t->x / 2.0f, t->y / 2.0f, t->text);
    }
    SDL_SetRenderScale(g_renderer, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderPresent(g_renderer);
}

double dq8_platform_time(void)
{
    return SDL_GetTicksNS() / 1e9;
}

float dq8_platform_dt(void)
{
    double now = dq8_platform_time();
    float dt = (float)(now - g_prev);
    g_prev = now;
    return dt;
}

int dq8_platform_key_down(int key)
{
    return key > 0 && key < DQ8_KEY_COUNT && g_down[key];
}

int dq8_platform_key_pressed(int key)
{
    return key > 0 && key < DQ8_KEY_COUNT && g_pressed[key];
}

void dq8_platform_mouse(int *x, int *y, int *buttons)
{
    *x = (int)g_mx;
    *y = (int)g_my;
    *buttons = (int)g_buttons;
}

void dq8_platform_mouse_motion(float *dx, float *dy)
{
    *dx = g_mdx;
    *dy = g_mdy;
}

float dq8_platform_mouse_wheel(void)
{
    return g_wheel;
}

const char *dq8_platform_text(void)
{
    return g_text;
}

void dq8_platform_set_title(const char *utf8)
{
    SDL_SetWindowTitle(g_window, utf8);
}

void dq8_platform_debug_text(int x, int y, uint32_t rgb, const char *text)
{
    if (g_item_count < MAX_TEXT_ITEMS) {
        TextItem *t = &g_items[g_item_count++];
        t->x = x;
        t->y = y;
        t->rgb = rgb;
        snprintf(t->text, sizeof(t->text), "%s", text);
    }
}

typedef struct {
    SDL_AtomicInt done;
    char         *out;
    size_t        outsz;
    int           ok;
} PickState;

static void SDLCALL pick_done(void *userdata, const char *const *files, int filter)
{
    PickState *st = userdata;
    (void)filter;
    if (files && files[0]) {
        snprintf(st->out, st->outsz, "%s", files[0]);
        st->ok = 1;
    } else if (!files) {
        fprintf(stderr, "file dialog failed: %s\n", SDL_GetError());
    }
    SDL_SetAtomicInt(&st->done, 1);
}

int dq8_platform_pick_file(const char *filter_name, const char *ext, char *out, size_t outsz)
{
    PickState st = { { 0 }, out, outsz, 0 };
    SDL_DialogFileFilter filter = { filter_name, ext };
    SDL_ShowOpenFileDialog(pick_done, &st, g_window, &filter, 1, NULL, false);
    /* the callback may run on another thread or while events are pumped */
    while (!SDL_GetAtomicInt(&st.done)) {
        SDL_Event e;
        SDL_WaitEventTimeout(&e, 20);
    }
    return st.ok;
}

const char *dq8_platform_pref_dir(void)
{
    static char *dir;
    if (!dir) {
        dir = SDL_GetPrefPath("CodeTappert", "DQ8ModelViewer");
    }
    return dir ? dir : "";
}

void dq8_platform_message(const char *title, const char *text)
{
    fprintf(stderr, "%s: %s\n", title, text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, g_window);
}
