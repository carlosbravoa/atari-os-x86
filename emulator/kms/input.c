/*
 * kms/input.c - evdev input backend for Atari800
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * Reads /dev/input/event* directly (no udev, no libinput). Keyboards are
 * grabbed so a VT underneath never sees the keys. Devices are classified
 * from their capability bits and hot-plugged through inotify.
 *
 * Key layout (positional, US-style keycaps):
 *   F1 menu   F2 OPTION  F3 SELECT  F4 START   F5 reset (Shift: cold)
 *   F6 HELP   F7 BREAK   F9 quit    F10 screenshot  F11 arrows=joystick toggle
 *   F12 turbo `/Win = Atari (inverse)  Home = CLEAR  Ins/Del = insert/delete
 *   Arrows + left Alt = joystick 0 (after F11), numpad 8/2/4/6 + 0 always.
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "akey.h"
#include "atari.h"
#include "binload.h"
#include "input.h"
#include "log.h"
#include "platform.h"
#include "ui.h"
#include "util.h"
#include "kms/kms_input.h"

int KMS_INPUT_arrows_joystick = FALSE; /* F11 toggles; FujiNet CONFIG and BASIC need cursor keys */
static int debug_keys = FALSE; /* -evdev-debug: log every key event */

#define INPUT_DIR "/dev/input"
#define MAX_DEVS 32
#define MAX_PADS 4

enum dev_type { DEV_NONE, DEV_KEYBOARD, DEV_PAD };

struct pad_state {
	int x, y;        /* -1, 0, 1 from the main stick */
	int hx, hy;      /* -1, 0, 1 from the hat */
	int fire;
	int start, select, option, menu, back;
	int xmin, xmax, ymin, ymax;
};

struct dev {
	int fd;
	enum dev_type type;
	char name[32]; /* "eventN" */
	int pad;       /* index into pads[] for DEV_PAD */
};

static struct dev devs[MAX_DEVS];
static struct pad_state pads[MAX_PADS];
static int pad_owner[MAX_PADS]; /* dev index or -1 */
static int inotify_fd = -1;

/* Keyboard state: one bit per KEY_* code, merged across all keyboards. */
static uint8_t keystate[(KEY_CNT + 7) / 8];
static int lastkey = -1;   /* most recently pressed key still held */
static int key_pressed = 0;
static int key_reported = 0;  /* lastkey has been returned at least once */
static int release_pending = 0; /* released before being reported: report once, then drop */

#define BIT_SET(arr, bit) ((arr)[(bit) / 8] & (1u << ((bit) % 8)))
#define key_down(k) BIT_SET(keystate, (k))

/* musl declares ioctl(int, int, ...); the EVIOC* request codes exceed
   INT_MAX, so route them through a cast (the kernel reads a 32-bit cmd). */
static int xioctl(int fd, unsigned long req, void *arg)
{
	return ioctl(fd, (int)req, arg);
}

/* ---- device discovery ------------------------------------------------- */

static int classify(int fd)
{
	uint8_t evbits[(EV_CNT + 7) / 8];
	uint8_t keybits[(KEY_CNT + 7) / 8];
	uint8_t absbits[(ABS_CNT + 7) / 8];
	int i;

	memset(evbits, 0, sizeof evbits);
	memset(keybits, 0, sizeof keybits);
	memset(absbits, 0, sizeof absbits);
	if (xioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0)
		return DEV_NONE;
	if (!BIT_SET(evbits, EV_KEY))
		return DEV_NONE;
	xioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
	if (BIT_SET(evbits, EV_ABS))
		xioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);

	/* A real keyboard has letters and Enter; lid switches and power
	   buttons also report EV_KEY but not these. */
	if (BIT_SET(keybits, KEY_A) && BIT_SET(keybits, KEY_Z) && BIT_SET(keybits, KEY_ENTER))
		return DEV_KEYBOARD;

	/* Gamepads and joysticks: any BTN_JOYSTICK/BTN_GAMEPAD button plus an
	   axis or hat. Touchpads have ABS_X too but only BTN_TOUCH/BTN_TOOL_*. */
	for (i = 0; i < 16; i++) {
		if (BIT_SET(keybits, BTN_JOYSTICK + i) || BIT_SET(keybits, BTN_GAMEPAD + i)) {
			if (BIT_SET(absbits, ABS_X) || BIT_SET(absbits, ABS_HAT0X))
				return DEV_PAD;
		}
	}
	return DEV_NONE;
}

static void read_abs_range(int fd, int axis, int *lo, int *hi)
{
	struct input_absinfo info;
	*lo = -32768;
	*hi = 32767;
	if (xioctl(fd, EVIOCGABS(axis), &info) == 0 && info.maximum > info.minimum) {
		*lo = info.minimum;
		*hi = info.maximum;
	}
}

static int find_dev(const char *name)
{
	int i;
	for (i = 0; i < MAX_DEVS; i++)
		if (devs[i].fd >= 0 && strcmp(devs[i].name, name) == 0)
			return i;
	return -1;
}

static void close_dev(int i)
{
	if (devs[i].fd < 0)
		return;
	if (devs[i].type == DEV_KEYBOARD)
		xioctl(devs[i].fd, EVIOCGRAB, (void *)0);
	if (devs[i].type == DEV_PAD && devs[i].pad >= 0) {
		pad_owner[devs[i].pad] = -1;
		memset(&pads[devs[i].pad], 0, sizeof pads[0]);
	}
	Log_print("evdev: removed %s", devs[i].name);
	close(devs[i].fd);
	devs[i].fd = -1;
	devs[i].type = DEV_NONE;
}

static void open_dev(const char *name)
{
	char path[64];
	char desc[64];
	int fd, i, type;

	if (strncmp(name, "event", 5) != 0 || find_dev(name) >= 0)
		return;
	snprintf(path, sizeof path, INPUT_DIR "/%s", name);
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	type = classify(fd);
	if (type == DEV_NONE) {
		close(fd);
		return;
	}
	for (i = 0; i < MAX_DEVS && devs[i].fd >= 0; i++)
		;
	if (i == MAX_DEVS) {
		close(fd);
		return;
	}
	devs[i].fd = fd;
	devs[i].type = type;
	devs[i].pad = -1;
	Util_strlcpy(devs[i].name, name, sizeof devs[i].name);
	desc[0] = '\0';
	xioctl(fd, EVIOCGNAME(sizeof desc), desc);

	if (type == DEV_KEYBOARD) {
		xioctl(fd, EVIOCGRAB, (void *)1);
	}
	else {
		int p;
		for (p = 0; p < MAX_PADS && pad_owner[p] >= 0; p++)
			;
		if (p == MAX_PADS) {
			close(fd);
			devs[i].fd = -1;
			devs[i].type = DEV_NONE;
			return;
		}
		pad_owner[p] = i;
		devs[i].pad = p;
		memset(&pads[p], 0, sizeof pads[p]);
		read_abs_range(fd, ABS_X, &pads[p].xmin, &pads[p].xmax);
		read_abs_range(fd, ABS_Y, &pads[p].ymin, &pads[p].ymax);
	}
	Log_print("evdev: %s %s: %s", type == DEV_KEYBOARD ? "keyboard" : "gamepad", name, desc);
}

static void scan_devices(void)
{
	DIR *d = opendir(INPUT_DIR);
	struct dirent *e;
	if (!d)
		return;
	while ((e = readdir(d)) != NULL)
		open_dev(e->d_name);
	closedir(d);
}

static void handle_inotify(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;

	while ((len = read(inotify_fd, buf, sizeof buf)) > 0) {
		char *p = buf;
		while (p < buf + len) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if (ev->len > 0) {
				if (ev->mask & (IN_CREATE | IN_ATTRIB))
					open_dev(ev->name);
				else if (ev->mask & IN_DELETE) {
					int i = find_dev(ev->name);
					if (i >= 0)
						close_dev(i);
				}
			}
			p += sizeof(struct inotify_event) + ev->len;
		}
	}
}

/* ---- event processing ------------------------------------------------- */

static int axis_dir(int value, int lo, int hi)
{
	int centre = (lo + hi) / 2;
	int dead = (hi - lo) / 4;
	if (value < centre - dead) return -1;
	if (value > centre + dead) return 1;
	return 0;
}

static void pad_event(struct pad_state *p, const struct input_event *ev)
{
	if (ev->type == EV_ABS) {
		switch (ev->code) {
		case ABS_X: p->x = axis_dir(ev->value, p->xmin, p->xmax); break;
		case ABS_Y: p->y = axis_dir(ev->value, p->ymin, p->ymax); break;
		case ABS_HAT0X: p->hx = ev->value < 0 ? -1 : ev->value > 0 ? 1 : 0; break;
		case ABS_HAT0Y: p->hy = ev->value < 0 ? -1 : ev->value > 0 ? 1 : 0; break;
		default: break;
		}
	}
	else if (ev->type == EV_KEY) {
		int down = ev->value != 0;
		switch (ev->code) {
		case BTN_SOUTH:   /* == BTN_A, BTN_GAMEPAD */
		case BTN_TRIGGER: /* == BTN_JOYSTICK */
		case BTN_THUMB:
		case BTN_TL:
		case BTN_TR:
			p->fire = down; break;
		case BTN_EAST:    /* == BTN_B */
			p->back = down; break;
		case BTN_START:   p->start = down; break;
		case BTN_SELECT:  p->select = down; break;
		case BTN_NORTH:   /* == BTN_X */
		case BTN_WEST:    /* == BTN_Y */
			p->option = down; break;
		case BTN_MODE:    p->menu = down; break;
		default: break;
		}
	}
}

static void key_event(const struct input_event *ev)
{
	if (ev->type != EV_KEY || ev->code >= KEY_CNT)
		return;
	if (ev->value == 1) {
		keystate[ev->code / 8] |= (uint8_t)(1u << (ev->code % 8));
		lastkey = ev->code;
		key_pressed = 1;
		key_reported = 0;
		release_pending = 0;
	}
	else if (ev->value == 0) {
		keystate[ev->code / 8] &= (uint8_t)~(1u << (ev->code % 8));
		if (ev->code == lastkey) {
			/* A tap shorter than one frame must not be lost: keep it
			   pressed until it has been reported once. */
			if (key_reported)
				key_pressed = 0;
			else
				release_pending = 1;
		}
	}
	if (debug_keys)
		Log_print("evdev: key %d value %d -> lastkey %d pressed %d", ev->code, ev->value, lastkey, key_pressed);
	/* value 2 = autorepeat: the Atari OS does its own repeat */
}

static void poll_events(void)
{
	struct input_event evs[64];
	int i;

	if (debug_keys) {
		static double last_poll = 0;
		double now = Util_time();
		if (last_poll > 0 && now - last_poll > 0.05)
			Log_print("evdev: %.0f ms since last poll", (now - last_poll) * 1000);
		last_poll = now;
	}
	if (inotify_fd >= 0)
		handle_inotify();

	for (i = 0; i < MAX_DEVS; i++) {
		ssize_t n;
		if (devs[i].fd < 0)
			continue;
		while ((n = read(devs[i].fd, evs, sizeof evs)) > 0) {
			int k, count = (int)(n / sizeof evs[0]);
			if (debug_keys && devs[i].type == DEV_KEYBOARD)
				Log_print("evdev: %s: %d events in one read", devs[i].name, count);
			for (k = 0; k < count; k++) {
				if (devs[i].type == DEV_KEYBOARD)
					key_event(&evs[k]);
				else
					pad_event(&pads[devs[i].pad], &evs[k]);
			}
		}
		if (n < 0 && errno != EAGAIN)
			close_dev(i); /* ENODEV: unplugged */
	}
}

/* ---- key translation -------------------------------------------------- */

struct keymap {
	uint16_t key;
	int plain;
	int shifted; /* -1: same as plain with AKEY_SHFT */
};

/* Letters are handled generically (AKEY_SHFT / AKEY_CTRL bits). */
static const struct keymap symbol_map[] = {
	{ KEY_1, AKEY_1, AKEY_EXCLAMATION },
	{ KEY_2, AKEY_2, AKEY_AT },
	{ KEY_3, AKEY_3, AKEY_HASH },
	{ KEY_4, AKEY_4, AKEY_DOLLAR },
	{ KEY_5, AKEY_5, AKEY_PERCENT },
	{ KEY_6, AKEY_6, AKEY_CARET },
	{ KEY_7, AKEY_7, AKEY_AMPERSAND },
	{ KEY_8, AKEY_8, AKEY_ASTERISK },
	{ KEY_9, AKEY_9, AKEY_PARENLEFT },
	{ KEY_0, AKEY_0, AKEY_PARENRIGHT },
	{ KEY_MINUS, AKEY_MINUS, AKEY_UNDERSCORE },
	{ KEY_EQUAL, AKEY_EQUAL, AKEY_PLUS },
	{ KEY_LEFTBRACE, AKEY_BRACKETLEFT, AKEY_BRACKETLEFT },
	{ KEY_RIGHTBRACE, AKEY_BRACKETRIGHT, AKEY_BRACKETRIGHT },
	{ KEY_BACKSLASH, AKEY_BACKSLASH, AKEY_BAR },
	{ KEY_SEMICOLON, AKEY_SEMICOLON, AKEY_COLON },
	{ KEY_APOSTROPHE, AKEY_QUOTE, AKEY_DBLQUOTE },
	{ KEY_COMMA, AKEY_COMMA, AKEY_LESS },
	{ KEY_DOT, AKEY_FULLSTOP, AKEY_GREATER },
	{ KEY_SLASH, AKEY_SLASH, AKEY_QUESTION },
	{ KEY_SPACE, AKEY_SPACE, AKEY_SPACE },
	{ KEY_ENTER, AKEY_RETURN, AKEY_RETURN },
	{ KEY_KPENTER, AKEY_RETURN, AKEY_RETURN },
	{ KEY_TAB, AKEY_TAB, AKEY_SETTAB },
	{ KEY_ESC, AKEY_ESCAPE, AKEY_ESCAPE },
	{ KEY_BACKSPACE, AKEY_BACKSPACE, AKEY_BACKSPACE },
	{ KEY_CAPSLOCK, AKEY_CAPSTOGGLE, AKEY_CAPSLOCK },
	{ KEY_GRAVE, AKEY_ATARI, AKEY_ATARI },
	{ KEY_LEFTMETA, AKEY_ATARI, AKEY_ATARI },
	{ KEY_RIGHTMETA, AKEY_ATARI, AKEY_ATARI },
	{ KEY_HOME, AKEY_CLEAR, AKEY_CLEAR },
	{ KEY_INSERT, AKEY_INSERT_CHAR, AKEY_INSERT_LINE },
	{ KEY_DELETE, AKEY_DELETE_CHAR, AKEY_DELETE_LINE },
	{ KEY_KPPLUS, AKEY_PLUS, AKEY_PLUS },
	{ KEY_KPMINUS, AKEY_MINUS, AKEY_MINUS },
	{ KEY_KPASTERISK, AKEY_ASTERISK, AKEY_ASTERISK },
	{ KEY_KPSLASH, AKEY_SLASH, AKEY_SLASH },
	{ KEY_KPDOT, AKEY_FULLSTOP, AKEY_FULLSTOP },
};

static const int letter_akey[26] = {
	AKEY_a, AKEY_b, AKEY_c, AKEY_d, AKEY_e, AKEY_f, AKEY_g, AKEY_h, AKEY_i,
	AKEY_j, AKEY_k, AKEY_l, AKEY_m, AKEY_n, AKEY_o, AKEY_p, AKEY_q, AKEY_r,
	AKEY_s, AKEY_t, AKEY_u, AKEY_v, AKEY_w, AKEY_x, AKEY_y, AKEY_z
};

/* evdev letter codes are not contiguous (QWERTY rows). */
static int letter_index(int key)
{
	switch (key) {
	case KEY_A: return 0;  case KEY_B: return 1;  case KEY_C: return 2;
	case KEY_D: return 3;  case KEY_E: return 4;  case KEY_F: return 5;
	case KEY_G: return 6;  case KEY_H: return 7;  case KEY_I: return 8;
	case KEY_J: return 9;  case KEY_K: return 10; case KEY_L: return 11;
	case KEY_M: return 12; case KEY_N: return 13; case KEY_O: return 14;
	case KEY_P: return 15; case KEY_Q: return 16; case KEY_R: return 17;
	case KEY_S: return 18; case KEY_T: return 19; case KEY_U: return 20;
	case KEY_V: return 21; case KEY_W: return 22; case KEY_X: return 23;
	case KEY_Y: return 24; case KEY_Z: return 25;
	default: return -1;
	}
}

static int arrows_are_joystick(void)
{
	return KMS_INPUT_arrows_joystick && !UI_is_active;
}

static int translate(int key, int shift, int ctrl)
{
	int shiftctrl = (shift ? AKEY_SHFT : 0) | (ctrl ? AKEY_CTRL : 0);
	int li = letter_index(key);
	size_t i;

	if (li >= 0)
		return letter_akey[li] | shiftctrl;

	switch (key) {
	case KEY_LEFT:  return shift ? AKEY_PLUS : AKEY_LEFT;
	case KEY_RIGHT: return shift ? AKEY_ASTERISK : AKEY_RIGHT;
	case KEY_UP:    return shift ? AKEY_MINUS : AKEY_UP;
	case KEY_DOWN:  return shift ? AKEY_EQUAL : AKEY_DOWN;
	case KEY_F6:    return AKEY_HELP ^ shiftctrl;
	default: break;
	}

	for (i = 0; i < sizeof symbol_map / sizeof symbol_map[0]; i++) {
		if (symbol_map[i].key == key) {
			int code = shift ? symbol_map[i].shifted : symbol_map[i].plain;
			return ctrl ? (code | AKEY_CTRL) : code;
		}
	}
	return AKEY_NONE;
}

/* ---- joystick synthesis ---------------------------------------------- */

static int stick_from_dirs(int dx, int dy)
{
	int s = INPUT_STICK_CENTRE;
	if (dx < 0) s &= ~0x04; /* left */
	if (dx > 0) s &= ~0x08; /* right */
	if (dy < 0) s &= ~0x01; /* up */
	if (dy > 0) s &= ~0x02; /* down */
	return s;
}

static int keyboard_stick(void)
{
	int dx = 0, dy = 0;
	if (key_down(KEY_KP4) || key_down(KEY_KP7) || key_down(KEY_KP1)) dx = -1;
	if (key_down(KEY_KP6) || key_down(KEY_KP9) || key_down(KEY_KP3)) dx = 1;
	if (key_down(KEY_KP8) || key_down(KEY_KP7) || key_down(KEY_KP9)) dy = -1;
	if (key_down(KEY_KP2) || key_down(KEY_KP1) || key_down(KEY_KP3)) dy = 1;
	if (arrows_are_joystick()) {
		if (key_down(KEY_LEFT))  dx = -1;
		if (key_down(KEY_RIGHT)) dx = 1;
		if (key_down(KEY_UP))    dy = -1;
		if (key_down(KEY_DOWN))  dy = 1;
	}
	return stick_from_dirs(dx, dy);
}

static int keyboard_fire(void)
{
	if (key_down(KEY_KP0) || key_down(KEY_KP5))
		return 1;
	if (arrows_are_joystick() && key_down(KEY_LEFTALT))
		return 1;
	return 0;
}

static int pad_stick(const struct pad_state *p)
{
	int dx = p->hx ? p->hx : p->x;
	int dy = p->hy ? p->hy : p->y;
	return stick_from_dirs(dx, dy);
}

int KMS_INPUT_Port(int num)
{
	int base = num * 2;
	int s0 = pad_stick(&pads[base]);
	int s1 = pad_stick(&pads[base + 1]);
	if (num == 0)
		s0 &= keyboard_stick();
	return s0 | (s1 << 4);
}

int KMS_INPUT_Trig(int num)
{
	if (num < 0 || num >= MAX_PADS)
		return 1;
	if (pads[num].fire)
		return 0;
	if (num == 0 && keyboard_fire())
		return 0;
	return 1;
}

/* ---- per-frame keyboard ----------------------------------------------- */

static int keyboard_inner(void);

int KMS_INPUT_Keyboard(void)
{
	static int last_reported = AKEY_NONE;
	int code = keyboard_inner();
	if (debug_keys && (code != last_reported || code != AKEY_NONE)) {
		Log_print("evdev: returning akey %d (ui %d)", code, UI_is_active);
		last_reported = code;
	}
	if (key_pressed) {
		key_reported = 1;
		if (release_pending) {
			release_pending = 0;
			key_pressed = 0;
		}
	}
	return code;
}

static int keyboard_inner(void)
{
	int shift, ctrl, p;

	poll_events();

	shift = key_down(KEY_LEFTSHIFT) || key_down(KEY_RIGHTSHIFT);
	ctrl = key_down(KEY_LEFTCTRL) || key_down(KEY_RIGHTCTRL);
	INPUT_key_shift = shift;

	BINLOAD_pause_loading = FALSE;

	INPUT_key_consol = INPUT_CONSOL_NONE;
	if (key_down(KEY_F2)) INPUT_key_consol &= ~INPUT_CONSOL_OPTION;
	if (key_down(KEY_F3)) INPUT_key_consol &= ~INPUT_CONSOL_SELECT;
	if (key_down(KEY_F4)) INPUT_key_consol &= ~INPUT_CONSOL_START;
	for (p = 0; p < MAX_PADS; p++) {
		if (pads[p].option) INPUT_key_consol &= ~INPUT_CONSOL_OPTION;
		if (pads[p].select) INPUT_key_consol &= ~INPUT_CONSOL_SELECT;
		if (pads[p].start)  INPUT_key_consol &= ~INPUT_CONSOL_START;
	}
	if (Atari800_machine_type == Atari800_MACHINE_5200 && !UI_is_active
	    && !(INPUT_key_consol & INPUT_CONSOL_START))
		return AKEY_5200_START;

	/* Gamepad shortcuts and menu navigation. */
	for (p = 0; p < MAX_PADS; p++) {
		if (pads[p].menu) {
			pads[p].menu = 0;
			return AKEY_UI;
		}
	}
	if (UI_is_active && !key_pressed) {
		int s = pad_stick(&pads[0]);
		if (!(s & 0x01)) return AKEY_UP;
		if (!(s & 0x02)) return AKEY_DOWN;
		if (!(s & 0x04)) return AKEY_LEFT;
		if (!(s & 0x08)) return AKEY_RIGHT;
		if (pads[0].fire) return AKEY_RETURN;
		if (pads[0].back) return AKEY_ESCAPE;
	}

	if (!key_pressed)
		return AKEY_NONE;

	switch (lastkey) {
	case KEY_F1:
		key_pressed = 0;
		return AKEY_UI;
	case KEY_F2: case KEY_F3: case KEY_F4:
		return AKEY_NONE; /* console keys, handled above */
	case KEY_F5:
		key_pressed = 0;
		return shift ? AKEY_COLDSTART : AKEY_WARMSTART;
	case KEY_F7:
		if (BINLOAD_wait_active) {
			BINLOAD_pause_loading = TRUE;
			return AKEY_NONE;
		}
		return AKEY_BREAK;
	case KEY_F9:
		return AKEY_EXIT;
	case KEY_F10:
		key_pressed = 0;
		return shift ? AKEY_SCREENSHOT_INTERLACE : AKEY_SCREENSHOT;
	case KEY_F11:
		key_pressed = 0;
		KMS_INPUT_arrows_joystick = !KMS_INPUT_arrows_joystick;
		Log_print("evdev: arrow keys are now %s", KMS_INPUT_arrows_joystick ? "joystick 0" : "cursor keys");
		return AKEY_NONE;
	case KEY_F12:
		key_pressed = 0;
		return AKEY_TURBO;
	case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN: case KEY_LEFTALT:
		if (arrows_are_joystick())
			return AKEY_NONE; /* consumed by the joystick */
		break;
	case KEY_KP0: case KEY_KP1: case KEY_KP2: case KEY_KP3: case KEY_KP4:
	case KEY_KP5: case KEY_KP6: case KEY_KP7: case KEY_KP8: case KEY_KP9:
		return AKEY_NONE;
	default:
		break;
	}

	if (Atari800_machine_type == Atari800_MACHINE_5200 && !UI_is_active) {
		switch (lastkey) {
		case KEY_P: return AKEY_5200_PAUSE;
		case KEY_R: return AKEY_5200_RESET;
		case KEY_0: return AKEY_5200_0;
		case KEY_1: return AKEY_5200_1;
		case KEY_2: return AKEY_5200_2;
		case KEY_3: return AKEY_5200_3;
		case KEY_4: return AKEY_5200_4;
		case KEY_5: return AKEY_5200_5;
		case KEY_6: return AKEY_5200_6;
		case KEY_7: return AKEY_5200_7;
		case KEY_8: return AKEY_5200_8;
		case KEY_9: return AKEY_5200_9;
		case KEY_EQUAL: return AKEY_5200_HASH;
		case KEY_MINUS: return AKEY_5200_ASTERISK;
		default: return AKEY_NONE;
		}
	}

	return translate(lastkey, shift, ctrl);
}

/* ---- config / lifecycle ---------------------------------------------- */

int KMS_INPUT_ReadConfig(char *option, char *parameters)
{
	if (strcmp(option, "KMS_ARROWS_JOYSTICK") == 0)
		return (KMS_INPUT_arrows_joystick = Util_sscanbool(parameters)) != -1;
	return FALSE;
}

void KMS_INPUT_WriteConfig(FILE *fp)
{
	fprintf(fp, "KMS_ARROWS_JOYSTICK=%d\n", KMS_INPUT_arrows_joystick);
}

int KMS_INPUT_Initialise(int *argc, char *argv[])
{
	int i, j;

	for (i = 0; i < MAX_DEVS; i++) {
		devs[i].fd = -1;
		devs[i].type = DEV_NONE;
	}
	for (i = 0; i < MAX_PADS; i++)
		pad_owner[i] = -1;

	for (i = j = 1; i < *argc; i++) {
		if (strcmp(argv[i], "-arrows-joystick") == 0)
			KMS_INPUT_arrows_joystick = TRUE;
		else if (strcmp(argv[i], "-arrows-cursor") == 0)
			KMS_INPUT_arrows_joystick = FALSE;
		else if (strcmp(argv[i], "-evdev-debug") == 0)
			debug_keys = TRUE;
		else {
			if (strcmp(argv[i], "-help") == 0) {
				Log_print("\t-arrows-joystick          Arrow keys + left Alt are joystick 0");
				Log_print("\t-arrows-cursor            Arrow keys are Atari cursor keys (default)");
				Log_print("\t-evdev-debug              Log every keyboard event");
			}
			argv[j++] = argv[i];
		}
	}
	*argc = j;

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0)
		inotify_add_watch(inotify_fd, INPUT_DIR, IN_CREATE | IN_DELETE | IN_ATTRIB);
	scan_devices();
	return TRUE;
}

void KMS_INPUT_Exit(void)
{
	int i;
	for (i = 0; i < MAX_DEVS; i++)
		close_dev(i);
	if (inotify_fd >= 0) {
		close(inotify_fd);
		inotify_fd = -1;
	}
}
