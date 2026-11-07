/*
 * kms/video.h - DRM/KMS dumb-buffer video backend for Atari800
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
#ifndef KMS_KMS_KMS_VIDEO_H_
#define KMS_KMS_KMS_VIDEO_H_

#include <stdio.h>

/* How the 336x240 Atari picture is placed on the panel. */
enum {
	KMS_SCALE_INTEGER, /* largest integer factor that fits, same on both axes (default) */
	KMS_SCALE_FIT,     /* fill the height, 4:3 width, fractional nearest-neighbour */
	KMS_SCALE_STRETCH  /* fill the whole panel */
};

extern int KMS_VIDEO_scale_mode;
/* Preferred DRM node ("" = probe /dev/dri/card*). */
extern char KMS_VIDEO_device[256];

int KMS_VIDEO_Initialise(int *argc, char *argv[]);
void KMS_VIDEO_Exit(void);
int KMS_VIDEO_ReadConfig(char *option, char *parameters);
void KMS_VIDEO_WriteConfig(FILE *fp);
void KMS_VIDEO_PaletteUpdate(void);
void KMS_VIDEO_DisplayScreen(void);

/* Show a line of text in the border for SECONDS (e.g. the F8 hint). */
void KMS_VIDEO_SetHint(const char *text, double seconds);
/* Driver name of the DRM device in use ("i915", "simpledrm", ...). */
extern char KMS_VIDEO_driver[32];

/* Panel dimensions and refresh, available after Initialise. */
extern int KMS_VIDEO_width;
extern int KMS_VIDEO_height;
extern double KMS_VIDEO_refresh;

/* Pacing (see video.c): lock when |refresh - Atari fps| < KMS_VIDEO_lock_hz. */
extern double KMS_VIDEO_lock_hz;
/* -kms-mode / KMS_MODE ("WxH" or "WxH@Hz"; "" = automatic). */
extern char KMS_VIDEO_mode_request[32];
/* The Atari's frame rate for the current TV system (49.86 or 59.92). */
double KMS_VIDEO_AtariFps(void);
/* TRUE when the page flips pace the emulation (outside turbo and menus). */
int KMS_VIDEO_PacingLocked(void);
/* TRUE when a mode at the current resolution would lock FPS (the mode is
   only chosen when the emulator starts). */
int KMS_VIDEO_HasModeFor(double fps);

#endif /* KMS_KMS_KMS_VIDEO_H_ */
