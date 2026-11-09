/*
 * kms/main.c - platform glue and main loop for the DRM/KMS + evdev port
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atari.h"
#include "akey.h"
#include "input.h"
#include "log.h"
#include "platform.h"
#include "util.h"
#ifdef SOUND
#include "sound.h"
#endif
#include "kms/kms_hdev.h"
#include "kms/kms_help.h"
#include "kms/kms_input.h"
#include "kms/kms_library.h"
#include "kms/kms_sysmenu.h"
#include "kms/kms_video.h"

static volatile sig_atomic_t quit_requested = 0;

/* PID 1 sends SIGTERM on shutdown; leave the display and config clean. */
static void on_signal(int sig)
{
	(void)sig;
	quit_requested = 1;
}

void PLATFORM_ConfigInit(void)
{
	/* NTSC unless the config says otherwise (DEFAULT_TV_MODE=PAL is kept):
	   the Atari was an NTSC machine first, and 59.92 Hz locks to the 60 Hz
	   panels of most laptops (kms/video.c pacing). */
	Atari800_tv_mode = Atari800_TV_NTSC;
#ifdef SOUND
	/* 20 ms of latency is plenty for direct ALSA; no sound server in the way. */
	Sound_desired.buffer_ms = 20;
	Sound_latency = 20;
#endif
}

int PLATFORM_Configure(char *option, char *parameters)
{
	return KMS_VIDEO_ReadConfig(option, parameters) ||
	       KMS_INPUT_ReadConfig(option, parameters) ||
	       KMS_SYSMENU_ReadConfig(option, parameters);
}

void PLATFORM_ConfigSave(FILE *fp)
{
	KMS_VIDEO_WriteConfig(fp);
	KMS_INPUT_WriteConfig(fp);
	KMS_SYSMENU_WriteConfig(fp);
}

int PLATFORM_Initialise(int *argc, char *argv[])
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (!KMS_VIDEO_Initialise(argc, argv))
		return FALSE;
#ifdef SOUND
	if (!Sound_Initialise(argc, argv))
		return FALSE;
#endif
	if (!KMS_INPUT_Initialise(argc, argv))
		return FALSE;
	KMS_SYSMENU_ApplySettings();
	return TRUE;
}

int PLATFORM_Exit(int run_monitor)
{
	(void)run_monitor; /* no console on the appliance: the monitor is not offered */
	KMS_INPUT_Exit();
	KMS_VIDEO_Exit();
	Log_flushlog();
	return 0;
}

int PLATFORM_Keyboard(void)
{
	if (quit_requested)
		return AKEY_EXIT;
	return KMS_INPUT_Keyboard();
}

void PLATFORM_DisplayScreen(void)
{
	KMS_VIDEO_DisplayScreen();
}

void PLATFORM_PaletteUpdate(void)
{
	KMS_VIDEO_PaletteUpdate();
}

int PLATFORM_PORT(int num)
{
	return KMS_INPUT_Port(num);
}

int PLATFORM_TRIG(int num)
{
	return KMS_INPUT_Trig(num);
}

int main(int argc, char **argv)
{
	/* stdout is a log file on the appliance: keep it line-buffered. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!Atari800_Initialise(&argc, argv))
		return 3;

	for (;;) {
		double t0, input_t;
		int menu_ran = FALSE;
#ifdef SOUND
		/* The sound card may appear after we started (SOF firmware, USB
		   audio): keep trying for a while instead of staying silent. */
		if (!Sound_enabled) {
			static double next_try = 0;
			static int tries = 0;
			double now = Util_time();
			if (tries < 30 && now >= next_try) {
				next_try = now + 2.0;
				tries++;
				if (Sound_Setup()) {
					Sound_Continue();
					KMS_SYSMENU_ApplySettings();
				}
			}
		}
#endif
		KMS_SYSMENU_LatencyPoll();
		/* The previous frame's flip lands first, then the input is read and
		   this frame emulated and queued at once, so it is shown one vblank
		   later (otherwise the frame sleep inside Atari800_Frame waits with
		   the input already read, and the picture misses a vblank). In Low
		   latency the wait also sleeps on until just before the next vblank
		   (the just-in-time frame, kms/video.c). A no-op unless paced by
		   the display. */
		t0 = Util_time();
		KMS_VIDEO_WaitFrame();
		INPUT_key_code = PLATFORM_Keyboard();
		input_t = KMS_INPUT_TakeEventTime();
		if (KMS_SYSMENU_requested) {
			KMS_SYSMENU_requested = FALSE;
			menu_ran = TRUE;
#ifdef SOUND
			Sound_Pause();
#endif
			KMS_SYSMENU_Run();
#ifdef SOUND
			Sound_Continue();
#endif
			KMS_INPUT_close_menus = FALSE;
			INPUT_key_code = AKEY_NONE;
		}
		if (KMS_HELP_requested) {
			KMS_HELP_requested = FALSE;
			menu_ran = TRUE;
#ifdef SOUND
			Sound_Pause();
#endif
			KMS_HELP_Run();
#ifdef SOUND
			Sound_Continue();
#endif
			KMS_INPUT_close_menus = FALSE;
			INPUT_key_code = AKEY_NONE;
		}
		KMS_LIBRARY_Poll();
		KMS_HDEV_Poll();
		if (KMS_LIBRARY_requested) {
			KMS_LIBRARY_requested = FALSE;
			menu_ran = TRUE;
#ifdef SOUND
			Sound_Pause();
#endif
			KMS_LIBRARY_Run();
#ifdef SOUND
			Sound_Continue();
#endif
			KMS_INPUT_close_menus = FALSE;
			INPUT_key_code = AKEY_NONE;
		}
		if (menu_ran)
			KMS_INPUT_TakeEventTime(); /* a menu was open: its input and time are not the game's */
		else
			KMS_VIDEO_FrameInput(input_t);
		Atari800_Frame();
		if (Atari800_display_screen)
			PLATFORM_DisplayScreen();
		KMS_VIDEO_FrameDone(Util_time() - t0, !menu_ran);
	}
}
