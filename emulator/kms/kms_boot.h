/*
 * kms/kms_boot.h - what a cold start boots: the mounted media, else the
 * boot target (BASIC, No BASIC, ASH, FujiNet CONFIG)
 *
 * Part of atari-os-x86. See kms_video.h for the licence note.
 */
#ifndef KMS_KMS_BOOT_H_
#define KMS_KMS_BOOT_H_

/* One-shot, set by the Library's "Run with BASIC": the next cold start
   that boots a mounted disk keeps BASIC on. */
extern int KMS_BOOT_with_basic;

/* Called first thing in Atari800_Coldstart() (atari.c). With something
   to boot mounted (a disk in D1:, a program, a cartridge other than
   ASH.CAR, a tape set to boot) the ASH cartridge comes out and BASIC goes
   off for this boot; with nothing mounted the boot target decides. */
void KMS_BOOT_ColdStart(void);

/* "basic", "nobasic", "ash" or "fujinet", from init's status reply. */
void KMS_BOOT_SetTarget(const char *target);
/* The target as known ("" when init was not asked), or inferred from the
   cartridge/NetSIO state when not known; never NULL. */
const char *KMS_BOOT_Target(void);
/* "BASIC", "No BASIC", "ASH", "FujiNet CONFIG". */
const char *KMS_BOOT_TargetLabel(const char *target);

/* Anything that "Eject all" would take out: a disk in D1:-D8:, a
   cartridge other than ASH.CAR, a tape, a program being loaded. */
int KMS_BOOT_SomethingMounted(void);
/* Eject all: dismount D1:-D8:, remove a cartridge other than ASH.CAR,
   remove the tape, then cold start (the boot target applies). */
void KMS_BOOT_EjectAll(void);

/* What the last cold start booted, e.g. "disk D1, BASIC off". */
const char *KMS_BOOT_LastRoute(void);
/* TRUE when the last cold start left BASIC on. */
int KMS_BOOT_BasicOn(void);

#endif /* KMS_KMS_BOOT_H_ */
