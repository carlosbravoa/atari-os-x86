/*
 * kms/kms_library.h - the F9 "Library": browse, run and copy Atari files
 *
 * Part of atari-os-x86. See kms_video.h for the licence note.
 */
#ifndef KMS_KMS_LIBRARY_H_
#define KMS_KMS_LIBRARY_H_

/* Set by the input layer when F9 is pressed; the main loop opens the
   Library on the next frame. */
extern int KMS_LIBRARY_requested;

/* Opens the Library. Returns TRUE when a program was started (the caller,
   e.g. the F1 menu, should then go straight back to the emulation). */
int KMS_LIBRARY_Run(void);

/* Called once per frame from the main loop: watches /media/usb for new
   drives and shows the border hint. Cheap when nothing happens. */
void KMS_LIBRARY_Poll(void);

#endif /* KMS_KMS_LIBRARY_H_ */
