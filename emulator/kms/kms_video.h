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
	KMS_SCALE_FIT,     /* fill the height, 4:3 width (Atari pixel shape kept), fractional */
	KMS_SCALE_STRETCH, /* fill the whole panel ("Fill") */
	KMS_SCALE_INTEGER_XY, /* the largest whole factor on each axis (5 x 4 on 1920x1080): crisp, scanline-friendly, wider */
	KMS_SCALE_FREE     /* KMS_VIDEO_free_x by KMS_VIDEO_free_y, clipped to the panel */
};

extern int KMS_VIDEO_scale_mode;
/* Free scaling factors (KMS_SCALE_X/KMS_SCALE_Y in the config). */
extern double KMS_VIDEO_free_x;
extern double KMS_VIDEO_free_y;
/* The factors of the current layout (picture size / source size), 0
   before Initialise. */
double KMS_VIDEO_ScaleX(void);
double KMS_VIDEO_ScaleY(void);
/* The largest factor that fits the panel on one axis (for the source
   width the borders give now). */
double KMS_VIDEO_FreeMax(int vertical);
/* "3 x 3" (Integer) or "3.18 x 3.33" into BUF. */
void KMS_VIDEO_ScaleFactors(char *buf, size_t size);

/* Display options (F8 > Display), KMS_SMOOTH/KMS_SCANLINES/KMS_BORDERS in
   the config. Call KMS_VIDEO_Relayout() after changing one. */
enum { KMS_SCANLINES_OFF, KMS_SCANLINES_LIGHT, KMS_SCANLINES_MEDIUM, KMS_SCANLINES_STRONG };
enum { KMS_BORDERS_FULL, KMS_BORDERS_TV, KMS_BORDERS_TIGHT }; /* 384, 336, 320 columns */
extern int KMS_VIDEO_smooth;    /* Fit/Fill: sharp-bilinear instead of nearest */
extern int KMS_VIDEO_scanlines; /* KMS_SCANLINES_* (dark rows at 75/50/25 %) */
extern int KMS_VIDEO_borders;   /* KMS_BORDERS_* */
void KMS_VIDEO_Relayout(void);
/* The picture is at least 2x high, so scanlines show. */
int KMS_VIDEO_ScanlinesPossible(void);
/* NTSC TV look (atari800's NTSC filter, saved as ARTIFACT_NTSC=NTSC-FULL):
   on when the TV system is NTSC and it is chosen. SetNtscLook fails
   (FALSE) on PAL. */
int KMS_VIDEO_NtscLook(void);
int KMS_VIDEO_SetNtscLook(int on);
/* The filter's preset, recognised from its resolution/artifacts/fringing/
   bleed (saved by atari800 as FILTER_NTSC_*): KMS_NTSC_* or KMS_NTSC_CUSTOM. */
enum { KMS_NTSC_COMPOSITE, KMS_NTSC_SVIDEO, KMS_NTSC_RGB, KMS_NTSC_MONOCHROME, KMS_NTSC_CUSTOM };
int KMS_VIDEO_NtscPreset(void);
const char *KMS_VIDEO_NtscPresetLabel(void); /* "Composite", "S-Video", ... */
/* Load a preset's filter knobs (not the colour controls) and turn the TV
   look on; FALSE on PAL. */
int KMS_VIDEO_SetNtscPreset(int preset);
/* The filter's sharpness, -1.0 .. 1.0 (FILTER_NTSC_SHARPNESS), applied at once. */
double KMS_VIDEO_NtscSharpness(void);
void KMS_VIDEO_SetNtscSharpness(double s);
/* "Integer"/"Fit"/"Fill"/"Free" and "Off"/"Light"/"Medium"/"Strong" */
const char *KMS_VIDEO_ScaleLabel(void);
const char *KMS_VIDEO_ScanlinesLabel(void);
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

/* Input lag, controller to display (kms/main.c drives it, once a frame):
   - WaitFrame (Low latency only): wait for the previous frame's flip now,
     before the input is read; TakeFramePaced is TRUE once after that (the
     frame sleep in atari800's Atari800_Sync has nothing left to wait for).
   - FrameInput(t): the kernel stamp of the oldest input this frame used
     (KMS_INPUT_TakeEventTime); the sample is the flip's vblank stamp minus
     it, taken when that frame's flip completes (not called when a menu
     was open).
   - FrameDone(total, valid): one turn of the main loop took TOTAL
     seconds; the time spent in it sleeping or waiting for flips is split
     off (the rest is emulate + blit). VALID FALSE: a menu ran, no sample.
   Latency: average and maximum over the last 120 samples (ms), FALSE
   while there is none. FrameTimes: emulate+blit and wait, average ms
   over the last 120 frames. */
void KMS_VIDEO_WaitFrame(void);
int KMS_VIDEO_TakeFramePaced(void);
void KMS_VIDEO_FrameInput(double t);
void KMS_VIDEO_FrameDone(double total, int valid);
int KMS_VIDEO_Latency(double *avg_ms, double *max_ms, int *n);
int KMS_VIDEO_FrameTimes(double *emu_ms, double *wait_ms);
/* Scheduling jitter: p50, p99 and maximum (ms) over the last 600 wake-ups,
   each how late a timed sleep returned, or how long after a flip's vblank
   the loop waiting for it ran again. FALSE while there is none. */
int KMS_VIDEO_Wake(double *p50_ms, double *p99_ms, double *max_ms);

#endif /* KMS_KMS_KMS_VIDEO_H_ */
