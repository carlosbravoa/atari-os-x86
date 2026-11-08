/*
 * kms/kms_hdev.h - ASH's h:/sd: device: SIO device $72 backed by the stick
 *
 * Part of atari-os-x86. See kms_video.h for the licence note.
 */
#ifndef KMS_KMS_HDEV_H_
#define KMS_KMS_HDEV_H_

#include "atari.h" /* UBYTE, UWORD */

/* Called from SIO_Handler() (sio.c, SIO patch) for DDEVIC $72. Moves
   `length` bytes between the Atari's memory at `data` and the host.
   Returns 'C' (complete), 'E' (error) or 'N' (NAK: unknown/malformed). */
UBYTE KMS_HDEV_Handler(int cmd, int aux1, int aux2, UWORD data, int length);

/* Once per frame from the main loop: runs a deferred MOUNT/UNMOUNT/CART/
   BOOT request (never done inside the SIO handler). */
void KMS_HDEV_Poll(void);

/* Called from Atari800_Coldstart(): sd: writes are switched off again. */
void KMS_HDEV_ColdStart(void);

/* Called from KMS_BOOT_EjectAll(): the XIO 41 program is out of D1: too. */
void KMS_HDEV_EjectAll(void);

#endif /* KMS_KMS_HDEV_H_ */
