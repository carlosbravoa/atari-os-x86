/*
 * kms/kms_sysmenu.h - the F8 "System" menu (talks to PID 1 over /run/atari.ctl)
 *
 * Part of atari-os-x86. See kms_video.h for the licence note.
 */
#ifndef KMS_KMS_SYSMENU_H_
#define KMS_KMS_SYSMENU_H_

#include <stdio.h>

/* Set by the input layer when F8 (or the gamepad guide button with a
   modifier) is pressed; the main loop opens the menu on the next frame. */
extern int KMS_SYSMENU_requested;

/* Persisted in the emulator config as KMS_VOLUME / KMS_BRIGHTNESS. */
extern int KMS_SYSMENU_volume;     /* 0..100, -1 = untouched */
extern int KMS_SYSMENU_brightness; /* 0..100, -1 = untouched */

void KMS_SYSMENU_Run(void);
/* Volume/brightness hotkeys (evdev KEY_* codes). */
void KMS_SYSMENU_HotKey(int keycode);
void KMS_SYSMENU_ApplySettings(void); /* after sound/video init */
int KMS_SYSMENU_ReadConfig(char *option, char *parameters);
void KMS_SYSMENU_WriteConfig(FILE *fp);

/* Provided by sound_alsa.c */
int ALSA_GetVolume(void);
void ALSA_SetVolume(int percent);

#endif /* KMS_KMS_SYSMENU_H_ */
