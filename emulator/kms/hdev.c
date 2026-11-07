/*
 * kms/hdev.c - ASH's h: and sd: devices: SIO device $72 backed by the stick
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * ASH, the Atari shell (github.com/carlosbravoa/atari800-atari-shell),
 * talks to the Tang Nano 20K board's file device with plain SIO frames:
 * device $72, unit 1, AUX2 = handle. Its h: is a flat folder of 8.3 files
 * (here atari/hdd on the stick), its sd: the board's whole SD card (here a
 * virtual root with /atari = the stick's atari folder and /usb = the USB
 * drives), and sd:'s control XIOs mount disks, insert cartridges and
 * reboot. This is the same device, reached through atari800's SIO patch
 * (SIO_Handler() in sio.c calls KMS_HDEV_Handler()), after the board's
 * firmware/firmware.c (sio_hdd, hdd_*, sd_*). Anything that reboots or
 * swaps media is queued and done by KMS_HDEV_Poll() from the main loop.
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

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "afile.h"
#include "atari.h"
#include "binload.h"
#include "cartridge.h"
#include "log.h"
#include "memory.h"
#include "sio.h"
#include "util.h"
#include "kms/kms_hdev.h"

#define STICK_USER "/media/atari"   /* MEDIA_USER in init/init.c */
#define USB_DIR    "/media/usb"     /* PID 1 mounts USB drives here */

#define HANDLES    2       /* ASH uses handle 0 (writes) and 1 (the stream) */
#define TEXT_MAX   1024    /* the one listing buffer, as on the board */
#define NAME_W     27      /* sd: listing: names cut to 27 characters */
#define MAX_ENTS   65535   /* the LIST start index is 16 bits */
#define EOL_S      "\233"  /* ATASCII end of line ($9B) */

enum { HK_FREE, HK_FILE, HK_TEXT };

static struct {
	int kind;
	FILE *f;
	int writing;
	dev_t dev;
	ino_t ino;
} hd[HANDLES];

static char text[TEXT_MAX];
static int text_len = 0;
static int text_pos = 0;

static int sd_rw = FALSE;          /* XIO 48; off again at every cold start */
static char boot_target[16] = "";
static int xex_mounted = FALSE;    /* D1: holds a program from XIO 41 */
static char xex_name[64] = "";

/* The one deferred request (XIO 41-44). */
enum { DEF_NONE, DEF_RUN, DEF_MOUNT, DEF_UNMOUNT, DEF_CART, DEF_COLD, DEF_WARM };
static int def_action = DEF_NONE;
static int def_drive = 0;
static char def_path[FILENAME_MAX];

/* ---- small helpers ---------------------------------------------------- */

static const char *base_name(const char *path)
{
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

/* out = dir + "/" + name, cut to size */
static void join(char *out, size_t size, const char *dir, const char *name)
{
	size_t n;
	Util_strlcpy(out, dir, size);
	n = strlen(out);
	if (n + 1 < size) {
		out[n++] = '/';
		Util_strlcpy(out + n, name, size - n);
	}
}

static void drop(int h)
{
	if (hd[h].kind == HK_FILE && hd[h].f != NULL)
		fclose(hd[h].f);
	hd[h].kind = HK_FREE;
	hd[h].f = NULL;
	hd[h].writing = FALSE;
}

static int drive_in(int i)
{
	return SIO_drive_status[i] != SIO_OFF && SIO_drive_status[i] != SIO_NO_DISK;
}

static int cart_in(const CARTRIDGE_image_t *cart)
{
	return cart->type != CARTRIDGE_NONE && cart->type != CARTRIDGE_UNKNOWN;
}

/* Handle h becomes the (one) text listing; any other listing is dropped. */
static void text_begin(int h)
{
	int o;
	drop(h);
	for (o = 0; o < HANDLES; o++)
		if (hd[o].kind == HK_TEXT)
			hd[o].kind = HK_FREE;
	text_len = 0;
	text_pos = 0;
	hd[h].kind = HK_TEXT;
}

/* Append s; FALSE (nothing added) if it would not leave room for the tail. */
static int text_put(const char *s)
{
	int len = (int)strlen(s);
	if (text_len + len + 6 > TEXT_MAX)
		return FALSE;
	memcpy(text + text_len, s, len);
	text_len += len;
	return TRUE;
}

/* ---- directory entries, sorted (directories first, then files) -------- */

typedef struct {
	char *name;
	int dir;
	unsigned long size;
} ent_t;

static ent_t *ents = NULL;
static int n_ents = 0;
static int cap_ents = 0;

static void ents_clear(void)
{
	int i;
	for (i = 0; i < n_ents; i++)
		free(ents[i].name);
	n_ents = 0;
}

static void ent_add(const char *name, int dir, unsigned long size)
{
	if (n_ents >= MAX_ENTS)
		return;
	if (n_ents == cap_ents) {
		cap_ents = cap_ents ? cap_ents * 2 : 64;
		ents = (ent_t *)Util_realloc(ents, cap_ents * sizeof *ents);
	}
	ents[n_ents].name = Util_strdup(name);
	ents[n_ents].dir = dir;
	ents[n_ents].size = size;
	n_ents++;
}

static int ent_cmp(const void *a, const void *b)
{
	const ent_t *x = (const ent_t *)a;
	const ent_t *y = (const ent_t *)b;
	int r;
	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	r = Util_stricmp(x->name, y->name);
	return r ? r : strcmp(x->name, y->name);
}

/* Read and sort dir (dot-files skipped). FALSE if it is not a directory. */
static int ents_read(const char *dir)
{
	DIR *d;
	struct dirent *e;
	ents_clear();
	d = opendir(dir);
	if (d == NULL)
		return FALSE;
	while ((e = readdir(d)) != NULL) {
		char p[FILENAME_MAX];
		struct stat st;
		if (e->d_name[0] == '.')
			continue;
		join(p, sizeof p, dir, e->d_name);
		if (stat(p, &st) != 0)
			continue;
		ent_add(e->d_name, S_ISDIR(st.st_mode), (unsigned long)st.st_size);
	}
	closedir(d);
	qsort(ents, n_ents, sizeof *ents, ent_cmp);
	return TRUE;
}

/* ---- h: (a flat folder of 8.3 files) ---------------------------------- */

static void hdd_dir(char *out, size_t size)
{
	const char *home;
	if (Util_direxists(STICK_USER)) {
		Util_strlcpy(out, STICK_USER "/hdd", size);
		return;
	}
	home = getenv("HOME");
	join(out, size, home && *home ? home : "/run/atari", "hdd");
}

/* "h1:read.me" -> "READ.ME": uppercase, 8.3 (one dot, never leading;
   extra characters dropped), only A-Z 0-9 _ -; a ':' starts over (a
   leading H: or H1:), anything else ('/', spaces) is dropped, so there is
   no way out of the folder. FALSE when nothing is left. */
static int hdd_name(const char *name, char *out)
{
	int k = 0, base = 0, ext = -1;
	const char *p;
	for (p = name; *p; p++) {
		char c = *p;
		if (c >= 'a' && c <= 'z')
			c -= 32;
		if (c == ':') {
			k = base = 0;
			ext = -1;
			continue;
		}
		if (c == '.') {
			if (k == 0 || ext >= 0)
				continue;
			out[k++] = '.';
			ext = 0;
			continue;
		}
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
			continue;
		if (ext >= 0) {
			if (ext >= 3)
				continue;
			ext++;
		}
		else {
			if (base >= 8)
				continue;
			base++;
		}
		out[k++] = c;
	}
	if (k > 0 && out[k - 1] == '.')
		k--;
	out[k] = '\0';
	return k > 0;
}

/* Does a directory entry name fit 8.3 (after uppercasing)? */
static int fits_83(const char *s)
{
	int base = 0, ext = -1;
	for (; *s; s++) {
		char c = *s;
		if (c >= 'a' && c <= 'z')
			c -= 32;
		if (c == '.') {
			if (base == 0 || ext >= 0)
				return FALSE;
			ext = 0;
		}
		else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
			if (ext >= 0) {
				if (++ext > 3)
					return FALSE;
			}
			else if (++base > 8)
				return FALSE;
		}
		else
			return FALSE;
	}
	return base > 0 && ext != 0;
}

/* h: name -> host path; an existing file is found whatever its case. */
static int hdd_resolve(const char *name, char *out, size_t size)
{
	char dir[FILENAME_MAX], nm[16];
	DIR *d;
	struct dirent *e;
	if (!hdd_name(name, nm))
		return FALSE;
	hdd_dir(dir, sizeof dir);
	join(out, size, dir, nm);
	d = opendir(dir);
	if (d != NULL) {
		while ((e = readdir(d)) != NULL)
			if (Util_stricmp(e->d_name, nm) == 0) {
				join(out, size, dir, e->d_name);
				break;
			}
		closedir(d);
	}
	return TRUE;
}

/* ---- sd: (a virtual root: /atari and /usb) ----------------------------- */

#define VP_BAD   0
#define VP_ROOT  1
#define VP_PATH  2

/* ASH shows sd: names cut to 27 characters and cannot type longer ones,
   so a component that names no entry in `dir` but is the unique
   case-insensitive prefix of one is completed to that entry's real name.
   An exact (case-insensitive) match always wins. */
static void sd_complete(const char *dir, char *comp, size_t size)
{
	DIR *d;
	struct dirent *e;
	char found[FILENAME_MAX];
	int matches = 0;
	size_t len = strlen(comp);

	if (len == 0)
		return;
	d = opendir(dir);
	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		if (Util_stricmp(e->d_name, comp) == 0) {
			matches = 1;
			Util_strlcpy(found, e->d_name, sizeof found);
			break;
		}
		if (Util_strnicmp(e->d_name, comp, len) == 0) {
			matches++;
			Util_strlcpy(found, e->d_name, sizeof found);
		}
	}
	closedir(d);
	if (matches == 1 && strlen(found) < size)
		strcpy(comp, found);
}

/* sd: path -> host path. /atari/... is the stick's atari folder, /usb/...
   the USB drives; anything else at the root is refused, and so is any
   ".." component. *depth = number of components, *usb = under /usb. */
static int sd_path(const char *sd, char *out, size_t size, int *depth, int *usb)
{
	char comp[FILENAME_MAX];
	const char *p = sd;
	int n = 0;
	size_t len;

	out[0] = '\0';
	*depth = 0;
	*usb = FALSE;
	for (;;) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		len = strcspn(p, "/");
		if (len >= sizeof comp)
			return VP_BAD;
		memcpy(comp, p, len);
		comp[len] = '\0';
		p += len;
		if (strcmp(comp, ".") == 0)
			continue;
		if (strcmp(comp, "..") == 0)
			return VP_BAD;
		if (n == 0) {
			if (Util_stricmp(comp, "atari") == 0)
				Util_strlcpy(out, STICK_USER, size);
			else if (Util_stricmp(comp, "usb") == 0 && Util_direxists(USB_DIR)) {
				Util_strlcpy(out, USB_DIR, size);
				*usb = TRUE;
			}
			else
				return VP_BAD;
		}
		else {
			if (strlen(out) + 1 + len >= size)
				return VP_BAD;
			sd_complete(out, comp, sizeof comp);
			strcat(out, "/");
			strcat(out, comp);
		}
		n++;
	}
	*depth = n;
	return n == 0 ? VP_ROOT : VP_PATH;
}

/* ---- the write guard --------------------------------------------------- */

static int under_ci(const char *path, const char *dir)
{
	size_t n = strlen(dir);
	return Util_strnicmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

/* Never written from sd:: the virtual folders themselves (/atari, /usb and
   each drive's mount point) and the shell's and the system's own files. */
static int sd_protected(const char *host, int depth, int usb)
{
	if (depth < (usb ? 3 : 2))
		return TRUE;
	return under_ci(host, STICK_USER "/ash") || under_ci(host, STICK_USER "/config");
}

/* Is `other` the file st describes? Same inode, or the same real path
   (case-insensitively: FAT; a cartridge is read and closed, so its inode
   number on vfat may have changed since). */
static int same_file(const char *host, const struct stat *st, const char *other)
{
	struct stat o;
	char a[PATH_MAX], b[PATH_MAX];
	if (other[0] == '\0' || stat(other, &o) != 0)
		return FALSE;
	if (o.st_dev == st->st_dev && o.st_ino == st->st_ino)
		return TRUE;
	return realpath(host, a) && realpath(other, b) && Util_stricmp(a, b) == 0;
}

static int inside(const char *dir_real, const char *other)
{
	char r[PATH_MAX];
	return other[0] && realpath(other, r) && under_ci(r, dir_real) && strlen(r) > strlen(dir_real);
}

/* In use: mounted on a drive, the inserted cartridge, open on the other
   handle; for a directory, holding any of those. Nothing there = free. */
static int busy(const char *host, int h)
{
	struct stat st;
	char real[PATH_MAX];
	int i, isdir;
	const CARTRIDGE_image_t *carts[2];

	if (stat(host, &st) != 0)
		return FALSE;
	isdir = S_ISDIR(st.st_mode);
	if (isdir && !realpath(host, real))
		return TRUE;
	carts[0] = &CARTRIDGE_main;
	carts[1] = &CARTRIDGE_piggyback;
	for (i = 0; i < SIO_MAX_DRIVES; i++)
		if (drive_in(i) && (isdir ? inside(real, SIO_filename[i]) : same_file(host, &st, SIO_filename[i])))
			return TRUE;
	for (i = 0; i < 2; i++)
		if (cart_in(carts[i]) && (isdir ? inside(real, carts[i]->filename) : same_file(host, &st, carts[i]->filename)))
			return TRUE;
	if (!isdir)
		for (i = 0; i < HANDLES; i++)
			if (i != h && hd[i].kind == HK_FILE && hd[i].dev == st.st_dev && hd[i].ino == st.st_ino)
				return TRUE;
	return FALSE;
}

/* ---- opening files on a handle ----------------------------------------- */

static int open_on(int h, const char *host, int mode)
{
	FILE *f;
	struct stat st;
	const char *how = mode == 8 ? "wb" : mode == 9 ? "ab" : "rb";
	if (mode != 8 && mode != 9 && (stat(host, &st) != 0 || !S_ISREG(st.st_mode)))
		return FALSE;
	f = fopen(host, how);
	if (f == NULL) {
		Log_print("hdev: cannot open %s: %s", host, strerror(errno));
		return FALSE;
	}
	if (mode == 9)
		fseek(f, 0, SEEK_END);
	if (fstat(fileno(f), &st) != 0) {
		fclose(f);
		return FALSE;
	}
	hd[h].kind = HK_FILE;
	hd[h].f = f;
	hd[h].writing = mode == 8 || mode == 9;
	hd[h].dev = st.st_dev;
	hd[h].ino = st.st_ino;
	return TRUE;
}

/* OPEN mode 6: the h: directory, DOS style. */
static int hdd_listing(int h)
{
	char dir[FILENAME_MAX], line[48];
	int i;
	hdd_dir(dir, sizeof dir);
	mkdir(dir, 0755);
	if (!ents_read(dir))
		return FALSE;
	text_begin(h);
	for (i = 0; i < n_ents; i++) {
		char nm[16];
		int len;
		if (ents[i].dir || !fits_83(ents[i].name))
			continue;
		Util_strlcpy(nm, ents[i].name, sizeof nm);
		Util_strupper(nm);
		len = snprintf(line, sizeof line, "%-14s%lu" EOL_S, nm, ents[i].size);
		if (text_len + len + 20 > TEXT_MAX)
			break;
		memcpy(text + text_len, line, len);
		text_len += len;
	}
	memcpy(text + text_len, "END OF DIRECTORY" EOL_S, 17);
	text_len += 17;
	return TRUE;
}

static int hdd_open(const char *name, int mode, int h)
{
	char path[FILENAME_MAX], dir[FILENAME_MAX];
	if (h >= HANDLES)
		return FALSE;
	drop(h);
	if (mode == 6)
		return hdd_listing(h);
	if (!hdd_resolve(name, path, sizeof path))
		return FALSE;
	if (mode == 8 || mode == 9) {
		hdd_dir(dir, sizeof dir);
		mkdir(dir, 0755);
		if (busy(path, h)) {
			Log_print("hdev: h: %s is in use, not written", base_name(path));
			return FALSE;
		}
		Log_print("hdev: h: write %s%s", base_name(path), mode == 9 ? " (append)" : "");
	}
	return open_on(h, path, mode);
}

static int hdd_delete(const char *name)
{
	char path[FILENAME_MAX];
	struct stat st;
	if (!hdd_resolve(name, path, sizeof path) || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return FALSE;
	if (busy(path, -1)) {
		Log_print("hdev: h: %s is in use, not deleted", base_name(path));
		return FALSE;
	}
	Log_print("hdev: h: delete %s", base_name(path));
	return unlink(path) == 0;
}

static int hdd_rename(const char *from, const char *to)
{
	char a[FILENAME_MAX], b[FILENAME_MAX];
	struct stat st, st2;
	if (!hdd_resolve(from, a, sizeof a) || !hdd_resolve(to, b, sizeof b))
		return FALSE;
	if (stat(a, &st) != 0 || !S_ISREG(st.st_mode))
		return FALSE;
	if (stat(b, &st2) == 0 && !(st2.st_dev == st.st_dev && st2.st_ino == st.st_ino))
		return FALSE; /* the new name exists */
	if (busy(a, -1)) {
		Log_print("hdev: h: %s is in use, not renamed", base_name(a));
		return FALSE;
	}
	Log_print("hdev: h: rename %s -> %s", base_name(a), base_name(b));
	return rename(a, b) == 0;
}

/* ---- sd: commands ------------------------------------------------------ */

/* XIO 40: "<d|f> <size8> <name>" lines from entry `start` on, then "END",
   or "MORE" when the buffer filled first (ASH lists again from there). */
static int sd_list(const char *sd, int start, int h)
{
	char host[FILENAME_MAX], line[2 + 8 + 1 + NAME_W + 2];
	const char *tail = "END" EOL_S;
	int depth, usb, i, r;

	if (h >= HANDLES)
		return FALSE;
	r = sd_path(sd, host, sizeof host, &depth, &usb);
	if (r == VP_BAD) {
		drop(h);
		return FALSE;
	}
	if (r == VP_ROOT) {
		ents_clear();
		ent_add("atari", TRUE, 0);
		if (Util_direxists(USB_DIR))
			ent_add("usb", TRUE, 0);
	}
	else if (!ents_read(host)) {
		drop(h);
		return FALSE;
	}
	text_begin(h);
	for (i = start; i < n_ents; i++) {
		char num[16] = "";
		if (!ents[i].dir)
			snprintf(num, sizeof num, "%lu", ents[i].size);
		snprintf(line, sizeof line, "%c %8s %.27s" EOL_S, ents[i].dir ? 'd' : 'f', num, ents[i].name);
		if (!text_put(line)) {
			tail = "MORE" EOL_S;
			break;
		}
	}
	text_put(tail);
	return TRUE;
}

static int sd_file(const char *sd, char *host, size_t size, int *depth, int *usb)
{
	return sd_path(sd, host, size, depth, usb) == VP_PATH;
}

/* XIO 47: any sd: file, read-only. */
static int sd_openr(const char *sd, int h)
{
	char host[FILENAME_MAX];
	int depth, usb;
	if (h >= HANDLES)
		return FALSE;
	drop(h);
	if (!sd_file(sd, host, sizeof host, &depth, &usb))
		return FALSE;
	return open_on(h, host, 4);
}

static int sd_refused(const char *what, const char *host, int depth, int usb, int h)
{
	if (sd_protected(host, depth, usb) || busy(host, h)) {
		Log_print("hdev: sd: %s %s refused", what, host);
		return TRUE;
	}
	return FALSE;
}

/* XIO 49: create/truncate for writing. */
static int sd_openw(const char *sd, int h)
{
	char host[FILENAME_MAX];
	int depth, usb;
	if (!sd_rw || h >= HANDLES)
		return FALSE;
	drop(h);
	if (!sd_file(sd, host, sizeof host, &depth, &usb) || sd_refused("write", host, depth, usb, h))
		return FALSE;
	Log_print("hdev: sd: write %s", host);
	return open_on(h, host, 8);
}

/* XIO 50: files and empty directories. */
static int sd_delete(const char *sd, int h)
{
	char host[FILENAME_MAX];
	struct stat st;
	int depth, usb;
	if (!sd_rw || h >= HANDLES)
		return FALSE;
	drop(h);
	if (!sd_file(sd, host, sizeof host, &depth, &usb) || stat(host, &st) != 0 ||
	    sd_refused("delete", host, depth, usb, h))
		return FALSE;
	Log_print("hdev: sd: delete %s", host);
	return (S_ISDIR(st.st_mode) ? rmdir(host) : unlink(host)) == 0;
}

/* XIO 51: rename or move within sd:. */
static int sd_rename(const char *from, const char *to, int h)
{
	char a[FILENAME_MAX], b[FILENAME_MAX];
	struct stat st, st2;
	int da, ua, db, ub;
	if (!sd_rw || h >= HANDLES)
		return FALSE;
	drop(h);
	if (!sd_file(from, a, sizeof a, &da, &ua) || !sd_file(to, b, sizeof b, &db, &ub))
		return FALSE;
	if (stat(a, &st) != 0)
		return FALSE;
	if (stat(b, &st2) == 0 && !(st2.st_dev == st.st_dev && st2.st_ino == st.st_ino))
		return FALSE; /* the new name exists */
	if (sd_refused("rename", a, da, ua, h) || sd_refused("rename to", b, db, ub, h))
		return FALSE;
	Log_print("hdev: sd: rename %s -> %s", a, b);
	return rename(a, b) == 0;
}

/* XIO 52 */
static int sd_mkdir(const char *sd)
{
	char host[FILENAME_MAX];
	int depth, usb;
	if (!sd_rw || !sd_file(sd, host, sizeof host, &depth, &usb) || sd_refused("mkdir", host, depth, usb, -1))
		return FALSE;
	Log_print("hdev: sd: mkdir %s", host);
	return mkdir(host, 0755) == 0;
}

static int xex_on(void)
{
	return xex_mounted || BINLOAD_bin_file != NULL;
}

static const char *boot_now(void)
{
	if (boot_target[0])
		return boot_target;
	/* not told yet: the ASH cartridge in means boot=ash */
	if (cart_in(&CARTRIDGE_main) && Util_striendswith(CARTRIDGE_main.filename, "/ash/ASH.CAR"))
		return "ash";
	return "basic";
}

/* XIO 45: what is mounted where, as a listing. */
static int status_list(int h)
{
	char line[64];
	int i;
	if (h >= HANDLES)
		return FALSE;
	text_begin(h);
	for (i = 0; i < 4; i++) {
		const char *nm = "-";
		if (i == 0 && xex_on() && xex_name[0])
			nm = xex_name;
		else if (drive_in(i))
			nm = base_name(SIO_filename[i]);
		snprintf(line, sizeof line, "d%d: %.30s" EOL_S, i + 1, nm);
		text_put(line);
	}
	snprintf(line, sizeof line, "cart: %.30s" EOL_S,
	         cart_in(&CARTRIDGE_main) ? base_name(CARTRIDGE_main.filename) : "-");
	text_put(line);
	snprintf(line, sizeof line, "xex: %d" EOL_S "boot: %s" EOL_S "END" EOL_S, xex_on() ? 1 : 0, boot_now());
	text_put(line);
	return TRUE;
}

/* XIO 41-44: checked now, done by KMS_HDEV_Poll() on the next frame. */
static int defer(int fn, const char *frame)
{
	char host[FILENAME_MAX];
	struct stat st;
	int depth, usb, drive = 0;
	const char *path = frame + 1;

	if (def_action != DEF_NONE)
		return FALSE; /* one at a time */
	if (fn == 44) {
		if (frame[0] != 'C' && frame[0] != 'W')
			return FALSE;
		def_action = frame[0] == 'C' ? DEF_COLD : DEF_WARM;
		Log_print("hdev: queued %s start", frame[0] == 'C' ? "cold" : "warm");
		return TRUE;
	}
	if (fn == 43)
		path = frame;
	else {
		drive = frame[0] - '0';
		if (drive < 1 || drive > 4)
			return FALSE;
	}
	if (fn == 42) {
		def_action = DEF_UNMOUNT;
		def_drive = drive;
		return TRUE;
	}
	if (!sd_file(path, host, sizeof host, &depth, &usb) || stat(host, &st) != 0 || !S_ISREG(st.st_mode))
		return FALSE;
	Util_strlcpy(def_path, host, sizeof def_path);
	def_drive = drive;
	if (fn == 43)
		def_action = DEF_CART;
	else if (Util_striendswith(host, ".xex"))
		def_action = DEF_RUN; /* = the board's "run" (D1:, cold boot) */
	else
		def_action = DEF_MOUNT;
	return TRUE;
}

static UBYTE xio(int fn, int h, UBYTE *raw)
{
	char *frame = (char *)raw;
	int ok;
	switch (fn) {
	case 32: {
		char *comma;
		raw[63] = 0;
		comma = strchr(frame, ',');
		if (comma == NULL)
			return 'E';
		*comma = '\0';
		ok = hdd_rename(frame, comma + 1);
		break;
	}
	case 33:
		raw[63] = 0;
		ok = hdd_delete(frame);
		break;
	case 40: {
		int start = raw[62] | (raw[63] << 8);
		raw[62] = 0;
		ok = sd_list(frame, start, h);
		break;
	}
	case 41: case 42: case 43: case 44:
		raw[63] = 0;
		ok = defer(fn, frame);
		break;
	case 45:
		ok = status_list(h);
		break;
	case 47:
		raw[63] = 0;
		ok = sd_openr(frame, h);
		break;
	case 48:
		if (raw[0] == 2 && h < HANDLES) {
			text_begin(h);
			text_put(sd_rw ? "rw" EOL_S "END" EOL_S : "ro" EOL_S "END" EOL_S);
			ok = TRUE;
		}
		else if (raw[0] <= 1) {
			sd_rw = raw[0];
			Log_print("hdev: sd: writes %s", sd_rw ? "enabled" : "disabled");
			ok = TRUE;
		}
		else
			ok = FALSE;
		break;
	case 49:
		raw[63] = 0;
		ok = sd_openw(frame, h);
		break;
	case 50:
		raw[63] = 0;
		ok = sd_delete(frame, h);
		break;
	case 51: {
		size_t n;
		raw[63] = 0;
		n = strlen(frame);
		ok = n < 62 && sd_rename(frame, frame + n + 1, h);
		break;
	}
	case 52:
		raw[63] = 0;
		ok = sd_mkdir(frame);
		break;
	default:
		return 'N';
	}
	return ok ? 'C' : 'E';
}

/* ---- the SIO side ------------------------------------------------------ */

static void get_frame(UWORD data, int length, UBYTE *raw)
{
	memset(raw, 0, 64);
	MEMORY_CopyFromMem(data, raw, length < 64 ? (length > 0 ? length : 0) : 64);
}

UBYTE KMS_HDEV_Handler(int cmd, int aux1, int aux2, UWORD data, int length)
{
	UBYTE buf[128];
	int h = aux2 & 3;
	int len = aux1 ? aux1 : 128;
	if (len > 128)
		len = 128;

	switch (cmd) {
	case 0x4f: /* OPEN: aux1 = 4 read, 6 directory, 8 write, 9 append */
		get_frame(data, length, buf);
		buf[63] = 0;
		return hdd_open((char *)buf, aux1, h) ? 'C' : 'E';
	case 0x52: /* READ exactly len bytes; fewer left is an error */
		if (h >= HANDLES || length < len)
			return 'E';
		if (hd[h].kind == HK_TEXT) {
			if (text_len - text_pos < len)
				return 'E';
			MEMORY_CopyToMem((UBYTE *)text + text_pos, data, len);
			text_pos += len;
			return 'C';
		}
		if (hd[h].kind == HK_FILE && !hd[h].writing) {
			long pos = ftell(hd[h].f);
			if (fread(buf, 1, len, hd[h].f) != (size_t)len) {
				clearerr(hd[h].f);
				fseek(hd[h].f, pos, SEEK_SET);
				return 'E';
			}
			MEMORY_CopyToMem(buf, data, len);
			return 'C';
		}
		return 'E';
	case 0x57: /* WRITE */
		if (h >= HANDLES || length < len || hd[h].kind != HK_FILE || !hd[h].writing)
			return 'E';
		MEMORY_CopyFromMem(data, buf, len);
		return fwrite(buf, 1, len, hd[h].f) == (size_t)len ? 'C' : 'E';
	case 0x43: /* CLOSE */
		if (h < HANDLES) {
			int r = 0;
			if (hd[h].kind == HK_FILE)
				r = fclose(hd[h].f);
			hd[h].f = NULL;
			drop(h);
			return r == 0 ? 'C' : 'E';
		}
		return 'E';
	case 0x53: { /* STATUS {err, flags, avail lo, hi}; also ASH's "is h: there" probe */
		unsigned long avail = 0;
		buf[0] = 1;
		buf[1] = buf[2] = buf[3] = 0;
		if (h >= HANDLES || hd[h].kind == HK_FREE)
			buf[0] = 133;
		else {
			if (hd[h].kind == HK_TEXT)
				avail = text_len - text_pos;
			else {
				struct stat st;
				long pos = ftell(hd[h].f);
				fflush(hd[h].f);
				if (fstat(fileno(hd[h].f), &st) == 0 && pos >= 0 && st.st_size > pos)
					avail = (unsigned long)(st.st_size - pos);
			}
			buf[1] = 1 | (avail == 0 ? 2 : 0);
			if (avail > 65535)
				avail = 65535;
			buf[2] = avail & 0xff;
			buf[3] = (avail >> 8) & 0xff;
		}
		MEMORY_CopyToMem(buf, data, length < 4 ? (length > 0 ? length : 0) : 4);
		return 'C';
	}
	case 0x58: /* XIO: aux1 = function, 64-byte frame */
		get_frame(data, length, buf);
		return xio(aux1, h, buf);
	default:
		return 'N';
	}
}

/* ---- the main-loop side ------------------------------------------------ */

void KMS_HDEV_Poll(void)
{
	int a = def_action;
	int r;
	if (a == DEF_NONE)
		return;
	def_action = DEF_NONE;
	switch (a) {
	case DEF_RUN:
		if (AFILE_DetectFileType(def_path) != AFILE_XEX) {
			Log_print("hdev: run %s: not a program", def_path);
			break;
		}
		Log_print("hdev: run %s (D1:, cold start)", def_path);
		/* the program gets the boot, not the shell's cartridge (the
		   board drops its cartridge too) */
		CARTRIDGE_Remove();
		if (AFILE_OpenFile(def_path, TRUE, 1, FALSE) == AFILE_ERROR) {
			Log_print("hdev: run %s failed", def_path);
			Atari800_Coldstart();
			break;
		}
		xex_mounted = TRUE;
		Util_strlcpy(xex_name, base_name(def_path), sizeof xex_name);
		break;
	case DEF_MOUNT:
		if (SIO_Mount(def_drive, def_path, FALSE))
			Log_print("hdev: D%d: %s", def_drive, def_path);
		else
			Log_print("hdev: D%d: mount %s failed", def_drive, def_path);
		if (def_drive == 1)
			xex_mounted = FALSE;
		break;
	case DEF_UNMOUNT:
		Log_print("hdev: D%d: unmounted", def_drive);
		SIO_Dismount(def_drive);
		if (def_drive == 1)
			xex_mounted = FALSE;
		break;
	case DEF_CART:
		r = CARTRIDGE_Insert(def_path);
		if (r == CARTRIDGE_CANT_OPEN || r == CARTRIDGE_BAD_FORMAT || r == CARTRIDGE_TOO_FEW_DATA)
			Log_print("hdev: cartridge %s: not a cartridge", def_path);
		else if (r > 0) {
			/* a raw ROM: the common types by size, nothing to ask here */
			if (r == 8 || r == 16) {
				CARTRIDGE_SetType(&CARTRIDGE_main, r == 8 ? CARTRIDGE_STD_8 : CARTRIDGE_STD_16);
				Log_print("hdev: cartridge %s (raw %d KB)", def_path, r);
			}
			else {
				Log_print("hdev: cartridge %s: raw %d KB, type unknown", def_path, r);
				CARTRIDGE_Remove();
			}
		}
		else
			Log_print("hdev: cartridge %s", def_path);
		/* the old cartridge is gone either way: start again */
		Atari800_Coldstart();
		break;
	case DEF_COLD:
		Log_print("hdev: cold start");
		Atari800_Coldstart();
		break;
	case DEF_WARM:
		Log_print("hdev: warm start");
		Atari800_Warmstart();
		break;
	default:
		break;
	}
}

void KMS_HDEV_ColdStart(void)
{
	int h;
	if (sd_rw)
		Log_print("hdev: cold start, sd: writes disabled");
	sd_rw = FALSE;
	for (h = 0; h < HANDLES; h++)
		drop(h);
}

void KMS_HDEV_SetBootTarget(const char *target)
{
	Util_strlcpy(boot_target, target, sizeof boot_target);
}
