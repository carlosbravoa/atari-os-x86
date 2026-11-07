/*
 * kms/input.h - evdev input backend for Atari800
 *
 * Part of atari-os-x86. See kms/video.h for the licence note.
 */
#ifndef KMS_KMS_INPUT_H_
#define KMS_KMS_INPUT_H_

#include <stdio.h>

/* Arrow keys + left Alt act as joystick 0 when TRUE (toggle with F11;
   default FALSE so FujiNet CONFIG and BASIC get cursor keys).
   Inside the built-in menu the arrows always navigate. */
extern int KMS_INPUT_arrows_joystick;

int KMS_INPUT_Initialise(int *argc, char *argv[]);
void KMS_INPUT_Exit(void);
int KMS_INPUT_ReadConfig(char *option, char *parameters);
void KMS_INPUT_WriteConfig(FILE *fp);

/* Drain pending events from every device. Returns the Atari key code to
   feed INPUT_key_code (AKEY_NONE when nothing is held) and updates
   INPUT_key_shift / INPUT_key_consol. */
int KMS_INPUT_Keyboard(void);

/* Joystick state: num 0 = sticks 0/1, 1 = sticks 2/3. */
int KMS_INPUT_Port(int num);
int KMS_INPUT_Trig(int num);

#endif /* KMS_KMS_INPUT_H_ */
