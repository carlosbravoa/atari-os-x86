/*
 * kms/kms_zipfile.h - a minimal .zip reader for the Library
 *
 * Part of atari-os-x86. See kms_video.h for the licence note.
 *
 * Stored (0) and deflated (8) entries, sizes from the central directory,
 * no zip64, no encryption. Enough for the zipped Atari collections people
 * have; atari800 itself only reads .gz.
 */
#ifndef KMS_KMS_ZIPFILE_H_
#define KMS_KMS_ZIPFILE_H_

#include <stddef.h>

#define ZIP_NAME_MAX  256
#define ZIP_MAX_SIZE  (16UL * 1024UL * 1024UL)  /* larger entries are skipped */

typedef struct {
	char name[ZIP_NAME_MAX];  /* path inside the archive, '/' separated */
	unsigned long csize;      /* compressed size */
	unsigned long usize;      /* uncompressed size */
	unsigned long crc;
	unsigned long local_off;  /* offset of the local file header */
	int method;
} ZIP_entry;

/* Reads the central directory of a zip file. Directories, encrypted
   entries, unsupported methods and entries over ZIP_MAX_SIZE are left out.
   Returns the number of entries (*out is malloc'ed, free() it; NULL when
   0) or -1 with a short reason in err. */
int ZIP_List(const char *path, ZIP_entry **out, char *err, size_t errsize);

/* Writes one entry to dest (created or truncated). Returns 1, or 0 with a
   short reason in err (dest is removed then). */
int ZIP_Extract(const char *path, const ZIP_entry *e, const char *dest, char *err, size_t errsize);

#endif /* KMS_KMS_ZIPFILE_H_ */
