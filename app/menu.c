/*
 * Modal list menu, see menu.h. Colors match the on-screen keyboard (dark, blue accent).
 */
#include "menu.h"

#include "gamepad.h"
#include <SDL_ttf.h>
#include <glib.h>
#include <string.h>

#define MAX_ITEMS 16
#define ITEM_H 52
#define HEADER_H 76
#define MENU_W 560
#define AXIS_THRESHOLD 16000
#define REPEAT_DELAY_MS 350
#define REPEAT_RATE_MS 90

static struct {
    SDL_Renderer *renderer;
    int screen_w, screen_h;
    TTF_Font *font;
    TTF_Font *font_small;

    bool visible;
    char *title;
    char *subtitle;
    char *items[MAX_ITEMS];
    int n_items;
    int selected;
    MenuActivate activate;
    void *user_data;

    int hold_dir;   /* -1 up, 1 down, 0 none */
    Uint32 hold_since, last_repeat;
    int axis_dir;
} menu;

bool menu_init(SDL_Renderer *renderer, int screen_w, int screen_h, const char *font_path)
{
    memset(&menu, 0, sizeof(menu));
    menu.renderer = renderer;
    menu.screen_w = screen_w;
    menu.screen_h = screen_h;
    if (!TTF_WasInit() && TTF_Init() < 0)
        return false;
    menu.font = TTF_OpenFont(font_path, 28);
    menu.font_small = TTF_OpenFont(font_path, 20);
    return menu.font && menu.font_small;
}

static void clear_items(void)
{
    for (int i = 0; i < menu.n_items; i++)
        g_free(menu.items[i]);
    menu.n_items = 0;
    g_clear_pointer(&menu.title, g_free);
    g_clear_pointer(&menu.subtitle, g_free);
}

void menu_set_renderer(SDL_Renderer *renderer)
{
    menu.renderer = renderer;
}

void menu_set_screen_size(int screen_w, int screen_h)
{
    menu.screen_w = screen_w;
    menu.screen_h = screen_h;
}

void menu_shutdown(void)
{
    clear_items();
    if (menu.font)
        TTF_CloseFont(menu.font);
    if (menu.font_small)
        TTF_CloseFont(menu.font_small);
    memset(&menu, 0, sizeof(menu));
}

void menu_open(const char *title, const char *subtitle, const char *const *items,
               MenuActivate activate, void *user_data)
{
    clear_items();
    menu.title = g_strdup(title);
    menu.subtitle = g_strdup(subtitle);
    for (int i = 0; items[i] && i < MAX_ITEMS; i++)
        menu.items[menu.n_items++] = g_strdup(items[i]);
    menu.selected = 0;
    menu.activate = activate;
    menu.user_data = user_data;
    menu.hold_dir = 0;
    menu.visible = true;
}

void menu_close(void)
{
    menu.visible = false;
}

bool menu_visible(void)
{
    return menu.visible;
}

static void move(int dir)
{
    menu.selected = (menu.selected + dir + menu.n_items) % menu.n_items;
}

static void start_hold(int dir)
{
    move(dir);
    menu.hold_dir = dir;
    menu.hold_since = menu.last_repeat = SDL_GetTicks();
}

/* Closed without choosing (B / SELECT): the callback gets index -1. */
static void dismiss(void)
{
    menu.visible = false;
    if (menu.activate)
        menu.activate(-1, menu.user_data);
}

static void activate_selected(void)
{
    int index = menu.selected;
    menu.visible = false;
    if (menu.activate)
        menu.activate(index, menu.user_data);
}

bool menu_handle_event(const SDL_Event *ev)
{
    if (!menu.visible)
        return false;
    switch (ev->type) {
    case SDL_JOYBUTTONDOWN:
        switch (ev->jbutton.button) {
        case BTN_A:
        case BTN_START:
            activate_selected();
            break;
        case BTN_B:
        case BTN_SELECT:
            dismiss();
            break;
        case BTN_MENU:
            return false;
        }
        return true;
    case SDL_JOYBUTTONUP:
        return ev->jbutton.button != BTN_MENU;
    case SDL_JOYHATMOTION:
        if (ev->jhat.value & SDL_HAT_UP)
            start_hold(-1);
        else if (ev->jhat.value & SDL_HAT_DOWN)
            start_hold(1);
        else
            menu.hold_dir = 0;
        return true;
    case SDL_JOYAXISMOTION:
        if (ev->jaxis.axis == AXIS_LY) {
            int dir = ev->jaxis.value > AXIS_THRESHOLD ? 1 : ev->jaxis.value < -AXIS_THRESHOLD ? -1 : 0;
            if (dir != menu.axis_dir) {
                menu.axis_dir = dir;
                if (dir)
                    start_hold(dir);
                else
                    menu.hold_dir = 0;
            }
        }
        return true;
    }
    return false;
}

bool menu_tick(void)
{
    if (!menu.visible || !menu.hold_dir)
        return false;
    Uint32 now = SDL_GetTicks();
    if (now - menu.hold_since < REPEAT_DELAY_MS || now - menu.last_repeat < REPEAT_RATE_MS)
        return false;
    menu.last_repeat = now;
    move(menu.hold_dir);
    return true;
}

static void draw_text(TTF_Font *font, const char *text, SDL_Color color, int x, int y, int max_w)
{
    if (!text || !*text)
        return;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface)
        return;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(menu.renderer, surface);
    SDL_Rect src = { 0, 0, MIN(surface->w, max_w), surface->h };
    SDL_Rect dst = { x, y, src.w, src.h };
    SDL_RenderCopy(menu.renderer, texture, &src, &dst);
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

void menu_draw(void)
{
    if (!menu.visible)
        return;
    const SDL_Color text = { 232, 234, 237, 255 };
    const SDL_Color dim = { 154, 160, 166, 255 };
    const SDL_Color accent = { 138, 180, 248, 255 };
    const SDL_Color selected_text = { 32, 33, 36, 255 };

    /* Dim the page */
    SDL_SetRenderDrawBlendMode(menu.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(menu.renderer, 0, 0, 0, 140);
    SDL_RenderFillRect(menu.renderer, NULL);
    SDL_SetRenderDrawBlendMode(menu.renderer, SDL_BLENDMODE_NONE);

    int h = HEADER_H + menu.n_items * ITEM_H + 12;
    SDL_Rect box = { (menu.screen_w - MENU_W) / 2, (menu.screen_h - h) / 2, MENU_W, h };
    SDL_SetRenderDrawColor(menu.renderer, 32, 33, 36, 255);
    SDL_RenderFillRect(menu.renderer, &box);
    SDL_SetRenderDrawColor(menu.renderer, 60, 64, 67, 255);
    SDL_RenderDrawRect(menu.renderer, &box);

    draw_text(menu.font, menu.title, accent, box.x + 20, box.y + 10, MENU_W - 40);
    draw_text(menu.font_small, menu.subtitle, dim, box.x + 20, box.y + 46, MENU_W - 40);

    for (int i = 0; i < menu.n_items; i++) {
        SDL_Rect item = { box.x + 6, box.y + HEADER_H + i * ITEM_H, MENU_W - 12, ITEM_H - 4 };
        bool selected = i == menu.selected;
        if (selected) {
            SDL_SetRenderDrawColor(menu.renderer, accent.r, accent.g, accent.b, 255);
            SDL_RenderFillRect(menu.renderer, &item);
        }
        draw_text(menu.font, menu.items[i], selected ? selected_text : text,
                  item.x + 18, item.y + (item.h - TTF_FontHeight(menu.font)) / 2, item.w - 36);
    }
}

void menu_draw_status(const char *text, double progress, int bottom)
{
    const SDL_Color color = { 232, 234, 237, 255 };
    const int h = 40;
    SDL_Rect strip = { 0, bottom - h, menu.screen_w, h };
    /* opaque: page text behind it (e.g. the wpe-tsp:// pages' hint bar) would show through */
    SDL_SetRenderDrawColor(menu.renderer, 32, 33, 36, 255);
    SDL_RenderFillRect(menu.renderer, &strip);
    if (progress >= 0) {
        SDL_SetRenderDrawColor(menu.renderer, 138, 180, 248, 255);
        SDL_RenderFillRect(menu.renderer, &(SDL_Rect){ 0, bottom - h, (int)(menu.screen_w * progress), 3 });
    }
    draw_text(menu.font_small, text, color, 16, bottom - h + (h - TTF_FontHeight(menu.font_small)) / 2, menu.screen_w - 32);
}
