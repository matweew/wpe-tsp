/*
 * Keyboards and mice via evdev, see hid.h. Devices are not grabbed: the system's keymon keeps
 * seeing them (volume keys). Keys are translated with one XKB keymap per layout (WPE's own
 * keymap is US only), taken from the precompiled share/xkb-keymaps.bin (scripts/xkb-keymaps.py).
 */
#include "hid.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#define INPUT_DIR "/dev/input"
#define BITS_LONG (sizeof(long) * 8)
#define TEST_BIT(bits, n) (((bits)[(n) / BITS_LONG] >> ((n) % BITS_LONG)) & 1)

typedef struct {
    char *path;
    int fd;
    guint source;
} Device;

typedef struct {
    char *name;                 /* XKB layout name: "us", "ua", ... */
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    xkb_mod_mask_t shift, lock;
} Layout;

static struct {
    HidCallbacks cb;
    GHashTable *devices;        /* path -> Device* */
    GFileMonitor *monitor;
    int rel_x, rel_y;           /* motion accumulated until SYN_REPORT */
    guint held;                 /* HID_MOD_* of modifier keys held, over all keyboards */
    gboolean caps_lock;
    GPtrArray *layouts;         /* of Layout* */
    guint layout;               /* active one */
} hid;

/* The keymap text of layout name ("ua", "ua(phonetic)") from the keymaps file, or NULL */
static char *keymap_text(GMappedFile *file, const char *name)
{
    const char *data = file ? g_mapped_file_get_contents(file) : NULL;
    gsize size = file ? g_mapped_file_get_length(file) : 0;
    const char *header = "XKBKEYMAPS 1\n";
    if (!data || size < strlen(header) || memcmp(data, header, strlen(header)))
        return NULL;
    const char *end = g_strstr_len(data, (gssize)size, "\n\n"); /* end of the index */
    if (!end)
        return NULL;
    gsize name_len = strlen(name), blobs = (gsize)(end + 2 - data);
    for (const char *line = data + strlen(header); line < end; line = strchr(line, '\n') + 1) {
        if (strncmp(line, name, name_len) || line[name_len] != '\t')
            continue;
        char *p;
        guint64 offset = g_ascii_strtoull(line + name_len + 1, &p, 10);
        guint64 length = g_ascii_strtoull(p + 1, NULL, 10);
        if (blobs + offset + length > size)
            return NULL;
        /* inflate: keymaps are ~65 KB of text */
        GConverter *zlib = G_CONVERTER(g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB));
        GString *out = g_string_new(NULL);
        const char *in = data + blobs + offset;
        gsize in_left = length;
        GConverterResult result;
        do {
            char buf[16384];
            gsize read = 0, written = 0;
            result = g_converter_convert(zlib, in, in_left, buf, sizeof(buf), G_CONVERTER_INPUT_AT_END,
                                         &read, &written, NULL);
            in += read;
            in_left -= read;
            g_string_append_len(out, buf, (gssize)written);
        } while (result == G_CONVERTER_CONVERTED);
        g_object_unref(zlib);
        if (result != G_CONVERTER_FINISHED) {
            g_string_free(out, TRUE);
            return NULL;
        }
        return g_string_free(out, FALSE);
    }
    return NULL;
}

static void load_layouts(const char *names, const char *keymaps_path)
{
    hid.layouts = g_ptr_array_new();
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    GMappedFile *file = g_mapped_file_new(keymaps_path, FALSE, NULL);
    if (!file)
        fprintf(stderr, "[wpe-tsp] input: %s missing, physical keyboards use WPE's US keymap\n", keymaps_path);
    char **list = g_strsplit_set(names && *names ? names : "us", ", ", -1);
    for (int i = 0; context && list[i]; i++) {
        char *name = g_ascii_strdown(g_strstrip(list[i]), -1);
        char *text = *name ? keymap_text(file, name) : NULL;
        struct xkb_keymap *keymap = text ? xkb_keymap_new_from_string(context, text, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                                      XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
        g_free(text);
        if (!keymap) {
            if (*name)
                fprintf(stderr, "[wpe-tsp] input: keyboard layout \"%s\" not available\n", name);
            g_free(name);
            continue;
        }
        Layout *l = g_new0(Layout, 1);
        l->name = name;
        l->keymap = keymap;
        l->state = xkb_state_new(keymap);
        l->shift = 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
        l->lock = 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CAPS);
        g_ptr_array_add(hid.layouts, l);
    }
    g_strfreev(list);
    if (file)
        g_mapped_file_unref(file);
    if (context)
        xkb_context_unref(context);
}

guint hid_layout_count(void)
{
    return hid.layouts ? hid.layouts->len : 0;
}

guint hid_layout_index(void)
{
    return hid.layout;
}

const char *hid_layout_name_at(guint index)
{
    return index < hid_layout_count() ? ((Layout *)g_ptr_array_index(hid.layouts, index))->name : NULL;
}

/* Letter keys of the three main rows (QWERTY's Q..], A..', Z../ positions), plus letters on the
 * keys around them (ґ is on the backslash key in "ua"), appended to the bottom row. */
char *hid_layout_letters(guint index, int row)
{
    static const guint rows[3][16] = {
        { KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE },
        { KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE },
        { KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH,
          KEY_BACKSLASH, KEY_102ND, KEY_GRAVE },
    };
    if (index >= hid_layout_count() || row < 0 || row > 2)
        return NULL;
    Layout *l = g_ptr_array_index(hid.layouts, index);
    xkb_state_update_mask(l->state, 0, 0, 0, 0, 0, 0);
    GString *out = g_string_new(NULL);
    for (int i = 0; i < 16 && rows[row][i]; i++) {
        gunichar c = g_unichar_toupper(xkb_keysym_to_utf32(xkb_state_key_get_one_sym(l->state, rows[row][i] + 8)));
        if (!g_unichar_isalpha(c))
            continue;
        char utf8[8];
        utf8[g_unichar_to_utf8(c, utf8)] = 0;
        if (out->len)
            g_string_append_c(out, ' ');
        g_string_append(out, utf8);
    }
    return g_string_free(out, FALSE);
}

static void next_layout(void)
{
    if (hid.layouts->len < 2)
        return;
    hid.layout = (hid.layout + 1) % hid.layouts->len;
    if (hid.cb.layout_changed)
        hid.cb.layout_changed(hid_layout_name(), hid.cb.user_data);
}

const char *hid_layout_name(void)
{
    return hid.layouts && hid.layouts->len ? ((Layout *)g_ptr_array_index(hid.layouts, hid.layout))->name : "us";
}

/* With Ctrl/Alt/Super held the first layout (Latin) is used, so shortcuts like Ctrl+C keep
 * working in any layout. */
static guint keysym_of(guint keycode)
{
    if (!hid.layouts->len)
        return 0;
    gboolean shortcut = hid.held & (HID_MOD_CONTROL | HID_MOD_ALT | HID_MOD_META);
    Layout *l = g_ptr_array_index(hid.layouts, shortcut ? 0 : hid.layout);
    xkb_state_update_mask(l->state, (hid.held & HID_MOD_SHIFT) ? l->shift : 0, 0, hid.caps_lock ? l->lock : 0, 0, 0, 0);
    return xkb_state_key_get_one_sym(l->state, keycode);
}

static guint modifier_of(guint code)
{
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return HID_MOD_SHIFT;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: return HID_MOD_CONTROL;
    case KEY_LEFTALT: case KEY_RIGHTALT: return HID_MOD_ALT;
    case KEY_LEFTMETA: case KEY_RIGHTMETA: return HID_MOD_META;
    default: return 0;
    }
}

static void key_event(guint code, int value)
{
    guint mod = modifier_of(code);
    if (mod) {
        hid.held = value ? hid.held | mod : hid.held & ~mod;
        /* Alt+Shift (either order) switches the layout, like on Windows */
        if (value == 1 && ((mod == HID_MOD_SHIFT && (hid.held & HID_MOD_ALT)) || (mod == HID_MOD_ALT && (hid.held & HID_MOD_SHIFT))))
            next_layout();
    } else if (code == KEY_CAPSLOCK && value == 1)
        hid.caps_lock = !hid.caps_lock;
    else if (code == KEY_SPACE && (hid.held & HID_MOD_META)) { /* Super+Space too */
        if (value == 1)
            next_layout();
        return;
    }
    if (hid.cb.key)
        hid.cb.key(code + 8, keysym_of(code + 8), value, hid.held | (hid.caps_lock ? HID_MOD_CAPS_LOCK : 0), hid.cb.user_data);
}

static void device_free(gpointer data)
{
    Device *d = data;
    if (d->source)
        g_source_remove(d->source);
    close(d->fd);
    g_free(d->path);
    g_free(d);
}

static gboolean on_device_readable(int fd, GIOCondition condition, gpointer user_data)
{
    Device *d = user_data;
    struct input_event ev[64];
    gboolean gone = (condition & (G_IO_ERR | G_IO_HUP)) != 0;
    ssize_t n = gone ? -1 : read(fd, ev, sizeof(ev));
    if (!gone && n < 0 && (errno == EAGAIN || errno == EINTR))
        return G_SOURCE_CONTINUE;
    if (n <= 0) { /* unplugged (read fails with ENODEV) */
        fprintf(stderr, "[wpe-tsp] input: %s removed\n", d->path);
        d->source = 0;
        g_hash_table_remove(hid.devices, d->path);
        return G_SOURCE_REMOVE;
    }
    for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
        const struct input_event *e = &ev[i];
        switch (e->type) {
        case EV_KEY:
            if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE) {
                if (e->value != 2 && hid.cb.button)
                    hid.cb.button(e->code == BTN_LEFT ? 1 : e->code == BTN_MIDDLE ? 2 : 3, e->value, hid.cb.user_data);
            } else if (e->code < BTN_MISC)
                key_event(e->code, e->value);
            break;
        case EV_REL:
            if (e->code == REL_X)
                hid.rel_x += e->value;
            else if (e->code == REL_Y)
                hid.rel_y += e->value;
            else if (e->code == REL_WHEEL && hid.cb.wheel)
                hid.cb.wheel(0, e->value, hid.cb.user_data);
            else if (e->code == REL_HWHEEL && hid.cb.wheel)
                hid.cb.wheel(e->value, 0, hid.cb.user_data);
            break;
        case EV_SYN:
            if ((hid.rel_x || hid.rel_y) && hid.cb.motion)
                hid.cb.motion(hid.rel_x, hid.rel_y, hid.cb.user_data);
            hid.rel_x = hid.rel_y = 0;
            break;
        }
    }
    return G_SOURCE_CONTINUE;
}

/* A keyboard has letter keys, a mouse relative X/Y: that leaves out the gamepad (absolute axes),
 * the power key, the headphone jack and the console's own buttons (sunxi-keyboard). */
static void try_open(const char *path)
{
    if (g_hash_table_contains(hid.devices, path))
        return;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    unsigned long key_bits[KEY_CNT / BITS_LONG + 1] = { 0 }, rel_bits[REL_CNT / BITS_LONG + 1] = { 0 };
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits);
    gboolean keyboard = TEST_BIT(key_bits, KEY_A) && TEST_BIT(key_bits, KEY_Z) && TEST_BIT(key_bits, KEY_SPACE);
    gboolean mouse = TEST_BIT(rel_bits, REL_X) && TEST_BIT(rel_bits, REL_Y) && TEST_BIT(key_bits, BTN_LEFT);
    if (!keyboard && !mouse) {
        close(fd);
        return;
    }
    char name[128] = "";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    fprintf(stderr, "[wpe-tsp] input: %s (%s)%s%s\n", path, g_strstrip(name),
            keyboard ? " keyboard" : "", mouse ? " mouse" : "");
    Device *d = g_new0(Device, 1);
    d->path = g_strdup(path);
    d->fd = fd;
    d->source = g_unix_fd_add(fd, G_IO_IN | G_IO_ERR | G_IO_HUP, on_device_readable, d);
    g_hash_table_insert(hid.devices, d->path, d);
}

/* udev creates the node first and sets its permissions a moment later: retry briefly */
static gboolean retry_open(gpointer path)
{
    try_open(path);
    return G_SOURCE_REMOVE;
}

static void on_dir_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer user_data)
{
    (void)monitor; (void)other; (void)user_data;
    if (event != G_FILE_MONITOR_EVENT_CREATED)
        return;
    char *path = g_file_get_path(file);
    char *base = g_path_get_basename(path);
    if (g_str_has_prefix(base, "event")) {
        try_open(path);
        g_timeout_add_full(G_PRIORITY_DEFAULT, 500, retry_open, g_strdup(path), g_free);
    }
    g_free(base);
    g_free(path);
}

void hid_init(const HidCallbacks *callbacks, const char *layouts, const char *keymaps_path)
{
    hid.cb = *callbacks;
    load_layouts(layouts, keymaps_path);
    hid.devices = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, device_free);
    GDir *dir = g_dir_open(INPUT_DIR, 0, NULL);
    for (const char *name; dir && (name = g_dir_read_name(dir)); ) {
        if (g_str_has_prefix(name, "event")) {
            char *path = g_build_filename(INPUT_DIR, name, NULL);
            try_open(path);
            g_free(path);
        }
    }
    if (dir)
        g_dir_close(dir);
    GFile *file = g_file_new_for_path(INPUT_DIR);
    hid.monitor = g_file_monitor_directory(file, G_FILE_MONITOR_NONE, NULL, NULL);
    if (hid.monitor)
        g_signal_connect(hid.monitor, "changed", G_CALLBACK(on_dir_changed), NULL);
    g_object_unref(file);
}
