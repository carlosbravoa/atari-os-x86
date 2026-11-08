/*
 * kms/library.c - the F9 "Library": browse, run and copy Atari files
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * Sources, cycled with Tab: the whole stick (its atari/ folder, opening in
 * library/; FujiNet's SD is fujinet/SD there) and every USB drive PID 1
 * has mounted under /media/usb. RETURN on a file offers Run, Run with
 * BASIC, Mount, Copy to the library, FujiNet's SD or a USB drive, and
 * Favourite. Recent programs and favourites persist in
 * atari/config/library.cfg. Drawn with atari800's UI driver, like the F1
 * and F8 menus; everything happens on the host side. A .zip is opened
 * here too: its Atari files are listed, the chosen one is extracted to
 * /run/atari-zip and then treated like any other file; Recent and
 * Favourites remember it as "<zip path>!<entry>".
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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include "afile.h"
#include "akey.h"
#include "atari.h"
#include "cartridge.h"
#include "cassette.h"
#include "log.h"
#include "platform.h"
#include "sio.h"
#include "ui.h"
#include "util.h"
#include "kms/kms_boot.h"
#include "kms/kms_library.h"
#include "kms/kms_video.h"
#include "kms/kms_zipfile.h"

#define STICK_USER   "/media/atari"   /* MEDIA_USER in init/init.c */
#define USB_DIR      "/media/usb"     /* PID 1 mounts USB drives here */
#define ZIP_RUN_DIR  "/run/atari-zip" /* zipped files are extracted here (tmpfs) */

#define MAX_SOURCES  10
#define MAX_VOLS     (MAX_SOURCES - 1)
#define MAX_ENTRIES  2000
#define MAX_RECENT   8
#define MAX_FAVS     32
#define SHOW_FAVS    12   /* favourites listed in the block at the Library root */
#define PAGE         56   /* directory entries per view; the UI driver takes 100 items at most */
#define MENU_MAX     96

/* Menu return values. Labels are -1 and never selectable. */
#define RV_FILTER    1
#define RV_PARENT    2
#define RV_PREV      3
#define RV_NEXT      4
#define RV_RECENT    100
#define RV_FAV       200
#define RV_ENTRY     1000

/* Action popup return values. */
#define ACT_RUN      1
#define ACT_MOUNT    2
#define ACT_RUN_BASIC 3
#define ACT_EJECT    4
#define ACT_COPY_LIB 21
#define ACT_COPY_SD  22
#define ACT_COPY_TO  23
#define ACT_COPY_USB 30   /* + source index, in the "Copy to..." popup */
#define ACT_FAV      50
#define ACT_CANCEL   51

int KMS_LIBRARY_requested = FALSE;

enum { SRC_STICK, SRC_USB };

typedef struct {
	int kind;
	char name[48];             /* "Stick", "USB GAMES" */
	char root[FILENAME_MAX];
	char start[32];            /* where the tab opens: "library" on the stick */
} lib_source;

typedef struct {
	char *name;
	int isdir;
	unsigned long size;
} lib_entry;

static lib_source sources[MAX_SOURCES];
static int n_sources = 0;

static lib_entry entries[MAX_ENTRIES];
static int n_entries = 0;
static int visible[MAX_ENTRIES]; /* indices into entries[] that pass the filter */
static int n_visible = 0;

static char recent[MAX_RECENT][FILENAME_MAX];
static int n_recent = 0;
static char favs[MAX_FAVS][FILENAME_MAX];
static int n_favs = 0;

static UI_tMenuItem menu[MENU_MAX + 1];
static char menu_text[MENU_MAX][40];
static char menu_suffix[MENU_MAX][12];
static int n_menu = 0;

static double usb_hint_until = 0;

/* ---- small helpers --------------------------------------------------- */

static int have_stick(void)
{
	return Util_direxists(STICK_USER);
}

static void home_path(char *out, size_t size, const char *leaf)
{
	const char *home = getenv("HOME");
	snprintf(out, size, "%.3000s/%s", home ? home : "/tmp", leaf);
}

static void join_path(char *out, size_t size, const char *a, const char *b)
{
	Util_strlcpy(out, a, size);
	if (b[0]) {
		size_t len = strlen(out);
		if (len + 1 < size) {
			out[len++] = '/';
			out[len] = '\0';
		}
		Util_strlcpy(out + len, b, size - len);
	}
}

static const char *base_name(const char *path)
{
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}

/* mkdir -p */
static int make_dirs(const char *dir)
{
	char tmp[FILENAME_MAX];
	char *p;
	Util_strlcpy(tmp, dir, sizeof tmp);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return FALSE;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return FALSE;
	return TRUE;
}

static int contains_nocase(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	if (n == 0)
		return TRUE;
	for (; *hay; hay++)
		if (Util_strnicmp(hay, needle, n) == 0)
			return TRUE;
	return FALSE;
}

static void format_size(unsigned long size, char *out, size_t n)
{
	const unsigned long mb = 1024UL * 1024UL;
	if (size < 1024)
		snprintf(out, n, "%luB", size);
	else if (size < mb)
		snprintf(out, n, "%luK", (size + 1023) / 1024);
	else
		snprintf(out, n, "%lu.%luM", size / mb, (size % mb) * 10 / mb);
}

static void message(const char *text)
{
	UI_driver->fMessage(text, TRUE);
}

/* The UI driver returns Esc everywhere once a hotkey (F5, a signal from
   init) has asked to leave the menus. */
static int aborted(void)
{
	return UI_alt_function >= 0;
}

/* ---- file types ------------------------------------------------------- */

static const char *const media_ext[] = {
	".atr", ".xfd", ".dcm", ".atx", ".pro", ".xex", ".com", ".exe", ".bas",
	".lst", ".car", ".rom", ".bin", ".cas", ".zip", ".a8s", ".atr.gz", ".xfd.gz", NULL
};

static int is_media(const char *name)
{
	int i;
	for (i = 0; media_ext[i]; i++)
		if (Util_striendswith(name, media_ext[i]))
			return TRUE;
	return FALSE;
}

enum { FK_DISK, FK_CART, FK_TAPE, FK_PROGRAM, FK_OTHER };

static int file_kind(const char *name)
{
	static const char *const cart[] = { ".car", ".rom", ".bin", NULL };
	static const char *const prog[] = { ".xex", ".com", ".exe", ".bas", ".lst", NULL };
	int i;
	for (i = 0; cart[i]; i++)
		if (Util_striendswith(name, cart[i]))
			return FK_CART;
	for (i = 0; prog[i]; i++)
		if (Util_striendswith(name, prog[i]))
			return FK_PROGRAM;
	if (Util_striendswith(name, ".cas"))
		return FK_TAPE;
	if (Util_striendswith(name, ".zip") || Util_striendswith(name, ".a8s"))
		return FK_OTHER;
	return FK_DISK;
}

static int is_zip(const char *name)
{
	return Util_striendswith(name, ".zip");
}

static int is_regular(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* "/x/PACK.ZIP!GAMES/A.XEX" (a zipped file in Recent or Favourites):
   copies the zip path to zip and points *entry at the name inside it.
   FALSE when ref is a plain path. Several ".zip!" in one path: the first
   whose zip exists wins. */
static int split_zip_ref(const char *ref, char *zip, size_t size, const char **entry)
{
	const char *p = ref, *first = NULL;
	while ((p = strchr(p, '!')) != NULL) {
		if (p - ref >= 4 && Util_strnicmp(p - 4, ".zip", 4) == 0 && p[1] != '\0') {
			size_t len = (size_t)(p - ref);
			if (!first)
				first = p;
			if (len < size) {
				memcpy(zip, ref, len);
				zip[len] = '\0';
				if (is_regular(zip)) {
					*entry = p + 1;
					return TRUE;
				}
			}
		}
		p++;
	}
	if (!first || (size_t)(first - ref) >= size)
		return FALSE;
	memcpy(zip, ref, (size_t)(first - ref));
	zip[first - ref] = '\0';
	*entry = first + 1;
	return TRUE;
}

/* out = a + "!" + b (truncated to size) */
static void bang_join(char *out, size_t size, const char *a, const char *b)
{
	size_t len;
	Util_strlcpy(out, a, size);
	len = strlen(out);
	if (len + 1 < size) {
		out[len++] = '!';
		out[len] = '\0';
		Util_strlcpy(out + len, b, size - len);
	}
}

/* What a path is called on screen: its file name, "NAME.ZIP!FILE.XEX" for
   a zipped one. */
static void display_name(const char *ref, char *out, size_t size)
{
	char zip[FILENAME_MAX];
	const char *entry;
	if (split_zip_ref(ref, zip, sizeof zip, &entry))
		bang_join(out, size, base_name(zip), base_name(entry));
	else
		Util_strlcpy(out, base_name(ref), size);
}

/* ---- sources ---------------------------------------------------------- */

static int name_cmp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

/* Entries of /media/usb that are mount points (another device than the
   directory itself). */
static int list_volumes(char names[][64], int max)
{
	DIR *d;
	struct dirent *e;
	struct stat top, st;
	char path[FILENAME_MAX];
	int n = 0;

	if (stat(USB_DIR, &top) != 0)
		return 0;
	d = opendir(USB_DIR);
	if (!d)
		return 0;
	while ((e = readdir(d)) != NULL && n < max) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, USB_DIR "/%.200s", e->d_name);
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev != top.st_dev)
			Util_strlcpy(names[n++], e->d_name, 64);
	}
	closedir(d);
	qsort(names, (size_t)n, sizeof names[0], name_cmp);
	return n;
}

/* Where "Copy to Library" and "Copy to FujiNet SD" put files. */
static void library_dir(char *out, size_t size)
{
	if (have_stick())
		Util_strlcpy(out, STICK_USER "/library", size);
	else
		home_path(out, size, "library");
}

static void sd_dir(char *out, size_t size)
{
	Util_strlcpy(out, have_stick() ? STICK_USER "/fujinet/SD" : "/run/fujinet/SD", size);
}

static void build_sources(void)
{
	char vols[MAX_VOLS][64];
	int i, nv = list_volumes(vols, MAX_VOLS);

	n_sources = 0;
	sources[0].kind = SRC_STICK;
	if (have_stick()) {
		/* the whole stick: library/, fujinet/SD, disks/, ash/, ... */
		Util_strlcpy(sources[0].name, "Stick", sizeof sources[0].name);
		Util_strlcpy(sources[0].root, STICK_USER, sizeof sources[0].root);
		Util_strlcpy(sources[0].start, "library", sizeof sources[0].start);
	}
	else {
		Util_strlcpy(sources[0].name, "Library", sizeof sources[0].name);
		home_path(sources[0].root, sizeof sources[0].root, "library");
		sources[0].start[0] = '\0';
	}
	n_sources++;
	for (i = 0; i < nv; i++) {
		lib_source *s = &sources[n_sources++];
		s->kind = SRC_USB;
		snprintf(s->name, sizeof s->name, "USB %.40s", vols[i]);
		snprintf(s->root, sizeof s->root, USB_DIR "/%s", vols[i]);
		s->start[0] = '\0';
	}
}

/* Not listed at the stick's root: nothing runnable, and ssh/ holds keys. */
static int hidden_on_stick(const char *name)
{
	return strcmp(name, "config") == 0 || strcmp(name, "ssh") == 0 || strcmp(name, "logs") == 0;
}

static int find_source(const char *root)
{
	int i;
	for (i = 0; i < n_sources; i++)
		if (strcmp(sources[i].root, root) == 0)
			return i;
	return -1;
}

static int below(const char *path, const char *dir)
{
	size_t len = strlen(dir);
	return strncmp(path, dir, len) == 0 && path[len] == '/';
}

/* The source a path lives in, or -1. */
static int source_of(const char *path)
{
	int i;
	for (i = n_sources - 1; i >= 0; i--) /* USB drives before the stick */
		if (below(path, sources[i].root))
			return i;
	return -1;
}

/* What a copy keeps of PATH's folders: the path below the library, the
   FujiNet SD folder or a USB drive's root; below the stick's other
   folders (disks/, ash/, ...) without that folder; else just the name. */
static const char *copy_rel(const char *path)
{
	char dir[FILENAME_MAX];
	int i;
	library_dir(dir, sizeof dir);
	if (below(path, dir))
		return path + strlen(dir) + 1;
	sd_dir(dir, sizeof dir);
	if (below(path, dir))
		return path + strlen(dir) + 1;
	for (i = 1; i < n_sources; i++)
		if (below(path, sources[i].root))
			return path + strlen(sources[i].root) + 1;
	if (have_stick() && below(path, STICK_USER)) {
		const char *rest = strchr(path + strlen(STICK_USER) + 1, '/');
		if (rest)
			return rest + 1;
	}
	return base_name(path);
}

/* ---- recent and favourites ------------------------------------------- */

static void cfg_path(char *out, size_t size)
{
	if (have_stick())
		Util_strlcpy(out, STICK_USER "/config/library.cfg", size);
	else
		home_path(out, size, "library.cfg");
}

static void load_cfg(void)
{
	char path[FILENAME_MAX], line[FILENAME_MAX + 16];
	FILE *f;

	n_recent = n_favs = 0;
	cfg_path(path, sizeof path);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		Util_chomp(line);
		if (strncmp(line, "recent=", 7) == 0 && line[7] == '/' && n_recent < MAX_RECENT)
			Util_strlcpy(recent[n_recent++], line + 7, FILENAME_MAX);
		else if (strncmp(line, "fav=", 4) == 0 && line[4] == '/' && n_favs < MAX_FAVS)
			Util_strlcpy(favs[n_favs++], line + 4, FILENAME_MAX);
	}
	fclose(f);
}

static void save_cfg(void)
{
	char path[FILENAME_MAX], tmp[FILENAME_MAX + 8];
	FILE *f;
	int i;

	cfg_path(path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	f = fopen(tmp, "w");
	if (!f) {
		Log_print("library: cannot write %s: %s", tmp, strerror(errno));
		return;
	}
	for (i = 0; i < n_recent; i++)
		fprintf(f, "recent=%s\n", recent[i]);
	for (i = 0; i < n_favs; i++)
		fprintf(f, "fav=%s\n", favs[i]);
	fflush(f);
	fsync(fileno(f));
	if (fclose(f) != 0 || rename(tmp, path) != 0)
		Log_print("library: cannot write %s: %s", path, strerror(errno));
}

static void add_recent(const char *path)
{
	char keep[FILENAME_MAX];
	int i, j;
	Util_strlcpy(keep, path, sizeof keep);
	for (i = j = 0; i < n_recent; i++)
		if (strcmp(recent[i], keep) != 0) {
			if (i != j)
				Util_strlcpy(recent[j], recent[i], FILENAME_MAX);
			j++;
		}
	n_recent = j < MAX_RECENT ? j : MAX_RECENT - 1;
	for (i = n_recent; i > 0; i--)
		Util_strlcpy(recent[i], recent[i - 1], FILENAME_MAX);
	Util_strlcpy(recent[0], keep, FILENAME_MAX);
	n_recent++;
}

static int fav_index(const char *path)
{
	int i;
	for (i = 0; i < n_favs; i++)
		if (strcmp(favs[i], path) == 0)
			return i;
	return -1;
}

static void toggle_fav(const char *path)
{
	int i = fav_index(path);
	if (i >= 0) {
		for (; i + 1 < n_favs; i++)
			Util_strlcpy(favs[i], favs[i + 1], FILENAME_MAX);
		n_favs--;
	}
	else if (n_favs < MAX_FAVS)
		Util_strlcpy(favs[n_favs++], path, FILENAME_MAX);
	else {
		message("Too many favourites");
		return;
	}
	save_cfg();
}

/* ---- directory listing ----------------------------------------------- */

static void free_entries(void)
{
	int i;
	for (i = 0; i < n_entries; i++)
		free(entries[i].name);
	n_entries = 0;
	n_visible = 0;
}

static int entry_cmp(const void *a, const void *b)
{
	const lib_entry *x = (const lib_entry *)a;
	const lib_entry *y = (const lib_entry *)b;
	int r;
	if (x->isdir != y->isdir)
		return x->isdir ? -1 : 1;
	r = Util_stricmp(x->name, y->name);
	return r ? r : strcmp(x->name, y->name);
}

static int read_dir(const char *dir, const char *filter, int stick_root)
{
	DIR *d;
	struct dirent *e;
	char path[FILENAME_MAX];
	int i;

	free_entries();
	d = opendir(dir);
	if (!d)
		return FALSE;
	while ((e = readdir(d)) != NULL) {
		struct stat st;
		if (e->d_name[0] == '.' || (stick_root && hidden_on_stick(e->d_name)))
			continue;
		join_path(path, sizeof path, dir, e->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (!S_ISDIR(st.st_mode) && !(S_ISREG(st.st_mode) && is_media(e->d_name)))
			continue;
		if (n_entries >= MAX_ENTRIES) {
			Log_print("library: more than %d entries in %s, rest not shown", MAX_ENTRIES, dir);
			break;
		}
		entries[n_entries].name = Util_strdup(e->d_name);
		entries[n_entries].isdir = S_ISDIR(st.st_mode);
		entries[n_entries].size = (unsigned long)st.st_size;
		n_entries++;
	}
	closedir(d);
	qsort(entries, (size_t)n_entries, sizeof entries[0], entry_cmp);
	for (i = 0; i < n_entries; i++)
		if (entries[i].isdir || contains_nocase(entries[i].name, filter))
			visible[n_visible++] = i;
	return TRUE;
}

/* ---- menu building ----------------------------------------------------- */

static void menu_reset(void)
{
	n_menu = 0;
	menu[0].flags = UI_ITEM_END;
}

static void menu_add(int retval, const char *prefix, const char *text, const char *suffix)
{
	UI_tMenuItem *m;
	if (n_menu >= MENU_MAX)
		return;
	m = &menu[n_menu];
	Util_strlcpy(menu_text[n_menu], text, sizeof menu_text[0]);
	m->flags = UI_ITEM_ACTION;
	m->retval = (SWORD)retval;
	m->prefix = prefix;
	m->item = menu_text[n_menu];
	if (suffix) {
		Util_strlcpy(menu_suffix[n_menu], suffix, sizeof menu_suffix[0]);
		m->suffix = menu_suffix[n_menu];
	}
	else
		m->suffix = NULL;
	n_menu++;
	menu[n_menu].flags = UI_ITEM_END;
	menu[n_menu].retval = 0;
	menu[n_menu].prefix = NULL;
	menu[n_menu].item = NULL;
	menu[n_menu].suffix = NULL;
}

/* A file or directory line: one-character prefix (" ", "*" favourite,
   "?" not plugged in), the name cut so the size stays visible. */
static void menu_add_file(int retval, const char *prefix, const char *name, int isdir, unsigned long size, int known)
{
	char text[40], suffix[24];
	int room;
	suffix[0] = '\0';
	if (!isdir && known)
		format_size(size, suffix, sizeof suffix);
	room = 38 - 1 - (int)strlen(suffix) - (suffix[0] ? 1 : 0);
	if (isdir) {
		snprintf(text, sizeof text, "%.*s/", room - 1, name);
	}
	else
		snprintf(text, sizeof text, "%.*s", room, name);
	menu_add(retval, prefix, text, suffix[0] ? suffix : NULL);
}

static void menu_add_path(int retval, const char *path)
{
	struct stat st;
	int there;
	char zip[FILENAME_MAX], shown[FILENAME_MAX];
	const char *entry;
	if (split_zip_ref(path, zip, sizeof zip, &entry)) {
		there = is_regular(zip);
		display_name(path, shown, sizeof shown);
		menu_add_file(retval, !there ? "?" : fav_index(path) >= 0 ? "*" : " ", shown, FALSE, 0, FALSE);
		return;
	}
	there = stat(path, &st) == 0 && S_ISREG(st.st_mode);
	menu_add_file(retval, !there ? "?" : fav_index(path) >= 0 ? "*" : " ",
	              base_name(path), FALSE, there ? (unsigned long)st.st_size : 0, there);
}

static int menu_has(int retval)
{
	int i;
	for (i = 0; i < n_menu; i++)
		if (menu[i].retval == retval)
			return TRUE;
	return FALSE;
}

static int menu_first_selectable(void)
{
	int i;
	for (i = 0; i < n_menu; i++)
		if (menu[i].retval >= 0)
			return menu[i].retval;
	return RV_FILTER;
}

/* ---- running and mounting -------------------------------------------- */

static void cant(const char *what, const char *path)
{
	char line[40];
	snprintf(line, sizeof line, "%s: %.*s", what, (int)(36 - strlen(what)), base_name(path));
	Log_print("library: %s: %s", what, path);
	message(line);
}

/* Position of the "1" in a first-disk name ("Game (Disk 1).atr",
   "game_d1.atr", "Game Side 1.atr"), or -1. */
static int disk1_pos(const char *file)
{
	static const char *const pats[] = { "(disk 1", " disk 1", " disk1", "_disk1", "_d1", " side 1", NULL };
	char lower[256];
	int i;

	if (strlen(file) >= sizeof lower)
		return -1;
	Util_strlcpy(lower, file, sizeof lower);
	Util_strlower(lower);
	for (i = 0; pats[i]; i++) {
		const char *p = lower;
		while ((p = strstr(p, pats[i])) != NULL) {
			size_t one = (size_t)(p - lower) + strlen(pats[i]) - 1;
			if (lower[one + 1] < '0' || lower[one + 1] > '9')
				return (int)one;
			p++;
		}
	}
	return -1;
}

/* "Game (Disk 1).atr" -> "Game (Disk 2).atr" ... in D2:..D4: */
static void mount_siblings(const char *path)
{
	char dir[FILENAME_MAX], want[256], found[FILENAME_MAX];
	const char *file = base_name(path);
	int pos, n;

	if (file == path || strlen(file) >= sizeof want)
		return;
	Util_strlcpy(dir, path, sizeof dir);
	dir[file - path - 1] = '\0';
	pos = disk1_pos(file);
	if (pos < 0)
		return;
	for (n = 2; n <= 4; n++) {
		DIR *d;
		struct dirent *e;
		int ok = FALSE;
		Util_strlcpy(want, file, sizeof want);
		want[pos] = (char)('0' + n);
		d = opendir(dir);
		while (d && !ok && (e = readdir(d)) != NULL)
			if (Util_stricmp(e->d_name, want) == 0) {
				join_path(found, sizeof found, dir, e->d_name);
				ok = TRUE;
			}
		if (d)
			closedir(d);
		if (!ok)
			break;
		if (SIO_Mount(n, found, FALSE))
			Log_print("library: D%d: %s", n, found);
	}
}

static int pick_cart_type(int kb)
{
	int type = UI_SelectCartType(kb);
	if (type == CARTRIDGE_NONE)
		return FALSE;
	CARTRIDGE_SetTypeAutoReboot(&CARTRIDGE_main, type);
	return TRUE;
}

/* ref is what Recent remembers: path itself, or "zip!entry" when path is
   the extracted copy of a zipped file. */
static int run_file(const char *path, const char *ref, int with_basic)
{
	int type, cart_out = FALSE;
	Log_print("library: run %s%s", path, with_basic ? " (with BASIC)" : "");
	/* Run boots this file: a cartridge left in would win over a disk or
	   a program (kms/boot.c only takes ASH's own one out) */
	if (file_kind(path) != FK_CART && CARTRIDGE_main.type != CARTRIDGE_NONE) {
		CARTRIDGE_Remove();
		cart_out = TRUE;
	}
	KMS_BOOT_with_basic = with_basic; /* read by the cold start below */
	type = AFILE_OpenFile(path, TRUE, 1, FALSE);
	KMS_BOOT_with_basic = FALSE;
	if (type == AFILE_ERROR) {
		cant("Could not run", path);
		if (cart_out)
			Atari800_Coldstart(); /* its ROM is unmapped: do not run on */
		return FALSE;
	}
	if ((type & 0xff) == AFILE_ROM && (type >> 8) != 0 && !pick_cart_type(type >> 8))
		return FALSE;
	switch (type) {
	case AFILE_ATR: case AFILE_XFD: case AFILE_ATR_GZ: case AFILE_XFD_GZ:
	case AFILE_DCM: case AFILE_PRO: case AFILE_ATX:
		mount_siblings(path);
		break;
	default:
		break;
	}
	add_recent(ref);
	save_cfg();
	return TRUE;
}

static void mount_file(const char *path)
{
	int r;
	switch (file_kind(path)) {
	case FK_DISK:
		if (SIO_Mount(1, path, FALSE)) {
			Log_print("library: D1: %s", path);
			message("Mounted as D1:");
		}
		else
			cant("Could not mount", path);
		break;
	case FK_CART:
		r = CARTRIDGE_InsertAutoReboot(path);
		if (r == CARTRIDGE_CANT_OPEN || r == CARTRIDGE_BAD_FORMAT) {
			cant("Not a cartridge", path);
			break;
		}
		if (r > 0 && !pick_cart_type(r))
			break;
		Log_print("library: cartridge %s", path);
		message("Cartridge inserted");
		break;
	case FK_TAPE:
		if (CASSETTE_Insert(path)) {
			Log_print("library: tape %s", path);
			message("Tape inserted");
		}
		else
			cant("Could not insert", path);
		break;
	default:
		break;
	}
}

/* ---- copying ---------------------------------------------------------- */

static int confirm(const char *question)
{
	menu_reset();
	menu_add(-1, NULL, question, NULL);
	menu_add(1, NULL, "Yes", NULL);
	menu_add(2, NULL, "No", NULL);
	return UI_driver->fSelect(question, UI_SELECT_POPUP, 2, menu, NULL) == 1;
}

static void copy_failed(int err)
{
	char line[40];
	snprintf(line, sizeof line, "Copy failed: %.24s", strerror(err));
	message(line);
}

/* Returns 0 or an errno value; the destination is removed on failure. */
static int copy_data(const char *from, const char *to)
{
	static unsigned char buf[65536];
	struct stat st;
	double total, done = 0;
	int in, out, last = -100, err = 0;

	in = open(from, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return errno;
	if (fstat(in, &st) != 0) {
		err = errno;
		close(in);
		return err;
	}
	total = (double)st.st_size;
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		err = errno;
		close(in);
		return err;
	}
	for (;;) {
		ssize_t n = read(in, buf, sizeof buf);
		ssize_t off = 0;
		int pct;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			err = errno;
			break;
		}
		if (n == 0)
			break;
		while (off < n) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w < 0) {
				if (errno == EINTR)
					continue;
				err = errno;
				break;
			}
			off += w;
		}
		if (err)
			break;
		done += (double)n;
		pct = total > 0 ? (int)(done * 100 / total) : 100;
		if (pct / 5 != last / 5) {
			char line[32];
			last = pct;
			snprintf(line, sizeof line, "Copying... %d%%", pct);
			UI_driver->fMessage(line, FALSE);
		}
	}
	close(in);
	if (!err && fsync(out) != 0)
		err = errno;
	if (close(out) != 0 && !err)
		err = errno;
	if (err)
		unlink(to);
	sync();
	return err;
}

/* Copy PATH into the folder DST_ROOT (keeping its folders, copy_rel());
   USB: say it is safe to unplug afterwards. */
static void copy_file(const char *path, const char *dst_root, int usb)
{
	char target[FILENAME_MAX], dir[FILENAME_MAX], line[40];
	struct stat src_st, dst_st;
	int err;

	if (stat(path, &src_st) != 0) {
		copy_failed(errno);
		return;
	}
	if (S_ISDIR(src_st.st_mode)) {
		message("Folders cannot be copied");
		return;
	}
	join_path(target, sizeof target, dst_root, copy_rel(path));
	if (stat(target, &dst_st) == 0) {
		if (dst_st.st_dev == src_st.st_dev && dst_st.st_ino == src_st.st_ino) {
			message("That is the same file");
			return;
		}
		if (S_ISDIR(dst_st.st_mode)) {
			message("A folder there has that name");
			return;
		}
		snprintf(line, sizeof line, "Overwrite %.26s?", base_name(target));
		if (!confirm(line))
			return;
	}
	Util_strlcpy(dir, target, sizeof dir);
	dir[base_name(dir) - dir - 1] = '\0';
	if (!make_dirs(dir)) {
		copy_failed(errno);
		return;
	}
	err = copy_data(path, target);
	if (err) {
		Log_print("library: copy %s -> %s failed: %s", path, target, strerror(err));
		copy_failed(err);
		return;
	}
	Log_print("library: copied %s -> %s", path, target);
	message(usb ? "Copied. Safe to unplug." : "Copied.");
}

/* ---- zip files -------------------------------------------------------- */

static ZIP_entry *zip_list = NULL;   /* for zip_cmp */

static int zip_cmp(const void *a, const void *b)
{
	const ZIP_entry *x = &zip_list[*(const int *)a];
	const ZIP_entry *y = &zip_list[*(const int *)b];
	int r = Util_stricmp(x->name, y->name);
	return r ? r : strcmp(x->name, y->name);
}

static void zip_failed(const char *zip, const char *why)
{
	char line[48];
	Log_print("library: zip %s: %s", zip, why);
	snprintf(line, sizeof line, "Could not open zip: %.18s", why);
	message(line);
}

/* Where zipped files are extracted: /run/atari-zip, else under $TMPDIR or
   /tmp. Emptied first; extracted files are stored flat by file name. */
static const char *zip_run_dir(void)
{
	static char dir[FILENAME_MAX];
	const char *tmp = getenv("TMPDIR");
	DIR *d;
	struct dirent *e;

	Util_strlcpy(dir, ZIP_RUN_DIR, sizeof dir);
	if (!make_dirs(dir) || access(dir, W_OK) != 0) {
		snprintf(dir, sizeof dir, "%.3000s/atari-zip", tmp && tmp[0] ? tmp : "/tmp");
		if (!make_dirs(dir) || access(dir, W_OK) != 0)
			return NULL;
	}
	d = opendir(dir);
	while (d && (e = readdir(d)) != NULL) {
		char path[FILENAME_MAX];
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		join_path(path, sizeof path, dir, e->d_name);
		unlink(path);
	}
	if (d)
		closedir(d);
	return dir;
}

static int zip_extract_one(const char *zip, const ZIP_entry *e, const char *dir, char *out, size_t size)
{
	char err[64];
	join_path(out, size, dir, base_name(e->name));
	if (!ZIP_Extract(zip, e, out, err, sizeof err)) {
		zip_failed(zip, err);
		return FALSE;
	}
	Log_print("library: zip %s!%s -> %s", zip, e->name, out);
	return TRUE;
}

/* Opens a zip: lists its Atari files, takes the one named want (Recent,
   Favourites) or the only one, or asks; extracts it (and, for a first
   disk, disks 2..4 from the same folder so the sibling scan finds them).
   Returns TRUE with the entry name and the extracted file. */
static int zip_prepare(const char *zip, const char *want, char *entry, size_t esize, char *file, size_t fsize)
{
	ZIP_entry *list;
	int *media;
	char err[64], title[40], extra[FILENAME_MAX];
	const char *dir;
	int n, m = 0, i, pick = -1, pos, ok = FALSE;

	n = ZIP_List(zip, &list, err, sizeof err);
	if (n < 0) {
		zip_failed(zip, err);
		return FALSE;
	}
	media = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
	if (!media) {
		free(list);
		return FALSE;
	}
	for (i = 0; i < n; i++)
		if (is_media(base_name(list[i].name)) && !is_zip(list[i].name))
			media[m++] = i;
	Log_print("library: zip %s: %d entries, %d Atari files", zip, n, m);
	zip_list = list;
	qsort(media, (size_t)m, sizeof media[0], zip_cmp);

	if (want) {
		for (i = 0; i < m && pick < 0; i++)
			if (strcmp(list[media[i]].name, want) == 0)
				pick = media[i];
		if (pick < 0)
			zip_failed(zip, "file not in it");
	}
	else if (m == 0)
		message("No Atari files in this zip");
	else if (m == 1)
		pick = media[0];
	else {
		int r;
		snprintf(title, sizeof title, "%.36s", base_name(zip));
		menu_reset();
		menu_add(-1, NULL, title, NULL);
		for (i = 0; i < m && i < MENU_MAX - 3; i++) {
			/* a popup prints the suffix right after the text: keep a gap */
			char size[16], sfx[20], text[40];
			format_size(list[media[i]].usize, size, sizeof size);
			snprintf(sfx, sizeof sfx, " %s", size);
			snprintf(text, sizeof text, "%.*s", 36 - (int)strlen(sfx), list[media[i]].name);
			menu_add(RV_ENTRY + i, " ", text, sfx);
		}
		if (i < m)
			menu_add(-1, NULL, "(more files not shown)", NULL);
		menu_add(ACT_CANCEL, NULL, "Cancel", NULL);
		r = UI_driver->fSelect(title, UI_SELECT_POPUP, RV_ENTRY, menu, NULL);
		if (r >= RV_ENTRY && r < RV_ENTRY + m)
			pick = media[r - RV_ENTRY];
	}

	if (pick >= 0 && (dir = zip_run_dir()) == NULL)
		zip_failed(zip, "nowhere to unpack");
	else if (pick >= 0 && zip_extract_one(zip, &list[pick], dir, file, fsize)) {
		const char *base = base_name(list[pick].name);
		size_t dirlen = (size_t)(base - list[pick].name);
		ok = TRUE;
		Util_strlcpy(entry, list[pick].name, esize);
		pos = disk1_pos(base);
		if (pos >= 0) {
			int k;
			for (k = 2; k <= 4; k++) {
				char sib[ZIP_NAME_MAX];
				int found = -1;
				Util_strlcpy(sib, base, sizeof sib);
				sib[pos] = (char)('0' + k);
				for (i = 0; i < m && found < 0; i++) {
					const char *name = list[media[i]].name;
					if (strlen(name) > dirlen && strncmp(name, list[pick].name, dirlen) == 0
					    && strchr(name + dirlen, '/') == NULL && Util_stricmp(name + dirlen, sib) == 0)
						found = media[i];
				}
				if (found < 0 || !zip_extract_one(zip, &list[found], dir, extra, sizeof extra))
					break;
			}
		}
	}
	free(media);
	free(list);
	zip_list = NULL;
	return ok;
}

/* ---- action popup ----------------------------------------------------- */

/* run: the file to open (the extracted copy for a zipped one; NULL when
   its drive is not plugged in); ref: what Recent and Favourites remember;
   copy: what "Copy to" copies (a zip is copied whole); title: its name.
   Returns TRUE when a program was started. */
/* "Copy to...": the USB drives other than the one COPY is on. */
static void copy_to_usb(const char *copy)
{
	char line[40];
	int i, r, from = source_of(copy);
	menu_reset();
	menu_add(-1, NULL, "Copy to", NULL);
	for (i = 1; i < n_sources; i++)
		if (i != from) {
			snprintf(line, sizeof line, "%.36s", sources[i].name);
			menu_add(ACT_COPY_USB + i, NULL, line, NULL);
		}
	menu_add(ACT_CANCEL, NULL, "Cancel", NULL);
	r = UI_driver->fSelect("Copy to", UI_SELECT_POPUP, menu_first_selectable(), menu, NULL);
	if (r > ACT_COPY_USB && r < ACT_COPY_USB + n_sources)
		copy_file(copy, sources[r - ACT_COPY_USB].root, TRUE);
}

static int is_disk_image(const char *name)
{
	static const char *const ext[] = { ".atr", ".xfd", ".dcm", ".atx", ".pro", ".atr.gz", ".xfd.gz", NULL };
	int i;
	for (i = 0; ext[i]; i++)
		if (Util_striendswith(name, ext[i]))
			return TRUE;
	return FALSE;
}

static int action_popup(const char *run, const char *ref, const char *copy, const char *title)
{
	char lib[FILENAME_MAX], sd[FILENAME_MAX];
	int kind, from, i, r, usb_targets = 0;
	int there = run != NULL && is_regular(run);

	kind = file_kind(there ? run : ref);
	from = source_of(copy);
	library_dir(lib, sizeof lib);
	sd_dir(sd, sizeof sd);
	for (i = 1; i < n_sources; i++)
		if (i != from)
			usb_targets++;

	menu_reset();
	menu_add(-1, NULL, title, NULL);
	if (there) {
		menu_add(ACT_RUN, NULL, "Run", NULL);
		if (kind == FK_DISK && is_disk_image(run))
			menu_add(ACT_RUN_BASIC, NULL, "Run with BASIC", NULL);
		if (kind == FK_DISK)
			menu_add(ACT_MOUNT, NULL, "Mount as D1:", NULL);
		else if (kind == FK_CART)
			menu_add(ACT_MOUNT, NULL, "Insert cartridge", NULL);
		else if (kind == FK_TAPE)
			menu_add(ACT_MOUNT, NULL, "Insert tape", NULL);
		if (!below(copy, lib))
			menu_add(ACT_COPY_LIB, NULL, "Copy to Library", NULL);
		if (!below(copy, sd))
			menu_add(ACT_COPY_SD, NULL, "Copy to FujiNet SD", NULL);
		if (usb_targets > 0)
			menu_add(ACT_COPY_TO, NULL, "Copy to...", NULL);
	}
	else
		menu_add(-1, NULL, "(its drive is not plugged in)", NULL);
	menu_add(ACT_FAV, NULL, fav_index(ref) >= 0 ? "Unfavourite" : "Favourite", NULL);
	if (KMS_BOOT_SomethingMounted())
		menu_add(ACT_EJECT, NULL, "Eject all & reboot", NULL);
	menu_add(ACT_CANCEL, NULL, "Cancel", NULL);

	r = UI_driver->fSelect(title, UI_SELECT_POPUP, there ? ACT_RUN : ACT_FAV, menu, NULL);
	if (r == ACT_RUN || r == ACT_RUN_BASIC)
		return run_file(run, ref, r == ACT_RUN_BASIC);
	if (r == ACT_EJECT) {
		Log_print("library: eject all");
		KMS_BOOT_EjectAll();
		return TRUE; /* back to the Atari */
	}
	if (r == ACT_MOUNT)
		mount_file(run);
	else if (r == ACT_COPY_LIB)
		copy_file(copy, lib, FALSE);
	else if (r == ACT_COPY_SD)
		copy_file(copy, sd, FALSE);
	else if (r == ACT_COPY_TO)
		copy_to_usb(copy);
	else if (r == ACT_FAV)
		toggle_fav(ref);
	return FALSE;
}

/* RETURN on a file, a Recent or a Favourites line. */
static int file_popup(const char *path_in)
{
	char path[FILENAME_MAX], zip[FILENAME_MAX], ref[FILENAME_MAX];
	char entry[ZIP_NAME_MAX], file[FILENAME_MAX], title[40];
	const char *want;

	Util_strlcpy(path, path_in, sizeof path); /* recent[] changes under us */
	if (split_zip_ref(path, zip, sizeof zip, &want)) {
		display_name(path, title, sizeof title);
		if (!is_regular(zip))
			return action_popup(NULL, path, zip, title);
		if (!zip_prepare(zip, want, entry, sizeof entry, file, sizeof file))
			return FALSE;
		return action_popup(file, path, zip, title);
	}
	if (is_zip(path) && is_regular(path)) {
		if (!zip_prepare(path, NULL, entry, sizeof entry, file, sizeof file))
			return FALSE;
		bang_join(ref, sizeof ref, path, entry);
		display_name(ref, title, sizeof title);
		return action_popup(file, ref, path, title);
	}
	snprintf(title, sizeof title, "%.36s", base_name(path));
	return action_popup(is_regular(path) ? path : NULL, path, path, title);
}

/* ---- the browser ------------------------------------------------------ */

static void make_title(char *out, size_t size, const lib_source *src, const char *rel, const char *filter)
{
	char path[FILENAME_MAX + 2], tail[40];
	size_t len;
	snprintf(path, sizeof path, "/%s", rel);
	len = strlen(path);
	if (len > 24)
		snprintf(tail, sizeof tail, "...%s", path + len - 21);
	else
		Util_strlcpy(tail, path, sizeof tail);
	if (filter[0])
		snprintf(out, size, "%.12s: %s (filter: %.8s)", src->name, tail, filter);
	else
		snprintf(out, size, "%s: %s", src->name, tail);
}

int KMS_LIBRARY_Run(void)
{
	static char filter[32] = "";
	char root[FILENAME_MAX], rel[FILENAME_MAX], dir[FILENAME_MAX];
	char want[256], title[80], line[48];
	int page = 0, sel = -1, ran = FALSE;
	int src;

	UI_is_active = TRUE;
	UI_driver->fInit();
	if (usb_hint_until > Util_time()) {
		KMS_VIDEO_SetHint("", 0); /* they found the key */
		usb_hint_until = 0;
	}
	load_cfg();
	build_sources();
	library_dir(dir, sizeof dir);
	make_dirs(dir);
	Util_strlcpy(root, sources[0].root, sizeof root);
	Util_strlcpy(rel, sources[0].start, sizeof rel);
	want[0] = '\0';
	Log_print("library: open (%d sources)", n_sources);

	while (!ran) {
		int r, seltype = 0, i, first, at_lib_root;

		build_sources();
		src = find_source(root);
		if (src < 0) {
			message("USB drive removed");
			Log_print("library: %s is gone", root);
			src = 0;
			Util_strlcpy(root, sources[0].root, sizeof root);
			Util_strlcpy(rel, sources[0].start, sizeof rel);
			page = 0;
			sel = -1;
		}
		join_path(dir, sizeof dir, root, rel);
		if (!read_dir(dir, filter, sources[src].kind == SRC_STICK && sources[src].start[0] && !rel[0]) && rel[0]) {
			rel[0] = '\0'; /* the folder went away */
			page = 0;
			sel = -1;
			continue;
		}
		if (want[0]) {
			for (i = 0; i < n_visible; i++)
				if (strcmp(entries[visible[i]].name, want) == 0) {
					page = i / PAGE;
					sel = RV_ENTRY + i;
					break;
				}
			want[0] = '\0';
		}
		if (page * PAGE >= n_visible)
			page = n_visible > 0 ? (n_visible - 1) / PAGE : 0;
		/* Recent and Favourites: at the stick's root and in library/ */
		at_lib_root = src == 0 && (!rel[0] || strcmp(rel, sources[0].start) == 0);

		menu_reset();
		snprintf(line, sizeof line, "[ Source: %.20s   TAB ]", sources[src].name);
		menu_add(-1, NULL, line, NULL);
		snprintf(line, sizeof line, "[ Filter: %.24s ]", filter[0] ? filter : "none");
		menu_add(RV_FILTER, NULL, line, NULL);
		if (at_lib_root) {
			menu_add(-1, NULL, n_recent ? "Recent:" : "Recent: nothing yet", NULL);
			for (i = 0; i < n_recent; i++)
				menu_add_path(RV_RECENT + i, recent[i]);
			if (n_favs) {
				menu_add(-1, NULL, "Favourites:", NULL);
				for (i = 0; i < n_favs && i < SHOW_FAVS; i++)
					menu_add_path(RV_FAV + i, favs[i]);
			}
			menu_add(-1, NULL, "", NULL);
		}
		if (rel[0])
			menu_add(RV_PARENT, " ", "..", NULL);
		if (page > 0)
			menu_add(RV_PREV, NULL, "[ Previous page ]", NULL);
		first = page * PAGE;
		for (i = first; i < n_visible && i < first + PAGE; i++) {
			const lib_entry *e = &entries[visible[i]];
			const char *prefix = " ";
			if (!e->isdir) {
				char full[FILENAME_MAX];
				join_path(full, sizeof full, dir, e->name);
				if (fav_index(full) >= 0)
					prefix = "*";
			}
			menu_add_file(RV_ENTRY + i, prefix, e->name, e->isdir, e->size, TRUE);
		}
		if (first + PAGE < n_visible) {
			snprintf(line, sizeof line, "[ Next page: %d more ]", n_visible - first - PAGE);
			menu_add(RV_NEXT, NULL, line, NULL);
		}
		if (n_visible == 0)
			menu_add(-1, NULL, filter[0] ? "  (nothing matches the filter)" : "  (no Atari files here)", NULL);

		/* Return on a label would read as Esc: never start on one. */
		if (sel < 0 || !menu_has(sel))
			sel = n_visible > 0 ? RV_ENTRY + first : menu_first_selectable();
		make_title(title, sizeof title, &sources[src], rel, filter);

		r = UI_driver->fSelect(title, 0, sel, menu, &seltype);
		if (aborted())
			break;
		if (r == -2) { /* Tab: next source */
			src = (src + 1) % n_sources;
			Util_strlcpy(root, sources[src].root, sizeof root);
			Util_strlcpy(rel, sources[src].start, sizeof rel);
			page = 0;
			sel = -1;
			continue;
		}
		if (r == -1 || seltype == UI_USER_DELETE || r == RV_PARENT) {
			char *slash;
			if (r == -1 && sources[src].start[0] && strcmp(rel, sources[src].start) == 0)
				break; /* Esc where the tab opens (library/) closes it too */
			if (!rel[0]) {
				if (r == -1)
					break; /* Esc at a source root closes the Library */
				sel = r;
				continue;
			}
			slash = strrchr(rel, '/');
			Util_strlcpy(want, slash ? slash + 1 : rel, sizeof want);
			if (slash)
				*slash = '\0';
			else
				rel[0] = '\0';
			page = 0;
			sel = -1;
			continue;
		}
		sel = r;
		if (r == RV_FILTER) {
			char edit[sizeof filter];
			Util_strlcpy(edit, filter, sizeof edit);
			if (UI_driver->fEditString("Filter (empty = show all)", edit, sizeof edit)) {
				Util_trim(edit);
				Util_strlcpy(filter, edit, sizeof filter);
				page = 0;
				sel = -1;
			}
		}
		else if (r == RV_PREV) {
			page--;
			sel = -1;
		}
		else if (r == RV_NEXT) {
			page++;
			sel = -1;
		}
		else if (r >= RV_RECENT && r < RV_RECENT + n_recent)
			ran = file_popup(recent[r - RV_RECENT]);
		else if (r >= RV_FAV && r < RV_FAV + n_favs)
			ran = file_popup(favs[r - RV_FAV]);
		else if (r >= RV_ENTRY && r < RV_ENTRY + n_visible) {
			const lib_entry *e = &entries[visible[r - RV_ENTRY]];
			if (e->isdir) {
				char sub[FILENAME_MAX];
				join_path(sub, sizeof sub, rel, e->name);
				Util_strlcpy(rel, sub[0] == '/' ? sub + 1 : sub, sizeof rel);
				page = 0;
				sel = -1;
			}
			else {
				char full[FILENAME_MAX];
				join_path(full, sizeof full, dir, e->name);
				ran = file_popup(full);
			}
		}
		if (aborted())
			break;
	}

	free_entries();
	UI_is_active = FALSE;
	/* flush keypresses (see KMS_SYSMENU_Run); AKEY_EXIT stays for the main loop */
	{
		int k;
		while ((k = PLATFORM_Keyboard()) != AKEY_NONE && k != AKEY_EXIT)
			Atari800_Sync();
	}
	UI_alt_function = -1;
	KMS_LIBRARY_requested = FALSE;
	return ran;
}

/* ---- plug-in hint ------------------------------------------------------- */

static int inotify_fd = -1;
static double next_watch_try = 0;
static double rescan_until = 0;
static double next_rescan = 0;
static char known_vols[MAX_VOLS][64];
static int n_known = 0;

static void check_volumes(double now)
{
	char vols[MAX_VOLS][64];
	int n = list_volumes(vols, MAX_VOLS);
	int i, j;

	for (i = 0; i < n; i++) {
		for (j = 0; j < n_known; j++)
			if (strcmp(vols[i], known_vols[j]) == 0)
				break;
		if (j == n_known) {
			Log_print("library: usb volume %s", vols[i]);
			KMS_VIDEO_SetHint("USB drive found: F9 = Library", 12.0);
			usb_hint_until = now + 12.0;
		}
	}
	for (j = 0; j < n_known; j++) {
		for (i = 0; i < n; i++)
			if (strcmp(vols[i], known_vols[j]) == 0)
				break;
		if (i == n)
			Log_print("library: usb volume %s removed", known_vols[j]);
	}
	for (i = 0; i < n; i++)
		Util_strlcpy(known_vols[i], vols[i], sizeof known_vols[i]);
	n_known = n;
}

void KMS_LIBRARY_Poll(void)
{
	double now = Util_time();

	if (inotify_fd < 0 && now >= next_watch_try) {
		next_watch_try = now + 5.0;
		inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (inotify_fd >= 0 && inotify_add_watch(inotify_fd, USB_DIR,
		        IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_ONLYDIR) < 0) {
			close(inotify_fd);
			inotify_fd = -1;
		}
		if (inotify_fd >= 0)
			rescan_until = now + 1.0; /* drives that were there at boot */
	}
	if (inotify_fd >= 0) {
		union {
			struct inotify_event ev;
			char buf[4096];
		} u;
		while (read(inotify_fd, u.buf, sizeof u.buf) > 0)
			rescan_until = now + 5.0; /* the mount follows the mkdir */
	}
	if (now < rescan_until && now >= next_rescan) {
		next_rescan = now + 0.25;
		check_volumes(now);
	}
}
