/*
 * kms/help.c - the F1 Help page: every key, the menus, the gamepad
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * Info screens of atari800's UI driver, like the System menu's
 * "Hardware status". The driver centres every line and shows at most 20,
 * so the lines of a page are padded to the same width (one left-aligned
 * block) and longer texts are split into pages. Any key turns the page,
 * Esc closes. The positional table of the Atari keyboard type comes from
 * kms/input.c, so the page shows what the keys do.
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

#include <stdio.h>
#include <string.h>

#include "akey.h"
#include "atari.h"
#include "log.h"
#include "platform.h"
#include "ui.h"
#include "util.h"
#include "kms/kms_help.h"
#include "kms/kms_input.h"
#include "kms/kms_sysmenu.h"

#define PAGE_LINES 20 /* rows 2..21 of the info screen; the prompt is on 22 */
#define LINE_MAX_W 38

int KMS_HELP_requested = FALSE;

/* The Atari font has no backtick (it is a diamond there), so the Atari
   key is named by its place. Every line is at most 38 columns. */
static const char *const keys_page[] = {
	"F1  this page      F8  System menu",
	"F9  Library        Esc back / close",
	"F2  OPTION   F3  SELECT   F4  START",
	"F5  reset    Shift+F5  power-cycle",
	"F6  HELP     F7  BREAK   F12 turbo",
	"F10 screenshot (Shift: interlaced)",
	"F11 arrows = cursor keys / joystick",
	"Win / key left of 1  Atari (inverse)",
	"Home CLEAR  Ins/Del insert/delete",
	"Caps Lock  CAPS       Tab  TAB",
	"Arrows, Shift+arrows  cursor keys",
	"Arrows + left Alt  joystick after F11",
	"Numpad 8 2 4 6 move, 0 fire (always)",
	"",
	NULL, /* keyboard type, filled in */
	NULL,
	"Gamepad: stick/D-pad move, A fire,",
	"  Start/Select/X START/SELECT/OPTION,",
	"  B back, Guide = System menu"
};

static const char *const menus_page[] = {
	"Library (F9): Tab next source,",
	"  RETURN open/run, Backspace up,",
	"  Esc close, Filter narrows names",
	"Menus: arrows, RETURN, Esc back",
	"Power off: F8 > Shut down, or the",
	"  power button. Never just unplug."
};

static const char *const gamepad_page[] = {
	"D-pad, left stick  joystick",
	"A, triggers, shoulders  fire",
	"Start  START      Select  SELECT",
	"X, Y   OPTION",
	"Guide  System menu (F8)",
	"",
	"In menus: D-pad moves, A selects,",
	"  B goes back (Esc).",
	"",
	"Gamepad 1 drives joystick 0 and",
	"  gamepad 2 joystick 1, unless",
	"  swapped in F8 > Controls; menus",
	"  always follow gamepad 1."
};

/* One info screen of N lines (at most PAGE_LINES), padded to the widest
   so the centred block reads left-aligned. FALSE when Esc closed it. */
static int show_page(const char *title, const char *const *lines, int n)
{
	static char page[PAGE_LINES * (LINE_MAX_W + 1) + 2];
	size_t pos = 0;
	int i, width = 0;

	if (n > PAGE_LINES)
		n = PAGE_LINES;
	for (i = 0; i < n; i++) {
		int len = lines[i] ? (int)strlen(lines[i]) : 0;
		if (len > width)
			width = len;
	}
	if (width > LINE_MAX_W)
		width = LINE_MAX_W;
	for (i = 0; i < n; i++) {
		snprintf(page + pos, LINE_MAX_W + 1, "%-*.*s", width, LINE_MAX_W, lines[i] ? lines[i] : "");
		pos += (size_t)width + 1; /* keeps the '\0' snprintf wrote */
	}
	page[pos++] = '\n';
	page[pos] = '\0';
	UI_driver->fInfoScreen(title, page);
	return KMS_INPUT_LastKey() != AKEY_ESCAPE && UI_alt_function < 0;
}

/* The positional table, two keys per line. */
static int show_positional(const char *title)
{
	static char rows[PAGE_LINES][LINE_MAX_W + 1];
	const char *lines[PAGE_LINES];
	const char *pc, *plain, *shifted;
	int n = 0, i = 0;

	lines[n++] = "Keyboard type Atari: a PC key types";
	lines[n++] = "the Atari key in its place.";
	lines[n++] = "";
	lines[n++] = "PC Atari Shift     PC Atari Shift";
	while (n < PAGE_LINES && KMS_INPUT_PositionalKey(i, &pc, &plain, &shifted)) {
		const char *pc2 = "", *plain2 = "", *shifted2 = "";
		KMS_INPUT_PositionalKey(i + 1, &pc2, &plain2, &shifted2);
		snprintf(rows[n], sizeof rows[n], "%-3s%-6s%-9s %-3s%-6s%s", pc, plain, shifted, pc2, plain2, shifted2);
		lines[n] = rows[n];
		n++;
		i += 2;
	}
	if (n < PAGE_LINES)
		lines[n++] = "";
	if (n < PAGE_LINES)
		lines[n++] = "Arrows move the cursor, as in PC.";
	return show_page(title, lines, n);
}

void KMS_HELP_Run(void)
{
	const char *keys[sizeof keys_page / sizeof keys_page[0]];
	int was_active = UI_is_active;
	int atari = !KMS_INPUT_keyboard_pc;
	int pages = atari ? 3 : 2;
	char title[16];
	size_t i;

	if (!was_active) {
		UI_is_active = TRUE;
		UI_driver->fInit();
	}
	KMS_SYSMENU_HintSeen();
	Log_print("help: open");

	for (i = 0; i < sizeof keys_page / sizeof keys_page[0]; i++)
		keys[i] = keys_page[i];
	for (i = 0; keys[i] != NULL; i++)
		;
	keys[i] = atari ? "Keyboard type: Atari (positional)" : "Keyboard type: PC   (F8 > Controls)";
	keys[i + 1] = atari ? "  Atari layout, page 3 (F8 > Controls)" : "  keys type what their caps say";

	snprintf(title, sizeof title, "Keys (1/%d)", pages);
	if (show_page(title, keys, (int)(sizeof keys / sizeof keys[0]))) {
		snprintf(title, sizeof title, "Menus (2/%d)", pages);
		if (show_page(title, menus_page, (int)(sizeof menus_page / sizeof menus_page[0])) && atari)
			show_positional("Atari keyboard (3/3)");
	}

	if (!was_active) {
		int k;
		UI_is_active = FALSE;
		/* flush keypresses (see KMS_SYSMENU_Run); AKEY_EXIT stays for the main loop */
		while ((k = PLATFORM_Keyboard()) != AKEY_NONE && k != AKEY_EXIT)
			Atari800_Sync();
		UI_alt_function = -1;
	}
	KMS_HELP_requested = FALSE;
}

void KMS_HELP_GamepadPage(void)
{
	show_page("Gamepad buttons", gamepad_page, (int)(sizeof gamepad_page / sizeof gamepad_page[0]));
}
