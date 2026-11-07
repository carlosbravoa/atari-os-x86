/*
 * kms/zipfile.c - a minimal .zip reader for the Library
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * Finds the end-of-central-directory record in the last 66 KB, walks the
 * central directory and inflates single entries with zlib's raw inflate.
 * Every offset is checked against the file size before use.
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include "atari.h"
#include "util.h"
#include "kms/kms_zipfile.h"

#define SIG_LOCAL    0x04034b50UL
#define SIG_CENTRAL  0x02014b50UL
#define SIG_END      0x06054b50UL
#define LOCAL_SIZE   30
#define CENTRAL_SIZE 46
#define END_SIZE     22
#define TAIL_SEARCH  (65535UL + END_SIZE)
#define CD_MAX       (8UL * 1024UL * 1024UL)   /* sanity limit for the directory */
#define MAX_ITEMS    4096

static unsigned long get16(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8);
}

static unsigned long get32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
	     | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void set_err(char *err, size_t size, const char *text)
{
	if (err && size)
		Util_strlcpy(err, text, size);
}

/* Reads len bytes at off; FALSE on a short read. */
static int read_at(FILE *f, unsigned long off, unsigned char *buf, size_t len)
{
	if (fseek(f, (long)off, SEEK_SET) != 0)
		return FALSE;
	return fread(buf, 1, len, f) == len;
}

/* Opens path and checks it is a regular file of a size fseek can handle. */
static FILE *open_zip(const char *path, unsigned long *size, char *err, size_t errsize)
{
	struct stat st;
	FILE *f = fopen(path, "rb");
	if (!f) {
		set_err(err, errsize, strerror(errno));
		return NULL;
	}
	if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
		set_err(err, errsize, "not a file");
		fclose(f);
		return NULL;
	}
	if ((unsigned long)st.st_size > (unsigned long)LONG_MAX) {
		set_err(err, errsize, "file too big");
		fclose(f);
		return NULL;
	}
	*size = (unsigned long)st.st_size;
	return f;
}

int ZIP_List(const char *path, ZIP_entry **out, char *err, size_t errsize)
{
	FILE *f;
	unsigned long size, tail, cd_off, cd_size, cd_count, end_pos = 0, pos;
	unsigned char *buf = NULL, *cd = NULL;
	ZIP_entry *list = NULL;
	int found = FALSE, n = 0;
	unsigned long i;

	*out = NULL;
	f = open_zip(path, &size, err, errsize);
	if (!f)
		return -1;
	if (size < END_SIZE) {
		set_err(err, errsize, "not a zip file");
		goto fail;
	}

	/* end of central directory: the last signature whose comment length
	   fits the rest of the file */
	tail = size < TAIL_SEARCH ? size : TAIL_SEARCH;
	buf = (unsigned char *)malloc(tail);
	if (!buf || !read_at(f, size - tail, buf, tail)) {
		set_err(err, errsize, "read error");
		goto fail;
	}
	for (pos = tail - END_SIZE + 1; pos-- > 0; ) {
		if (get32(buf + pos) == SIG_END && pos + END_SIZE + get16(buf + pos + 20) <= tail) {
			end_pos = size - tail + pos;
			found = TRUE;
			break;
		}
	}
	if (!found) {
		set_err(err, errsize, "not a zip file");
		goto fail;
	}
	pos = end_pos - (size - tail);
	cd_count = get16(buf + pos + 10);
	cd_size = get32(buf + pos + 12);
	cd_off = get32(buf + pos + 16);
	if (get16(buf + pos + 4) != 0 || get16(buf + pos + 6) != 0) {
		set_err(err, errsize, "multi-part zip");
		goto fail;
	}
	if (cd_count == 0xffffUL || cd_size == 0xffffffffUL || cd_off == 0xffffffffUL) {
		set_err(err, errsize, "zip64 unsupported");
		goto fail;
	}
	if (cd_off > end_pos || cd_size > end_pos - cd_off || cd_size > CD_MAX) {
		set_err(err, errsize, "damaged zip file");
		goto fail;
	}
	free(buf);
	buf = NULL;
	if (cd_count == 0) {
		fclose(f);
		return 0;
	}

	cd = (unsigned char *)malloc(cd_size ? cd_size : 1);
	list = (ZIP_entry *)malloc(sizeof(ZIP_entry) * (cd_count < MAX_ITEMS ? cd_count : MAX_ITEMS));
	if (!cd || !list || !read_at(f, cd_off, cd, cd_size)) {
		set_err(err, errsize, "read error");
		goto fail;
	}

	pos = 0;
	for (i = 0; i < cd_count; i++) {
		const unsigned char *h;
		unsigned long flags, method, name_len, extra_len, comment_len, usize, csize, local;
		if (cd_size - pos < CENTRAL_SIZE || get32(cd + pos) != SIG_CENTRAL) {
			set_err(err, errsize, "damaged zip file");
			goto fail;
		}
		h = cd + pos;
		flags = get16(h + 8);
		method = get16(h + 10);
		csize = get32(h + 20);
		usize = get32(h + 24);
		name_len = get16(h + 28);
		extra_len = get16(h + 30);
		comment_len = get16(h + 32);
		local = get32(h + 42);
		if (cd_size - pos - CENTRAL_SIZE < name_len + extra_len + comment_len) {
			set_err(err, errsize, "damaged zip file");
			goto fail;
		}
		pos += CENTRAL_SIZE + name_len + extra_len + comment_len;

		if (name_len == 0 || name_len >= ZIP_NAME_MAX)
			continue;
		if (h[CENTRAL_SIZE + name_len - 1] == '/' || h[CENTRAL_SIZE + name_len - 1] == '\\')
			continue; /* a directory */
		if (flags & 1)
			continue; /* encrypted */
		if (method != 0 && method != 8)
			continue;
		if (usize > ZIP_MAX_SIZE || csize > ZIP_MAX_SIZE * 2)
			continue;
		if (local > cd_off || (method == 0 && csize != usize))
			continue;
		if (n >= MAX_ITEMS)
			break;
		memcpy(list[n].name, h + CENTRAL_SIZE, name_len);
		list[n].name[name_len] = '\0';
		if (strlen(list[n].name) != name_len)
			continue; /* embedded NUL */
		{
			char *p;
			for (p = list[n].name; *p; p++)
				if (*p == '\\')
					*p = '/';
		}
		list[n].csize = csize;
		list[n].usize = usize;
		list[n].crc = get32(h + 16);
		list[n].local_off = local;
		list[n].method = (int)method;
		n++;
	}
	free(cd);
	fclose(f);
	if (n == 0) {
		free(list);
		list = NULL;
	}
	*out = list;
	return n;

fail:
	free(buf);
	free(cd);
	free(list);
	fclose(f);
	return -1;
}

int ZIP_Extract(const char *path, const ZIP_entry *e, const char *dest, char *err, size_t errsize)
{
#ifdef HAVE_LIBZ
	static unsigned char in[65536], outbuf[65536];
	unsigned char hdr[LOCAL_SIZE];
	unsigned long size, data, left, written = 0, crc;
	FILE *f, *o;
	z_stream z;
	int ok = FALSE, zinit = FALSE, zret = Z_OK;

	f = open_zip(path, &size, err, errsize);
	if (!f)
		return FALSE;
	if (e->local_off > size || size - e->local_off < LOCAL_SIZE
	    || !read_at(f, e->local_off, hdr, LOCAL_SIZE) || get32(hdr) != SIG_LOCAL) {
		set_err(err, errsize, "damaged zip file");
		fclose(f);
		return FALSE;
	}
	data = e->local_off + LOCAL_SIZE + get16(hdr + 26) + get16(hdr + 28);
	if (data > size || size - data < e->csize || fseek(f, (long)data, SEEK_SET) != 0) {
		set_err(err, errsize, "damaged zip file");
		fclose(f);
		return FALSE;
	}
	o = fopen(dest, "wb");
	if (!o) {
		set_err(err, errsize, strerror(errno));
		fclose(f);
		return FALSE;
	}

	crc = crc32(0L, Z_NULL, 0);
	memset(&z, 0, sizeof z);
	if (e->method == 8) {
		if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
			set_err(err, errsize, "zlib error");
			goto done;
		}
		zinit = TRUE;
	}
	left = e->csize;
	for (;;) {
		size_t chunk = left < sizeof in ? (size_t)left : sizeof in;
		if (chunk > 0 && fread(in, 1, chunk, f) != chunk) {
			set_err(err, errsize, "read error");
			goto done;
		}
		left -= (unsigned long)chunk;
		if (e->method == 0) {
			if (chunk == 0)
				break;
			if (fwrite(in, 1, chunk, o) != chunk) {
				set_err(err, errsize, strerror(errno));
				goto done;
			}
			crc = crc32(crc, in, (uInt)chunk);
			written += (unsigned long)chunk;
			continue;
		}
		z.next_in = in;
		z.avail_in = (uInt)chunk;
		do {
			size_t got;
			z.next_out = outbuf;
			z.avail_out = sizeof outbuf;
			zret = inflate(&z, Z_NO_FLUSH);
			if (zret != Z_OK && zret != Z_STREAM_END && !(zret == Z_BUF_ERROR && chunk == 0)) {
				set_err(err, errsize, "damaged zip data");
				goto done;
			}
			got = sizeof outbuf - z.avail_out;
			if (written + got > e->usize) {
				set_err(err, errsize, "damaged zip data");
				goto done;
			}
			if (got && fwrite(outbuf, 1, got, o) != got) {
				set_err(err, errsize, strerror(errno));
				goto done;
			}
			crc = crc32(crc, outbuf, (uInt)got);
			written += (unsigned long)got;
		} while (z.avail_out == 0 && zret != Z_STREAM_END);
		if (zret == Z_STREAM_END)
			break;
		if (chunk == 0) {
			set_err(err, errsize, "damaged zip data");
			goto done;
		}
	}
	if (written != e->usize || (crc & 0xffffffffUL) != e->crc) {
		set_err(err, errsize, "checksum mismatch");
		goto done;
	}
	ok = TRUE;
done:
	if (zinit)
		inflateEnd(&z);
	fclose(f);
	if (fclose(o) != 0 && ok) {
		set_err(err, errsize, strerror(errno));
		ok = FALSE;
	}
	if (!ok)
		unlink(dest);
	return ok;
#else
	(void)path; (void)e; (void)dest;
	set_err(err, errsize, "built without zlib");
	return FALSE;
#endif
}
