/*
 * kms/kms_help.h - the F1 Help page: every key, the menus, the gamepad
 *
 * Part of atari-os-x86. See kms_video.h for the licence note.
 */
#ifndef KMS_KMS_HELP_H_
#define KMS_KMS_HELP_H_

/* Set by the input layer when F1 is pressed; the main loop shows the
   Help page on the next frame. */
extern int KMS_HELP_requested;

/* The Help page (paged; any key turns the page, Esc closes). Also F8 >
   "Help: keys and menus"; works from inside a menu too. */
void KMS_HELP_Run(void);

/* F8 > Controls > "Gamepad buttons": the gamepad mapping. */
void KMS_HELP_GamepadPage(void);

#endif /* KMS_KMS_HELP_H_ */
