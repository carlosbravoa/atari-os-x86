/*
 * kms/help.c - the F1 Help: every key, one per line, in a scrolling list
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * Drawn with atari800's UI driver like the F8 and F9 menus: a list whose
 * lines are keys (section titles are labels the cursor skips), so the
 * arrow keys (and Page Up/Down, or a gamepad) scroll through it, Esc or
 * F1 closes it. The positional table of the Atari keyboard type comes
 * from kms/input.c, so the page shows what the keys really do. The
 * gamepad page (F8 > Controls) is a plain info screen.
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

#define MAX_LINES 96 /* the UI driver's Select holds 100 items */
#define LINE_MAX_W 38

int KMS_HELP_requested = FALSE;

/* Lines of the list; a line starting with '#' is a section title (a
   label), one starting with '!' a label in inverse video (bit 7 set per
   character, ui_basic.c's Plot). The keyboard type block is inserted
   where "@KEYBOARD" is.
   Every line is at most 38 columns. The Atari font has no backtick (it
   is a diamond there), so the Atari key is named by its place. */
static const char *const help_lines[] = {
	"#Function keys",
	"F1   Help (this list; F1 closes it)",
	"F2   OPTION",
	"F3   SELECT",
	"F4   START",
	"F5   Reset (warm)",
	"Shift+F5   Power-cycle (cold reset)",
	"F6   HELP key",
	"F7   BREAK key",
	"F8   System menu (F8 closes it)",
	"F9   Library (F9 closes it)",
	"F10  Screenshot (Shift: interlaced)",
	"F11  Arrows: cursor keys / joystick",
	"F12  Turbo (full speed) on/off",
	"#Atari keys on the PC keyboard",
	"Win, or the key left of 1   Atari",
	"           (inverse video)",
	"Home        CLEAR",
	"Ins / Del   Insert / delete character",
	"Shift+Ins / Del   Insert/delete line",
	"Caps Lock   CAPS",
	"Tab         TAB",
	"Arrows      Cursor (with Shift too)",
	"Ctrl+key    Ctrl+key (graphics)",
	"@KEYBOARD",
	"#Joystick on the keyboard",
	"Numpad 8 2 4 6   Move",
	"Numpad 0         Fire",
	"Arrows + left Alt   after F11",
	"#Gamepad",
	"Stick, D-pad     Joystick",
	"A, triggers      Fire",
	"Start / Select   START / SELECT",
	"X, Y             OPTION",
	"B                Back (Esc) in menus",
	"Guide            System menu",
	"F8 > Controls: swap gamepads 1 and 2",
	"#Library (F9)",
	"Tab         Next source (USB drives)",
	"RETURN      Open folder, run file",
	"Backspace   Up one folder",
	"Esc         Close",
	"Drives line: what is mounted; eject",
	"#Menus",
	"Arrows move, RETURN picks, Esc back",
	"#Power off",
	"F8 > Shut down, or the power button.",
	"!Never cut power without shutting down",
	NULL
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

static UI_tMenuItem items[MAX_LINES + 1];
static char rows[24][LINE_MAX_W + 1]; /* the keyboard type block */
static char inverse[4][LINE_MAX_W + 1]; /* '!' lines, bit 7 set */
static int n_items, n_inverse;

static void add_line(const char *text)
{
	UI_tMenuItem *m;
	if (n_items >= MAX_LINES)
		return;
	m = &items[n_items];
	m->flags = UI_ITEM_ACTION;
	m->retval = text[0] == '#' || text[0] == '!' ? -1 : n_items; /* -1: a label, skipped */
	m->prefix = NULL;
	if (text[0] == '!' && n_inverse < (int)(sizeof inverse / sizeof inverse[0])) {
		char *out = inverse[n_inverse++];
		size_t i;
		for (i = 0; i < LINE_MAX_W && text[i + 1]; i++)
			out[i] = (char)(text[i + 1] | 0x80);
		out[i] = '\0';
		m->item = out;
	}
	else
		m->item = (char *)(text[0] == '#' || text[0] == '!' ? text + 1 : text);
	m->suffix = NULL;
	n_items++;
}

static void add_keyboard_block(void)
{
	int n = 0;
	if (KMS_INPUT_keyboard_pc) {
		add_line("#Keyboard type: PC  (F8 > Controls)");
		add_line("Every key types what its cap says.");
		add_line("Braces and tilde have no Atari key.");
		return;
	}
	add_line("#Keyboard type: Atari (F8 > Controls)");
	add_line("The PC key types the Atari key in");
	add_line("its place:  PC key   plain   Shift");
	for (;;) {
		const char *pc, *plain, *shifted;
		if (n >= (int)(sizeof rows / sizeof rows[0]) || !KMS_INPUT_PositionalKey(n, &pc, &plain, &shifted))
			break;
		snprintf(rows[n], sizeof rows[n], "            %-8s %-7s %s", pc, plain, shifted);
		add_line(rows[n]);
		n++;
	}
}

static void build(void)
{
	size_t i;
	n_items = 0;
	n_inverse = 0;
	for (i = 0; help_lines[i] != NULL; i++) {
		if (strcmp(help_lines[i], "@KEYBOARD") == 0)
			add_keyboard_block();
		else
			add_line(help_lines[i]);
	}
	items[n_items].flags = UI_ITEM_END;
	items[n_items].retval = 0;
	items[n_items].prefix = NULL;
	items[n_items].item = NULL;
	items[n_items].suffix = NULL;
}

/* One info screen of N lines (at most 20), padded to the widest so the
   centred block reads left-aligned. */
static void show_page(const char *title, const char *const *lines, int n)
{
	static char page[20 * (LINE_MAX_W + 1) + 2];
	size_t pos = 0;
	int i, width = 0;

	if (n > 20)
		n = 20;
	for (i = 0; i < n; i++) {
		int len = (int)strlen(lines[i]);
		if (len > width)
			width = len;
	}
	if (width > LINE_MAX_W)
		width = LINE_MAX_W;
	for (i = 0; i < n; i++) {
		snprintf(page + pos, LINE_MAX_W + 1, "%-*.*s", width, LINE_MAX_W, lines[i]);
		pos += (size_t)width + 1; /* keeps the '\0' snprintf wrote */
	}
	page[pos++] = '\n';
	page[pos] = '\0';
	UI_driver->fInfoScreen(title, page);
}

void KMS_HELP_Run(void)
{
	int was_active = UI_is_active;
	int owner = KMS_INPUT_menu_owner;
	int sel = 1;

	if (!was_active) {
		UI_is_active = TRUE;
		UI_driver->fInit();
	}
	KMS_SYSMENU_HintSeen();
	KMS_INPUT_menu_owner = 1;
	Log_print("help: open");

	build();
	/* RETURN on a line does nothing; arrows scroll; Esc (or F1, which
	   the input layer turns into Esc) closes */
	for (;;) {
		int r = UI_driver->fSelect("Keys  (arrows scroll, Esc closes)", 0, sel, items, NULL);
		if (r < 0 || UI_alt_function >= 0 || KMS_INPUT_close_menus)
			break;
		sel = r;
	}

	if (!was_active) {
		int k;
		UI_is_active = FALSE;
		/* flush keypresses (see KMS_SYSMENU_Run); AKEY_EXIT stays for the main loop */
		while ((k = PLATFORM_Keyboard()) != AKEY_NONE && k != AKEY_EXIT)
			Atari800_Sync();
		UI_alt_function = -1;
	}
	KMS_INPUT_menu_owner = owner;
	if (!KMS_INPUT_close_menus)
		KMS_HELP_requested = FALSE; /* a toggle may have asked for another menu */
}

void KMS_HELP_GamepadPage(void)
{
	show_page("Gamepad buttons", gamepad_page, (int)(sizeof gamepad_page / sizeof gamepad_page[0]));
}
