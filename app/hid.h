/*
 * USB/Bluetooth keyboards and mice, read straight from evdev (/dev/input/event*): the device's
 * SDL finds input devices through udev, which isn't usable here, so SDL never reports them.
 * Devices are picked up when plugged in. The gamepad and the console's own buttons are left out.
 */
#pragma once

#include <glib.h>

/* Modifier state reported with keys */
#define HID_MOD_SHIFT     (1 << 0)
#define HID_MOD_CONTROL   (1 << 1)
#define HID_MOD_ALT       (1 << 2)
#define HID_MOD_META      (1 << 3)
#define HID_MOD_CAPS_LOCK (1 << 4)  /* toggled on */

typedef struct {
    /* keycode: XKB keycode (evdev code + 8); keysym: in the active layout (0: no layouts could be
     * compiled); value: 1 press, 0 release, 2 autorepeat; modifiers: HID_MOD_* in effect for
     * this key (modifier keys themselves included) */
    void (*key)(guint keycode, guint keysym, int value, guint modifiers, void *user_data);
    void (*motion)(int dx, int dy, void *user_data);
    /* button: 1 left, 2 middle, 3 right */
    void (*button)(guint button, gboolean pressed, void *user_data);
    /* wheel notches, positive = away from the user (up) / right */
    void (*wheel)(int dx, int dy, void *user_data);
    /* Alt+Shift / Super+Space switched the layout; name: XKB layout ("us", "ua", ...) */
    void (*layout_changed)(const char *name, void *user_data);
    void *user_data;
} HidCallbacks;

/* layouts: comma-separated XKB layouts/variants (KEYBOARD_LAYOUTS: "us,ua(phonetic)"), the first
 * is the default; keymaps_path: share/xkb-keymaps.bin (scripts/xkb-keymaps.py) */
void hid_init(const HidCallbacks *callbacks, const char *layouts, const char *keymaps_path);
const char *hid_layout_name(void);              /* the active one */
guint hid_layout_count(void);                   /* layouts that could be loaded */
guint hid_layout_index(void);                   /* the active one's index */
const char *hid_layout_name_at(guint index);
/* Letters of a layout's three main rows (row 0..2) for an on-screen keyboard: uppercase,
 * space-separated (newly allocated), or NULL */
char *hid_layout_letters(guint index, int row);
