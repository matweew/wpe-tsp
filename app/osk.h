/*
 * On-screen keyboard docked at the bottom of the screen (Android landscape style),
 * driven by the gamepad.
 *
 *   URL mode:  letters (all layouts) + URL symbols; edits an internal line, ENTER returns it
 *              (the browser loads it as a URL or searches for it).
 *   Form mode: letters of the added languages (the browser's XKB keyboard layouts, built-in
 *              English until then); every key goes straight to the page.
 */
#pragma once

#include <SDL.h>
#include <stdbool.h>

typedef enum {
    OSK_MODE_URL,
    OSK_MODE_FORM,
} OskMode;

typedef enum {
    OSK_FIELD_TEXT,
    OSK_FIELD_PASSWORD,
    OSK_FIELD_NUMBER,   /* digits / phone / PIN: open on the symbols page */
    OSK_FIELD_EMAIL,
    OSK_FIELD_URL,
} OskField;

typedef struct {
    void (*commit)(const char *text, void *user_data);    /* form mode: insert text */
    void (*backspace)(void *user_data);                   /* form mode: delete one char */
    void (*enter)(const char *line, void *user_data);     /* URL mode: line; form mode: NULL */
    void (*closed)(void *user_data);                      /* keyboard dismissed (B) */
    bool (*is_search)(const char *line, void *user_data); /* URL mode: would ENTER search? */
    void *user_data;
} OskCallbacks;

/* ui_scale: sizes are for the Smart Pro (1.0); the Brick's denser panels use more pixels */
bool osk_init(SDL_Renderer *renderer, int screen_w, int screen_h,
              const char *font_path, const OskCallbacks *callbacks, float ui_scale);
void osk_shutdown(void);
/* Switch renderer (e.g. the window is recreated): call with NULL before destroying the old one. */
void osk_set_renderer(SDL_Renderer *renderer);
/* Screen orientation changed (portrait mode): lay out for the new size */
void osk_set_screen_size(int screen_w, int screen_h);

void osk_show(OskMode mode, OskField field, const char *initial_text);
void osk_hide(void);
bool osk_visible(void);
OskMode osk_mode(void);
int osk_height(void);

/* Add a language: name is its key label ("EN", "UA"), rows its three letter rows (uppercase,
 * space-separated). The first one added replaces the built-in English; the language key cycles
 * through them in this order, and the address bar starts on the first. */
void osk_add_language(const char *name, const char *const rows[3]);
/* Show the letters of language index (order added), e.g. to match a physical keyboard's layout. */
void osk_select_language(int index);

/* Name of the search engine, for the empty address bar's placeholder ("Search Google or type a URL"). */
void osk_set_search_name(const char *name);

/* Text of the focused page field (form mode preview), as reported by WebKit. */
void osk_set_field_text(const char *text);

/* A physical keyboard key while the URL bar is open: text (UTF-8, may be NULL) is typed;
 * BackSpace/Delete, Left/Right/Home/End, Return and Escape edit, enter or close.
 * keyval: WPE_KEY_* (X11 keysym). Returns true if handled. */
bool osk_physical_key(unsigned keyval, const char *text);

/* Returns true if the event was consumed by the keyboard. */
bool osk_handle_event(const SDL_Event *event);
/* Call every frame: handles held-button auto-repeat. Returns true if a redraw is needed. */
bool osk_tick(void);
void osk_draw(void);
