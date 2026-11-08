/*
 * kms/boot.c - what a cold start boots
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * The rule: what is mounted boots. A disk in D1:, a program being
 * loaded, a cartridge other than our ASH.CAR or a tape set to boot takes
 * the ASH cartridge out for that boot and turns BASIC off (like holding
 * OPTION on a real XL), unless "Run with BASIC" asked otherwise. With
 * nothing mounted the boot target chosen in F8 applies: BASIC, No BASIC
 * (the XL self test), ASH (its cartridge goes in) or FujiNet CONFIG,
 * except that ASH.CAR put in by hand (Library Run, F1, ASH's CART) boots
 * whatever the target, until it comes out or "Eject all".
 * Every way of cold starting goes through Atari800_Coldstart() and so
 * through here: the F1 menu, Shift+F5, the Library, ASH's reboot, F8.
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

#include "atari.h"
#include "binload.h"
#include "cartridge.h"
#include "cassette.h"
#include "log.h"
#include "sio.h"
#include "util.h"
#ifdef NETSIO
#include "netsio.h"
#endif
#include "kms/kms_boot.h"
#include "kms/kms_hdev.h"

#define ASH_CART "/media/atari/ash/ASH.CAR" /* ASH_CART in init/init.c */

int KMS_BOOT_with_basic = FALSE;

static char target[16] = "";
static char last_route[64] = "not started";
static int basic_on = FALSE;
/* Atari800_disable_basic as the config file had it: used while the boot
   target is not known (no control socket, e.g. on a desktop). */
static int config_basic_off = -1;
/* ASH.CAR was in when the last cold start was set up (we put it there or
   left it), and whether the user put it in since (then it is a cartridge
   like any other until it comes out). */
static int ash_was_in = FALSE;
static int ash_explicit = FALSE;

static int drive_has_image(int i)
{
	return SIO_drive_status[i] == SIO_READ_ONLY || SIO_drive_status[i] == SIO_READ_WRITE;
}

static int cart_in(void)
{
	return CARTRIDGE_main.type != CARTRIDGE_NONE;
}

static int cart_is_ash(void)
{
	const char *f = CARTRIDGE_main.filename;
	const char *b = strrchr(f, '/');
	return cart_in() && Util_stricmp(b ? b + 1 : f, "ASH.CAR") == 0;
}

static int tape_boots(void)
{
	return CASSETTE_status != CASSETTE_STATUS_NONE && (CASSETTE_hold_start || CASSETTE_hold_start_on_reboot);
}

void KMS_BOOT_SetTarget(const char *t)
{
	Util_strlcpy(target, t, sizeof target);
}

const char *KMS_BOOT_Target(void)
{
	if (target[0])
		return target;
	/* not told: guess from how we were started */
#ifdef NETSIO
	if (netsio_enabled)
		return "fujinet";
#endif
	if (cart_is_ash())
		return "ash";
	return "basic";
}

const char *KMS_BOOT_TargetLabel(const char *t)
{
	if (strcmp(t, "nobasic") == 0)
		return "No BASIC";
	if (strcmp(t, "ash") == 0)
		return "ASH";
	if (strcmp(t, "fujinet") == 0)
		return "FujiNet CONFIG";
	return "BASIC";
}

int KMS_BOOT_SomethingMounted(void)
{
	int i;
	for (i = 0; i < SIO_MAX_DRIVES; i++)
		if (drive_has_image(i))
			return TRUE;
	return (cart_in() && (!cart_is_ash() || ash_explicit)) || CARTRIDGE_piggyback.type != CARTRIDGE_NONE
	       || CASSETTE_status != CASSETTE_STATUS_NONE || BINLOAD_bin_file != NULL;
}

void KMS_BOOT_EjectAll(void)
{
	int i;
	for (i = 1; i <= SIO_MAX_DRIVES; i++)
		SIO_DisableDrive(i); /* as at power-up: no drive answers, no BOOT ERROR */
	if (cart_in() && !cart_is_ash())
		CARTRIDGE_Remove();
	else
		CARTRIDGE_Remove_Second();
	ash_explicit = FALSE; /* the boot target decides about ASH again */
	if (CASSETTE_status != CASSETTE_STATUS_NONE)
		CASSETTE_Remove();
	CASSETTE_hold_start = FALSE;
	KMS_HDEV_EjectAll();
	Log_print("boot: eject all");
	Atari800_Coldstart();
}

const char *KMS_BOOT_LastRoute(void)
{
	return last_route;
}

int KMS_BOOT_BasicOn(void)
{
	return basic_on;
}

int KMS_BOOT_ConfigDisableBasic(void)
{
	/* the live value is the last route's; the file keeps the setting */
	return config_basic_off >= 0 ? config_basic_off : Atari800_disable_basic;
}

static void insert_ash(void)
{
	int r;
	if (!Util_fileexists(ASH_CART)) {
		Log_print("boot: %s is missing", ASH_CART);
		return;
	}
	r = CARTRIDGE_Insert(ASH_CART); /* no auto-reboot: we are in the cold start */
	if (r != 0 && r != CARTRIDGE_BAD_CHECKSUM) {
		Log_print("boot: cannot insert %s (%d)", ASH_CART, r);
		CARTRIDGE_Remove();
	}
}

void KMS_BOOT_ColdStart(void)
{
	const char *what = NULL;
	int ash_out = FALSE;

	if (config_basic_off < 0)
		config_basic_off = Atari800_disable_basic; /* the first cold start */
	else if (!cart_is_ash())
		ash_explicit = FALSE;
	else if (!ash_was_in && !ash_explicit) {
		ash_explicit = TRUE; /* inserted since the last cold start: asked for */
		Log_print("boot: ASH.CAR inserted by hand");
	}

	if (BINLOAD_bin_file != NULL)
		what = BINLOAD_loading_basic ? "BASIC program" : "program";
	else if (drive_has_image(0))
		what = "disk D1";
	else if (cart_in() && !cart_is_ash())
		what = "cartridge";
	else if (tape_boots())
		what = "tape";
	else if (cart_is_ash() && ash_explicit)
		what = "ASH cartridge"; /* asked for, nothing else to boot */

	if (what != NULL) {
		if (cart_is_ash() && strcmp(what, "ASH cartridge") != 0) {
			CARTRIDGE_Remove();
			ash_out = TRUE;
		}
		Atari800_disable_basic = !KMS_BOOT_with_basic;
		basic_on = !Atari800_disable_basic || BINLOAD_loading_basic;
		snprintf(last_route, sizeof last_route, "%s, BASIC %s", what, basic_on ? "on" : "off");
		Log_print("boot: cold start -> %s (BASIC %s%s)", what, basic_on ? "on" : "off",
		          ash_out ? ", ASH cart out" : "");
	}
	else if (target[0]) {
		if (strcmp(target, "ash") == 0) {
			if (!cart_is_ash())
				insert_ash();
			Atari800_disable_basic = TRUE;
		}
		else {
			if (cart_is_ash())
				CARTRIDGE_Remove(); /* taken out earlier, or left in the config */
			Atari800_disable_basic = strcmp(target, "basic") != 0;
		}
		basic_on = !Atari800_disable_basic;
		snprintf(last_route, sizeof last_route, "%s, BASIC %s", KMS_BOOT_TargetLabel(target), basic_on ? "on" : "off");
		Log_print("boot: cold start -> target %s (BASIC %s)", target, basic_on ? "on" : "off");
	}
	else {
		/* target unknown: atari800's own settings */
		Atari800_disable_basic = config_basic_off;
		basic_on = !Atari800_disable_basic;
		snprintf(last_route, sizeof last_route, "default, BASIC %s", basic_on ? "on" : "off");
		Log_print("boot: cold start -> default (BASIC %s)", basic_on ? "on" : "off");
	}
	KMS_BOOT_with_basic = FALSE;
	ash_was_in = cart_is_ash();
}
