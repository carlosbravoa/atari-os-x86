/*
 * kms/kms_sysmenu.h - the F8 "System" menu (talks to PID 1 over /run/atari.ctl)
 *
 * Part of atari-os-x86. See kms_video.h for the licence note.
 */
#ifndef KMS_KMS_SYSMENU_H_
#define KMS_KMS_SYSMENU_H_

#include <stdio.h>

/* Set by the input layer when F8 (or the gamepad's Guide button) is
   pressed; the main loop opens the menu on the next frame. */
extern int KMS_SYSMENU_requested;

/* TRUE while atari800's own menu (UI_Run) runs from F8 > "Advanced:
   emulator menu...": its "System menu" entry then sets KMS_SYSMENU_reopen
   and closes, back to the System menu that opened it (ui.c patch). */
extern int KMS_SYSMENU_advanced_open;
extern int KMS_SYSMENU_reopen;

/* Persisted in the emulator config as KMS_VOLUME / KMS_BRIGHTNESS. */
extern int KMS_SYSMENU_volume;     /* 0..100, -1 = untouched */
extern int KMS_SYSMENU_brightness; /* 0..100, -1 = untouched */
extern int KMS_SYSMENU_hint_done;  /* the F8 hint has been shown and used */

void KMS_SYSMENU_Run(void);
/* F1, F8 or F9 was opened: the boot hint ("F1 = Help ...") goes and does
   not come back (KMS_HINT_DONE). */
void KMS_SYSMENU_HintSeen(void);
/* The build this stick runs (init's VERSION: git describe, e.g. "v0.2-3-g724be30"
   or a bare hash), "?" until init answers. Shown at the foot of the menus. */
const char *KMS_SYSMENU_Version(void);
/* Volume/brightness hotkeys (evdev KEY_* codes). */
void KMS_SYSMENU_HotKey(int keycode);
void KMS_SYSMENU_ApplySettings(void); /* after sound/video init */
int KMS_SYSMENU_ReadConfig(char *option, char *parameters);
void KMS_SYSMENU_WriteConfig(FILE *fp);

/* Provided by sound_alsa.c */
int ALSA_GetVolume(void);
void ALSA_SetVolume(int percent);

#endif /* KMS_KMS_SYSMENU_H_ */
