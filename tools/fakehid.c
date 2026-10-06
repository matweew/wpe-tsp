/*
 * fakehid: a virtual USB keyboard + mouse (uinput) for testing the browser on the device.
 * Run it through scripts/device-input.sh, which builds it, copies it to the device and passes
 * the steps on. The device appears, the steps run in order, then it is unplugged again.
 *
 * Steps (one argument each):
 *   t:TEXT     type text: a-z A-Z 0-9 space . , / : - (US layout; uppercase and ':' with Shift)
 *   k:CODE     press and release a key: Linux KEY_* code (Enter 28, Esc 1, Down 108, ...)
 *   C:CODE     Ctrl + key (C:38 = Ctrl+L)
 *   M:MOD,KEY  modifier + key, both KEY_* codes (M:56,42 = Alt+Shift)
 *   m:DX,DY    move the mouse by DX,DY pixels (in 10 steps)
 *   c[:N]      click mouse button N (1 left, default; 2 middle; 3 right)
 *   w:N        mouse wheel, N notches (positive = up)
 *   s:MS       wait MS milliseconds
 *
 * Built static for aarch64: no dependence on the device's libraries.
 */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd;

static void emit(int type, int code, int value)
{
    struct input_event ev = { 0 };
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
        perror("write");
}

static void sync_report(void)
{
    emit(EV_SYN, SYN_REPORT, 0);
    usleep(15000); /* like a real keyboard: the browser polls every 16 ms */
}

static void press(int code)
{
    emit(EV_KEY, code, 1);
    sync_report();
    emit(EV_KEY, code, 0);
    sync_report();
}

static void press_with(int modifier, int code)
{
    emit(EV_KEY, modifier, 1);
    sync_report();
    press(code);
    emit(EV_KEY, modifier, 0);
    sync_report();
}

/* KEY_* code of a character on a US keyboard, 0 if unsupported; *shift: needs Shift */
static int key_of(char c, int *shift)
{
    static const int letters[26] = {
        KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
        KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    };
    *shift = 0;
    if (c >= 'A' && c <= 'Z') {
        *shift = 1;
        c = (char)(c - 'A' + 'a');
    }
    if (c >= 'a' && c <= 'z')
        return letters[c - 'a'];
    if (c >= '1' && c <= '9')
        return KEY_1 + (c - '1');
    switch (c) {
    case '0': return KEY_0;
    case ' ': return KEY_SPACE;
    case '.': return KEY_DOT;
    case ',': return KEY_COMMA;
    case '/': return KEY_SLASH;
    case '-': return KEY_MINUS;
    case ':': *shift = 1; return KEY_SEMICOLON;
    }
    return 0;
}

static int run_step(const char *step)
{
    const char *arg = step[0] && step[1] == ':' ? step + 2 : NULL;
    int a = 0, b = 0;
    switch (step[0]) {
    case 't':
        for (const char *c = arg ? arg : ""; *c; c++) {
            int shift, code = key_of(*c, &shift);
            if (!code)
                fprintf(stderr, "fakehid: can't type '%c', skipped\n", *c);
            else if (shift)
                press_with(KEY_LEFTSHIFT, code);
            else
                press(code);
        }
        return 0;
    case 'k':
        if (!arg)
            break;
        press(atoi(arg));
        return 0;
    case 'C':
        if (!arg)
            break;
        press_with(KEY_LEFTCTRL, atoi(arg));
        return 0;
    case 'M':
        if (!arg || sscanf(arg, "%d,%d", &a, &b) != 2)
            break;
        press_with(a, b);
        return 0;
    case 'm':
        if (!arg || sscanf(arg, "%d,%d", &a, &b) != 2)
            break;
        for (int i = 0; i < 10; i++) {
            emit(EV_REL, REL_X, a / 10 + (i < abs(a % 10) ? (a > 0 ? 1 : -1) : 0));
            emit(EV_REL, REL_Y, b / 10 + (i < abs(b % 10) ? (b > 0 ? 1 : -1) : 0));
            sync_report();
        }
        return 0;
    case 'c': {
        int button = arg ? atoi(arg) : 1;
        int code = button == 2 ? BTN_MIDDLE : button == 3 ? BTN_RIGHT : BTN_LEFT;
        emit(EV_KEY, code, 1);
        sync_report();
        emit(EV_KEY, code, 0);
        sync_report();
        return 0;
    }
    case 'w':
        if (!arg)
            break;
        emit(EV_REL, REL_WHEEL, atoi(arg));
        sync_report();
        return 0;
    case 's':
        if (!arg)
            break;
        usleep((useconds_t)atoi(arg) * 1000);
        return 0;
    }
    fprintf(stderr, "fakehid: bad step \"%s\"\n", step);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: fakehid STEP... (see tools/fakehid.c)\n");
        return 2;
    }
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("/dev/uinput");
        return 1;
    }
    /* A keyboard with every key plus a mouse: the browser opens devices with letter keys
     * (keyboard) or relative X/Y + left button (mouse) */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    for (int code = 1; code < 256; code++)
        ioctl(fd, UI_SET_KEYBIT, code);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    struct uinput_setup setup = { 0 };
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x5678;
    strcpy(setup.name, "fakehid test keyboard mouse");
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);
    sleep(2); /* the browser picks new devices up via inotify (+ a retry after 0.5 s) */

    int errors = 0;
    for (int i = 1; i < argc; i++)
        errors += run_step(argv[i]);

    sleep(1); /* let the last events be read before the device disappears */
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return errors ? 1 : 0;
}
