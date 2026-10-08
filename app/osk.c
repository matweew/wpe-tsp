/*
 * On-screen keyboard: Android-landscape-style panel docked at the bottom of the screen.
 * See osk.h. Letter rows come from the browser's XKB keyboard layouts (osk_add_language).
 */
#include "osk.h"

#include "gamepad.h"

#include <SDL_ttf.h>
#include <glib.h>
#include <math.h>
#include <string.h>


#define AXIS_THRESHOLD 16000
#define REPEAT_DELAY_MS 350
#define REPEAT_RATE_MS 70

/* Sizes in Smart Pro pixels, multiplied by the device's UI scale (S()) */
#define BAR_H S(46)
#define ROW_H S(56)
#define PAD S(6)
#define NUM_ROWS 4

#define MAX_KEYS 16
#define MAX_LANGS 16
#define KEY_STR 24

#define LABEL_BACKSPACE "\xe2\x8c\xab" /* ⌫ */
#define LABEL_SHIFT     "\xe2\x87\xa7" /* ⇧ */
#define LABEL_ENTER     "\xe2\x86\xb5" /* ↵ */
#define LABEL_BULLET    "\xe2\x80\xa2" /* • */
#define LABEL_CLEAR     "\xe2\x9c\x95" /* ✕ */

/* sel_row value for the ✕ (clear) button in the URL bar */
#define SEL_ROW_CLEAR (-1)
#define CLEAR_W S(64)

typedef enum { K_CHAR, K_SHIFT, K_BKSP, K_ENTER, K_SPACE, K_SYM, K_SYM2, K_ABC, K_LANG, K_PASTE } KeyKind;

typedef struct {
    KeyKind kind;
    char label[KEY_STR];
    char out[KEY_STR];
    float w;            /* width in key units; a full row is 10 units */
} Key;

typedef struct {
    Key keys[MAX_KEYS];
    int n;
} Row;

typedef struct {
    char name[8];
    char letters[3][MAX_KEYS][8];   /* uppercase, from the XKB layout (osk_add_language) */
    int n[3];
} Lang;

typedef enum { PAGE_LETTERS, PAGE_SYM, PAGE_SYM2 } PageKind;
typedef enum { SHIFT_OFF, SHIFT_ONCE, SHIFT_LOCK } ShiftState;
typedef enum { HOLD_NONE, HOLD_UP, HOLD_DOWN, HOLD_LEFT, HOLD_RIGHT, HOLD_PRESS, HOLD_BKSP, HOLD_SPACE } HoldAction;

typedef struct {
    SDL_Texture *texture;
    int w, h;
} Glyph;

static struct {
    SDL_Renderer *renderer;
    int screen_w, screen_h;
    TTF_Font *font_key;
    TTF_Font *font_bar;
    GHashTable *glyphs; /* "<font><color>text" -> Glyph* */
    OskCallbacks cb;

    Lang langs[MAX_LANGS];
    int n_langs;
    bool builtin_en;    /* langs[0] is the built-in English fallback, replaced by the first added */
    int lang;

    bool visible;
    OskMode mode;
    OskField field;
    PageKind page;
    ShiftState shift;
    Row rows[NUM_ROWS];
    int sel_row, sel_col;

    GString *line;          /* URL mode edit line */
    bool line_selected;     /* URL mode: whole line selected, the next key replaces it */
    gsize caret;            /* URL mode: cursor position in osk.line, in bytes */
    char *field_text;       /* form mode: text of the page field */
    char *search_name;      /* e.g. "Google", for the URL bar placeholder */

    HoldAction hold;
    Uint32 hold_since, last_repeat;
    float scale;            /* UI scale: same physical size on every panel (1 = Smart Pro) */
    int axis_dir[2];
} osk;

static int S(int px)
{
    return (int)lroundf(px * osk.scale);
}

/* ------------------------------------------------------------------------- */
/* Layouts                                                                   */
/* ------------------------------------------------------------------------- */

static void lang_add_row(Lang *lang, int row, const char *line)
{
    gchar **tokens = g_strsplit_set(line, " \t", -1);
    for (int i = 0; tokens[i] && lang->n[row] < MAX_KEYS; i++) {
        if (!*tokens[i])
            continue;
        g_strlcpy(lang->letters[row][lang->n[row]++], tokens[i], sizeof(lang->letters[0][0]));
    }
    g_strfreev(tokens);
}

static void load_builtin_en(Lang *lang)
{
    memset(lang, 0, sizeof(*lang));
    g_strlcpy(lang->name, "EN", sizeof(lang->name));
    lang_add_row(lang, 0, "Q W E R T Y U I O P");
    lang_add_row(lang, 1, "A S D F G H J K L");
    lang_add_row(lang, 2, "Z X C V B N M");
}

static void row_add(Row *row, KeyKind kind, const char *label, const char *out, float w)
{
    if (row->n >= MAX_KEYS)
        return;
    Key *k = &row->keys[row->n++];
    k->kind = kind;
    g_strlcpy(k->label, label, sizeof(k->label));
    g_strlcpy(k->out, out ? out : label, sizeof(k->out));
    k->w = w;
}

static void row_add_chars(Row *row, const char *chars)
{
    gchar **tokens = g_strsplit(chars, " ", -1);
    for (int i = 0; tokens[i]; i++)
        if (*tokens[i])
            row_add(row, K_CHAR, tokens[i], NULL, 1);
    g_strfreev(tokens);
}

static void add_letter(Row *row, const char *letter)
{
    gchar *s = osk.shift != SHIFT_OFF ? g_utf8_strup(letter, -1) : g_utf8_strdown(letter, -1);
    row_add(row, K_CHAR, s, NULL, 1);
    g_free(s);
}

static void build_rows(void)
{
    memset(osk.rows, 0, sizeof(osk.rows));
    Row *r = osk.rows;
    bool url = osk.mode == OSK_MODE_URL;

    switch (osk.page) {
    case PAGE_LETTERS: {
        const Lang *lang = &osk.langs[osk.lang];
        for (int i = 0; i < lang->n[0]; i++)
            add_letter(&r[0], lang->letters[0][i]);
        for (int i = 0; i < lang->n[1]; i++)
            add_letter(&r[1], lang->letters[1][i]);
        row_add(&r[2], K_SHIFT, LABEL_SHIFT, NULL, 1.5f);
        for (int i = 0; i < lang->n[2]; i++)
            add_letter(&r[2], lang->letters[2][i]);
        row_add(&r[2], K_BKSP, LABEL_BACKSPACE, NULL, 1.5f);
        if (url) {
            row_add(&r[3], K_SYM, "?123", NULL, 1.5f);
            if (osk.n_langs > 1)
                row_add(&r[3], K_LANG, lang->name, NULL, 1);
            row_add(&r[3], K_CHAR, "/", NULL, 1);
            row_add(&r[3], K_CHAR, ".com", NULL, 1.5f);
            row_add(&r[3], K_SPACE, "", " ", osk.n_langs > 1 ? 1.5f : 2.5f);
            row_add(&r[3], K_PASTE, "Paste", NULL, 1);
            row_add(&r[3], K_CHAR, ".", NULL, 1);
            row_add(&r[3], K_ENTER, LABEL_ENTER, NULL, 1.5f);
        } else {
            row_add(&r[3], K_SYM, "?123", NULL, 1.5f);
            if (osk.n_langs > 1)
                row_add(&r[3], K_LANG, lang->name, NULL, 1);
            row_add(&r[3], K_CHAR, osk.field == OSK_FIELD_EMAIL ? "@" : ",", NULL, 1);
            row_add(&r[3], K_SPACE, lang->name, " ", osk.n_langs > 1 ? 3 : 4);
            row_add(&r[3], K_PASTE, "Paste", NULL, 1);
            row_add(&r[3], K_CHAR, ".", NULL, 1);
            row_add(&r[3], K_ENTER, LABEL_ENTER, NULL, 1.5f);
        }
        break;
    }
    case PAGE_SYM:
        row_add_chars(&r[0], "1 2 3 4 5 6 7 8 9 0");
        row_add_chars(&r[1], "@ # $ _ & - + ( ) /");
        row_add(&r[2], K_SYM2, "=\\<", NULL, 1.5f);
        row_add_chars(&r[2], url ? ": ; ? = % ~ !" : "* \" ' : ; ! ?");
        row_add(&r[2], K_BKSP, LABEL_BACKSPACE, NULL, 1.5f);
        break;
    case PAGE_SYM2:
        row_add_chars(&r[0], "~ ` | " LABEL_BULLET " \xe2\x88\x9a \xcf\x80 \xc3\xb7 \xc3\x97 \xc2\xb6 \xe2\x88\x86"); /* √ π ÷ × ¶ ∆ */
        row_add_chars(&r[1], "\xc2\xa3 \xc2\xa2 \xe2\x82\xac \xc2\xa5 ^ \xc2\xb0 = { } \\"); /* £ ¢ € ¥ ^ ° = { } \ */
        row_add(&r[2], K_SYM, "?123", NULL, 1.5f);
        row_add_chars(&r[2], "% \xc2\xa9 \xc2\xae \xe2\x84\xa2 [ ] < >"); /* % © ® ™ [ ] < > */
        row_add(&r[2], K_BKSP, LABEL_BACKSPACE, NULL, 1.5f);
        break;
    }

    if (osk.page != PAGE_LETTERS) {
        row_add(&r[3], K_ABC, "ABC", NULL, 1.5f);
        if (url) {
            row_add(&r[3], K_CHAR, "https://", NULL, 2);
            row_add(&r[3], K_CHAR, "www.", NULL, 1.5f);
            row_add(&r[3], K_SPACE, "", " ", 1);
        } else {
            row_add(&r[3], K_CHAR, ",", NULL, 1);
            row_add(&r[3], K_SPACE, "", " ", 4);
        }
        row_add(&r[3], K_PASTE, "Paste", NULL, 1);
        row_add(&r[3], K_CHAR, ".", NULL, 1);
        row_add(&r[3], K_ENTER, LABEL_ENTER, NULL, url ? 2 : 1.5f);
    }

    if (osk.sel_row == SEL_ROW_CLEAR && osk.mode == OSK_MODE_URL)
        return;
    osk.sel_row = CLAMP(osk.sel_row, 0, NUM_ROWS - 1);
    osk.sel_col = CLAMP(osk.sel_col, 0, osk.rows[osk.sel_row].n - 1);
}


/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------- */

int osk_height(void)
{
    return BAR_H + NUM_ROWS * ROW_H + (NUM_ROWS + 1) * PAD;
}

static float row_units(const Row *row)
{
    float u = 0;
    for (int i = 0; i < row->n; i++)
        u += row->keys[i].w;
    return u;
}

static SDL_Rect key_rect(int row_index, int col)
{
    const Row *row = &osk.rows[row_index];
    float units = row_units(row);
    float unit = (osk.screen_w - 2 * PAD) / MAX(10.0f, units);
    float x = (osk.screen_w - units * unit) / 2; /* center short rows */
    for (int i = 0; i < col; i++)
        x += row->keys[i].w * unit;
    SDL_Rect rect;
    rect.x = (int)x + PAD / 2;
    rect.y = osk.screen_h - osk_height() + BAR_H + PAD + row_index * (ROW_H + PAD);
    rect.w = (int)(row->keys[col].w * unit) - PAD;
    rect.h = ROW_H;
    return rect;
}

/* The ✕ button at the right end of the URL bar. */
static SDL_Rect clear_rect(void)
{
    int top = osk.screen_h - osk_height();
    return (SDL_Rect){ osk.screen_w - S(12) - CLEAR_W, top + S(5), CLEAR_W, BAR_H - S(10) };
}

/* ------------------------------------------------------------------------- */
/* Text rendering                                                            */
/* ------------------------------------------------------------------------- */

static void glyph_free(gpointer data)
{
    Glyph *g = data;
    SDL_DestroyTexture(g->texture);
    g_free(g);
}

static Glyph *glyph_get(TTF_Font *font, const char *text, SDL_Color color)
{
    if (!text || !*text)
        return NULL;
    gchar *key = g_strdup_printf("%p%02x%02x%02x%s", (void *)font, color.r, color.g, color.b, text);
    Glyph *g = g_hash_table_lookup(osk.glyphs, key);
    if (g) {
        g_free(key);
        return g;
    }
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) {
        g_free(key);
        return NULL;
    }
    g = g_new0(Glyph, 1);
    g->texture = SDL_CreateTextureFromSurface(osk.renderer, surface);
    g->w = surface->w;
    g->h = surface->h;
    SDL_FreeSurface(surface);
    g_hash_table_insert(osk.glyphs, key, g);
    return g;
}

/* Uncached (for the frequently changing edit line). */
static void draw_text_once(TTF_Font *font, const char *text, SDL_Color color, int x, int y, int max_w)
{
    if (!text || !*text)
        return;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface)
        return;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(osk.renderer, surface);
    SDL_Rect src = { 0, 0, surface->w, surface->h };
    if (src.w > max_w) { /* keep the end of the line (where the caret is) visible */
        src.x = src.w - max_w;
        src.w = max_w;
    }
    SDL_Rect dst = { x, y, src.w, src.h };
    SDL_RenderCopy(osk.renderer, texture, &src, &dst);
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

/* Edit line with the caret at byte offset `caret`; scrolls horizontally to keep it visible. */
static void draw_line_with_caret(TTF_Font *font, const char *text, gsize caret, SDL_Color color,
                                 int x, int y, int max_w)
{
    int caret_x = 0, h = TTF_FontHeight(font);
    if (caret > 0) {
        gchar *prefix = g_strndup(text, caret);
        TTF_SizeUTF8(font, prefix, &caret_x, NULL);
        g_free(prefix);
    }
    int offset = MAX(0, caret_x - (max_w - S(12))); /* scroll so the caret stays inside */
    if (*text) {
        SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
        if (surface) {
            SDL_Texture *texture = SDL_CreateTextureFromSurface(osk.renderer, surface);
            SDL_Rect src = { offset, 0, MIN(surface->w - offset, max_w), surface->h };
            if (src.w > 0)
                SDL_RenderCopy(osk.renderer, texture, &src, &(SDL_Rect){ x, y, src.w, src.h });
            SDL_DestroyTexture(texture);
            SDL_FreeSurface(surface);
        }
    }
    SDL_SetRenderDrawColor(osk.renderer, color.r, color.g, color.b, 255);
    SDL_RenderFillRect(osk.renderer, &(SDL_Rect){ x + caret_x - offset, y + S(3), S(2), h - S(6) });
}

/* ------------------------------------------------------------------------- */
/* Actions                                                                   */
/* ------------------------------------------------------------------------- */

static void clear_line(void)
{
    g_string_truncate(osk.line, 0);
    osk.caret = 0;
    osk.line_selected = false;
}

/* ✕ selected: d-pad left/right moves the text cursor (one character, UTF-8 aware). */
static void move_caret(int dir)
{
    if (osk.line_selected) {
        /* First move drops the selection, like a desktop text field */
        osk.line_selected = false;
        osk.caret = dir < 0 ? 0 : osk.line->len;
        return;
    }
    const char *str = osk.line->str;
    if (dir < 0 && osk.caret > 0) {
        const char *prev = g_utf8_find_prev_char(str, str + osk.caret);
        osk.caret = prev ? (gsize)(prev - str) : 0;
    } else if (dir > 0 && osk.caret < osk.line->len) {
        osk.caret = (gsize)(g_utf8_next_char(str + osk.caret) - str);
    }
}

static void type_text(const char *text)
{
    if (osk.mode == OSK_MODE_URL) {
        if (osk.line_selected) /* typing replaces the selected URL */
            clear_line();
        g_string_insert(osk.line, (gssize)osk.caret, text);
        osk.caret += strlen(text);
    }
    else if (osk.cb.commit)
        osk.cb.commit(text, osk.cb.user_data);
}

/* Paste key: the browser's clipboard (text copied from pages). The address bar is one line. */
static void paste(void)
{
    char *text = osk.cb.paste_text ? osk.cb.paste_text(osk.cb.user_data) : NULL;
    if (text && osk.mode == OSK_MODE_URL) {
        g_strdelimit(text, "\r\n\t", ' ');
        g_strstrip(text);
    }
    if (text && *text)
        type_text(text);
    g_free(text);
}

static void backspace(void)
{
    if (osk.mode == OSK_MODE_URL) {
        if (osk.line_selected) {
            clear_line();
            return;
        }
        if (osk.caret > 0) {
            const char *prev = g_utf8_find_prev_char(osk.line->str, osk.line->str + osk.caret);
            gsize start = prev ? (gsize)(prev - osk.line->str) : 0;
            g_string_erase(osk.line, (gssize)start, (gssize)(osk.caret - start));
            osk.caret = start;
        }
    } else if (osk.cb.backspace)
        osk.cb.backspace(osk.cb.user_data);
}

static void enter(void)
{
    if (osk.mode == OSK_MODE_URL) {
        gchar *line = g_strdup(g_strstrip(osk.line->str));
        osk_hide();
        if (osk.cb.enter && *line)
            osk.cb.enter(line, osk.cb.user_data);
        g_free(line);
    } else if (osk.cb.enter)
        osk.cb.enter(NULL, osk.cb.user_data);
}

static void set_page(PageKind page)
{
    osk.page = page;
    build_rows();
}

static void toggle_shift(void)
{
    osk.shift = osk.shift == SHIFT_OFF ? SHIFT_ONCE : osk.shift == SHIFT_ONCE ? SHIFT_LOCK : SHIFT_OFF;
    if (osk.page == PAGE_LETTERS)
        build_rows();
}

static void next_lang(void)
{
    if (osk.n_langs < 2)
        return;
    osk.lang = (osk.lang + 1) % osk.n_langs;
    osk.page = PAGE_LETTERS;
    build_rows();
}

static void press_selected(void)
{
    if (osk.sel_row == SEL_ROW_CLEAR) {
        clear_line();
        return;
    }
    const Key *k = &osk.rows[osk.sel_row].keys[osk.sel_col];
    switch (k->kind) {
    case K_CHAR:
        type_text(k->out);
        if (osk.shift == SHIFT_ONCE && osk.page == PAGE_LETTERS) {
            osk.shift = SHIFT_OFF;
            build_rows();
        }
        break;
    case K_SPACE: type_text(" "); break;
    case K_BKSP: backspace(); break;
    case K_ENTER: enter(); break;
    case K_SHIFT: toggle_shift(); break;
    case K_SYM: set_page(PAGE_SYM); break;
    case K_SYM2: set_page(PAGE_SYM2); break;
    case K_ABC: set_page(PAGE_LETTERS); break;
    case K_LANG: next_lang(); break;
    case K_PASTE: paste(); break;
    }
}

static void move_horizontal(int dir)
{
    if (osk.sel_row == SEL_ROW_CLEAR) {
        move_caret(dir);
        return;
    }
    int n = osk.rows[osk.sel_row].n;
    osk.sel_col = (osk.sel_col + dir + n) % n;
}

/* Move to the key in the next row whose center is closest to the current key's center. */
static void move_vertical(int dir)
{
    bool has_clear = osk.mode == OSK_MODE_URL;
    SDL_Rect cur = osk.sel_row == SEL_ROW_CLEAR ? clear_rect() : key_rect(osk.sel_row, osk.sel_col);
    int cx = cur.x + cur.w / 2;
    int row;
    if (osk.sel_row == SEL_ROW_CLEAR)
        row = dir > 0 ? 0 : NUM_ROWS - 1;
    else if (has_clear && ((osk.sel_row == 0 && dir < 0) || (osk.sel_row == NUM_ROWS - 1 && dir > 0))) {
        osk.sel_row = SEL_ROW_CLEAR; /* the ✕ sits above the top row (and wraps from the bottom) */
        return;
    } else
        row = (osk.sel_row + dir + NUM_ROWS) % NUM_ROWS;
    int best = 0, best_dist = G_MAXINT;
    for (int i = 0; i < osk.rows[row].n; i++) {
        SDL_Rect r = key_rect(row, i);
        int dist = abs(r.x + r.w / 2 - cx);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    osk.sel_row = row;
    osk.sel_col = best;
}

static void do_hold_action(HoldAction action)
{
    switch (action) {
    case HOLD_UP: move_vertical(-1); break;
    case HOLD_DOWN: move_vertical(1); break;
    case HOLD_LEFT: move_horizontal(-1); break;
    case HOLD_RIGHT: move_horizontal(1); break;
    case HOLD_PRESS: press_selected(); break;
    case HOLD_BKSP: backspace(); break;
    case HOLD_SPACE:
        if (osk.mode == OSK_MODE_URL && osk.line_selected)
            osk.line_selected = false; /* Y on a selected URL: keep it and edit at the end */
        else
            type_text(" ");
        break;
    case HOLD_NONE: break;
    }
}

static void start_hold(HoldAction action)
{
    do_hold_action(action);
    osk.hold = action;
    osk.hold_since = osk.last_repeat = SDL_GetTicks();
}

static void stop_hold(HoldAction action)
{
    if (osk.hold == action)
        osk.hold = HOLD_NONE;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

bool osk_init(SDL_Renderer *renderer, int screen_w, int screen_h,
              const char *font_path, const OskCallbacks *callbacks, float ui_scale)
{
    memset(&osk, 0, sizeof(osk));
    osk.scale = ui_scale > 0 ? ui_scale : 1;
    osk.renderer = renderer;
    osk.screen_w = screen_w;
    osk.screen_h = screen_h;
    osk.cb = *callbacks;
    if (!TTF_WasInit() && TTF_Init() < 0) {
        g_warning("TTF_Init failed: %s", TTF_GetError());
        return false;
    }
    osk.font_key = TTF_OpenFont(font_path, S(26));
    osk.font_bar = TTF_OpenFont(font_path, S(24));
    if (!osk.font_key || !osk.font_bar) {
        g_warning("Failed to open font %s: %s", font_path, TTF_GetError());
        return false;
    }
    osk.glyphs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, glyph_free);
    osk.line = g_string_new(NULL);
    load_builtin_en(&osk.langs[0]); /* until the browser adds its keyboard layouts */
    osk.n_langs = 1;
    osk.builtin_en = true;
    return true;
}

void osk_shutdown(void)
{
    if (osk.glyphs)
        g_hash_table_destroy(osk.glyphs);
    if (osk.font_key)
        TTF_CloseFont(osk.font_key);
    if (osk.font_bar)
        TTF_CloseFont(osk.font_bar);
    if (osk.line)
        g_string_free(osk.line, TRUE);
    g_free(osk.field_text);
    g_free(osk.search_name);
    memset(&osk, 0, sizeof(osk));
}

void osk_set_renderer(SDL_Renderer *renderer)
{
    g_hash_table_remove_all(osk.glyphs); /* cached textures belong to the old renderer */
    osk.renderer = renderer;
}

void osk_set_screen_size(int screen_w, int screen_h)
{
    osk.screen_w = screen_w;
    osk.screen_h = screen_h;
}

void osk_show(OskMode mode, OskField field, const char *initial_text)
{
    osk.visible = true;
    osk.mode = mode;
    osk.field = field;
    osk.shift = SHIFT_OFF;
    osk.hold = HOLD_NONE;
    osk.page = field == OSK_FIELD_NUMBER ? PAGE_SYM : PAGE_LETTERS;
    if (mode == OSK_MODE_URL)
        osk.lang = 0;
    g_string_assign(osk.line, initial_text ? initial_text : "");
    osk.caret = osk.line->len;
    osk.line_selected = mode == OSK_MODE_URL && osk.line->len > 0;
    osk.sel_row = 0;
    osk.sel_col = 0;
    build_rows();
}

void osk_hide(void)
{
    osk.visible = false;
    osk.hold = HOLD_NONE;
}

bool osk_visible(void)
{
    return osk.visible;
}

OskMode osk_mode(void)
{
    return osk.mode;
}

void osk_add_language(const char *name, const char *const rows[3])
{
    int index = osk.builtin_en ? 0 : osk.n_langs;
    if (index >= MAX_LANGS)
        return;
    Lang *lang = &osk.langs[index];
    memset(lang, 0, sizeof(*lang));
    g_strlcpy(lang->name, name, sizeof(lang->name));
    for (int row = 0; row < 3; row++)
        lang_add_row(lang, row, rows[row] ? rows[row] : "");
    if (!lang->n[0] || !lang->n[1]) { /* no letters (e.g. a broken layout): keep what we had */
        if (osk.builtin_en)
            load_builtin_en(lang);
        return;
    }
    osk.builtin_en = false;
    osk.n_langs = index + 1;
}

void osk_select_language(int index)
{
    if (index < 0 || index >= osk.n_langs || index == osk.lang)
        return;
    osk.lang = index;
    osk.page = PAGE_LETTERS;
    build_rows();
}

void osk_set_search_name(const char *name)
{
    g_free(osk.search_name);
    osk.search_name = g_strdup(name);
}

void osk_set_field_text(const char *text)
{
    g_free(osk.field_text);
    osk.field_text = g_strdup(text);
}

bool osk_physical_key(unsigned keyval, const char *text)
{
    if (!osk.visible || osk.mode != OSK_MODE_URL)
        return false;
    switch (keyval) {
    case 0xff08: /* BackSpace */
        backspace();
        return true;
    case 0xffff: /* Delete: the character after the caret */
        if (osk.line_selected)
            clear_line();
        else if (osk.caret < osk.line->len) {
            gsize next = (gsize)(g_utf8_next_char(osk.line->str + osk.caret) - osk.line->str);
            g_string_erase(osk.line, (gssize)osk.caret, (gssize)(next - osk.caret));
        }
        return true;
    case 0xff51: move_caret(-1); return true;   /* Left */
    case 0xff53: move_caret(1); return true;    /* Right */
    case 0xff50:                                /* Home */
    case 0xff57:                                /* End */
        osk.line_selected = false;
        osk.caret = keyval == 0xff50 ? 0 : osk.line->len;
        return true;
    case 0xff0d: /* Return */
    case 0xff8d: /* KP_Enter */
        enter();
        return true;
    case 0xff1b: /* Escape */
        osk_hide();
        if (osk.cb.closed)
            osk.cb.closed(osk.cb.user_data);
        return true;
    }
    if (text && *text && (unsigned char)text[0] >= 0x20 && text[0] != 0x7f) {
        type_text(text);
        return true;
    }
    return false;
}

bool osk_handle_event(const SDL_Event *ev)
{
    if (!osk.visible)
        return false;

    switch (ev->type) {
    case SDL_JOYBUTTONDOWN:
        switch (ev->jbutton.button) {
        case BTN_A: start_hold(HOLD_PRESS); return true;
        case BTN_X: start_hold(HOLD_BKSP); return true;
        case BTN_Y: start_hold(HOLD_SPACE); return true;
        case BTN_B:
            osk_hide();
            if (osk.cb.closed)
                osk.cb.closed(osk.cb.user_data);
            return true;
        case BTN_L1: toggle_shift(); return true;
        case BTN_R1: next_lang(); return true;
        case BTN_SELECT: set_page(osk.page == PAGE_LETTERS ? PAGE_SYM : PAGE_LETTERS); return true;
        case BTN_START: enter(); return true;
        }
        return false; /* MENU stays with the system */
    case SDL_JOYBUTTONUP:
        switch (ev->jbutton.button) {
        case BTN_A: stop_hold(HOLD_PRESS); return true;
        case BTN_X: stop_hold(HOLD_BKSP); return true;
        case BTN_Y: stop_hold(HOLD_SPACE); return true;
        }
        return ev->jbutton.button != BTN_MENU;
    case SDL_JOYHATMOTION: {
        Uint8 v = ev->jhat.value;
        if (v & SDL_HAT_UP) start_hold(HOLD_UP);
        else if (v & SDL_HAT_DOWN) start_hold(HOLD_DOWN);
        else if (v & SDL_HAT_LEFT) start_hold(HOLD_LEFT);
        else if (v & SDL_HAT_RIGHT) start_hold(HOLD_RIGHT);
        else if (osk.hold >= HOLD_UP && osk.hold <= HOLD_RIGHT) osk.hold = HOLD_NONE;
        return true;
    }
    case SDL_JOYAXISMOTION: {
        int axis = ev->jaxis.axis;
        if (axis != AXIS_LX && axis != AXIS_LY)
            return true;
        int dir = ev->jaxis.value > AXIS_THRESHOLD ? 1 : ev->jaxis.value < -AXIS_THRESHOLD ? -1 : 0;
        if (dir != osk.axis_dir[axis]) {
            osk.axis_dir[axis] = dir;
            if (dir)
                start_hold(axis == AXIS_LX ? (dir > 0 ? HOLD_RIGHT : HOLD_LEFT) : (dir > 0 ? HOLD_DOWN : HOLD_UP));
            else if (osk.hold >= HOLD_UP && osk.hold <= HOLD_RIGHT)
                osk.hold = HOLD_NONE;
        }
        return true;
    }
    }
    return false;
}

bool osk_tick(void)
{
    if (!osk.visible || osk.hold == HOLD_NONE)
        return false;
    Uint32 now = SDL_GetTicks();
    if (now - osk.hold_since < REPEAT_DELAY_MS || now - osk.last_repeat < REPEAT_RATE_MS)
        return false;
    osk.last_repeat = now;
    /* Don't auto-repeat mode switches (shift, pages, enter) */
    if (osk.hold == HOLD_PRESS) {
        if (osk.sel_row == SEL_ROW_CLEAR)
            return false;
        KeyKind kind = osk.rows[osk.sel_row].keys[osk.sel_col].kind;
        if (kind != K_CHAR && kind != K_SPACE && kind != K_BKSP)
            return false;
    }
    do_hold_action(osk.hold);
    return true;
}

static void fill_rect(SDL_Rect r, Uint8 cr, Uint8 cg, Uint8 cb)
{
    SDL_SetRenderDrawColor(osk.renderer, cr, cg, cb, 255);
    SDL_RenderFillRect(osk.renderer, &r);
}

void osk_draw(void)
{
    if (!osk.visible)
        return;
    const SDL_Color text_color = { 232, 234, 237, 255 };
    const SDL_Color dim_color = { 154, 160, 166, 255 };
    const SDL_Color accent_color = { 138, 180, 248, 255 };
    const SDL_Color selected_text = { 32, 33, 36, 255 };

    int top = osk.screen_h - osk_height();
    fill_rect((SDL_Rect){ 0, top, osk.screen_w, osk_height() }, 32, 33, 36);

    /* Preview bar */
    fill_rect((SDL_Rect){ 0, top, osk.screen_w, BAR_H }, 41, 42, 45);
    const char *tag = osk.mode == OSK_MODE_URL ? "Search or URL" : osk.langs[osk.lang].name;
    Glyph *g = glyph_get(osk.font_bar, tag, accent_color);
    int text_x = S(16);
    if (g) {
        SDL_RenderCopy(osk.renderer, g->texture, NULL, &(SDL_Rect){ S(16), top + (BAR_H - g->h) / 2, g->w, g->h });
        text_x += g->w + S(16);
    }
    const char *shown = osk.mode == OSK_MODE_URL ? osk.line->str : osk.field_text;
    gchar *masked = NULL;
    if (shown && osk.field == OSK_FIELD_PASSWORD) {
        GString *m = g_string_new(NULL);
        for (glong i = g_utf8_strlen(shown, -1); i > 0; i--)
            g_string_append(m, LABEL_BULLET);
        shown = masked = g_string_free(m, FALSE);
    }
    bool has_clear = osk.mode == OSK_MODE_URL;
    int text_max_w = osk.screen_w - text_x - S(16) - (has_clear ? CLEAR_W + S(12) : 0);
    int text_y = top + (BAR_H - TTF_FontHeight(osk.font_bar)) / 2;

    /* ✕ selected: explain what the d-pad does here (and keep the URL clear of the hint) */
    if (has_clear && osk.sel_row == SEL_ROW_CLEAR) {
        Glyph *hg = glyph_get(osk.font_bar, "\xe2\x97\x80 \xe2\x96\xb6 cursor   A clear", dim_color); /* ◀ ▶ */
        if (hg) {
            int hx = clear_rect().x - S(16) - hg->w;
            SDL_RenderCopy(osk.renderer, hg->texture, NULL, &(SDL_Rect){ hx, top + (BAR_H - hg->h) / 2, hg->w, hg->h });
            text_max_w = MIN(text_max_w, hx - S(16) - text_x);
        }
    }
    if (osk.mode == OSK_MODE_URL && !osk.line->len) {
        /* Empty address bar: caret + placeholder that says search works here */
        gchar *hint = g_strdup_printf("Search %s or type a URL", osk.search_name ? osk.search_name : "the web");
        draw_text_once(osk.font_bar, "|", text_color, text_x, text_y, text_max_w);
        draw_text_once(osk.font_bar, hint, dim_color, text_x + S(10), text_y, text_max_w - S(10));
        g_free(hint);
    } else if (osk.mode == OSK_MODE_URL && osk.line_selected) {
        /* Whole URL selected: highlight it, no caret (the next key replaces it) */
        int tw = 0, th = 0;
        TTF_SizeUTF8(osk.font_bar, shown, &tw, &th);
        fill_rect((SDL_Rect){ text_x - S(3), text_y, MIN(tw, text_max_w) + S(6), th }, 52, 94, 168);
        draw_text_once(osk.font_bar, shown, text_color, text_x, text_y, text_max_w);
    } else if (osk.mode == OSK_MODE_URL) {
        draw_line_with_caret(osk.font_bar, osk.line->str, osk.caret, text_color, text_x, text_y, text_max_w);
    } else {
        /* Form field preview: the page owns the caret, show the text with a trailing one */
        gchar *with_caret = g_strconcat(shown ? shown : "", "|", NULL);
        draw_text_once(osk.font_bar, with_caret, text_color, text_x, text_y, text_max_w);
        g_free(with_caret);
    }
    g_free(masked);

    if (has_clear) {
        SDL_Rect cr = clear_rect();
        bool sel = osk.sel_row == SEL_ROW_CLEAR;
        if (sel)
            fill_rect(cr, accent_color.r, accent_color.g, accent_color.b);
        else
            fill_rect(cr, 45, 48, 52);
        Glyph *xg = glyph_get(osk.font_key, LABEL_CLEAR, sel ? selected_text : text_color);
        if (xg)
            SDL_RenderCopy(osk.renderer, xg->texture, NULL,
                           &(SDL_Rect){ cr.x + (cr.w - xg->w) / 2, cr.y + (cr.h - xg->h) / 2, xg->w, xg->h });
    }

    /* Keys */
    for (int r = 0; r < NUM_ROWS; r++) {
        for (int c = 0; c < osk.rows[r].n; c++) {
            const Key *k = &osk.rows[r].keys[c];
            SDL_Rect rect = key_rect(r, c);
            bool selected = r == osk.sel_row && c == osk.sel_col;
            bool special = k->kind != K_CHAR && k->kind != K_SPACE;
            if (selected)
                fill_rect(rect, accent_color.r, accent_color.g, accent_color.b);
            else if (special)
                fill_rect(rect, 45, 48, 52);
            else
                fill_rect(rect, 60, 64, 67);

            SDL_Color color = selected ? selected_text : text_color;
            if (!selected && k->kind == K_SHIFT && osk.shift != SHIFT_OFF)
                color = accent_color;
            if (!selected && k->kind == K_SPACE)
                color = dim_color;
            const char *label = k->label;
            if (k->kind == K_ENTER && osk.mode == OSK_MODE_URL && osk.line->len) {
                /* Say what ENTER will do with the typed text */
                bool search = !osk.line_selected && osk.cb.is_search && osk.cb.is_search(osk.line->str, osk.cb.user_data);
                label = search ? "Search" : "Go";
            }
            Glyph *kg = glyph_get(osk.font_key, label, color);
            if (kg) {
                SDL_Rect dst = { rect.x + (rect.w - kg->w) / 2, rect.y + (rect.h - kg->h) / 2, kg->w, kg->h };
                SDL_RenderCopy(osk.renderer, kg->texture, NULL, &dst);
            }
            if (k->kind == K_SHIFT && osk.shift == SHIFT_LOCK) /* caps-lock underline */
                fill_rect((SDL_Rect){ rect.x + rect.w / 2 - S(12), rect.y + rect.h - S(10), S(24), S(3) },
                          color.r, color.g, color.b);
        }
    }
}
