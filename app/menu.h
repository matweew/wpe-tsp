/*
 * Simple modal list menu (the SELECT context menu), drawn centered over the page.
 */
#pragma once

#include <SDL.h>
#include <stdbool.h>

/* index of the chosen item, or -1 if the menu was closed without choosing (B / SELECT) */
typedef void (*MenuActivate)(int index, void *user_data);

/* ui_scale: sizes are for the Smart Pro (1.0); the Brick's denser panels use more pixels */
bool menu_init(SDL_Renderer *renderer, int screen_w, int screen_h, const char *font_path, float ui_scale);
void menu_shutdown(void);
void menu_set_renderer(SDL_Renderer *renderer);
/* Screen orientation changed (portrait mode): lay out for the new size */
void menu_set_screen_size(int screen_w, int screen_h);

/* items: NULL-terminated array of labels (copied). */
void menu_open(const char *title, const char *subtitle, const char *const *items,
               MenuActivate activate, void *user_data);
void menu_close(void);
bool menu_visible(void);

/* Returns true if the event was consumed by the menu. */
bool menu_handle_event(const SDL_Event *event);
/* Held d-pad auto-repeat. Returns true if a redraw is needed. */
bool menu_tick(void);
void menu_draw(void);

/* One-line status strip (e.g. download progress) ending at y = bottom; progress 0..1 draws a bar,
 * <0 none. Uses the menu's fonts, so it works with or without an open menu. */
void menu_draw_status(const char *text, double progress, int bottom);
