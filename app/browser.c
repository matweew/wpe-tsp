/*
 * wpe-tsp: minimal WPE WebKit browser for TrimUI Smart Pro.
 *
 * Implements a tiny WPEPlatform (display/toplevel/view) on top of SDL2:
 *  - the WebProcess renders with GLES (PowerVR, surfaceless) into DMA-buf frames
 *    (ION, WebKit patch 0008) that we show as SDL textures without copying (dmabuf.c),
 *    or, as the fallback, hands us shared-memory frames (WPEBufferSHM, ARGB8888)
 *    that we upload into an SDL texture;
 *  - everything is presented via the GE8300 SDL2 renderer;
 *  - the gamepad drives a virtual mouse pointer, scrolling and navigation.
 *
 * Controls (NetSurf-port style, A is the main action, B only cancels):
 *   left stick   move pointer            A       left click (hold to drag/select)
 *                (light tilt = precise)  B       stop loading
 *   right stick  smooth scroll           Y       address bar (keyboard)
 *   d-pad        arrow keys              X       reload
 *   L1 / R1      page up / page down     START   Enter
 *   L2 / R2      top / bottom of page    SELECT  menu (back, forward, home, zoom, exit)
 *   MENU is left to the system.
 *
 * Text fields open the on-screen keyboard (osk.c) when clicked.
 */
#include <SDL.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#include "dmabuf.h"
#include "downloads.h"
#include "gamepad.h"
#include "menu.h"
#include "osk.h"
#include "pages.h"
#include "player.h"
#include "ytdlp.h"
#include "hid.h"

#define SCREEN_W 1280            /* physical screen (landscape) */
#define SCREEN_H 720
/* Device scale: pages are laid out for SCREEN/SCALE CSS pixels and rendered at full resolution.
 * Overridable with WPE_TSP_SCALE. */
#define DEFAULT_SCALE 1.5

/* Memory budget (MB) for the device's 1 GB RAM. WebKit frees caches at the conservative /
 * strict fractions of the limit; the web process is killed (and the page reloaded) at the kill
 * fraction, before the kernel OOM killer would take down the whole browser. */
#define WEB_PROCESS_MEMORY_LIMIT_MB 550  /* default; settings.conf PAGE_MEMORY_LIMIT_MB */
#define NETWORK_PROCESS_MEMORY_LIMIT_MB 200
#define MEMORY_POLL_INTERVAL_S 5.0
/* System-wide watchdog: below this much MemAvailable the page is killed. Without swap the
 * kernel thrashes (re-reading code from the SD card) long before its OOM killer acts. */
#define LOW_MEMORY_KILL_MB 90
#define WATCHDOG_INTERVAL_MS 1000

/* Defaults for settings.conf values (see launch.sh) */
#define DEFAULT_POINTER_HIDE_SECONDS 10   /* 0 = never hide */
#define DEFAULT_HISTORY_SIZE 20           /* 0 = don't record history */
#define DEFAULT_DOWNLOAD_DIR "/mnt/SDCARD/Downloads"
#define DEFAULT_KEYBOARD_LAYOUTS "us,ua,ru" /* physical and on-screen keyboards */
#define NOTICE_MS 2000                       /* short status-strip notices (keyboard layout) */
/* USER_AGENT=mobile: iPhone Safari (our engine is WebKit, so sites' Safari code paths fit best).
 * Mobile sites are much lighter, which matters with 1 GB of RAM. */
#define MOBILE_USER_AGENT "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 " \
                          "(KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1"
#define MAX_MEDIA_ITEMS 14                /* menu rows for "video/audio on this page" */
#define SEARCH_URL "https://www.google.com/search?q="

#define AXIS_DEADZONE 6000
#define POINTER_MAX_SPEED 14.0  /* px per tick at full stick deflection */
#define SCROLL_MAX_SPEED 40.0   /* px per tick at full stick deflection */
#define KEY_REPEAT_DELAY_MS 350 /* d-pad arrow keys */
#define KEY_REPEAT_RATE_MS 50
#define TICK_MS 16

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;       /* shared-memory frames are uploaded into this one */
    SDL_Texture *frame;         /* the page frame to show (texture or a DMA-buf's), or NULL */
    SDL_Joystick *joystick;

    WebKitWebView *web_view;
    WPEView *wpe_view;
    GMainLoop *loop;

    gboolean portrait;          /* page shown rotated 90 degrees counterclockwise, like mpv's portrait videos */
    int screen_w, screen_h;     /* logical screen: SCREEN_W x SCREEN_H, swapped in portrait mode */
    SDL_Texture *canvas;        /* portrait mode: everything is drawn here, then rotated onto the screen */

    double px, py;              /* pointer position */
    int axis[NUM_AXES];         /* joystick axes (as seen by the user in portrait mode) */
    gboolean trigger_down[2];   /* L2, R2 (analog axes) */
    guint arrow_keyval;         /* arrow key held via d-pad, 0 if none */
    guint32 arrow_since, arrow_last;
    guint pointer_buttons;      /* WPE_MODIFIER_POINTER_BUTTON* currently held */
    gboolean needs_present;
    const char *home_url;
    const char *search_url;     /* query is appended (URL-escaped) */

    double scale;               /* device scale factor (WPE_TSP_SCALE) */
    gboolean pointer_hidden;    /* auto-hidden after pointer_hide_ms without pointer use */
    guint32 pointer_used_ms;    /* last pointer movement or click */
    guint32 pointer_hide_ms;
    gboolean swallow_a_release; /* A only revealed the hidden pointer: don't send the release */
    guint swallow_mouse;        /* mouse buttons (bit n = button n) whose press only revealed the pointer */
    char *link_under_pointer;   /* from WebKit's hit test, for "Save link under pointer" */
    gboolean editable_under_pointer; /* from WebKit's hit test: a text field / editable area */
    WebKitPolicyDecision *pending_download; /* response waiting for the download prompt */
    GPtrArray *media_uris;      /* candidates shown by "Save video/audio from this page" */
    gboolean play_youtube;      /* open YouTube video pages in mpv (PLAY_YOUTUBE_IN_MPV) */
    char *video_page;           /* watch URL of the YouTube page last sent to mpv (canonical) */
    char *user_agent;           /* configured UA (USER_AGENT), NULL = WebKit's own (desktop) */
    gboolean ua_switched;       /* SELECT menu switched to the other site version (desktop <-> mobile) */
    double load_progress;       /* 0..1 while loading, 1 when done */
    WPEInputMethodContext *im_context;  /* context of the focused editable, if any */
    guint32 last_click_ms;
    gboolean hid_typing;        /* a physical key was typed: form fields don't pop up the on-screen keyboard */
    char *notice;               /* short status-strip message, cleared by notice_timer */
    guint notice_timer;
    int view_height;
} App;

static App app;

static guint32 now_ms(void)
{
    return (guint32)(g_get_monotonic_time() / 1000);
}

/* ------------------------------------------------------------------------- */
/* Presentation                                                              */
/* ------------------------------------------------------------------------- */

static void draw_pointer(void)
{
    int x = (int)app.px, y = (int)app.py;
    SDL_Rect outline[] = { { x - 9, y - 2, 19, 5 }, { x - 2, y - 9, 5, 19 } };
    SDL_Rect fill[] = { { x - 8, y - 1, 17, 3 }, { x - 1, y - 8, 3, 17 } };
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
    SDL_RenderFillRects(app.renderer, outline, 2);
    SDL_SetRenderDrawColor(app.renderer, 255, 255, 255, 255);
    SDL_RenderFillRects(app.renderer, fill, 2);
}

static void present(void)
{
    if (!app.renderer) /* display released to mpv */
        return;
    if (app.portrait) {
        if (!app.canvas) /* (re)created with the renderer */
            app.canvas = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                           app.screen_w, app.screen_h);
        SDL_SetRenderTarget(app.renderer, app.canvas);
    }
    SDL_SetRenderDrawColor(app.renderer, 255, 255, 255, 255);
    SDL_RenderClear(app.renderer);
    if (app.frame) {
        int tw, th;
        SDL_QueryTexture(app.frame, NULL, NULL, &tw, &th);
        SDL_RenderCopy(app.renderer, app.frame, NULL, &(SDL_Rect){ 0, 0, tw, th });
    }
    if (app.load_progress < 1.0) {
        /* Load progress: thin bar along the top edge */
        SDL_SetRenderDrawColor(app.renderer, 26, 115, 232, 255);
        SDL_RenderFillRect(app.renderer, &(SDL_Rect){ 0, 0, (int)(app.screen_w * MAX(app.load_progress, 0.05)), 5 });
    }
    double dl_progress = -1;
    char *dl_status = player_status(); /* video loading/errors first, then notices, yt-dlp, downloads */
    if (!dl_status && app.notice)
        dl_status = g_strdup(app.notice);
    if (!dl_status)
        dl_status = ytdlp_status(&dl_progress);
    if (!dl_status)
        dl_status = downloads_status(&dl_progress);
    if (dl_status) { /* download progress / result, above the keyboard if it's open */
        menu_draw_status(dl_status, dl_progress, osk_visible() ? app.screen_h - osk_height() : app.screen_h);
        g_free(dl_status);
    }
    if (osk_visible())
        osk_draw();
    else if (!menu_visible() && !app.pointer_hidden)
        draw_pointer();
    menu_draw();
    if (app.portrait) {
        /* Rotate the portrait canvas counterclockwise about the screen center: its top lands on
         * the screen's left edge (the device is held turned clockwise, right stick at the bottom). */
        SDL_SetRenderTarget(app.renderer, NULL);
        SDL_RenderCopyEx(app.renderer, app.canvas, NULL,
                         &(SDL_Rect){ (SCREEN_W - app.screen_w) / 2, (SCREEN_H - app.screen_h) / 2, app.screen_w, app.screen_h },
                         -90, NULL, SDL_FLIP_NONE);
    }
    SDL_RenderPresent(app.renderer);
    app.needs_present = FALSE;
}

/* Upload stats (WPE_TSP_STATS=1): how much of each frame actually reaches the GPU. */
static struct {
    gboolean enabled;
    guint frames;
    guint64 bytes;
    gint64 since;
} stats;

static void count_frame(guint64 bytes, int width, int height);

/* Make the page buffer the frame to show. DMA-buf frames are shown as they are; shared-memory
 * ones are copied into the screen texture, only the damaged region (buffer pixels, the union
 * of everything that changed since the frame we last uploaded) unless the texture is new or
 * no damage info is available. */
static void upload_buffer(WPEBuffer *buffer, const SDL_Rect *damage)
{
    if (!app.renderer) /* display released to mpv: the frame is re-uploaded when it's back */
        return;
    if (WPE_IS_BUFFER_DMA_BUF(buffer)) {
        app.frame = dmabuf_texture(app.renderer, buffer);
        count_frame(0, wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer));
        return;
    }
    if (!WPE_IS_BUFFER_SHM(buffer)) {
        g_warning("Unsupported buffer type %s", G_OBJECT_TYPE_NAME(buffer));
        return;
    }
    WPEBufferSHM *shm = WPE_BUFFER_SHM(buffer);
    const guint8 *data = g_bytes_get_data(wpe_buffer_shm_get_data(shm), NULL);
    int stride = (int)wpe_buffer_shm_get_stride(shm);
    int width = wpe_buffer_get_width(buffer);
    int height = wpe_buffer_get_height(buffer);
    int tw = 0, th = 0;
    if (app.texture)
        SDL_QueryTexture(app.texture, NULL, NULL, &tw, &th);
    if (!app.texture || tw != width || th != height) {
        if (app.texture)
            SDL_DestroyTexture(app.texture);
        /* WPE_PIXEL_FORMAT_ARGB8888 == little-endian BGRA bytes == SDL ARGB8888 */
        app.texture = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, width, height);
        damage = NULL; /* new texture: needs everything */
    }

    SDL_Rect full = { 0, 0, width, height };
    SDL_Rect rect = full;
    if (damage && !SDL_IntersectRect(damage, &full, &rect))
        return; /* damage entirely outside the buffer: nothing to do */

    SDL_UpdateTexture(app.texture, &rect, data + (size_t)rect.y * stride + (size_t)rect.x * 4, stride);
    app.frame = app.texture;
    count_frame((guint64)rect.w * rect.h * 4, width, height);
}

static void count_frame(guint64 bytes, int width, int height)
{
    if (stats.enabled) {
        stats.frames++;
        stats.bytes += bytes;
        gint64 now = g_get_monotonic_time();
        if (!stats.since)
            stats.since = now;
        if (now - stats.since >= 5 * G_USEC_PER_SEC) {
            double secs = (now - stats.since) / (double)G_USEC_PER_SEC;
            fprintf(stderr, "[wpe-tsp] %.1f fps, upload %.1f MB/s (%.0f%% of full frames)\n",
                    stats.frames / secs, stats.bytes / secs / 1e6,
                    100.0 * stats.bytes / ((double)stats.frames * width * height * 4));
            stats.frames = 0;
            stats.bytes = 0;
            stats.since = now;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* WPEPlatform implementation: WPEViewSDL                                    */
/* ------------------------------------------------------------------------- */

#define WPE_TYPE_VIEW_SDL (wpe_view_sdl_get_type())
G_DECLARE_FINAL_TYPE(WPEViewSDL, wpe_view_sdl, WPE, VIEW_SDL, WPEView)

struct _WPEViewSDL {
    WPEView parent;
    WPEBuffer *pending;
    WPEBuffer *committed;
    guint frame_idle_id;
    SDL_Rect pending_damage;    /* union of damage since the last uploaded frame */
    gboolean pending_full;      /* whole frame must be uploaded */
};

G_DEFINE_FINAL_TYPE(WPEViewSDL, wpe_view_sdl, WPE_TYPE_VIEW)

/* Show the pending frame, then tell WebKit it is on screen so it can draw the next one. */
static gboolean view_frame_idle(gpointer user_data)
{
    WPEViewSDL *self = WPE_VIEW_SDL(user_data);
    WPEView *view = WPE_VIEW(self);
    self->frame_idle_id = 0;
    if (!self->pending)
        return G_SOURCE_REMOVE;

    upload_buffer(self->pending, self->pending_full ? NULL : &self->pending_damage);
    self->pending_full = FALSE;
    self->pending_damage = (SDL_Rect){ 0, 0, 0, 0 };
    present();

    if (self->committed) {
        /* WebKit draws into a DMA-buf again as soon as it's released: not while the GPU may
         * still be reading it for the frame that was on screen until now. */
        if (WPE_IS_BUFFER_DMA_BUF(self->committed) && app.renderer)
            dmabuf_wait_presented();
        wpe_view_buffer_released(view, self->committed);
        g_object_unref(self->committed);
    }
    self->committed = self->pending;
    self->pending = NULL;
    wpe_view_buffer_rendered(view, self->committed);
    return G_SOURCE_REMOVE;
}

static gboolean view_render_buffer(WPEView *view, WPEBuffer *buffer, const WPERectangle *damage_rects,
                                   guint n_damage_rects, GError **error)
{
    (void)error;
    WPEViewSDL *self = WPE_VIEW_SDL(view);
    /* Accumulate damage: if a frame gets replaced before it is shown, its changes must still
     * be uploaded together with the next one. No damage info means "everything changed". */
    if (!n_damage_rects)
        self->pending_full = TRUE;
    for (guint i = 0; i < n_damage_rects && !self->pending_full; i++) {
        SDL_Rect r = { damage_rects[i].x, damage_rects[i].y, damage_rects[i].width, damage_rects[i].height };
        if (SDL_RectEmpty(&self->pending_damage))
            self->pending_damage = r;
        else
            SDL_UnionRect(&self->pending_damage, &r, &self->pending_damage);
    }
    if (self->pending) {
        /* A newer frame replaced one we never showed: release the old one right away. */
        wpe_view_buffer_released(view, self->pending);
        g_object_unref(self->pending);
    }
    self->pending = g_object_ref(buffer);
    if (!self->frame_idle_id)
        self->frame_idle_id = g_idle_add_full(G_PRIORITY_HIGH_IDLE, view_frame_idle, self, NULL);
    return TRUE;
}

static void view_toplevel_changed(WPEView *view, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec; (void)user_data;
    WPEToplevel *toplevel = wpe_view_get_toplevel(view);
    if (!toplevel) {
        wpe_view_unmap(view);
        return;
    }
    int width, height;
    wpe_toplevel_get_size(toplevel, &width, &height);
    if (width && height)
        wpe_view_resized(view, width, height);
    wpe_view_map(view);
}

static void wpe_view_sdl_constructed(GObject *object)
{
    G_OBJECT_CLASS(wpe_view_sdl_parent_class)->constructed(object);
    g_signal_connect(object, "notify::toplevel", G_CALLBACK(view_toplevel_changed), NULL);
}

static void wpe_view_sdl_dispose(GObject *object)
{
    WPEViewSDL *self = WPE_VIEW_SDL(object);
    if (self->frame_idle_id) {
        g_source_remove(self->frame_idle_id);
        self->frame_idle_id = 0;
    }
    g_clear_object(&self->pending);
    g_clear_object(&self->committed);
    G_OBJECT_CLASS(wpe_view_sdl_parent_class)->dispose(object);
}

static void wpe_view_sdl_class_init(WPEViewSDLClass *klass)
{
    G_OBJECT_CLASS(klass)->constructed = wpe_view_sdl_constructed;
    G_OBJECT_CLASS(klass)->dispose = wpe_view_sdl_dispose;
    WPE_VIEW_CLASS(klass)->render_buffer = view_render_buffer;
}

static void wpe_view_sdl_init(WPEViewSDL *self)
{
    (void)self;
}

/* ------------------------------------------------------------------------- */
/* WPEToplevelSDL: one fixed fullscreen 1280x720 "window"                    */
/* ------------------------------------------------------------------------- */

#define WPE_TYPE_TOPLEVEL_SDL (wpe_toplevel_sdl_get_type())
G_DECLARE_FINAL_TYPE(WPEToplevelSDL, wpe_toplevel_sdl, WPE, TOPLEVEL_SDL, WPEToplevel)

struct _WPEToplevelSDL {
    WPEToplevel parent;
};

G_DEFINE_FINAL_TYPE(WPEToplevelSDL, wpe_toplevel_sdl, WPE_TYPE_TOPLEVEL)

static void wpe_toplevel_sdl_constructed(GObject *object)
{
    G_OBJECT_CLASS(wpe_toplevel_sdl_parent_class)->constructed(object);
    WPEToplevel *toplevel = WPE_TOPLEVEL(object);
    /* Sizes are logical (CSS) pixels; WebKit renders buffers at size * scale. */
    wpe_toplevel_scale_changed(toplevel, app.scale);
    wpe_toplevel_resized(toplevel, (int)(app.screen_w / app.scale), (int)(app.screen_h / app.scale));
    wpe_toplevel_state_changed(toplevel, WPE_TOPLEVEL_STATE_FULLSCREEN | WPE_TOPLEVEL_STATE_ACTIVE);
}

static gboolean toplevel_resize(WPEToplevel *toplevel, int width, int height)
{
    (void)toplevel; (void)width; (void)height;
    return FALSE; /* fixed screen size */
}

static void wpe_toplevel_sdl_class_init(WPEToplevelSDLClass *klass)
{
    G_OBJECT_CLASS(klass)->constructed = wpe_toplevel_sdl_constructed;
    WPE_TOPLEVEL_CLASS(klass)->resize = toplevel_resize;
}

static void wpe_toplevel_sdl_init(WPEToplevelSDL *self)
{
    (void)self;
}

/* ------------------------------------------------------------------------- */
/* WPEInputMethodContextSDL: bridges focused editables to the on-screen kbd  */
/* ------------------------------------------------------------------------- */

#define WPE_TYPE_INPUT_METHOD_CONTEXT_SDL (wpe_input_method_context_sdl_get_type())
G_DECLARE_FINAL_TYPE(WPEInputMethodContextSDL, wpe_input_method_context_sdl, WPE, INPUT_METHOD_CONTEXT_SDL, WPEInputMethodContext)

struct _WPEInputMethodContextSDL {
    WPEInputMethodContext parent;
};

G_DEFINE_FINAL_TYPE(WPEInputMethodContextSDL, wpe_input_method_context_sdl, WPE_TYPE_INPUT_METHOD_CONTEXT)

static void show_form_keyboard(void);
static void set_view_height(int height);

static void im_focus_in(WPEInputMethodContext *context)
{
    app.im_context = context;
    /* Only pop up for user clicks, not for fields the page autofocuses on load, and not while
     * a physical keyboard is in use. */
    if (now_ms() - app.last_click_ms < 1000 && !app.hid_typing)
        show_form_keyboard();
}

static void im_focus_out(WPEInputMethodContext *context)
{
    if (app.im_context != context)
        return;
    app.im_context = NULL;
    if (osk_visible() && osk_mode() == OSK_MODE_FORM) {
        osk_hide();
        set_view_height(app.screen_h);
    }
}

static void im_set_surrounding(WPEInputMethodContext *context, const gchar *text, guint length,
                               guint cursor_index, guint selection_index)
{
    (void)length; (void)cursor_index; (void)selection_index;
    if (context == app.im_context) {
        osk_set_field_text(text);
        app.needs_present = TRUE;
    }
}

static void wpe_input_method_context_sdl_dispose(GObject *object)
{
    if (app.im_context == WPE_INPUT_METHOD_CONTEXT(object))
        app.im_context = NULL;
    G_OBJECT_CLASS(wpe_input_method_context_sdl_parent_class)->dispose(object);
}

static void wpe_input_method_context_sdl_class_init(WPEInputMethodContextSDLClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = wpe_input_method_context_sdl_dispose;
    WPEInputMethodContextClass *im_class = WPE_INPUT_METHOD_CONTEXT_CLASS(klass);
    im_class->focus_in = im_focus_in;
    im_class->focus_out = im_focus_out;
    im_class->set_surrounding = im_set_surrounding;
}

static void wpe_input_method_context_sdl_init(WPEInputMethodContextSDL *self)
{
    (void)self;
}

/* ------------------------------------------------------------------------- */
/* WPEDisplaySDL                                                             */
/* ------------------------------------------------------------------------- */

#define WPE_TYPE_DISPLAY_SDL (wpe_display_sdl_get_type())
G_DECLARE_FINAL_TYPE(WPEDisplaySDL, wpe_display_sdl, WPE, DISPLAY_SDL, WPEDisplay)

struct _WPEDisplaySDL {
    WPEDisplay parent;
};

G_DEFINE_FINAL_TYPE(WPEDisplaySDL, wpe_display_sdl, WPE_TYPE_DISPLAY)

static gboolean display_connect(WPEDisplay *display, GError **error)
{
    (void)display; (void)error;
    return TRUE;
}

static WPEView *display_create_view(WPEDisplay *display)
{
    return WPE_VIEW(g_object_new(WPE_TYPE_VIEW_SDL, "display", display, NULL));
}

static WPEToplevel *display_create_toplevel(WPEDisplay *display, guint max_views)
{
    (void)max_views;
    return WPE_TOPLEVEL(g_object_new(WPE_TYPE_TOPLEVEL_SDL, "display", display, NULL));
}

static WPEInputMethodContext *display_create_input_method_context(WPEDisplay *display, WPEView *view)
{
    (void)display;
    return WPE_INPUT_METHOD_CONTEXT(g_object_new(WPE_TYPE_INPUT_METHOD_CONTEXT_SDL, "view", view, NULL));
}

static void wpe_display_sdl_class_init(WPEDisplaySDLClass *klass)
{
    WPEDisplayClass *display_class = WPE_DISPLAY_CLASS(klass);
    display_class->create_input_method_context = display_create_input_method_context;
    display_class->connect = display_connect;
    display_class->create_view = display_create_view;
    display_class->create_toplevel = display_create_toplevel;
    /* No get_egl_display / get_drm_device: frames come as shared memory only. */
}

static void wpe_display_sdl_init(WPEDisplaySDL *self)
{
    (void)self;
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

static void send_event(WPEEvent *event)
{
    if (!event)
        return;
    wpe_view_event(app.wpe_view, event);
    wpe_event_unref(event);
}

static void send_pointer_move(double dx, double dy)
{
    app.px = CLAMP(app.px + dx, 0, app.screen_w - 1);
    app.py = CLAMP(app.py + dy, 0, app.screen_h - 1);
    send_event(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, app.wpe_view, WPE_INPUT_SOURCE_MOUSE, now_ms(),
                                          (WPEModifiers)app.pointer_buttons, app.px / app.scale, app.py / app.scale,
                                          dx / app.scale, dy / app.scale));
    app.needs_present = TRUE;
}

/* button: 1 left, 2 middle, 3 right */
static void send_button(guint button, gboolean pressed)
{
    WPEEventType type;
    guint press_count;
    guint32 time = now_ms();
    guint mask = button == 1 ? WPE_MODIFIER_POINTER_BUTTON1 : button == 2 ? WPE_MODIFIER_POINTER_BUTTON2
                                                                          : WPE_MODIFIER_POINTER_BUTTON3;
    if (pressed) {
        type = WPE_EVENT_POINTER_DOWN;
        app.pointer_buttons |= mask;
        app.last_click_ms = time;
        press_count = wpe_view_compute_press_count(app.wpe_view, app.px / app.scale, app.py / app.scale, button, time);
    } else {
        /* WPE only allows a press count on button-down events. */
        type = WPE_EVENT_POINTER_UP;
        app.pointer_buttons &= ~mask;
        press_count = 0;
    }
    send_event(wpe_event_pointer_button_new(type, app.wpe_view, WPE_INPUT_SOURCE_MOUSE, time,
                                            (WPEModifiers)app.pointer_buttons, button, app.px / app.scale, app.py / app.scale,
                                            press_count));

}

static void send_click(gboolean pressed)
{
    send_button(1, pressed);
    /* Clicking an already-focused field (keyboard was dismissed) brings the keyboard back. Only
     * when the click is on an editable element: a field keeps the focus until the web process
     * reports the click's focus change, so clicking a checkbox next to it would match too. */
    if (!pressed && app.im_context && app.editable_under_pointer && !osk_visible() && !app.hid_typing)
        show_form_keyboard();
}

/* WebKit wheel convention: positive delta scrolls towards the top/left. */
static void send_scroll(double dx, double dy)
{
    send_event(wpe_event_scroll_new(app.wpe_view, WPE_INPUT_SOURCE_TOUCHSCREEN, now_ms(), 0,
                                    -dx / app.scale, -dy / app.scale, TRUE, FALSE, app.px / app.scale, app.py / app.scale));
}

static void send_key_event(WPEEventType type, guint keyval)
{
    send_event(wpe_event_keyboard_new(type, app.wpe_view, WPE_INPUT_SOURCE_KEYBOARD, now_ms(), 0, 0, keyval));
}

static void send_key(guint keyval)
{
    send_event(wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_DOWN, app.wpe_view, WPE_INPUT_SOURCE_KEYBOARD, now_ms(), 0, 0, keyval));
    send_event(wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_UP, app.wpe_view, WPE_INPUT_SOURCE_KEYBOARD, now_ms(), 0, 0, keyval));
}

/* ------------------------------------------------------------------------- */
/* On-screen keyboard glue                                                   */
/* ------------------------------------------------------------------------- */

/* Shrink the page while the keyboard is up, so WebKit keeps the focused field visible. */
static void resize_view(int height)
{
    app.view_height = height;
    int width = (int)(app.screen_w / app.scale);
    height = (int)(height / app.scale);
    WPEToplevel *toplevel = wpe_view_get_toplevel(app.wpe_view);
    if (toplevel)
        wpe_toplevel_resized(toplevel, width, height);
    wpe_view_resized(app.wpe_view, width, height);
    app.needs_present = TRUE;
}

static void set_view_height(int height)
{
    if (app.view_height != height)
        resize_view(height);
}

/* Portrait mode: the page is laid out for the screen turned on its side and shown rotated. */
static void set_portrait(gboolean portrait)
{
    if (app.portrait == portrait)
        return;
    app.portrait = portrait;
    app.screen_w = portrait ? SCREEN_H : SCREEN_W;
    app.screen_h = portrait ? SCREEN_W : SCREEN_H;
    if (app.canvas) {
        SDL_DestroyTexture(app.canvas);
        app.canvas = NULL;
    }
    osk_set_screen_size(app.screen_w, app.screen_h);
    menu_set_screen_size(app.screen_w, app.screen_h);
    memset(app.axis, 0, sizeof(app.axis)); /* held values were for the other orientation */
    app.px = app.screen_w / 2.0; /* the page is laid out anew: start from the center */
    app.py = app.screen_h / 2.0;
    resize_view(osk_visible() ? app.screen_h - osk_height() : app.screen_h);
}

static OskField field_for_purpose(WPEInputPurpose purpose)
{
    switch (purpose) {
    case WPE_INPUT_PURPOSE_PASSWORD:
    case WPE_INPUT_PURPOSE_PIN:
        return OSK_FIELD_PASSWORD;
    case WPE_INPUT_PURPOSE_DIGITS:
    case WPE_INPUT_PURPOSE_NUMBER:
    case WPE_INPUT_PURPOSE_PHONE:
        return OSK_FIELD_NUMBER;
    case WPE_INPUT_PURPOSE_EMAIL:
        return OSK_FIELD_EMAIL;
    case WPE_INPUT_PURPOSE_URL:
        return OSK_FIELD_URL;
    default:
        return OSK_FIELD_TEXT;
    }
}

static void show_form_keyboard(void)
{
    if (!app.im_context)
        return;
    osk_show(OSK_MODE_FORM, field_for_purpose(wpe_input_method_context_get_input_purpose(app.im_context)), NULL);
    set_view_height(app.screen_h - osk_height());
}

static void show_url_keyboard(void)
{
    osk_show(OSK_MODE_URL, OSK_FIELD_URL, webkit_web_view_get_uri(app.web_view));
    if (app.hid_typing) /* typing on a physical keyboard: show its layout */
        osk_select_language((int)hid_layout_index());
    app.needs_present = TRUE;
}

static void osk_commit(const char *text, void *user_data)
{
    (void)user_data;
    if (app.im_context)
        g_signal_emit_by_name(app.im_context, "committed", text);
}

static void osk_backspace(void *user_data)
{
    (void)user_data;
    send_key(WPE_KEY_BackSpace);
}

/* Turn what was typed in the URL bar into a URL: keep URLs, add a scheme to host names, search the rest. */
static gboolean input_is_url(const char *input)
{
    return strstr(input, "://") || g_str_has_prefix(input, "about:") || (!strchr(input, ' ') && strchr(input, '.'));
}

static bool osk_is_search(const char *line, void *user_data)
{
    (void)user_data;
    char *trimmed = g_strstrip(g_strdup(line));
    bool search = *trimmed && !input_is_url(trimmed);
    g_free(trimmed);
    return search;
}

/* Display name of the search engine from its URL: "https://www.google.com/..." -> "Google". */
static char *search_engine_name(const char *search_url)
{
    char *host = NULL;
    GUri *uri = g_uri_parse(search_url, G_URI_FLAGS_NONE, NULL);
    if (uri && g_uri_get_host(uri)) {
        const char *h = g_uri_get_host(uri);
        if (g_str_has_prefix(h, "www."))
            h += 4;
        host = g_strndup(h, strcspn(h, "."));
        if (!g_ascii_strcasecmp(host, "duckduckgo")) {
            g_free(host);
            host = g_strdup("DuckDuckGo");
        } else
            host[0] = g_ascii_toupper(host[0]);
    }
    if (uri)
        g_uri_unref(uri);
    return host;
}

static char *url_from_input(const char *input)
{
    if (strstr(input, "://") || g_str_has_prefix(input, "about:"))
        return g_strdup(input);
    if (input_is_url(input))
        return g_strconcat("https://", input, NULL);
    char *query = g_uri_escape_string(input, NULL, TRUE);
    char *url = g_strconcat(app.search_url, query, NULL);
    g_free(query);
    return url;
}

static void osk_enter(const char *line, void *user_data)
{
    (void)user_data;
    if (!line) { /* form field: Enter key (submits forms) */
        send_key(WPE_KEY_Return);
        return;
    }
    char *url = url_from_input(line);
    webkit_web_view_load_uri(app.web_view, url);
    g_free(url);
    set_view_height(app.screen_h);
}

static void osk_closed(void *user_data)
{
    (void)user_data;
    set_view_height(app.screen_h);
}

/* Stick deflection in [-1, 1] with a dead zone; higher power = finer control near the center. */
static double axis_curve(int raw, double power)
{
    if (abs(raw) < AXIS_DEADZONE)
        return 0;
    double v = pow((abs(raw) - AXIS_DEADZONE) / (32767.0 - AXIS_DEADZONE), power);
    return raw < 0 ? -v : v;
}

/* SELECT menu */
/* ------------------------------------------------------------------------- */
/* History: last visited pages, newest first, kept in $XDG_DATA_HOME/wpe-browser/history.txt */
/* (one "uri<TAB>unix-time<TAB>title" per line) and shown as the page wpe-tsp://history.   */
/* WPE itself only keeps the session's back/forward list.                                  */
/* ------------------------------------------------------------------------- */

#define HISTORY_URI "wpe-tsp://history"
#define HISTORY_CLEAR_URI "wpe-tsp://history/clear"

typedef struct {
    char *uri;
    char *title;
    gint64 time;    /* last visit, seconds since the epoch */
} HistoryEntry;

static GPtrArray *history;      /* of HistoryEntry*, newest first */
static guint history_size;      /* max entries; 0 = disabled */
static char *history_path;

static void history_entry_free(gpointer data)
{
    HistoryEntry *e = data;
    g_free(e->uri);
    g_free(e->title);
    g_free(e);
}

static void history_save(void)
{
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < history->len; i++) {
        HistoryEntry *e = g_ptr_array_index(history, i);
        g_string_append_printf(out, "%s\t%" G_GINT64_FORMAT "\t%s\n", e->uri, e->time, e->title ? e->title : "");
    }
    GError *error = NULL;
    if (!g_file_set_contents(history_path, out->str, (gssize)out->len, &error)) {
        fprintf(stderr, "[wpe-tsp] saving history failed: %s\n", error->message);
        g_error_free(error);
    }
    g_string_free(out, TRUE);
}

static void history_load(void)
{
    history = g_ptr_array_new_with_free_func(history_entry_free);
    char *dir = g_build_filename(g_get_user_data_dir(), "wpe-browser", NULL);
    g_mkdir_with_parents(dir, 0755);
    history_path = g_build_filename(dir, "history.txt", NULL);
    g_free(dir);

    char *contents = NULL;
    if (!g_file_get_contents(history_path, &contents, NULL, NULL))
        return;
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i] && history->len < history_size; i++) {
        char **fields = g_strsplit(lines[i], "\t", 3);
        if (g_strv_length(fields) == 3) {
            HistoryEntry *e = g_new0(HistoryEntry, 1);
            e->uri = g_strdup(fields[0]);
            e->time = g_ascii_strtoll(fields[1], NULL, 10);
            e->title = g_strdup(fields[2]);
            g_ptr_array_add(history, e);
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(contents);
}

/* Record a visit (moves an already-known URL to the top). Only web pages are recorded. */
static void history_add(const char *uri, const char *title)
{
    if (!history_size || !uri || !(g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")))
        return;
    for (guint i = 0; i < history->len; i++) {
        HistoryEntry *e = g_ptr_array_index(history, i);
        if (!strcmp(e->uri, uri)) {
            g_ptr_array_remove_index(history, i);
            break;
        }
    }
    HistoryEntry *e = g_new0(HistoryEntry, 1);
    e->uri = g_strdup(uri);
    e->title = g_strdup(title && *title ? title : "");
    e->time = g_get_real_time() / G_USEC_PER_SEC;
    g_ptr_array_insert(history, 0, e);
    if (history->len > history_size)
        g_ptr_array_set_size(history, history_size);
    history_save();
}

/* Titles often arrive after the load finished: update the newest entry. */
static void on_title_changed(WebKitWebView *web_view, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec; (void)user_data;
    const char *title = webkit_web_view_get_title(web_view);
    const char *uri = webkit_web_view_get_uri(web_view);
    if (!history || !history->len || !title || !*title || !uri)
        return;
    HistoryEntry *e = g_ptr_array_index(history, 0);
    if (strcmp(e->uri, uri) || (e->title && !strcmp(e->title, title)))
        return;
    g_free(e->title);
    e->title = g_strdup(title);
    history_save();
}

static char *time_ago(gint64 then)
{
    gint64 d = g_get_real_time() / G_USEC_PER_SEC - then;
    if (d < 60)
        return g_strdup("just now");
    if (d < 3600)
        return g_strdup_printf("%d min ago", (int)(d / 60));
    if (d < 86400)
        return g_strdup_printf("%d h ago", (int)(d / 3600));
    return g_strdup_printf("%d days ago", (int)(d / 86400));
}

/* The history page. The d-pad sends arrow keys: they move a highlight through the entries,
 * START (Enter) opens the highlighted one; pointer clicks work as on any page. */
static char *history_page_html(void)
{
    GString *html = g_string_new(NULL);
    g_string_append_printf(html, PAGE_HEAD "<h1>History</h1>", "History");
    for (guint i = 0; i < history->len; i++) {
        HistoryEntry *e = g_ptr_array_index(history, i);
        GUri *parsed = g_uri_parse(e->uri, G_URI_FLAGS_NONE, NULL);
        const char *host = parsed && g_uri_get_host(parsed) ? g_uri_get_host(parsed) : e->uri;
        if (g_str_has_prefix(host, "www."))
            host += 4;
        char *uri = g_markup_escape_text(e->uri, -1);
        char *title = g_markup_escape_text(e->title && *e->title ? e->title : host, -1);
        char *site = g_markup_escape_text(host, -1);
        char *ago = time_ago(e->time);
        g_string_append_printf(html, "<a href='%s'><div class='t'>%s</div><div class='s'>%s &middot; %s</div></a>",
                               uri, title, site, ago);
        g_free(uri); g_free(title); g_free(site); g_free(ago);
        if (parsed)
            g_uri_unref(parsed);
    }
    if (history->len)
        g_string_append(html, "<a class='clear' href='" HISTORY_CLEAR_URI "'><div class='t'>Clear history</div></a>");
    else
        g_string_append(html, "<div class='empty'>No pages visited yet.</div>");
    g_string_append(html, PAGE_TAIL);
    return g_string_free(html, FALSE);
}

/* wpe-tsp:// pages: history and downloads */
static void on_app_scheme_request(WebKitURISchemeRequest *request, gpointer user_data)
{
    (void)user_data;
    const char *uri = webkit_uri_scheme_request_get_uri(request);
    char *html;
    if (g_str_has_prefix(uri, DOWNLOADS_URI)) {
        html = downloads_handle_page(uri);
    } else {
        if (!strcmp(uri, HISTORY_CLEAR_URI)) {
            g_ptr_array_set_size(history, 0);
            history_save();
        }
        html = history_page_html();
    }
    gsize len = strlen(html);
    GInputStream *stream = g_memory_input_stream_new_from_data(html, (gssize)len, g_free);
    webkit_uri_scheme_request_finish(request, stream, (gint64)len, "text/html");
    g_object_unref(stream);
}

/* ------------------------------------------------------------------------- */
/* Video: YouTube pages and media files play in the YouTube client's mpv            */
/* ------------------------------------------------------------------------- */

static int create_display(void);

/* mpv needs the screen: drop our window/renderer (and everything drawn with it). */
static void player_release_display(void *user_data)
{
    (void)user_data;
    wpe_view_unmap(app.wpe_view); /* page hidden: WebKit stops rendering it, timers are throttled */
    osk_set_renderer(NULL);
    menu_set_renderer(NULL);
    if (app.texture) {
        SDL_DestroyTexture(app.texture);
        app.texture = NULL;
    }
    app.frame = NULL;
    if (app.canvas) {
        SDL_DestroyTexture(app.canvas);
        app.canvas = NULL;
    }
    dmabuf_renderer_lost();
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    app.renderer = NULL;
    app.window = NULL;
}

static void player_restore_display(void *user_data)
{
    (void)user_data;
    if (create_display() < 0) {
        g_main_loop_quit(app.loop);
        return;
    }
    osk_set_renderer(app.renderer);
    menu_set_renderer(app.renderer);
    wpe_view_map(app.wpe_view);
    /* Buttons pressed in mpv must not reach the page */
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    memset(app.axis, 0, sizeof(app.axis));
    app.trigger_down[0] = app.trigger_down[1] = FALSE;
    app.arrow_keyval = 0;
    /* Show the last page frame again (frames that arrived meanwhile weren't uploaded) */
    WPEViewSDL *view = WPE_VIEW_SDL(app.wpe_view);
    if (view->committed)
        upload_buffer(view->committed, NULL);
    app.needs_present = TRUE;
}

static void player_status_changed(void *user_data)
{
    (void)user_data;
    app.needs_present = TRUE;
}

static void player_finished(gboolean played, void *user_data)
{
    (void)played; (void)user_data;
    /* Stay where we are: the video page (comments, likes, description) stays open to read */
    app.needs_present = TRUE;
}

/* The page moved to a YouTube video (link click or YouTube's in-page navigation): play it in mpv
 * while the page itself keeps loading, so it can be read when the video ends. */
static void on_uri_changed(WebKitWebView *web_view, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec; (void)user_data;
    char *watch = player_youtube_watch_url(webkit_web_view_get_uri(web_view));
    /* The same video again (YouTube rewriting its own URL after playback, a reload): don't replay.
     * Leaving the page resets this, so coming back to it later plays it again. */
    gboolean same = watch && app.video_page && !strcmp(watch, app.video_page);
    if (!same) {
        g_free(app.video_page);
        app.video_page = NULL;
    }
    if (watch && !same && app.play_youtube && !player_busy() && player_available()) {
        app.video_page = g_strdup(watch);
        player_play_youtube(watch);
    }
    g_free(watch);
}

/* Embedded YouTube players (iframes of youtube.com/embed/ID) can't play in this build: cover
 * them with a "Play in mpv" button that sends the video to the browser. Injected into all
 * frames; YouTube may rebuild its DOM, so the button is re-added when it disappears. */
static const char EMBED_PLAY_JS[] =
    "(() => {"
    "const m = location.pathname.match(/^\\/embed\\/([A-Za-z0-9_-]{11})/);"
    "if (!m || !window.webkit || !window.webkit.messageHandlers.wpeTspPlay) return;"
    "const url = 'https://www.youtube.com/watch?v=' + m[1];"
    /* DOM calls only: YouTube enforces Trusted Types, which rejects innerHTML strings */
    "const el = (css, parent) => { const e = document.createElement('div'); e.style.cssText = css;"
    "  if (parent) parent.appendChild(e); return e; };"
    "const add = () => {"
    "  if (document.getElementById('wpe-tsp-play') || !document.body) return;"
    "  const b = el('position:fixed;left:0;top:0;right:0;bottom:0;z-index:2147483647;display:flex;"
    "flex-direction:column;align-items:center;justify-content:center;background:rgba(0,0,0,.6);cursor:pointer;"
    "font:600 18px sans-serif;color:#fff');"
    "  b.id = 'wpe-tsp-play';"
    "  const icon = el('width:84px;height:58px;border-radius:14px;background:#f00;display:flex;"
    "align-items:center;justify-content:center', b);"
    "  el('width:0;height:0;border-left:24px solid #fff;border-top:15px solid transparent;"
    "border-bottom:15px solid transparent;margin-left:6px', icon);"
    "  el('margin-top:12px', b).textContent = 'Play in mpv';"
    "  b.addEventListener('click', e => { e.preventDefault(); e.stopPropagation();"
    "    window.webkit.messageHandlers.wpeTspPlay.postMessage(url); }, true);"
    "  document.body.appendChild(b);"
    "};"
    "add();"
    "new MutationObserver(add).observe(document.documentElement, { childList: true, subtree: true });"
    "})()";

/* YouTube's own pages (watch, Shorts): the page stays open next to mpv, but its player only shows
 * "can't play". Cover it with the video's thumbnail and a "Play in mpv" button (to watch again).
 * YouTube navigates in-page and rebuilds its DOM, so the button follows the current address and is
 * re-added when it disappears (checked once per frame at most). */
static const char WATCH_PLAY_JS[] =
    "(() => {"
    "if (!window.webkit || !window.webkit.messageHandlers.wpeTspPlay) return;"
    "const videoId = () => { const u = new URL(location.href);"
    "  if (u.pathname === '/watch') return u.searchParams.get('v');"
    "  const m = u.pathname.match(/^\\/shorts\\/([A-Za-z0-9_-]{11})/); return m && m[1]; };"
    /* DOM calls only: YouTube enforces Trusted Types, which rejects innerHTML strings */
    "const el = (css, parent) => { const e = document.createElement('div'); e.style.cssText = css;"
    "  if (parent) parent.appendChild(e); return e; };"
    "let button = null;"
    "const build = () => {"
    "  const b = el('position:absolute;left:0;top:0;right:0;bottom:0;z-index:2147483647;display:flex;"
    "flex-direction:column;align-items:center;justify-content:center;cursor:pointer;"
    "background:#000 center/cover no-repeat;font:600 18px sans-serif;color:#fff;text-shadow:0 1px 3px #000');"
    "  b.id = 'wpe-tsp-watch-play';"
    "  const icon = el('width:84px;height:58px;border-radius:14px;background:#f00;display:flex;"
    "align-items:center;justify-content:center', b);"
    "  el('width:0;height:0;border-left:24px solid #fff;border-top:15px solid transparent;"
    "border-bottom:15px solid transparent;margin-left:6px', icon);"
    "  el('margin-top:12px', b).textContent = 'Play in mpv';"
    "  b.addEventListener('click', e => { e.preventDefault(); e.stopPropagation();"
    "    window.webkit.messageHandlers.wpeTspPlay.postMessage(location.href); }, true);"
    "  return b;"
    "};"
    "let queued = false;"
    "const update = () => {"
    "  queued = false;"
    "  const id = videoId();"
    "  const player = id && document.querySelector('.html5-video-player');"
    "  if (!player) { if (button) button.remove(); return; }"
    "  if (!button) button = build();"
    "  if (button.parentNode !== player) {"
    "    if (getComputedStyle(player).position === 'static') player.style.position = 'relative';"
    "    player.appendChild(button); }"
    "  if (button.dataset.id !== id) { button.dataset.id = id;"
    "    button.style.backgroundImage = 'linear-gradient(rgba(0,0,0,.35),rgba(0,0,0,.35)),"
    "url(\"https://i.ytimg.com/vi/' + id + '/hqdefault.jpg\")'; }"
    "};"
    "update();"
    "new MutationObserver(() => { if (!queued) { queued = true; requestAnimationFrame(update); } })"
    "  .observe(document.documentElement, { childList: true, subtree: true });"
    "})()";

/* <video>/<audio> can't play in this build: replace each with a box (poster/size kept) whose click
 * sends the source URL to mpv, which also handles HLS (.m3u8) and DASH (.mpd). blob: sources (page-
 * built MediaSource streams) only exist inside the page, so those elements are left alone. */
static const char MEDIA_PLAY_JS[] =
    "(() => {"
    "if (!window.webkit || !window.webkit.messageHandlers.wpeTspPlayMedia) return;"
    "const abs = u => { try { return new URL(u, document.baseURI).href; } catch (e) { return null; } };"
    "const playable = u => { u = u && abs(u);"
    "  return u && /^https?:/.test(u) ? u : null; };"
    "const sourceOf = el => { let u = playable(el.getAttribute('src')); if (u) return u;"
    "  for (const s of el.querySelectorAll('source')) {"
    "    if ((u = playable(s.getAttribute('src')))) return u; }"
    "  return null; };"
    "const div = (css, parent) => { const e = document.createElement('div'); e.style.cssText = css;"
    "  if (parent) parent.appendChild(e); return e; };"
    "const seen = new WeakSet();"
    "const replace = el => {"
    "  if (seen.has(el)) return; seen.add(el);"
    "  const url = sourceOf(el); if (!url || !el.parentNode) return;"
    "  const audio = el.tagName === 'AUDIO';"
    "  const w = parseInt(el.getAttribute('width')) || 0, h = parseInt(el.getAttribute('height')) || 0;"
    "  const box = div('display:flex;align-items:center;justify-content:center;gap:12px;cursor:pointer;"
    "box-sizing:border-box;max-width:100%;margin:4px 0;border-radius:8px;color:#fff;font:600 16px sans-serif;"
    "background:#202124 center/cover no-repeat;' + (audio ? 'height:56px;padding:0 16px;width:' + (w || 360) + 'px'"
    "    : 'flex-direction:column;width:' + (w ? w + 'px' : '100%') + ';aspect-ratio:' + (w && h ? w + '/' + h : '16/9')));"
    "  const poster = !audio && el.getAttribute('poster');"
    "  if (poster && abs(poster)) box.style.backgroundImage = 'url(\"' + abs(poster) + '\")';"
    "  const icon = div('width:' + (audio ? 40 : 72) + 'px;height:' + (audio ? 30 : 50) + 'px;border-radius:10px;"
    "background:#f00;display:flex;align-items:center;justify-content:center;flex:none', box);"
    "  div('width:0;height:0;border-left:' + (audio ? 14 : 22) + 'px solid #fff;border-top:' + (audio ? 9 : 14) + 'px solid transparent;"
    "border-bottom:' + (audio ? 9 : 14) + 'px solid transparent;margin-left:5px', icon);"
    "  const name = decodeURIComponent(url.split(/[?#]/)[0].split('/').pop() || url);"
    "  div('text-shadow:0 1px 3px #000;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:90%', box)"
    "    .textContent = (audio ? 'Play audio' : 'Play video') + ' \xc2\xb7 ' + name;"
    "  box.addEventListener('click', e => { e.preventDefault(); e.stopPropagation();"
    "    window.webkit.messageHandlers.wpeTspPlayMedia.postMessage(url); }, true);"
    "  el.parentNode.insertBefore(box, el);"
    "  el.style.display = 'none';"
    "};"
    "let queued = false;"
    "const scan = () => { queued = false; document.querySelectorAll('video,audio').forEach(replace); };"
    "scan();"
    "new MutationObserver(() => { if (!queued) { queued = true; requestAnimationFrame(scan); } })"
    "  .observe(document.documentElement, { childList: true, subtree: true });"
    "})()";

static void on_media_play(WebKitUserContentManager *manager, JSCValue *value, gpointer user_data)
{
    (void)manager; (void)user_data;
    char *uri = jsc_value_to_string(value);
    if (uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))
        && !player_busy() && player_available())
        player_play_url(uri);
    g_free(uri);
}

static void on_embed_play(WebKitUserContentManager *manager, JSCValue *value, gpointer user_data)
{
    (void)manager; (void)user_data;
    char *uri = jsc_value_to_string(value);
    char *watch = player_youtube_watch_url(uri); /* only ever accept a YouTube video URL */
    if (watch && !player_busy() && player_available())
        player_play_youtube(watch);
    g_free(watch);
    g_free(uri);
}

static void setup_embed_play(WebKitWebView *web_view)
{
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(web_view);
    static const char *const embeds[] = {
        "*://www.youtube.com/embed/*", "*://youtube.com/embed/*",
        "*://www.youtube-nocookie.com/embed/*", "*://youtube-nocookie.com/embed/*", NULL
    };
    WebKitUserScript *script = webkit_user_script_new(EMBED_PLAY_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, embeds, NULL);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
    static const char *const youtube[] = { "*://www.youtube.com/*", "*://m.youtube.com/*", "*://youtube.com/*", NULL };
    script = webkit_user_script_new(WATCH_PLAY_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, youtube, NULL);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
    webkit_user_content_manager_register_script_message_handler(ucm, "wpeTspPlay", NULL);
    g_signal_connect(ucm, "script-message-received::wpeTspPlay", G_CALLBACK(on_embed_play), NULL);

    script = webkit_user_script_new(MEDIA_PLAY_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
    webkit_user_content_manager_register_script_message_handler(ucm, "wpeTspPlayMedia", NULL);
    g_signal_connect(ucm, "script-message-received::wpeTspPlayMedia", G_CALLBACK(on_media_play), NULL);
}

/* ------------------------------------------------------------------------- */
/* Ad/tracker blocking: WebKit content filter compiled from share/adblock/rules.json */
/* (EasyList + EasyPrivacy domain rules, see scripts/make-adblock.py). Compiled once */
/* per rules file into $XDG_DATA_HOME/wpe-browser/content-filters, then loaded fast. */
/* ------------------------------------------------------------------------- */

static struct {
    WebKitUserContentFilterStore *store;
    char *rules_path;
    char *identifier;       /* "adblock-<size>-<mtime>": a new rules file gets compiled again */
    gint64 started;
} adblock;

static void adblock_apply(WebKitUserContentFilter *filter, const char *how)
{
    webkit_user_content_manager_add_filter(webkit_web_view_get_user_content_manager(app.web_view), filter);
    fprintf(stderr, "[wpe-tsp] ad blocking on (%s in %.1f s)\n", how,
            (g_get_monotonic_time() - adblock.started) / (double)G_USEC_PER_SEC);
    webkit_user_content_filter_unref(filter);
}

static void adblock_removed(GObject *object, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;
    webkit_user_content_filter_store_remove_finish(WEBKIT_USER_CONTENT_FILTER_STORE(object), result, NULL);
}

/* Drop compiled filters of older rules files */
static void adblock_identifiers(GObject *object, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;
    char **ids = webkit_user_content_filter_store_fetch_identifiers_finish(WEBKIT_USER_CONTENT_FILTER_STORE(object), result);
    for (int i = 0; ids && ids[i]; i++)
        if (strcmp(ids[i], adblock.identifier))
            webkit_user_content_filter_store_remove(adblock.store, ids[i], NULL, adblock_removed, NULL);
    g_strfreev(ids);
}

static void adblock_compiled(GObject *object, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    WebKitUserContentFilter *filter = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(object), result, &error);
    if (!filter) {
        fprintf(stderr, "[wpe-tsp] ad blocking: compiling %s failed: %s\n", adblock.rules_path, error->message);
        g_error_free(error);
        return;
    }
    adblock_apply(filter, "compiled");
    webkit_user_content_filter_store_fetch_identifiers(adblock.store, NULL, adblock_identifiers, NULL);
}

static void adblock_loaded(GObject *object, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;
    WebKitUserContentFilter *filter = webkit_user_content_filter_store_load_finish(WEBKIT_USER_CONTENT_FILTER_STORE(object), result, NULL);
    if (filter) {
        adblock_apply(filter, "loaded");
        return;
    }
    /* First start with this rules file: compile it (in the background; pages load meanwhile) */
    fprintf(stderr, "[wpe-tsp] ad blocking: compiling %s ...\n", adblock.rules_path);
    GFile *file = g_file_new_for_path(adblock.rules_path);
    webkit_user_content_filter_store_save_from_file(adblock.store, adblock.identifier, file, NULL, adblock_compiled, NULL);
    g_object_unref(file);
}

static void setup_adblock(const char *rules_path)
{
    GStatBuf st;
    if (g_stat(rules_path, &st) != 0) {
        fprintf(stderr, "[wpe-tsp] ad blocking: no rules at %s\n", rules_path);
        return;
    }
    adblock.rules_path = g_strdup(rules_path);
    adblock.identifier = g_strdup_printf("adblock-%lld-%lld", (long long)st.st_size, (long long)st.st_mtime);
    char *dir = g_build_filename(g_get_user_data_dir(), "wpe-browser", "content-filters", NULL);
    adblock.store = webkit_user_content_filter_store_new(dir);
    g_free(dir);
    adblock.started = g_get_monotonic_time();
    webkit_user_content_filter_store_load(adblock.store, adblock.identifier, NULL, adblock_loaded, NULL);
}

/* ------------------------------------------------------------------------- */
/* Downloads: prompt for files the browser can't show, SELECT -> Downloads submenu  */
/* ------------------------------------------------------------------------- */

static void on_downloads_changed(void *user_data)
{
    (void)user_data;
    app.needs_present = TRUE;
}

static gboolean play_file_idle(gpointer path)
{
    if (!player_busy() && player_available())
        player_play_url(path);
    return G_SOURCE_REMOVE;
}

/* Downloads page "Play": deferred, since it's requested from inside WebKit's scheme handler */
static void on_downloads_play(const char *path, void *user_data)
{
    (void)user_data;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, play_file_idle, g_strdup(path), g_free);
}

/* yt-dlp: version check after start (over the network at most once a day); a missing or
 * outdated yt-dlp is offered for download, and the result is reported. */
#define YTDLP_CHECK_DELAY_S 30 /* don't compete with the first page load */

static struct {
    char *title, *subtitle;     /* prompt waiting until the screen is free (no menu/keyboard/video) */
    gboolean offer;             /* Download/Later; otherwise a report with OK */
} ytdlp_prompt;

static void ytdlp_queue_prompt(gboolean offer, char *title, char *subtitle)
{
    g_free(ytdlp_prompt.title);
    g_free(ytdlp_prompt.subtitle);
    ytdlp_prompt.title = title;
    ytdlp_prompt.subtitle = subtitle;
    ytdlp_prompt.offer = offer;
}

static gboolean ytdlp_progress_tick(gpointer user_data)
{
    (void)user_data;
    app.needs_present = TRUE;
    return ytdlp_busy() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void ytdlp_installed(gboolean ok, const char *message, void *user_data)
{
    (void)user_data;
    ytdlp_queue_prompt(FALSE, g_strdup(ok ? "yt-dlp installed" : "yt-dlp wasn't installed"), g_strdup(message));
    app.needs_present = TRUE;
}

static void ytdlp_prompt_answered(int index, void *user_data)
{
    gboolean offer = GPOINTER_TO_INT(user_data);
    if (offer && index == 0) {
        ytdlp_install(ytdlp_installed, NULL);
        g_timeout_add(250, ytdlp_progress_tick, NULL);
    }
    app.needs_present = TRUE;
}

static void ytdlp_show_prompt(void)
{
    static const char *const offer_items[] = { "Download and install", "Later", NULL };
    static const char *const ok_items[] = { "OK", NULL };
    menu_open(ytdlp_prompt.title, ytdlp_prompt.subtitle, ytdlp_prompt.offer ? offer_items : ok_items,
              ytdlp_prompt_answered, GINT_TO_POINTER(ytdlp_prompt.offer));
    g_clear_pointer(&ytdlp_prompt.title, g_free);
    g_clear_pointer(&ytdlp_prompt.subtitle, g_free);
    app.needs_present = TRUE;
}

/* user_data: TRUE if the user just needed yt-dlp (tried to play a YouTube video) */
static void ytdlp_checked(const char *installed, const char *latest, gboolean fresh, void *user_data)
{
    gboolean asked = GPOINTER_TO_INT(user_data);
    if (!installed && latest)
        ytdlp_queue_prompt(TRUE, g_strdup("yt-dlp isn't installed"),
                           g_strdup_printf("YouTube videos need it. Download yt-dlp %s (about 40 MB)?", latest));
    else if (!installed && asked)
        ytdlp_queue_prompt(FALSE, g_strdup("yt-dlp isn't installed"),
                           g_strdup("YouTube videos need it, but GitHub can't be reached to download it."));
    else if (installed && latest && ytdlp_version_cmp(installed, latest) < 0 && (fresh || asked))
        ytdlp_queue_prompt(TRUE, g_strdup("yt-dlp update available"),
                           g_strdup_printf("Installed %s, latest %s. YouTube often needs the newest one.",
                                           installed, latest));
}

static gboolean ytdlp_start_check(gpointer user_data)
{
    (void)user_data;
    ytdlp_check(FALSE, ytdlp_checked, GINT_TO_POINTER(FALSE));
    return G_SOURCE_REMOVE;
}

static void player_ytdlp_missing(void *user_data)
{
    (void)user_data;
    ytdlp_check(FALSE, ytdlp_checked, GINT_TO_POINTER(TRUE));
}

/* Prompt items: [Play,] Download, Cancel ("Play" only for video/audio when mpv is available) */
static gboolean pending_playable;
static char *pending_uri;

static void download_prompt_answered(int index, void *user_data)
{
    (void)user_data;
    WebKitPolicyDecision *decision = app.pending_download;
    app.pending_download = NULL;
    if (!decision)
        return;
    int download_index = pending_playable ? 1 : 0;
    if (pending_playable && index == 0) {
        webkit_policy_decision_ignore(decision);
        player_play_url(pending_uri);
    } else if (index == download_index)
        webkit_policy_decision_download(decision);
    else
        webkit_policy_decision_ignore(decision);
    g_clear_pointer(&pending_uri, g_free);
    g_object_unref(decision);
    app.needs_present = TRUE;
}

static gboolean response_is_attachment(WebKitURIResponse *response)
{
    SoupMessageHeaders *headers = webkit_uri_response_get_http_headers(response);
    const char *disposition = headers ? soup_message_headers_get_one(headers, "Content-Disposition") : NULL;
    return disposition && !g_ascii_strncasecmp(disposition, "attachment", 10);
}

/* A page navigated to something the browser can't display (video, archive, ...) or that the
 * server marks as an attachment: ask before downloading it. */
static gboolean on_decide_policy(WebKitWebView *web_view, WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type, gpointer user_data)
{
    (void)web_view; (void)user_data;
    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
        return FALSE;
    WebKitResponsePolicyDecision *response_decision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
    WebKitURIResponse *response = webkit_response_policy_decision_get_response(response_decision);
    if (!webkit_response_policy_decision_is_main_frame_main_resource(response_decision))
        return FALSE;
    if (webkit_response_policy_decision_is_mime_type_supported(response_decision) && !response_is_attachment(response))
        return FALSE;
    if (app.pending_download) { /* already asking about another one */
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }

    const char *suggested = webkit_uri_response_get_suggested_filename(response);
    char *name = suggested && *suggested ? g_strdup(suggested) : g_path_get_basename(webkit_uri_response_get_uri(response));
    gint64 length = (gint64)webkit_uri_response_get_content_length(response);
    char *size = length > 0 ? downloads_format_size(length) : g_strdup("unknown size");
    char *free_space = downloads_format_size(downloads_free_space());
    const char *mime = webkit_uri_response_get_mime_type(response);
    char *subtitle = g_strdup_printf("%s \xc2\xb7 %s \xc2\xb7 %s free", size, mime ? mime : "file", free_space);
    pending_playable = mime && (g_str_has_prefix(mime, "video/") || g_str_has_prefix(mime, "audio/")) && player_available();
    g_free(pending_uri);
    pending_uri = g_strdup(webkit_uri_response_get_uri(response));
    char *title = g_strdup_printf(pending_playable ? "Play or download %s?" : "Download %s?", name);
    static const char *const download_items[] = { "Download", "Cancel", NULL };
    static const char *const media_items[] = { "Play", "Download", "Cancel", NULL };
    app.pending_download = g_object_ref(decision);
    menu_open(title, subtitle, pending_playable ? media_items : download_items, download_prompt_answered, NULL);
    app.needs_present = TRUE;
    g_free(title); g_free(subtitle); g_free(free_space); g_free(size); g_free(name);
    return TRUE;
}

static void on_mouse_target_changed(WebKitWebView *web_view, WebKitHitTestResult *hit, guint modifiers, gpointer user_data)
{
    (void)web_view; (void)modifiers; (void)user_data;
    g_free(app.link_under_pointer);
    app.link_under_pointer = webkit_hit_test_result_context_is_link(hit) ? g_strdup(webkit_hit_test_result_get_link_uri(hit)) : NULL;
    app.editable_under_pointer = webkit_hit_test_result_context_is_editable(hit);
}

static void media_menu_activated(int index, void *user_data)
{
    (void)user_data;
    if (index >= 0 && app.media_uris && (guint)index < app.media_uris->len)
        downloads_start(g_ptr_array_index(app.media_uris, index));
    app.needs_present = TRUE;
}

/* Video/audio addresses on the page: <video>/<audio>/<source> src (our build has no media
 * elements, but the attributes are still there) and links to media files. Streams (blob:) skipped. */
static const char MEDIA_SCAN_JS[] =
    "(() => {"
    "const out = new Map();"
    "const add = (u, label) => { try { u = new URL(u, document.baseURI).href; } catch (e) { return; }"
    "  if (/^https?:/.test(u) && !out.has(u)) out.set(u, label); };"
    "document.querySelectorAll('video[src],audio[src],source[src]').forEach(e => add(e.getAttribute('src'), e.tagName.toLowerCase()));"
    "document.querySelectorAll('a[href]').forEach(a => {"
    "  if (/\\.(mp4|m4v|webm|mkv|mov|avi|3gp|mp3|m4a|ogg|oga|opus|flac|wav)([?#]|$)/i.test(a.getAttribute('href')))"
    "    add(a.getAttribute('href'), 'link'); });"
    "return [...out].map(([u, l]) => u + '\\t' + l).join('\\n');"
    "})()";

static void on_media_scanned(GObject *object, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, NULL);
    char *text = value ? jsc_value_to_string(value) : g_strdup("");
    if (value)
        g_object_unref(value);

    if (app.media_uris)
        g_ptr_array_free(app.media_uris, TRUE);
    app.media_uris = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    char **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i] && app.media_uris->len < MAX_MEDIA_ITEMS; i++) {
        char *tab = strchr(lines[i], '\t');
        if (!tab)
            continue;
        *tab = 0;
        char *path = g_uri_unescape_string(lines[i], NULL);
        char *base = g_path_get_basename(path ? path : lines[i]);
        char *q = strpbrk(base, "?#");
        if (q)
            *q = 0;
        g_ptr_array_add(app.media_uris, g_strdup(lines[i]));
        g_ptr_array_add(labels, g_strdup_printf("%s  \xc2\xb7  %s", base, tab + 1));
        g_free(base);
        g_free(path);
    }
    g_strfreev(lines);
    g_free(text);

    char *subtitle = g_strdup_printf("%u found \xc2\xb7 saved to %s", app.media_uris->len, downloads_dir());
    if (!app.media_uris->len)
        g_ptr_array_add(labels, g_strdup("No video/audio files found on this page"));
    g_ptr_array_add(labels, NULL);
    menu_open("Save video/audio", subtitle, (const char *const *)labels->pdata, media_menu_activated, NULL);
    g_free(subtitle);
    g_ptr_array_free(labels, TRUE);
    app.needs_present = TRUE;
}

enum { DL_MENU_LINK, DL_MENU_MEDIA, DL_MENU_SHOW };

static void downloads_menu_activated(int index, void *user_data)
{
    (void)user_data;
    switch (index) {
    case DL_MENU_LINK:
        if (app.link_under_pointer)
            downloads_start(app.link_under_pointer);
        break;
    case DL_MENU_MEDIA:
        webkit_web_view_evaluate_javascript(app.web_view, MEDIA_SCAN_JS, -1, NULL, NULL, NULL, on_media_scanned, NULL);
        break;
    case DL_MENU_SHOW:
        webkit_web_view_load_uri(app.web_view, DOWNLOADS_URI);
        break;
    }
    app.needs_present = TRUE;
}

static void open_downloads_menu(void)
{
    const char *link = app.link_under_pointer ? "Save link under pointer" : "Save link under pointer (none)";
    const char *items[] = { link, "Save video/audio from this page", "Show downloads", NULL };
    char *subtitle = g_strdup_printf("Saved to %s", downloads_dir());
    menu_open("Downloads", subtitle, items, downloads_menu_activated, NULL);
    g_free(subtitle);
    app.needs_present = TRUE;
}

/* Is the UA currently in effect the desktop one (WebKit's own)? */
static gboolean showing_desktop_site(void)
{
    gboolean configured_desktop = app.user_agent == NULL;
    return app.ua_switched ? !configured_desktop : configured_desktop;
}

/* "Request desktop site" / "Request mobile site": switch UA for the session and reload. */
static void toggle_desktop_site(void)
{
    app.ua_switched = !app.ua_switched;
    WebKitSettings *settings = webkit_web_view_get_settings(app.web_view);
    const char *ua = !app.ua_switched ? app.user_agent      /* back to the configured one */
                   : app.user_agent ? NULL                  /* configured mobile/custom -> desktop */
                   : MOBILE_USER_AGENT;                     /* configured desktop -> mobile */
    webkit_settings_set_user_agent(settings, ua);
    fprintf(stderr, "[wpe-tsp] user agent: %s\n", webkit_settings_get_user_agent(settings));
    webkit_web_view_reload(app.web_view);
}

enum { MENU_BACK, MENU_FORWARD, MENU_HOME, MENU_HISTORY, MENU_DOWNLOADS, MENU_DESKTOP_SITE, MENU_PORTRAIT, MENU_ADDRESS, MENU_ZOOM_IN, MENU_ZOOM_OUT, MENU_ZOOM_RESET, MENU_EXIT };

static void menu_activated(int index, void *user_data)
{
    (void)user_data;
    double zoom = webkit_web_view_get_zoom_level(app.web_view);
    switch (index) {
    case MENU_BACK: webkit_web_view_go_back(app.web_view); break;
    case MENU_FORWARD: webkit_web_view_go_forward(app.web_view); break;
    case MENU_HOME: webkit_web_view_load_uri(app.web_view, app.home_url ? app.home_url : "about:blank"); break;
    case MENU_HISTORY: webkit_web_view_load_uri(app.web_view, HISTORY_URI); break;
    case MENU_DOWNLOADS: open_downloads_menu(); return;
    case MENU_DESKTOP_SITE: toggle_desktop_site(); break;
    case MENU_PORTRAIT: set_portrait(!app.portrait); break;
    case MENU_ADDRESS: show_url_keyboard(); break;
    case MENU_ZOOM_IN: webkit_web_view_set_zoom_level(app.web_view, MIN(zoom + 0.1, 3.0)); break;
    case MENU_ZOOM_OUT: webkit_web_view_set_zoom_level(app.web_view, MAX(zoom - 0.1, 0.3)); break;
    case MENU_ZOOM_RESET: webkit_web_view_set_zoom_level(app.web_view, 1.0); break;
    case MENU_EXIT: g_main_loop_quit(app.loop); break;
    }
    app.needs_present = TRUE;
}

static void open_menu(void)
{
    const char *items[] = {
        "Back", "Forward", "Home", "History", "Downloads",
        showing_desktop_site() ? "Request mobile site" : "Request desktop site",
        app.portrait ? "Landscape mode" : "Portrait mode",
        "Address bar", "Zoom in", "Zoom out", "Reset zoom", "Exit", NULL
    };
    const char *title = webkit_web_view_get_title(app.web_view);
    const char *uri = webkit_web_view_get_uri(app.web_view);
    char *zoom_title = g_strdup_printf("%s  (%d%%)", title && *title ? title : "WPE Browser",
                                       (int)(webkit_web_view_get_zoom_level(app.web_view) * 100 + 0.5));
    menu_open(zoom_title, uri, items, menu_activated, NULL);
    g_free(zoom_title);
    app.needs_present = TRUE;
}

static void handle_button(int button, gboolean pressed)
{
    switch (button) {
    case BTN_A:
        /* With the pointer hidden, A first only shows it: never click somewhere unseen */
        if (pressed && app.pointer_hidden) {
            app.pointer_hidden = FALSE;
            app.pointer_used_ms = now_ms();
            app.swallow_a_release = TRUE;
            app.needs_present = TRUE;
            return;
        }
        if (!pressed && app.swallow_a_release) {
            app.swallow_a_release = FALSE;
            return;
        }
        app.pointer_used_ms = now_ms();
        app.hid_typing = FALSE; /* gamepad in use: fields bring up the on-screen keyboard again */
        send_click(pressed);
        return;
    }
    if (!pressed)
        return;
    switch (button) {
    case BTN_B: { /* B cancels: stops a loading page; on the app's own pages it goes back */
        const char *uri = webkit_web_view_get_uri(app.web_view);
        if (uri && g_str_has_prefix(uri, "wpe-tsp://") && webkit_web_view_can_go_back(app.web_view))
            webkit_web_view_go_back(app.web_view);
        else if (webkit_web_view_is_loading(app.web_view))
            webkit_web_view_stop_loading(app.web_view);
        break;
    }
    case BTN_Y: show_url_keyboard(); break;
    case BTN_X: webkit_web_view_reload(app.web_view); break;
    case BTN_L1: send_key(WPE_KEY_Page_Up); break;
    case BTN_R1: send_key(WPE_KEY_Page_Down); break;
    case BTN_START: send_key(WPE_KEY_Return); break;
    case BTN_SELECT: open_menu(); break;
    }
}

/* D-pad sends arrow keys, auto-repeating while held. */
static void handle_hat(Uint8 value)
{
    guint keyval = (value & SDL_HAT_UP) ? WPE_KEY_Up : (value & SDL_HAT_DOWN) ? WPE_KEY_Down
                 : (value & SDL_HAT_LEFT) ? WPE_KEY_Left : (value & SDL_HAT_RIGHT) ? WPE_KEY_Right : 0;
    if (keyval == app.arrow_keyval)
        return;
    if (app.arrow_keyval)
        send_key_event(WPE_EVENT_KEYBOARD_KEY_UP, app.arrow_keyval);
    app.arrow_keyval = keyval;
    if (keyval) {
        send_key_event(WPE_EVENT_KEYBOARD_KEY_DOWN, keyval);
        app.arrow_since = app.arrow_last = now_ms();
    }
}

/* L2/R2 are analog axes: act once per press. */
static void handle_trigger(int index, int value)
{
    gboolean down = value > TRIGGER_THRESHOLD;
    if (down == app.trigger_down[index])
        return;
    app.trigger_down[index] = down;
    if (down)
        send_key(index == 0 ? WPE_KEY_Home : WPE_KEY_End);
}

/* ------------------------------------------------------------------------- */
/* Physical keyboard and mouse (hid.c). Not rotated in portrait mode: they're used in the       */
/* user's frame, which is what the logical screen already is.                                   */
/* ------------------------------------------------------------------------- */

static WPEModifiers hid_wpe_modifiers(guint m)
{
    return (WPEModifiers)(((m & HID_MOD_SHIFT) ? WPE_MODIFIER_KEYBOARD_SHIFT : 0)
                          | ((m & HID_MOD_CONTROL) ? WPE_MODIFIER_KEYBOARD_CONTROL : 0)
                          | ((m & HID_MOD_ALT) ? WPE_MODIFIER_KEYBOARD_ALT : 0)
                          | ((m & HID_MOD_META) ? WPE_MODIFIER_KEYBOARD_META : 0)
                          | ((m & HID_MOD_CAPS_LOCK) ? WPE_MODIFIER_KEYBOARD_CAPS_LOCK : 0));
}

/* The menu is driven by the gamepad: feed it the matching d-pad/button events */
static void menu_feed_key(guint keyval, gboolean pressed)
{
    SDL_Event ev = { 0 };
    if (keyval == WPE_KEY_Up || keyval == WPE_KEY_Down) {
        ev.type = SDL_JOYHATMOTION;
        ev.jhat.value = !pressed ? SDL_HAT_CENTERED : keyval == WPE_KEY_Up ? SDL_HAT_UP : SDL_HAT_DOWN;
    } else if (keyval == WPE_KEY_Return || keyval == WPE_KEY_KP_Enter || keyval == WPE_KEY_space
               || keyval == WPE_KEY_Escape || keyval == WPE_KEY_Menu) {
        ev.type = pressed ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
        ev.jbutton.button = keyval == WPE_KEY_Escape || keyval == WPE_KEY_Menu ? BTN_B : BTN_A;
    } else
        return;
    menu_handle_event(&ev);
}

/* Browser shortcuts. Returns TRUE if the key was one. */
static gboolean hid_shortcut(guint keyval, WPEModifiers mods)
{
    gboolean ctrl = mods & WPE_MODIFIER_KEYBOARD_CONTROL, alt = mods & WPE_MODIFIER_KEYBOARD_ALT;
    if (keyval == WPE_KEY_F6 || (ctrl && (keyval == WPE_KEY_l || keyval == WPE_KEY_L)))
        show_url_keyboard();
    else if (keyval == WPE_KEY_F5 || (ctrl && (keyval == WPE_KEY_r || keyval == WPE_KEY_R)))
        webkit_web_view_reload(app.web_view);
    else if (alt && keyval == WPE_KEY_Left)
        webkit_web_view_go_back(app.web_view);
    else if (alt && keyval == WPE_KEY_Right)
        webkit_web_view_go_forward(app.web_view);
    else if (keyval == WPE_KEY_Menu)
        open_menu();
    else
        return FALSE;
    return TRUE;
}

static void hid_key(guint keycode, guint keysym, int value, guint modifiers, void *user_data)
{
    (void)user_data;
    if (player_busy() || !app.renderer) /* mpv has the screen */
        return;
    WPEModifiers mods = hid_wpe_modifiers(modifiers);
    guint keyval = keysym; /* in the active layout (hid.c) */
    if (!keyval) /* no layout could be compiled: WPE's own (US) keymap */
        wpe_keymap_translate_keyboard_state(wpe_display_get_keymap(wpe_view_get_display(app.wpe_view)), keycode,
                                            mods & (WPE_MODIFIER_KEYBOARD_SHIFT | WPE_MODIFIER_KEYBOARD_CAPS_LOCK),
                                            0, &keyval, NULL, NULL, NULL);
    if (!keyval)
        return;
    app.needs_present = TRUE;
    gboolean pressed = value != 0;

    if (menu_visible()) {
        if (value != 2) /* the menu repeats held keys itself */
            menu_feed_key(keyval, pressed);
        return;
    }
    if (osk_visible() && osk_mode() == OSK_MODE_URL) {
        if (pressed && !(mods & (WPE_MODIFIER_KEYBOARD_CONTROL | WPE_MODIFIER_KEYBOARD_ALT))) {
            char text[8] = "";
            guint32 c = wpe_keyval_to_unicode(keyval);
            if (c)
                text[g_unichar_to_utf8(c, text)] = 0;
            osk_physical_key(keyval, text);
        }
        return;
    }
    if (pressed) {
        app.hid_typing = TRUE;
        if (osk_visible()) { /* form field: the physical keyboard replaces the on-screen one */
            osk_hide();
            set_view_height(app.screen_h);
        }
        if (value == 1 && hid_shortcut(keyval, mods))
            return;
    }
    send_event(wpe_event_keyboard_new(pressed ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, app.wpe_view,
                                      WPE_INPUT_SOURCE_KEYBOARD, now_ms(), mods, keycode, keyval));
}

static gboolean notice_expired(gpointer user_data)
{
    (void)user_data;
    app.notice_timer = 0;
    g_clear_pointer(&app.notice, g_free);
    app.needs_present = TRUE;
    return G_SOURCE_REMOVE;
}

static void show_notice(const char *text)
{
    g_free(app.notice);
    app.notice = g_strdup(text);
    if (app.notice_timer)
        g_source_remove(app.notice_timer);
    app.notice_timer = g_timeout_add(NOTICE_MS, notice_expired, NULL);
    app.needs_present = TRUE;
}

/* Short label of an XKB layout for the on-screen keyboard's language key: "ua(phonetic)" -> "UA",
 * English layouts -> "EN" */
static char *layout_label(const char *layout)
{
    char *base = g_strndup(layout, strcspn(layout, "("));
    char *label = !strcmp(base, "us") || !strcmp(base, "gb") ? g_strdup("EN") : g_ascii_strup(base, 7);
    g_free(base);
    return label;
}

/* The on-screen keyboard gets the letters of every physical-keyboard layout (KEYBOARD_LAYOUTS),
 * in the same order */
static void add_osk_languages(void)
{
    for (guint i = 0; i < hid_layout_count(); i++) {
        char *rows[3];
        for (int r = 0; r < 3; r++)
            rows[r] = hid_layout_letters(i, r);
        char *label = layout_label(hid_layout_name_at(i));
        osk_add_language(label, (const char *const *)rows);
        g_free(label);
        for (int r = 0; r < 3; r++)
            g_free(rows[r]);
    }
}

static void hid_layout_changed(const char *name, void *user_data)
{
    (void)user_data;
    osk_select_language((int)hid_layout_index()); /* the address bar shows (and types) the same */
    const char *variant = strchr(name, '(');
    char *label = layout_label(name);
    char *text = variant ? g_strdup_printf("Keyboard: %s %s", label, variant) : g_strdup_printf("Keyboard: %s", label);
    show_notice(text);
    g_free(text);
    g_free(label);
}

static void hid_show_pointer(void)
{
    app.pointer_used_ms = now_ms();
    if (app.pointer_hidden) {
        app.pointer_hidden = FALSE;
        app.needs_present = TRUE;
    }
}

static void hid_motion(int dx, int dy, void *user_data)
{
    (void)user_data;
    if (player_busy() || !app.renderer || menu_visible())
        return;
    hid_show_pointer();
    send_pointer_move(dx, dy);
}

static void hid_button(guint button, gboolean pressed, void *user_data)
{
    (void)user_data;
    if (player_busy() || !app.renderer || menu_visible())
        return;
    if (!pressed && (app.swallow_mouse & (1u << button))) {
        app.swallow_mouse &= ~(1u << button);
        return;
    }
    if (pressed && app.pointer_hidden) { /* like A: first only show where the pointer is */
        hid_show_pointer();
        app.swallow_mouse |= 1u << button;
        return;
    }
    hid_show_pointer();
    app.hid_typing = FALSE; /* clicking a field may bring up the on-screen keyboard again */
    if (button == 1)
        send_click(pressed);
    else
        send_button(button, pressed);
}

static void hid_wheel(int dx, int dy, void *user_data)
{
    (void)user_data;
    if (player_busy() || !app.renderer || menu_visible())
        return;
    /* Wheel notches (not precise deltas): WebKit scrolls by its line step per notch.
     * Positive deltas scroll towards the top/left. */
    send_event(wpe_event_scroll_new(app.wpe_view, WPE_INPUT_SOURCE_MOUSE, now_ms(), 0, -dx, dy, FALSE, FALSE,
                                    app.px / app.scale, app.py / app.scale));
}

/* Portrait mode: the device is held turned clockwise, so the d-pad and both sticks are turned
 * with it. Map them to what the user sees: left is up, right is down, down is left and up is
 * right. Applied before the menu and keyboard see the events too. */
static void rotate_input(SDL_Event *ev)
{
    if (!app.portrait)
        return;
    if (ev->type == SDL_JOYHATMOTION) {
        Uint8 v = ev->jhat.value;
        ev->jhat.value = ((v & SDL_HAT_LEFT) ? SDL_HAT_UP : 0) | ((v & SDL_HAT_RIGHT) ? SDL_HAT_DOWN : 0)
                       | ((v & SDL_HAT_DOWN) ? SDL_HAT_LEFT : 0) | ((v & SDL_HAT_UP) ? SDL_HAT_RIGHT : 0);
    } else if (ev->type == SDL_JOYAXISMOTION) {
        /* logical x = -physical y, logical y = physical x */
        int v = ev->jaxis.value;
        switch (ev->jaxis.axis) {
        case AXIS_LX: ev->jaxis.axis = AXIS_LY; break;
        case AXIS_LY: ev->jaxis.axis = AXIS_LX; ev->jaxis.value = (Sint16)MIN(-v, 32767); break;
        case AXIS_RX: ev->jaxis.axis = AXIS_RY; break;
        case AXIS_RY: ev->jaxis.axis = AXIS_RX; ev->jaxis.value = (Sint16)MIN(-v, 32767); break;
        }
    }
}

static gboolean input_tick(gpointer user_data)
{
    (void)user_data;
    SDL_Event ev;
    if (player_busy()) {
        /* mpv has the screen and the gamepad; while yt-dlp resolves, only B (cancel) works */
        while (SDL_PollEvent(&ev))
            if (!player_playing() && ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == BTN_B)
                player_cancel();
        if (app.needs_present)
            present();
        return G_SOURCE_CONTINUE;
    }
    while (SDL_PollEvent(&ev)) {
        rotate_input(&ev);
        if (menu_handle_event(&ev) || osk_handle_event(&ev)) {
            app.needs_present = TRUE;
            continue;
        }
        switch (ev.type) {
        case SDL_QUIT:
            g_main_loop_quit(app.loop);
            break;
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
            handle_button(ev.jbutton.button, ev.type == SDL_JOYBUTTONDOWN);
            break;
        case SDL_JOYHATMOTION:
            handle_hat(ev.jhat.value);
            break;
        case SDL_JOYAXISMOTION:
            if (ev.jaxis.axis < NUM_AXES)
                app.axis[ev.jaxis.axis] = ev.jaxis.value;
            if (ev.jaxis.axis == AXIS_L2)
                handle_trigger(0, ev.jaxis.value);
            else if (ev.jaxis.axis == AXIS_R2)
                handle_trigger(1, ev.jaxis.value);
            break;
        }
    }

    if (menu_visible() || osk_visible()) {
        if (menu_tick() || osk_tick())
            app.needs_present = TRUE;
        if (app.needs_present)
            present();
        return G_SOURCE_CONTINUE;
    }
    if (ytdlp_prompt.title) /* the screen is free now */
        ytdlp_show_prompt();

    /* Arrow key auto-repeat */
    guint32 now = now_ms();
    if (app.arrow_keyval && now - app.arrow_since >= KEY_REPEAT_DELAY_MS && now - app.arrow_last >= KEY_REPEAT_RATE_MS) {
        app.arrow_last = now;
        send_key_event(WPE_EVENT_KEYBOARD_KEY_DOWN, app.arrow_keyval);
    }

    /* Pointer: left stick */
    double dx = axis_curve(app.axis[AXIS_LX], 3) * POINTER_MAX_SPEED;
    double dy = axis_curve(app.axis[AXIS_LY], 3) * POINTER_MAX_SPEED;
    if (dx || dy) {
        app.pointer_used_ms = now;
        if (app.pointer_hidden) {
            app.pointer_hidden = FALSE;
            app.needs_present = TRUE;
        }
        send_pointer_move(dx, dy);
    } else if (!app.pointer_hidden && app.pointer_hide_ms && now - app.pointer_used_ms >= app.pointer_hide_ms) {
        app.pointer_hidden = TRUE; /* idle: hide until the stick or A is used again */
        app.needs_present = TRUE;
    }

    /* Scroll: right stick */
    double sx = axis_curve(app.axis[AXIS_RX], 2) * SCROLL_MAX_SPEED;
    double sy = axis_curve(app.axis[AXIS_RY], 2) * SCROLL_MAX_SPEED;
    if (sx || sy)
        send_scroll(sx, sy);

    if (app.needs_present)
        present();
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------- */
/* WebKit callbacks                                                          */
/* ------------------------------------------------------------------------- */

/* Reload after a WebProcess crash, but don't loop forever if it keeps crashing. */
#define MAX_CRASH_RELOADS 3
static int crash_reloads;

static void on_load_progress(WebKitWebView *web_view, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec; (void)user_data;
    app.load_progress = webkit_web_view_is_loading(web_view) ? webkit_web_view_get_estimated_load_progress(web_view) : 1.0;
    app.needs_present = TRUE;
}

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data)
{
    (void)user_data;
    static const char *names[] = { "started", "redirected", "committed", "finished" };
    fprintf(stderr, "[wpe-tsp] load %s: %s\n", names[load_event], webkit_web_view_get_uri(web_view));
    if (load_event == WEBKIT_LOAD_FINISHED) {
        crash_reloads = 0;
        history_add(webkit_web_view_get_uri(web_view), webkit_web_view_get_title(web_view));
    }
}

static gboolean on_load_failed(WebKitWebView *web_view, WebKitLoadEvent load_event, char *uri, GError *error, gpointer user_data)
{
    (void)web_view; (void)load_event; (void)user_data;
    fprintf(stderr, "[wpe-tsp] load failed: %s: %s\n", uri, error->message);
    return FALSE;
}

static gboolean killed_for_memory; /* set by the watchdog before terminating the page */

static void show_out_of_memory_page(WebKitWebView *web_view, const char *uri)
{
    char *escaped = g_markup_escape_text(uri ? uri : "", -1);
    char *html = g_strdup_printf(
        "<html><head><meta name='viewport' content='width=device-width'><style>"
        "body{background:#202124;color:#e8eaed;font-family:sans-serif;margin:8vh 8vw}"
        "h1{font-size:1.4em;color:#8ab4f8}p{color:#9aa0a6;line-height:1.5}code{color:#e8eaed;word-break:break-all}"
        "</style></head><body><h1>This page needs more memory than the device has</h1>"
        "<p>It was closed to keep the device responsive.</p><p><code>%s</code></p>"
        "<p>X: try again &nbsp;&middot;&nbsp; SELECT: menu (Back, Home)</p></body></html>", escaped);
    webkit_web_view_load_alternate_html(web_view, html, uri, NULL);
    g_free(html);
    g_free(escaped);
}

static void on_web_process_terminated(WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, gpointer user_data)
{
    (void)user_data;
    const char *uri = webkit_web_view_get_uri(web_view);
    /* Out of memory (WebKit's per-process limit or our watchdog): reloading would just repeat it. */
    if (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT || killed_for_memory) {
        fprintf(stderr, "[wpe-tsp] page killed for using too much memory: %s\n", uri ? uri : "");
        killed_for_memory = FALSE;
        show_out_of_memory_page(web_view, uri);
        return;
    }
    if (crash_reloads >= MAX_CRASH_RELOADS) {
        fprintf(stderr, "[wpe-tsp] web process terminated (reason %d), giving up after %d reloads\n", reason, crash_reloads);
        return;
    }
    crash_reloads++;
    fprintf(stderr, "[wpe-tsp] web process terminated (reason %d), reloading\n", reason);
    webkit_web_view_reload(web_view);
}

static long mem_available_mb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char line[128];
    long kb = -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1)
            break;
    fclose(f);
    return kb < 0 ? -1 : kb / 1024;
}

/* Make our WebKit child processes the kernel OOM killer's first choice (pages, not the UI).
 * The web process gets the highest score: losing it only loses the current page.
 * Also reaps exited WebKit processes: on this kernel (4.9, no pidfd) GLib reports child exits
 * without always reaping them, which would leave zombies behind. */
static void adjust_child_oom_scores(void)
{
    GDir *proc = g_dir_open("/proc", 0, NULL);
    if (!proc)
        return;
    pid_t self = getpid();
    const char *name;
    while ((name = g_dir_read_name(proc))) {
        if (!g_ascii_isdigit(name[0]))
            continue;
        char path[64], buf[256];
        g_snprintf(path, sizeof(path), "/proc/%s/stat", name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        /* stat: "pid (comm) state ppid ..." */
        char *comm_end = strrchr(buf, ')');
        int ppid = 0;
        char state = 0;
        if (!comm_end || sscanf(comm_end + 2, "%c %d", &state, &ppid) != 2 || ppid != self)
            continue;
        if (state == 'Z' && strstr(buf, "(WPE")) {
            waitpid((pid_t)atoi(name), NULL, WNOHANG);
            continue;
        }
        const char *score = strstr(buf, "(WPEWebProcess") ? "1000" : strstr(buf, "(WPENetworkProc") ? "500" : NULL;
        if (!score)
            continue;
        g_snprintf(path, sizeof(path), "/proc/%s/oom_score_adj", name);
        if ((f = fopen(path, "w"))) {
            fputs(score, f);
            fclose(f);
        }
    }
    g_dir_close(proc);
}

static gboolean memory_watchdog(gpointer user_data)
{
    (void)user_data;
    static int ticks;
    if (ticks++ % 3 == 0)
        adjust_child_oom_scores();
    long available = mem_available_mb();
    if (available >= 0 && available < LOW_MEMORY_KILL_MB && !killed_for_memory) {
        fprintf(stderr, "[wpe-tsp] low memory (%ld MB available): terminating the page\n", available);
        killed_for_memory = TRUE;
        webkit_web_view_terminate_web_process(app.web_view);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_unix_signal(gpointer user_data)
{
    (void)user_data;
    g_main_loop_quit(app.loop);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */

static int create_display(void)
{
    app.window = SDL_CreateWindow("WPE", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  SCREEN_W, SCREEN_H, SDL_WINDOW_FULLSCREEN);
    if (!app.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }
    app.renderer = SDL_CreateRenderer(app.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app.renderer) {
        fprintf(stderr, "Accelerated renderer failed (%s), using software\n", SDL_GetError());
        app.renderer = SDL_CreateRenderer(app.window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!app.renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_RendererInfo info;
    if (!SDL_GetRendererInfo(app.renderer, &info))
        fprintf(stderr, "[wpe-tsp] SDL renderer: %s\n", info.name);
    SDL_ShowCursor(SDL_DISABLE);
    return 0;
}

static int init_sdl(void)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    if (create_display() < 0)
        return -1;
    if (SDL_NumJoysticks() > 0)
        app.joystick = SDL_JoystickOpen(0);
    return 0;
}

int main(int argc, char **argv)
{
    /* Home page: HOME_URL from settings.conf only. Without one, the view keeps WebKit's own empty
     * page. The start page follows START_PAGE (below); a command-line URL overrides it. */
    const char *home_env = g_getenv("WPE_TSP_HOME_URL");
    const char *search_env = g_getenv("WPE_TSP_SEARCH_URL");
    app.home_url = home_env && *home_env ? home_env : NULL;
    app.search_url = search_env && *search_env ? search_env : SEARCH_URL;
    const char *hide_env = g_getenv("WPE_TSP_POINTER_HIDE_SECONDS");
    app.pointer_hide_ms = 1000 * (hide_env ? (guint32)g_ascii_strtoull(hide_env, NULL, 10) : DEFAULT_POINTER_HIDE_SECONDS);
    app.pointer_used_ms = now_ms();
    const char *history_env = g_getenv("WPE_TSP_HISTORY_SIZE");
    history_size = history_env ? (guint)MIN(g_ascii_strtoull(history_env, NULL, 10), 60) : DEFAULT_HISTORY_SIZE;
    history_load();

    /* START_PAGE: "home" (default), "last" (newest History entry; home if there is none) or
     * "address" (empty page with the address bar open). */
    const char *start_env = g_getenv("WPE_TSP_START_PAGE");
    gboolean start_with_address_bar = FALSE;
    const char *start_url = app.home_url;
    if (argc > 1)
        start_url = argv[1];
    else if (start_env && !g_ascii_strcasecmp(start_env, "last") && history->len)
        start_url = ((HistoryEntry *)g_ptr_array_index(history, 0))->uri;
    else if (start_env && !g_ascii_strcasecmp(start_env, "address")) {
        start_url = NULL;
        start_with_address_bar = TRUE;
    } else if (start_env && *start_env && g_ascii_strcasecmp(start_env, "home") && g_ascii_strcasecmp(start_env, "last"))
        fprintf(stderr, "[wpe-tsp] START_PAGE=%s unknown, using home\n", start_env);
    const char *play_env = g_getenv("WPE_TSP_PLAY_YOUTUBE_IN_MPV");
    app.play_youtube = !play_env || strcmp(play_env, "0");
    PlayerCallbacks player_callbacks = {
        .release_display = player_release_display,
        .restore_display = player_restore_display,
        .status_changed = player_status_changed,
        .finished = player_finished,
        .ytdlp_missing = player_ytdlp_missing,
    };
    /* mpv and yt-dlp are part of the app: <app>/mpv, <app>/bin/yt-dlp */
    char *exe_path = g_file_read_link("/proc/self/exe", NULL);
    char *app_dir = g_path_get_dirname(exe_path);
    char *app_root = g_path_get_dirname(app_dir);
    player_init(app_root, &player_callbacks);
    if (app.play_youtube) {
        char *ytdlp = player_ytdlp_path();
        ytdlp_init(ytdlp);
        g_free(ytdlp);
        g_timeout_add_seconds(YTDLP_CHECK_DELAY_S, ytdlp_start_check, NULL);
    }
    g_free(exe_path); g_free(app_dir); g_free(app_root);
    const char *scale_env = g_getenv("WPE_TSP_SCALE");
    app.scale = scale_env ? CLAMP(g_ascii_strtod(scale_env, NULL), 1.0, 3.0) : DEFAULT_SCALE;
    app.load_progress = 1.0;
    stats.enabled = g_getenv("WPE_TSP_STATS") != NULL;
    app.screen_w = SCREEN_W;
    app.screen_h = SCREEN_H;
    app.px = SCREEN_W / 2.0;
    app.py = SCREEN_H / 2.0;

    if (init_sdl() < 0)
        return 1;

    /* Data files live next to the binary: <app>/bin/wpe-tsp, <app>/share/... */
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *bindir = g_path_get_dirname(exe);
    char *app_data_dir = g_build_filename(bindir, "..", "share", NULL);
    char *font_path = g_build_filename(app_data_dir, "fonts", "DejaVuSans.ttf", NULL);
    char *keymaps_path = g_build_filename(app_data_dir, "xkb-keymaps.bin", NULL);
    OskCallbacks osk_callbacks = {
        .commit = osk_commit,
        .backspace = osk_backspace,
        .enter = osk_enter,
        .closed = osk_closed,
        .is_search = osk_is_search,
    };
    if (!osk_init(app.renderer, SCREEN_W, SCREEN_H, font_path, &osk_callbacks))
        fprintf(stderr, "[wpe-tsp] on-screen keyboard unavailable\n");
    char *engine = search_engine_name(app.search_url);
    osk_set_search_name(engine);
    g_free(engine);
    if (!menu_init(app.renderer, SCREEN_W, SCREEN_H, font_path))
        fprintf(stderr, "[wpe-tsp] menu unavailable\n");
    g_free(exe); g_free(bindir); g_free(font_path); /* app_data_dir, keymaps_path: used below */
    app.view_height = app.screen_h;
    present();

    WPEDisplay *display = WPE_DISPLAY(g_object_new(WPE_TYPE_DISPLAY_SDL, NULL));
    GError *error = NULL;
    if (!wpe_display_connect(display, &error)) {
        fprintf(stderr, "wpe_display_connect failed: %s\n", error->message);
        return 1;
    }
    wpe_display_set_primary(display);

    /* Memory pressure handling, tuned for 1 GB RAM (WebKit's default limit is all of RAM). */
    WebKitMemoryPressureSettings *web_memory = webkit_memory_pressure_settings_new();
    const char *limit_env = g_getenv("WPE_TSP_PAGE_MEMORY_LIMIT_MB");
    guint page_limit_mb = limit_env ? (guint)CLAMP(g_ascii_strtoull(limit_env, NULL, 10), 150, 900) : WEB_PROCESS_MEMORY_LIMIT_MB;
    webkit_memory_pressure_settings_set_memory_limit(web_memory, page_limit_mb);
    /* strict before conservative: WebKit requires conservative < strict at every call */
    webkit_memory_pressure_settings_set_strict_threshold(web_memory, 0.6);
    webkit_memory_pressure_settings_set_conservative_threshold(web_memory, 0.4);
    webkit_memory_pressure_settings_set_kill_threshold(web_memory, 1.0);
    webkit_memory_pressure_settings_set_poll_interval(web_memory, MEMORY_POLL_INTERVAL_S);

    WebKitMemoryPressureSettings *network_memory = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(network_memory, NETWORK_PROCESS_MEMORY_LIMIT_MB);
    webkit_memory_pressure_settings_set_strict_threshold(network_memory, 0.75);
    webkit_memory_pressure_settings_set_conservative_threshold(network_memory, 0.5);
    webkit_memory_pressure_settings_set_poll_interval(network_memory, MEMORY_POLL_INTERVAL_S);
    webkit_network_session_set_memory_pressure_settings(network_memory); /* before the network process starts */
    webkit_memory_pressure_settings_free(network_memory);

    WebKitWebContext *web_context = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
                                                                    "memory-pressure-settings", web_memory, NULL));
    webkit_memory_pressure_settings_free(web_memory);
    /* Single-page browsing: no back/forward page cache, small in-memory resource caches. */
    webkit_web_context_set_cache_model(web_context, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
    webkit_web_context_register_uri_scheme(web_context, "wpe-tsp", on_app_scheme_request, NULL, NULL);
    /* The default session keeps local storage/IndexedDB on disk but cookies only in memory
     * unless a persistent jar is set: logins would be lost on every restart. */
    WebKitNetworkSession *network_session = webkit_network_session_get_default();
    char *cookie_path = g_build_filename(webkit_website_data_manager_get_base_data_directory(
                                             webkit_network_session_get_website_data_manager(network_session)),
                                         "cookies.sqlite", NULL);
    webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(network_session),
                                                 cookie_path, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    g_free(cookie_path);
    const char *download_env = g_getenv("WPE_TSP_DOWNLOAD_DIR");
    downloads_init(webkit_network_session_get_default(), download_env && *download_env ? download_env : DEFAULT_DOWNLOAD_DIR,
                   on_downloads_changed, on_downloads_play, NULL);

    WebKitSettings *settings = webkit_settings_new_with_settings(
        "enable-developer-extras", FALSE,
        "enable-smooth-scrolling", TRUE,
        /* Diagnostics: WPE_TSP_CONSOLE=1 prints the pages' console messages/errors to the log */
        "enable-write-console-messages-to-stdout", g_getenv("WPE_TSP_CONSOLE") != NULL,
        /* WEBGL=1: WebGL via ANGLE on the PowerVR GPU. Off by default: costs memory, and some
         * sites (e.g. Google Maps' vector map) get much heavier with it. */
        "enable-webgl", !g_strcmp0(g_getenv("WPE_TSP_WEBGL"), "1"),
        NULL);
    /* USER_AGENT: "mobile" (default), "desktop" (WebKit's own) or a full UA string */
    const char *ua_env = g_getenv("WPE_TSP_USER_AGENT");
    if (!ua_env || !*ua_env || !strcmp(ua_env, "mobile"))
        app.user_agent = g_strdup(MOBILE_USER_AGENT);
    else if (strcmp(ua_env, "desktop"))
        app.user_agent = g_strdup(ua_env);
    if (app.user_agent)
        webkit_settings_set_user_agent(settings, app.user_agent);
    fprintf(stderr, "[wpe-tsp] user agent: %s\n", webkit_settings_get_user_agent(settings));
    app.web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                                "display", display,
                                                "web-context", web_context,
                                                "settings", settings,
                                                NULL));
    g_object_unref(settings);
    g_object_unref(web_context);
    app.wpe_view = webkit_web_view_get_wpe_view(app.web_view);
    wpe_view_focus_in(app.wpe_view);

    g_signal_connect(app.web_view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(app.web_view, "notify::title", G_CALLBACK(on_title_changed), NULL);
    g_signal_connect(app.web_view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(app.web_view, "notify::uri", G_CALLBACK(on_uri_changed), NULL);
    if (app.play_youtube)
        setup_embed_play(app.web_view);
    /* AD_BLOCK=1 (default): share/adblock/rules.json; WPE_TSP_ADBLOCK_RULES overrides the file */
    const char *adblock_env = g_getenv("WPE_TSP_AD_BLOCK");
    if (!adblock_env || strcmp(adblock_env, "0")) {
        const char *rules_env = g_getenv("WPE_TSP_ADBLOCK_RULES");
        char *rules = rules_env && *rules_env ? g_strdup(rules_env)
                                              : g_build_filename(app_data_dir, "adblock", "rules.json", NULL);
        setup_adblock(rules);
        g_free(rules);
    }
    g_free(app_data_dir);
    g_signal_connect(app.web_view, "mouse-target-changed", G_CALLBACK(on_mouse_target_changed), NULL);
    g_signal_connect(app.web_view, "notify::estimated-load-progress", G_CALLBACK(on_load_progress), NULL);
    g_signal_connect(app.web_view, "notify::is-loading", G_CALLBACK(on_load_progress), NULL);
    g_signal_connect(app.web_view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(app.web_view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);

    static const HidCallbacks hid_callbacks = { hid_key, hid_motion, hid_button, hid_wheel, hid_layout_changed, NULL };
    const char *layouts_env = g_getenv("WPE_TSP_KEYBOARD_LAYOUTS");
    hid_init(&hid_callbacks, layouts_env && *layouts_env ? layouts_env : DEFAULT_KEYBOARD_LAYOUTS, keymaps_path);
    g_free(keymaps_path);
    add_osk_languages();

    if (start_url)
        webkit_web_view_load_uri(app.web_view, start_url);
    else if (start_with_address_bar)
        show_url_keyboard();

    app.loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(TICK_MS, input_tick, NULL);
    g_timeout_add(WATCHDOG_INTERVAL_MS, memory_watchdog, NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
    g_main_loop_run(app.loop);

    g_object_unref(app.web_view);
    g_object_unref(display);
    osk_shutdown();
    menu_shutdown();
    if (app.joystick)
        SDL_JoystickClose(app.joystick);
    SDL_Quit();
    return 0;
}
