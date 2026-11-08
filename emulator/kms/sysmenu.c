/*
 * kms/sysmenu.c - the F8 "System" menu
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * Drawn with atari800's own UI toolkit, so it looks like the F1 menu.
 * Anything that needs root-level system control (the boot target,
 * Wi-Fi, SSH, reboot, power off) is a one-line request to PID 1 over the
 * Unix socket /run/atari.ctl; volume and backlight are set directly.
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
#include <math.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <linux/input.h>

#include "atari.h"
#include "akey.h"
#include "cartridge.h"
#include "cfg.h"
#include "input.h"
#include "log.h"
#include "platform.h"
#include "sio.h"
#include "ui.h"
#include "util.h"
#include "kms/kms_boot.h"
#include "kms/kms_help.h"
#include "kms/kms_input.h"
#include "kms/kms_library.h"
#include "kms/kms_sysmenu.h"
#include "kms/kms_video.h"
#include <sys/utsname.h>

#define STICK_USER "/media/atari" /* MEDIA_USER in init/init.c */

static void wrapped_page(const char *title, const char *text);

#define CTL_SOCK "/run/atari.ctl"
#define ASH_CART "/media/atari/ash/ASH.CAR" /* ASH_CART in init/init.c */

int KMS_SYSMENU_requested = FALSE;
int KMS_SYSMENU_volume = -1;
int KMS_SYSMENU_brightness = -1;
int KMS_SYSMENU_hint_done = FALSE;
int KMS_SYSMENU_advanced_open = FALSE;
int KMS_SYSMENU_reopen = FALSE;

/* ---- talking to PID 1 ------------------------------------------------ */

/* Send one command line, collect the reply up to the "OK"/"ERR" line.
   Returns TRUE on OK. The reply (without the final status line) is left
   in *reply. */
static int ctl_request(const char *cmd, char *reply, size_t size)
{
	struct sockaddr_un addr;
	int fd;
	size_t used = 0;
	int ok = FALSE;
	char line[512];
	FILE *f;

	reply[0] = '\0';
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return FALSE;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	Util_strlcpy(addr.sun_path, CTL_SOCK, sizeof addr.sun_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		snprintf(reply, size, "no control socket");
		return FALSE;
	}
	if (write(fd, cmd, strlen(cmd)) < 0 || write(fd, "\n", 1) < 0) {
		close(fd);
		return FALSE;
	}
	f = fdopen(fd, "r");
	if (!f) {
		close(fd);
		return FALSE;
	}
	while (fgets(line, sizeof line, f)) {
		if (strcmp(line, "OK\n") == 0) {
			ok = TRUE;
			break;
		}
		if (strncmp(line, "ERR", 3) == 0) {
			Util_strlcpy(reply, line + 4, size);
			reply[strcspn(reply, "\n")] = '\0'; /* shown as is by fMessage */
			break;
		}
		if (used + strlen(line) < size) {
			memcpy(reply + used, line, strlen(line) + 1);
			used += strlen(line);
		}
	}
	fclose(f);
	return ok;
}

/* Value of "key " line in a status reply, or def. */
static const char *status_get(const char *status, const char *key, char *out, size_t size, const char *def)
{
	const char *p = status;
	size_t klen = strlen(key);
	while (p && *p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == ' ') {
			size_t vlen = len - klen - 1;
			if (vlen >= size)
				vlen = size - 1;
			memcpy(out, p + klen + 1, vlen);
			out[vlen] = '\0';
			return out;
		}
		p = nl ? nl + 1 : NULL;
	}
	Util_strlcpy(out, def, size);
	return out;
}

/* The boot target from a status reply: "basic", "nobasic", "ash" or
   "fujinet" (an init from before "boot" only says "fujinet on|off"). */
static const char *boot_of(const char *status, char *out, size_t size)
{
	char fuji[16];
	status_get(status, "fujinet", fuji, sizeof fuji, "off");
	return status_get(status, "boot", out, size, strcmp(fuji, "on") == 0 ? "fujinet" : "basic");
}

static const char *boot_label(const char *target)
{
	return KMS_BOOT_TargetLabel(target);
}

/* ---- backlight ------------------------------------------------------- */

static char backlight_dir[256] = "";

static int backlight_find(void)
{
	DIR *d;
	struct dirent *e;
	if (backlight_dir[0])
		return TRUE;
	d = opendir("/sys/class/backlight");
	if (!d)
		return FALSE;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(backlight_dir, sizeof backlight_dir, "/sys/class/backlight/%.200s", e->d_name);
		break;
	}
	closedir(d);
	return backlight_dir[0] != '\0';
}

static int read_int_file(const char *path, int def)
{
	FILE *f = fopen(path, "r");
	int v = def;
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = def;
		fclose(f);
	}
	return v;
}

static int backlight_get(void)
{
	char path[300];
	int max, cur;
	if (!backlight_find())
		return -1;
	snprintf(path, sizeof path, "%s/max_brightness", backlight_dir);
	max = read_int_file(path, 0);
	snprintf(path, sizeof path, "%s/brightness", backlight_dir);
	cur = read_int_file(path, 0);
	return max > 0 ? cur * 100 / max : -1;
}

static void backlight_set(int percent)
{
	char path[300];
	int max;
	FILE *f;
	if (!backlight_find())
		return;
	if (percent < 3)
		percent = 3; /* never fully dark: the user would be stuck */
	if (percent > 100)
		percent = 100;
	snprintf(path, sizeof path, "%s/max_brightness", backlight_dir);
	max = read_int_file(path, 0);
	snprintf(path, sizeof path, "%s/brightness", backlight_dir);
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%d\n", max * percent / 100);
		fclose(f);
	}
}

/* ---- config persistence --------------------------------------------- */

int KMS_SYSMENU_ReadConfig(char *option, char *parameters)
{
	if (strcmp(option, "KMS_VOLUME") == 0)
		KMS_SYSMENU_volume = Util_sscandec(parameters);
	else if (strcmp(option, "KMS_BRIGHTNESS") == 0)
		KMS_SYSMENU_brightness = Util_sscandec(parameters);
	else if (strcmp(option, "KMS_HINT_DONE") == 0)
		KMS_SYSMENU_hint_done = Util_sscanbool(parameters) == 1;
	else
		return FALSE;
	return TRUE;
}

void KMS_SYSMENU_WriteConfig(FILE *fp)
{
	if (KMS_SYSMENU_volume >= 0)
		fprintf(fp, "KMS_VOLUME=%d\n", KMS_SYSMENU_volume);
	if (KMS_SYSMENU_brightness >= 0)
		fprintf(fp, "KMS_BRIGHTNESS=%d\n", KMS_SYSMENU_brightness);
	fprintf(fp, "KMS_HINT_DONE=%d\n", KMS_SYSMENU_hint_done);
}

void KMS_SYSMENU_ApplySettings(void)
{
	static int hinted = FALSE;
	if (KMS_SYSMENU_volume >= 0)
		ALSA_SetVolume(KMS_SYSMENU_volume);
	if (KMS_SYSMENU_brightness >= 0)
		backlight_set(KMS_SYSMENU_brightness);
	if (!KMS_SYSMENU_hint_done && !hinted) {
		hinted = TRUE;
		KMS_VIDEO_SetHint("F1 = Help   F8 = System   F9 = Library", 25.0);
	}
	{
		/* once, before the first cold start: what to boot when
		   nothing is mounted (kms/boot.c; ASH's STATUS shows it) */
		static int asked = FALSE;
		char status[1024], buf[16];
		if (!asked && ctl_request("status", status, sizeof status)) {
			asked = TRUE; /* init may not be listening yet: try again next time */
			KMS_BOOT_SetTarget(boot_of(status, buf, sizeof buf));
		}
	}
}

const char *KMS_SYSMENU_Version(void)
{
	static char version[40] = "";
	if (!version[0]) {
		char status[1024];
		if (ctl_request("status", status, sizeof status))
			status_get(status, "version", version, sizeof version, "?");
		else
			return "?"; /* ask again next time */
	}
	return version;
}

void KMS_SYSMENU_HintSeen(void)
{
	if (!KMS_SYSMENU_hint_done) {
		KMS_SYSMENU_hint_done = TRUE; /* they found the keys */
		KMS_VIDEO_SetHint("", 0);
	}
}

/* ---- hardware status page -------------------------------------------- */

static int read_line(const char *path, char *out, size_t size)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		out[0] = '\0';
		return FALSE;
	}
	if (!fgets(out, (int)size, f))
		out[0] = '\0';
	fclose(f);
	out[strcspn(out, "\n")] = '\0';
	return out[0] != '\0';
}

#include <stdarg.h>

/* Append one '\0'-terminated line to an InfoScreen buffer. */
static void add_line(char *page, size_t *n, size_t size, const char *fmt, ...)
{
	char line[200];
	va_list ap;
	size_t len;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	len = strlen(line);
	if (*n + len + 1 < size) {
		memcpy(page + *n, line, len + 1);
		*n += len + 1;
	}
}

/* The hardware/status lines ('\0'-separated, as fInfoScreen wants them),
   shared by "Hardware status" and the problem report. Returns the length. */
static size_t hardware_lines(const char *status, char *page, size_t size)
{
	char line[200], v[128], w[128];
	size_t n = 0;
	FILE *f;

	status_get(status, "version", v, sizeof v, "?");
	add_line(page, &n, size, "Version: %.29s", v);
	read_line("/sys/class/dmi/id/product_name", v, sizeof v);
	read_line("/sys/class/dmi/id/sys_vendor", w, sizeof w);
	add_line(page, &n, size, "%.16s %.20s", w, v);
	f = fopen("/proc/cpuinfo", "r");
	v[0] = '\0';
	while (f && fgets(line, sizeof line, f)) {
		if (strncmp(line, "model name", 10) == 0) {
			char *p = strchr(line, ':');
			if (p) {
				p++;
				while (*p == ' ') p++;
				Util_strlcpy(v, p, sizeof v);
				v[strcspn(v, "\n")] = '\0';
			}
			break;
		}
	}
	if (f) fclose(f);
	add_line(page, &n, size, "CPU: %.34s", v);
	f = fopen("/proc/meminfo", "r");
	if (f) {
		long kb = 0;
		if (fscanf(f, "MemTotal: %ld", &kb) == 1)
			add_line(page, &n, size, "RAM: %ld MB", kb / 1024);
		fclose(f);
	}
	{
		/* "CPU: 12% emu, 3% fujinet, 48 C, 2.1 W" from init's sampler
		   ("perf <emu%> <fuji%> <tempC> <watts>", n/a when unknown) */
		char emu[16], fuji[16], temp[16], watts[16];
		status_get(status, "perf", v, sizeof v, "");
		if (v[0] && sscanf(v, "%15s %15s %15s %15s", emu, fuji, temp, watts) == 4) {
			size_t len;
			snprintf(line, sizeof line, "CPU: %s%% emu, %s%% fujinet", emu, fuji);
			len = strlen(line);
			if (strcmp(temp, "n/a") != 0)
				len += (size_t)snprintf(line + len, sizeof line - len, ", %s C", temp);
			if (strcmp(watts, "n/a") != 0)
				snprintf(line + len, sizeof line - len, ", %s W", watts);
			add_line(page, &n, size, "%s", line);
		}
	}
	add_line(page, &n, size, "Display: %s %dx%d @ %.4g Hz", KMS_VIDEO_driver, KMS_VIDEO_width, KMS_VIDEO_height, KMS_VIDEO_refresh);
	KMS_VIDEO_ScaleFactors(w, sizeof w);
	add_line(page, &n, size, "  %s %s%s", KMS_VIDEO_ScaleLabel(), w,
	         KMS_VIDEO_smooth && KMS_VIDEO_scale_mode != KMS_SCALE_INTEGER ? ", smooth" : "");
	add_line(page, &n, size, "  Scanlines %s%s%s",
	         KMS_VIDEO_ScanlinesPossible() || KMS_VIDEO_scanlines == KMS_SCANLINES_OFF ? KMS_VIDEO_ScanlinesLabel() : "n/a",
	         KMS_VIDEO_NtscLook() ? ", " : "", KMS_VIDEO_NtscLook() ? KMS_VIDEO_NtscPresetLabel() : "");
	add_line(page, &n, size, "Atari %s %.1f Hz, pacing: %s", Atari800_tv_mode == Atari800_TV_PAL ? "PAL" : "NTSC",
	         KMS_VIDEO_AtariFps(), KMS_VIDEO_PacingLocked() && !Atari800_turbo ? "display" : "timer");
	if (read_line("/proc/asound/cards", v, sizeof v)) {
		char *p = strchr(v, ':');
		add_line(page, &n, size, "Sound: %.32s", p ? p + 2 : v);
	}
	else
		add_line(page, &n, size, "Sound: no card");
	status_get(status, "wifi", v, sizeof v, "no adapter");
	add_line(page, &n, size, "Wi-Fi: %.30s", v);
	status_get(status, "ip", v, sizeof v, "none");
	add_line(page, &n, size, "IP: %s", v);
	status_get(status, "battery", v, sizeof v, "");
	if (v[0])
		add_line(page, &n, size, "Battery: %s%%", v);
	status_get(status, "media", v, sizeof v, "?");
	status_get(status, "efi", w, sizeof w, "?");
	add_line(page, &n, size, "Media: %s, UEFI %s", v, w);
	/* init's fsck.fat of the stick at boot: "fsck clean|repaired|failed|skipped" */
	status_get(status, "fsck", v, sizeof v, "skipped");
	if (strcmp(v, "clean") == 0)
		add_line(page, &n, size, "Stick: FAT clean");
	else if (strcmp(v, "repaired") == 0)
		add_line(page, &n, size, "Stick: FAT repaired at boot");
	else if (strcmp(v, "failed") == 0)
		add_line(page, &n, size, "Stick: FAT check failed");
	/* loopback mode (atari.image): "image <dev> <path>", the path's tail
	   when it is longer than the line */
	status_get(status, "image", v, sizeof v, "");
	if (v[0]) {
		char *path = strchr(v, ' ');
		if (path) {
			size_t plen;
			*path++ = '\0';
			plen = strlen(path);
			add_line(page, &n, size, "Image on %.20s:", v);
			add_line(page, &n, size, "  %s%s", plen > 34 ? "..." : "", plen > 34 ? path + plen - 31 : path);
		}
	}
	boot_of(status, v, sizeof v);
	status_get(status, "ssh", w, sizeof w, "?");
	add_line(page, &n, size, "Boot: %s, SSH %s", boot_label(v), w);
	add_line(page, &n, size, "Started: %.30s", KMS_BOOT_LastRoute());
	{
		struct utsname u;
		if (uname(&u) == 0)
			add_line(page, &n, size, "Linux %.30s", u.release);
	}
	{
		double up = 0;
		f = fopen("/proc/uptime", "r");
		if (f) {
			if (fscanf(f, "%lf", &up) != 1) up = 0;
			fclose(f);
		}
		add_line(page, &n, size, "Up %d min", (int)(up / 60));
	}
	if (n + 2 < size) { /* page is a pointer here */
		page[n++] = '\n';
		page[n] = '\0';
	}
	return n;
}

static void hardware_page(const char *status)
{
	static char page[2048];
	hardware_lines(status, page, sizeof page);
	UI_driver->fInfoScreen("Hardware", page);
}

/* ---- problem report ------------------------------------------------------ */

/* The last LINES lines of PATH appended to OUT (from its last 64 KB). */
static void tail_file(FILE *out, const char *path, int lines)
{
	static char buf[65536];
	FILE *f = fopen(path, "r");
	long size, n;
	char *p, *q, *end;
	int count = 0;

	if (!f) {
		fprintf(out, "(no %s)\n", path);
		return;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	if (size > (long)sizeof buf - 1)
		fseek(f, size - ((long)sizeof buf - 1), SEEK_SET);
	else
		fseek(f, 0, SEEK_SET);
	n = (long)fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	if (n < 0)
		n = 0;
	buf[n] = '\0';
	p = buf;
	if (size > n) { /* started mid-line: drop the partial first line */
		p = strchr(buf, '\n');
		p = p ? p + 1 : buf + n;
	}
	end = buf + n;
	/* the last LINES lines, leaving out the key-by-key noise of
	   -evdev-debug (the debug image) */
	q = end;
	while (q > p) {
		if (q[-1] == '\n') {
			if (strncmp(q, "evdev: ", 7) != 0 && ++count > lines)
				break;
		}
		q--;
	}
	if (q > p && q[-1] != '\n')
		q = p;
	while (q < end) {
		char *nl = memchr(q, '\n', (size_t)(end - q));
		size_t len = nl ? (size_t)(nl - q) + 1 : (size_t)(end - q);
		if (strncmp(q, "evdev: ", 7) != 0)
			fwrite(q, 1, len, out);
		q += len;
	}
}

/* "Report a problem...": one line from the user, then a text file on the
   stick with what a GitHub issue needs: version, machine, settings,
   what is mounted, the recent emulator and FujiNet logs. init's "logs"
   puts dmesg.txt and the full logs next to it. */
static void report_menu(const char *status)
{
	static char page[2048];
	char text[120] = "", path[300], stamp[32], line[1024];
	size_t n, i;
	time_t now = time(NULL);
	FILE *f;
	int k;

	if (!UI_driver->fEditString("What went wrong? (one line)", text, sizeof text))
		return;
	Util_trim(text);
	strftime(stamp, sizeof stamp, "%m%d-%H%M%S", localtime(&now)); /* short: the name must fit one menu line */
	snprintf(path, sizeof path, "%s/logs/report-%s.txt", STICK_USER, stamp);
	ctl_request("logs", line, sizeof line); /* dmesg.txt and the full logs */
	mkdir(STICK_USER "/logs", 0755);
	f = fopen(path, "w");
	if (!f) {
		UI_driver->fMessage("Could not write to the stick", TRUE);
		return;
	}
	fprintf(f, "atari-os-x86 problem report\n%s\nProblem: %s\n\n== Machine ==\n",
	        ctime(&now), text[0] ? text : "(no description)");
	n = hardware_lines(status, page, sizeof page);
	for (i = 0; i < n && page[i] != '\n'; i += strlen(page + i) + 1) /* '\n' ends the page */
		fprintf(f, "%s\n", page + i);
	fprintf(f, "\n== Settings ==\nKeyboard type: %s\nArrows: %s\nBoot target: %s\n",
	        KMS_INPUT_keyboard_pc ? "PC" : "Atari",
	        KMS_INPUT_arrows_joystick ? "joystick 0" : "cursor keys", KMS_BOOT_Target());
	fprintf(f, "\n== Mounted ==\n");
	for (k = 0; k < 4; k++)
		if (SIO_drive_status[k] == SIO_READ_ONLY || SIO_drive_status[k] == SIO_READ_WRITE)
			fprintf(f, "D%d: %s\n", k + 1, SIO_filename[k]);
	if (CARTRIDGE_main.type != CARTRIDGE_NONE)
		fprintf(f, "Cartridge: %s\n", CARTRIDGE_main.filename);
	fprintf(f, "\n== atari800.log (last 80 lines) ==\n");
	tail_file(f, "/run/atari800.log", 80);
	fprintf(f, "\n== fujinet.log (last 40 lines) ==\n");
	tail_file(f, "/run/fujinet.log", 40);
	fprintf(f, "\n(dmesg.txt, the full logs and system.txt are in this folder)\n");
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	Log_print("sysmenu: report %s", path);
	snprintf(line, sizeof line, "Saved on the stick:\natari/logs/report-%s.txt\n"
	         "dmesg and the full logs are next to it. Attach the file to a GitHub issue of atari-os-x86.", stamp);
	wrapped_page("Report saved", line);
}

/* ---- hotkeys ---------------------------------------------------------- */

void KMS_SYSMENU_HotKey(int keycode)
{
	static int muted_volume = -1;
	int v;
	switch (keycode) {
	case KEY_VOLUMEUP:
	case KEY_VOLUMEDOWN:
		v = ALSA_GetVolume();
		if (v < 0)
			return;
		v += keycode == KEY_VOLUMEUP ? 5 : -5;
		if (v < 0) v = 0;
		if (v > 100) v = 100;
		ALSA_SetVolume(v);
		KMS_SYSMENU_volume = v;
		muted_volume = -1;
		break;
	case KEY_MUTE:
		v = ALSA_GetVolume();
		if (v < 0)
			return;
		if (v > 0) {
			muted_volume = v;
			ALSA_SetVolume(0);
		}
		else {
			ALSA_SetVolume(muted_volume > 0 ? muted_volume : 50);
			KMS_SYSMENU_volume = muted_volume > 0 ? muted_volume : 50;
		}
		break;
	case KEY_BRIGHTNESSUP:
	case KEY_BRIGHTNESSDOWN:
		v = backlight_get();
		if (v < 0)
			return;
		v += keycode == KEY_BRIGHTNESSUP ? 10 : -10;
		backlight_set(v);
		KMS_SYSMENU_brightness = backlight_get();
		break;
	default:
		break;
	}
}

/* ---- menu pieces ----------------------------------------------------- */

static void percent_label(char *label, int value, void *user_data)
{
	(void)user_data;
	snprintf(label, 11, "%d%%", value);
}

static void wait_seconds(double s)
{
	double until = Util_time() + s;
	while (Util_time() < until) {
		PLATFORM_Keyboard(); /* keep input devices drained */
		Util_sleep(0.05);
	}
}

/* An info page with TEXT word-wrapped to 36 columns; a '\n' in TEXT
   starts a new paragraph (after an empty line). */
static void wrapped_page(const char *title, const char *text)
{
	static char page[1024];
	size_t n = 0;
	const char *p = text;
	while (*p && n + 40 < sizeof page) {
		size_t len = strcspn(p, "\n");
		if (len > 36) {
			len = 36;
			while (len > 0 && p[len] != ' ')
				len--;
			if (len == 0)
				len = 36;
		}
		memcpy(page + n, p, len);
		n += len;
		page[n++] = '\0';
		p += len;
		while (*p == ' ')
			p++;
		if (*p == '\n') {
			page[n++] = '\0'; /* empty line between paragraphs */
			p++;
		}
	}
	page[n++] = '\n';
	page[n] = '\0';
	UI_driver->fInfoScreen(title, page);
}

/* fMessage shows one 38-column line; longer texts (an ERR reply from
   init) go to an info page, word-wrapped. */
static void long_message(const char *text)
{
	if (strlen(text) <= 38)
		UI_driver->fMessage(text, TRUE);
	else
		wrapped_page("Wi-Fi", text);
}

/* After "wifi connect": poll the status once a second for up to 20 s,
   showing wpa_supplicant's state, until connected with an address or a
   failure ("wifierr") is reported. */
static void wifi_wait_connected(const char *want_ssid)
{
	char status[1024], state[80], ip[64], err[64], word[24], line[128];
	int i, room, connected = FALSE;

	state[0] = ip[0] = err[0] = '\0';
	for (i = 0; i < 20; i++) {
		wait_seconds(1.0);
		if (!ctl_request("status", status, sizeof status))
			continue;
		status_get(status, "wifi", state, sizeof state, "?");
		status_get(status, "ip", ip, sizeof ip, "");
		status_get(status, "wifierr", err, sizeof err, "");
		connected = strncmp(state, "connected", 9) == 0;
		if (err[0] || (connected && ip[0]))
			break;
		/* "SCANNING wifly", "ASSOCIATING wifly", "not connected" */
		if (connected)
			Util_strlcpy(word, "getting address", sizeof word);
		else if (strncmp(state, "not ", 4) == 0 || sscanf(state, "%23s", word) != 1)
			word[0] = '\0';
		snprintf(line, sizeof line, "Connecting... %s", word);
		UI_driver->fMessage(line, FALSE);
	}
	room = 38 - 14 - (int)strlen(ip);
	if (room < 4)
		room = 4;
	if (connected && ip[0])
		snprintf(line, sizeof line, "Connected: %.*s (%.20s)", room, want_ssid, ip);
	else if (err[0])
		snprintf(line, sizeof line, "Wi-Fi: %.30s", err);
	else
		snprintf(line, sizeof line, "Wi-Fi: %.30s", state);
	Log_print("sysmenu: wifi connect \"%s\": %s", want_ssid, line);
	UI_driver->fMessage(line, TRUE);
}

static void wifi_menu(void)
{
	static char results[4096];
	static char ssids[16][64];
	static char flags[16][32];
	static char items[16][80];
	UI_tMenuItem menu[20];
	int option = 0;

	for (;;) {
		char status[1024], state[64], ssid[80], line[160];
		int n = 0, i, choice;
		const char *p;

		ctl_request("status", status, sizeof status);
		status_get(status, "wifi", state, sizeof state, "no adapter");

		UI_driver->fMessage("Scanning...", FALSE);
		if (!ctl_request("wifi scan", results, sizeof results)) {
			UI_driver->fMessage(results[0] ? results : "Wi-Fi not available", TRUE);
			return;
		}
		wait_seconds(4.0);
		ctl_request("wifi results", results, sizeof results);

		/* "net <signal> <flags> <ssid>" per line */
		p = results;
		while (*p && n < 16) {
			const char *nl = strchr(p, '\n');
			size_t len = nl ? (size_t)(nl - p) : strlen(p);
			if (len < sizeof line) {
				int sig;
				char fl[32], ss[64];
				memcpy(line, p, len);
				line[len] = '\0';
				if (sscanf(line, "net %d %31s %63[^\n]", &sig, fl, ss) == 3) {
					Util_strlcpy(ssids[n], ss, sizeof ssids[n]);
					Util_strlcpy(flags[n], fl, sizeof flags[n]);
					snprintf(items[n], sizeof items[n], "%-24.24s %4d dBm %s", ss, sig,
					         strcmp(fl, "open") == 0 ? "open" : "");
					n++;
				}
			}
			p = nl ? nl + 1 : p + len;
		}

		snprintf(ssid, sizeof ssid, "Wi-Fi: %s", state);
		menu[0].flags = UI_ITEM_ACTION; menu[0].retval = -1; menu[0].prefix = NULL; menu[0].item = ssid; menu[0].suffix = NULL;
		for (i = 0; i < n; i++) {
			menu[1 + i].flags = UI_ITEM_ACTION;
			menu[1 + i].retval = i;
			menu[1 + i].prefix = NULL;
			menu[1 + i].item = items[i];
			menu[1 + i].suffix = NULL;
		}
		menu[1 + n].flags = UI_ITEM_ACTION; menu[1 + n].retval = 100; menu[1 + n].prefix = NULL; menu[1 + n].item = (char *)"Rescan"; menu[1 + n].suffix = NULL;
		menu[2 + n].flags = UI_ITEM_ACTION; menu[2 + n].retval = 101; menu[2 + n].prefix = NULL; menu[2 + n].item = (char *)"Forget saved network"; menu[2 + n].suffix = NULL;
		menu[3 + n].flags = UI_ITEM_END; menu[3 + n].retval = 0; menu[3 + n].prefix = NULL; menu[3 + n].item = NULL; menu[3 + n].suffix = NULL;

		choice = UI_driver->fSelect("Wi-Fi networks", 0, option, menu, NULL);
		if (choice < 0)
			return;
		option = choice;
		if (choice == 100)
			continue;
		if (choice == 101) {
			ctl_request("wifi forget", results, sizeof results);
			UI_driver->fMessage("Network forgotten", TRUE);
			continue;
		}
		if (choice < n) {
			char psk[80] = "";
			char cmd[200];
			if (strcmp(flags[choice], "open") != 0) {
				/* WPA2 passphrases are 8..63 characters. (The Atari
				   has no { } ~ ` keys, and its font cannot show them
				   in this title either: see KNOWN-ISSUES.md.) The
				   edit box is 32 columns, the title must fit. */
				if (!UI_driver->fEditString("Password (8-63 characters)", psk, sizeof psk))
					continue;
			}
			snprintf(cmd, sizeof cmd, "wifi connect\t%s\t%s", ssids[choice], psk);
			UI_driver->fMessage("Connecting...", FALSE);
			if (!ctl_request(cmd, results, sizeof results)) {
				long_message(results[0] ? results : "Connection failed");
				continue;
			}
			wifi_wait_connected(ssids[choice]);
			return;
		}
	}
}

/* "Boot into" (what starts when nothing is mounted): BASIC, No BASIC,
   ASH or FujiNet CONFIG. Choosing another one makes init save it and
   restart the emulator; TRUE when that is under way. */
static int boot_menu(const char *current)
{
	static const char *const names[] = { "basic", "nobasic", "ash", "fujinet" };
	static char ash_item[32];
	UI_tMenuItem menu[] = {
		UI_MENU_ACTION(1, "BASIC"),
		UI_MENU_ACTION(2, "No BASIC (self test)"),
		UI_MENU_ACTION(3, ash_item),
		UI_MENU_ACTION(4, "FujiNet CONFIG"),
		UI_MENU_END
	};
	char cmd[32], reply[512], line[64];
	int have_ash = Util_fileexists(ASH_CART);
	int cur = 1, choice;

	for (choice = 0; choice < 4; choice++)
		if (strcmp(current, names[choice]) == 0)
			cur = choice + 1;
	Util_strlcpy(ash_item, have_ash ? "ASH" : "ASH (not installed)", sizeof ash_item);
	choice = UI_driver->fSelect("Boot into", UI_SELECT_POPUP, cur, menu, NULL);
	if (choice < 1 || choice == cur)
		return FALSE;
	if (choice == 3 && !have_ash) {
		UI_driver->fMessage("ASH is not installed on this stick", TRUE);
		return FALSE;
	}
	snprintf(cmd, sizeof cmd, "boot %s", names[choice - 1]);
	if (!ctl_request(cmd, reply, sizeof reply)) {
		UI_driver->fMessage(reply[0] ? reply : "Not available", TRUE);
		return FALSE;
	}
	KMS_BOOT_SetTarget(names[choice - 1]);
	snprintf(line, sizeof line, "Restarting into %s...", boot_label(names[choice - 1]));
	UI_driver->fMessage(line, FALSE);
	return TRUE; /* init restarts us now */
}

/* "TV system": PAL or NTSC, switched live (no cold start) and saved as
   atari800's DEFAULT_TV_MODE. The display mode follows on the next start
   when the panel has one closer to the new rate. */
static void tv_menu(void)
{
	UI_tMenuItem menu[] = {
		UI_MENU_ACTION(Atari800_TV_PAL, "PAL (50 Hz)"),
		UI_MENU_ACTION(Atari800_TV_NTSC, "NTSC (60 Hz, default)"),
		UI_MENU_END
	};
	char line[64];
	const char *name;
	int choice = UI_driver->fSelect("TV system", UI_SELECT_POPUP, Atari800_tv_mode, menu, NULL);
	if (choice != Atari800_TV_PAL && choice != Atari800_TV_NTSC)
		return;
	if (choice != Atari800_tv_mode) {
		Atari800_SetTVMode(choice);
		CFG_WriteConfig();
	}
	name = choice == Atari800_TV_PAL ? "PAL: 50 Hz" : "NTSC: 60 Hz";
	if (KMS_VIDEO_PacingLocked())
		snprintf(line, sizeof line, "%s, smooth on this screen", name);
	else if (KMS_VIDEO_HasModeFor(KMS_VIDEO_AtariFps()))
		snprintf(line, sizeof line, "%s, smooth after a restart", name);
	else
		snprintf(line, sizeof line, "%s, judder on a %.0f Hz screen", name, KMS_VIDEO_refresh);
	Log_print("sysmenu: TV system: %s", line);
	UI_driver->fMessage(line, TRUE);
}

/* ---- display options ---------------------------------------------------- */

/* A popup over NAMES (N of them) with CUR selected; the choice or -1. */
static int choose(const char *title, const char *const *names, int n, int cur)
{
	UI_tMenuItem menu[8];
	int i;
	for (i = 0; i < n && i < 7; i++) {
		menu[i].flags = UI_ITEM_ACTION;
		menu[i].retval = i;
		menu[i].prefix = NULL;
		menu[i].item = (char *)names[i];
		menu[i].suffix = NULL;
	}
	menu[i].flags = UI_ITEM_END; menu[i].retval = 0; menu[i].prefix = NULL; menu[i].item = NULL; menu[i].suffix = NULL;
	i = UI_driver->fSelect(title, UI_SELECT_POPUP, cur, menu, NULL);
	return i >= 0 && i < n ? i : -1;
}

static void display_changed(const char *what)
{
	KMS_VIDEO_Relayout(); /* the menu itself is drawn with it: visible at once */
	CFG_WriteConfig();
	Log_print("sysmenu: display: %s", what);
}

/* Free scaling sliders: the value is factor * 20 - 20 (0.05 steps from
   1.00); USER points at the axis (0 horizontal, 1 vertical, 2 both).
   The picture follows while the slider moves (Esc puts it back). */
static double free_saved_x, free_saved_y;

static void factor_label(char *label, int value, void *user_data)
{
	int axis = *(int *)user_data;
	double f = (value + 20) / 20.0;
	KMS_VIDEO_free_x = axis == 1 ? free_saved_x : f;
	KMS_VIDEO_free_y = axis == 0 ? free_saved_y : f;
	KMS_VIDEO_Relayout();
	snprintf(label, 11, "%.2f", f);
}

/* "Pixel aspect: 0.95 (4:3 TV)": the Atari picture's 336 columns on a
   4:3 TV make pixels 4*240/(3*336) as wide as tall. */
static void aspect_text(char *buf, size_t size)
{
	double a = KMS_VIDEO_free_y > 0 ? KMS_VIDEO_free_x / KMS_VIDEO_free_y : 0;
	double tv = 4.0 * 240 / (3.0 * 336);
	snprintf(buf, size, "Pixel aspect: %.2f%s", a,
	         fabs(a - tv) <= 0.02 * tv ? " (4:3 TV)" : fabs(a - 1.0) <= 0.02 ? " (square)" : "");
}

/* The Free scaling popup; FALSE when another scaling mode is wanted. */
static int free_menu(void)
{
	static char x_item[40], y_item[40], both_item[40], aspect_item[40];
	int option = 0;

	for (;;) {
		UI_tMenuItem menu[] = {
			UI_MENU_ACTION(0, x_item),
			UI_MENU_ACTION(1, y_item),
			UI_MENU_ACTION(2, both_item),
			UI_MENU_LABEL(aspect_item),
			UI_MENU_ACTION(3, "Other scaling (Integer, Fit, Fill)"),
			UI_MENU_ACTION_ACCEL(4, "Back", "Esc"),
			UI_MENU_END
		};
		int axis, cur, max, v;
		double maxf;

		snprintf(x_item, sizeof x_item, "Horizontal: %.2f", KMS_VIDEO_free_x);
		snprintf(y_item, sizeof y_item, "Vertical:   %.2f", KMS_VIDEO_free_y);
		if (fabs(KMS_VIDEO_free_x - KMS_VIDEO_free_y) < 0.005)
			snprintf(both_item, sizeof both_item, "Both:       %.2f", KMS_VIDEO_free_y);
		else
			snprintf(both_item, sizeof both_item, "Both:       (make equal)");
		aspect_text(aspect_item, sizeof aspect_item);

		option = UI_driver->fSelect("Free scaling", UI_SELECT_POPUP, option, menu, NULL);
		if (option == 3)
			return FALSE;
		if (option < 0 || option > 2)
			return TRUE;
		axis = option;
		if (axis == 0)
			maxf = KMS_VIDEO_FreeMax(0);
		else if (axis == 1)
			maxf = KMS_VIDEO_FreeMax(1);
		else
			maxf = KMS_VIDEO_FreeMax(0) < KMS_VIDEO_FreeMax(1) ? KMS_VIDEO_FreeMax(0) : KMS_VIDEO_FreeMax(1);
		max = (int)(maxf * 20 + 1e-6) - 20;
		if (max < 0)
			max = 0;
		cur = (int)((axis == 0 ? KMS_VIDEO_free_x : KMS_VIDEO_free_y) * 20 + 0.5) - 20;
		free_saved_x = KMS_VIDEO_free_x;
		free_saved_y = KMS_VIDEO_free_y;
		v = UI_driver->fSelectSlider(axis == 0 ? "Horizontal" : axis == 1 ? "Vertical" : "Both",
		                             cur, max, factor_label, &axis);
		if (v < 0) { /* Esc: back to the factors from before */
			KMS_VIDEO_free_x = free_saved_x;
			KMS_VIDEO_free_y = free_saved_y;
			KMS_VIDEO_Relayout();
		}
		else {
			char what[40];
			KMS_VIDEO_ScaleFactors(what, sizeof what);
			display_changed(what);
		}
	}
}

static void sharpness_label(char *label, int value, void *user_data)
{
	(void)user_data;
	KMS_VIDEO_SetNtscSharpness(value / 20.0 - 1.0);
	snprintf(label, 11, "%+.2f", value / 20.0 - 1.0);
}

/* The TV look popup: Crisp or one of the filter presets, and the
   sharpness on top of it (FILTER_NTSC_* in the config, atari800's own). */
static void tv_look_menu(void)
{
	static const char *const names[] = { "Composite", "S-Video", "RGB monitor" };
	static char sharp_item[40];
	int option = KMS_VIDEO_NtscLook() ? KMS_VIDEO_NtscPreset() + 1 : 0;

	if (option > 3)
		option = 4; /* Custom/Monochrome: on the sharpness line */
	for (;;) {
		UI_tMenuItem menu[] = {
			UI_MENU_ACTION(0, "Crisp (no filter)"),
			UI_MENU_ACTION(1, "Composite (soft, colour bleed)"),
			UI_MENU_ACTION(2, "S-Video (sharper, less bleed)"),
			UI_MENU_ACTION(3, "RGB monitor (sharp, artifacts)"),
			UI_MENU_ACTION(4, sharp_item),
			UI_MENU_ACTION_ACCEL(5, "Back", "Esc"),
			UI_MENU_END
		};
		double s = KMS_VIDEO_NtscSharpness();
		int v;

		snprintf(sharp_item, sizeof sharp_item, "Sharpness: %+.2f", s);
		option = UI_driver->fSelect("TV look", UI_SELECT_POPUP, option, menu, NULL);
		switch (option) {
		case 0:
			if (KMS_VIDEO_NtscLook() && KMS_VIDEO_SetNtscLook(FALSE)) {
				CFG_WriteConfig();
				Log_print("sysmenu: display: TV look Crisp");
			}
			break;
		case 1:
		case 2:
		case 3:
			if (KMS_VIDEO_SetNtscPreset(option - 1)) {
				CFG_WriteConfig();
				Log_print("sysmenu: display: TV look %s", names[option - 1]);
			}
			break;
		case 4:
			v = UI_driver->fSelectSlider("Sharpness", (int)((s + 1.0) * 20 + 0.5), 40, sharpness_label, NULL);
			if (v >= 0) {
				CFG_WriteConfig();
				Log_print("sysmenu: display: TV sharpness %+.2f", KMS_VIDEO_NtscSharpness());
			}
			else
				KMS_VIDEO_SetNtscSharpness(s);
			break;
		default:
			return;
		}
	}
}

/* "Display...": scaling, smoothing, scanlines, borders, TV look, TV
   system, brightness. Every change is live and saved (KMS_* keys; the TV
   look is atari800's ARTIFACT_NTSC, the TV system DEFAULT_TV_MODE). */
static void display_menu(void)
{
	static const char *const scales[] = { "Integer (sharpest)", "Fit (4:3, larger)", "Fill (whole screen)",
	                                      "Free (set the factors)" };
	static const char *const scans[] = { "Off", "Light (75%)", "Medium (50%)", "Strong (25%)" };
	static const char *const borders[] = { "Full (384: all of it)", "TV (336, default)", "Tight (320)" };
	static char scale_item[40], smooth_item[40], scan_item[40], border_item[40], look_item[40];
	static char tv_item[40], bri_item[40];
	static const char *const border_labels[] = { "Full", "TV", "Tight" };
	int option = 1;

	for (;;) {
		UI_tMenuItem menu[] = {
			UI_MENU_ACTION(1, scale_item),
			UI_MENU_ACTION(2, smooth_item),
			UI_MENU_ACTION(3, scan_item),
			UI_MENU_ACTION(4, border_item),
			UI_MENU_ACTION(5, look_item),
			UI_MENU_ACTION(7, tv_item),
			UI_MENU_ACTION(8, bri_item),
			UI_MENU_ACTION_ACCEL(6, "Back", "Esc"),
			UI_MENU_END
		};
		int integer = KMS_VIDEO_scale_mode == KMS_SCALE_INTEGER;
		int pal = Atari800_tv_mode == Atari800_TV_PAL;
		int bri = backlight_get();
		int v;
		char factors[20];

		KMS_VIDEO_ScaleFactors(factors, sizeof factors);
		snprintf(scale_item, sizeof scale_item, "Scaling: %s  %s", KMS_VIDEO_ScaleLabel(), factors);
		snprintf(smooth_item, sizeof smooth_item, "Smoothing: %s%s", KMS_VIDEO_smooth ? "Smooth" : "Sharp",
		         integer ? " (not Integer)" : "");
		snprintf(scan_item, sizeof scan_item, "Scanlines: %s%s", KMS_VIDEO_ScanlinesLabel(),
		         KMS_VIDEO_ScanlinesPossible() ? "" : " (needs 2x)");
		snprintf(border_item, sizeof border_item, "Borders: %s", border_labels[KMS_VIDEO_borders]);
		if (KMS_VIDEO_NtscLook())
			snprintf(look_item, sizeof look_item, "TV look: %s, sharp %+.2f", KMS_VIDEO_NtscPresetLabel(),
			         KMS_VIDEO_NtscSharpness());
		else
			snprintf(look_item, sizeof look_item, "TV look: Crisp%s", pal ? " (PAL)" : "");
		snprintf(tv_item, sizeof tv_item, "TV system: %s", pal ? "PAL" : "NTSC");
		if (bri >= 0)
			snprintf(bri_item, sizeof bri_item, "Brightness: %d%%", bri);
		else
			snprintf(bri_item, sizeof bri_item, "Brightness: not controllable");

		option = UI_driver->fSelect("Display", 0, option, menu, NULL);
		switch (option) {
		case 1:
			if (KMS_VIDEO_scale_mode == KMS_SCALE_FREE && free_menu())
				break;
			v = choose("Scaling", scales, 4, KMS_VIDEO_scale_mode);
			if (v == KMS_SCALE_FREE) {
				if (KMS_VIDEO_scale_mode != KMS_SCALE_FREE) {
					/* start from where the picture is: nothing moves yet */
					KMS_VIDEO_free_x = KMS_VIDEO_ScaleX();
					KMS_VIDEO_free_y = KMS_VIDEO_ScaleY();
					KMS_VIDEO_scale_mode = v;
					display_changed(KMS_VIDEO_ScaleLabel());
				}
				if (!free_menu()) {
					v = choose("Scaling", scales, 3, KMS_SCALE_FIT);
					if (v >= 0) {
						KMS_VIDEO_scale_mode = v;
						display_changed(KMS_VIDEO_ScaleLabel());
					}
				}
			}
			else if (v >= 0 && v != KMS_VIDEO_scale_mode) {
				KMS_VIDEO_scale_mode = v;
				display_changed(KMS_VIDEO_ScaleLabel());
			}
			break;
		case 2:
			if (integer) {
				UI_driver->fMessage("Integer scaling is already sharp", TRUE);
				break;
			}
			KMS_VIDEO_smooth = !KMS_VIDEO_smooth;
			display_changed(KMS_VIDEO_smooth ? "smooth" : "sharp");
			break;
		case 3:
			v = choose("Scanlines", scans, 4, KMS_VIDEO_scanlines);
			if (v >= 0 && v != KMS_VIDEO_scanlines) {
				KMS_VIDEO_scanlines = v;
				display_changed(scans[v]);
				if (v != KMS_SCANLINES_OFF && !KMS_VIDEO_ScanlinesPossible())
					UI_driver->fMessage("Scanlines need a picture 2x high", TRUE);
			}
			break;
		case 4:
			v = choose("Borders", borders, 3, KMS_VIDEO_borders);
			if (v >= 0 && v != KMS_VIDEO_borders) {
				KMS_VIDEO_borders = v;
				display_changed(borders[v]);
			}
			break;
		case 5:
			if (pal) {
				UI_driver->fMessage("NTSC only (TV system is PAL)", TRUE);
				break;
			}
			tv_look_menu();
			break;
		case 7:
			tv_menu();
			break;
		case 8:
			if (bri >= 0) {
				v = UI_driver->fSelectSlider("Brightness", bri, 100, percent_label, NULL);
				if (v >= 0) {
					backlight_set(v);
					KMS_SYSMENU_brightness = backlight_get();
					CFG_WriteConfig();
				}
			}
			break;
		default:
			return;
		}
	}
}

/* ---- controls ------------------------------------------------------------ */

static void controls_changed(const char *what)
{
	CFG_WriteConfig();
	Log_print("sysmenu: controls: %s", what);
}

/* "Controls...": arrows as cursor keys or joystick 0 (the F11 state),
   keyboard type PC or Atari, which joystick gamepad 1 drives, and the
   gamepad mapping. Live and saved (KMS_ARROWS_JOYSTICK, KMS_KEYBOARD,
   KMS_PAD_SWAP). */
static void controls_menu(void)
{
	static const char *const kbds[] = { "PC (keys type their caps)", "Atari (positional)" };
	static char arrows_item[40], kbd_item[40], pad_item[40];
	int option = 1;

	for (;;) {
		UI_tMenuItem menu[] = {
			UI_MENU_ACTION_ACCEL(1, arrows_item, "F11"),
			UI_MENU_ACTION(2, kbd_item),
			UI_MENU_ACTION(3, pad_item),
			UI_MENU_ACTION(4, "Gamepad buttons"),
			UI_MENU_ACTION_ACCEL(5, "Back", "Esc"),
			UI_MENU_END
		};
		int v;

		snprintf(arrows_item, sizeof arrows_item, "Arrows: %s", KMS_INPUT_arrows_joystick ? "joystick 0" : "cursor keys");
		snprintf(kbd_item, sizeof kbd_item, "Keyboard type: %s", KMS_INPUT_keyboard_pc ? "PC" : "Atari");
		snprintf(pad_item, sizeof pad_item, "Gamepad 1 drives: joystick %d", KMS_INPUT_pad_swap ? 1 : 0);

		option = UI_driver->fSelect("Controls", 0, option, menu, NULL);
		switch (option) {
		case 1:
			KMS_INPUT_arrows_joystick = !KMS_INPUT_arrows_joystick;
			controls_changed(KMS_INPUT_arrows_joystick ? "arrows joystick 0" : "arrows cursor keys");
			break;
		case 2:
			v = choose("Keyboard type", kbds, 2, KMS_INPUT_keyboard_pc ? 0 : 1);
			if (v >= 0 && v != (KMS_INPUT_keyboard_pc ? 0 : 1)) {
				KMS_INPUT_keyboard_pc = v == 0;
				controls_changed(KMS_INPUT_keyboard_pc ? "keyboard pc" : "keyboard atari");
			}
			break;
		case 3:
			KMS_INPUT_pad_swap = !KMS_INPUT_pad_swap;
			controls_changed(KMS_INPUT_pad_swap ? "gamepads swapped" : "gamepads in order");
			break;
		case 4:
			KMS_HELP_GamepadPage();
			break;
		default:
			return;
		}
	}
}

/* ---- network ------------------------------------------------------------- */

/* "Network...": Wi-Fi, the IP address, SSH on/off and its password. */
static void network_menu(void)
{
	static char wifi_item[80], ip_item[64], ssh_item[48], pw_item[40];
	int option = 1;

	for (;;) {
		char status[1024], buf[128], reply[1024];
		UI_tMenuItem menu[] = {
			UI_MENU_SUBMENU(1, wifi_item),
			UI_MENU_LABEL(ip_item),
			UI_MENU_ACTION(2, ssh_item),
			UI_MENU_ACTION(3, pw_item),
			UI_MENU_ACTION_ACCEL(4, "Back", "Esc"),
			UI_MENU_END
		};

		if (!ctl_request("status", status, sizeof status))
			Util_strlcpy(status, "", sizeof status);
		snprintf(wifi_item, sizeof wifi_item, "Wi-Fi: %.30s", status_get(status, "wifi", buf, sizeof buf, "no adapter"));
		snprintf(ip_item, sizeof ip_item, "IP: %.34s", status_get(status, "ip", buf, sizeof buf, "no network"));
		snprintf(ssh_item, sizeof ssh_item, "Remote access (SSH): %s",
		         strcmp(status_get(status, "ssh", buf, sizeof buf, "?"), "on") == 0 ? "on" : "off");
		snprintf(pw_item, sizeof pw_item, "SSH password: %s",
		         strcmp(status_get(status, "password", buf, sizeof buf, "none"), "set") == 0 ? "set" : "not set");

		option = UI_driver->fSelect("Network", 0, option, menu, NULL);
		switch (option) {
		case 1:
			wifi_menu();
			break;
		case 2: {
			int on = strcmp(status_get(status, "ssh", buf, sizeof buf, "off"), "on") == 0;
			if (ctl_request(on ? "ssh off" : "ssh on", reply, sizeof reply)) {
				if (on)
					UI_driver->fMessage("SSH stopped", TRUE);
				else
					UI_driver->fInfoScreen("Remote access",
						"SSH is running as root.\0"
						"Key: atari/ssh/id_ed25519 on the stick\0"
						"\0"
						"ssh -i <key> root@<ip shown in menu>\0"
						"\n");
			}
			else
				UI_driver->fMessage(reply[0] ? reply : "Not available", TRUE);
			break;
		}
		case 3: {
			char pw[64] = "";
			if (UI_driver->fEditString("SSH password (empty = keys only)", pw, sizeof pw)) {
				char cmd[96];
				snprintf(cmd, sizeof cmd, "password %s", pw[0] ? pw : "clear");
				if (ctl_request(cmd, reply, sizeof reply))
					UI_driver->fMessage(pw[0] ? "Password set for root" : "Password cleared", TRUE);
				else
					UI_driver->fMessage(reply[0] ? reply : "Failed", TRUE);
			}
			break;
		}
		default:
			return;
		}
	}
}

/* The Network line of the top level: the Wi-Fi network when connected,
   else the address (Ethernet), else the Wi-Fi state. */
static void network_label(const char *status, char *out, size_t size)
{
	char wifi[96], ip[64];
	status_get(status, "wifi", wifi, sizeof wifi, "no adapter");
	status_get(status, "ip", ip, sizeof ip, "");
	if (strncmp(wifi, "connected ", 10) == 0)
		snprintf(out, size, "Network: %.28s", wifi + 10);
	else if (ip[0])
		snprintf(out, size, "Network: %.28s", ip);
	else
		snprintf(out, size, "Network: %.28s", wifi);
}

/* ---- install onto the laptop's disk ------------------------------------ */

/* "500 GB", "8.6 GB", "1.0 TB" (decimal, as disks are sold) */
static void size_label(char *out, size_t size, const char *bytes)
{
	double b = strtod(bytes, NULL);
	if (b >= 1e12)
		snprintf(out, size, "%.1f TB", b / 1e12);
	else if (b >= 10e9)
		snprintf(out, size, "%.0f GB", b / 1e9);
	else
		snprintf(out, size, "%.1f GB", b / 1e9);
}

/* Send "install <disk>" and follow init's report: "step <text>" and
   "progress <pct>" lines update the message line until "OK" (TRUE) or
   "ERR <text>" (FALSE, the text in err). */
static int install_stream(const char *cmd, char *err, size_t size)
{
	struct sockaddr_un addr;
	char buf[1024], step[64], line[96];
	size_t used = 0;
	int fd, result = -1;

	Util_strlcpy(err, "the installer stopped unexpectedly", size);
	step[0] = '\0';
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return FALSE;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	Util_strlcpy(addr.sun_path, CTL_SOCK, sizeof addr.sun_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0
	    || write(fd, cmd, strlen(cmd)) < 0 || write(fd, "\n", 1) < 0) {
		close(fd);
		Util_strlcpy(err, "no control socket", size);
		return FALSE;
	}
	while (result < 0) {
		struct pollfd pfd;
		ssize_t got;
		char *nl;
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 100) <= 0) {
			PLATFORM_Keyboard(); /* keep input devices drained; no cancelling now */
			continue;
		}
		got = read(fd, buf + used, sizeof buf - 1 - used);
		if (got <= 0)
			break;
		used += (size_t)got;
		buf[used] = '\0';
		while (result < 0 && (nl = strchr(buf, '\n')) != NULL) {
			*nl = '\0';
			if (strncmp(buf, "step ", 5) == 0) {
				Util_strlcpy(step, buf + 5, sizeof step);
				UI_driver->fMessage(step, FALSE);
			}
			else if (strncmp(buf, "progress ", 9) == 0) {
				snprintf(line, sizeof line, "%s %d%%", step, atoi(buf + 9));
				UI_driver->fMessage(line, FALSE);
			}
			else if (strcmp(buf, "OK") == 0)
				result = TRUE;
			else if (strncmp(buf, "ERR", 3) == 0) {
				Util_strlcpy(err, buf[3] ? buf + 4 : "failed", size);
				result = FALSE;
			}
			used -= (size_t)(nl + 1 - buf);
			memmove(buf, nl + 1, used + 1);
		}
		if (used >= sizeof buf - 1)
			used = 0; /* an over-long line: drop it */
	}
	close(fd);
	return result == TRUE;
}

/* "Install onto this laptop's disk...": choose the disk, confirm, type
   ERASE, then follow the install. TRUE when the laptop is rebooting. */
static int install_menu(void)
{
	static char disks[2048];
	static char names[8][32], models[8][64], sizes[8][16], labels[8][100];
	UI_tMenuItem menu[9];
	char text[512], typed[16], cmd[48], err[256], reply[256];
	const char *p;
	int n = 0, choice = 0, i, j;

	if (!ctl_request("disks", disks, sizeof disks)) {
		UI_driver->fMessage(disks[0] ? disks : "Not available", TRUE);
		return FALSE;
	}
	for (p = disks; *p && n < 8; ) {
		char bytes[32];
		const char *nl = strchr(p, '\n');
		if (sscanf(p, "disk %31s %31s %63[^\n]", names[n], bytes, models[n]) == 3) {
			size_label(sizes[n], sizeof sizes[n], bytes);
			n++;
		}
		p = nl ? nl + 1 : p + strlen(p);
	}
	if (n == 0) {
		UI_driver->fMessage("No internal disk found", TRUE);
		return FALSE;
	}
	if (n > 1) {
		for (i = 0; i < n; i++) {
			int twin = FALSE;
			for (j = 0; j < n; j++)
				if (j != i && strcmp(models[i], models[j]) == 0 && strcmp(sizes[i], sizes[j]) == 0)
					twin = TRUE;
			/* identical disks: the device name tells them apart */
			if (twin)
				snprintf(labels[i], sizeof labels[i], "%.24s (%s) %s", models[i], sizes[i], names[i]);
			else
				snprintf(labels[i], sizeof labels[i], "%.28s (%s)", models[i], sizes[i]);
			menu[i].flags = UI_ITEM_ACTION;
			menu[i].retval = i;
			menu[i].prefix = NULL;
			menu[i].item = labels[i];
			menu[i].suffix = NULL;
		}
		menu[n].flags = UI_ITEM_END; menu[n].retval = 0; menu[n].prefix = NULL; menu[n].item = NULL; menu[n].suffix = NULL;
		choice = UI_driver->fSelect("Install onto", UI_SELECT_POPUP, 0, menu, NULL);
		if (choice < 0 || choice >= n)
			return FALSE;
	}
	snprintf(text, sizeof text,
	         "This erases EVERYTHING on %.63s, %.15s.\n"
	         "The laptop will then boot the Atari from its own disk, BIOS or UEFI, "
	         "and the USB stick is no longer needed.\n"
	         "Nothing else can be kept.", models[choice], sizes[choice]);
	wrapped_page("Install onto this laptop", text);
	typed[0] = '\0';
	if (!UI_driver->fEditString("Type ERASE to continue", typed, sizeof typed) || strcmp(typed, "ERASE") != 0) {
		Log_print("sysmenu: install onto %s cancelled", names[choice]);
		UI_driver->fMessage("Cancelled: nothing was changed", TRUE);
		return FALSE;
	}
	Log_print("sysmenu: installing onto %s (%s, %s)", names[choice], models[choice], sizes[choice]);
	snprintf(cmd, sizeof cmd, "install %.31s", names[choice]);
	UI_driver->fMessage("Starting...", FALSE);
	if (!install_stream(cmd, err, sizeof err)) {
		Log_print("sysmenu: install failed: %s", err);
		snprintf(text, sizeof text, "Install failed.\n%s\nThe stick keeps running as before.", err);
		wrapped_page("Install", text);
		return FALSE;
	}
	Log_print("sysmenu: install onto %s done", names[choice]);
	UI_driver->fInfoScreen("Installed",
		"Installed. Remove the USB stick\0"
		"and press RETURN to reboot.\0"
		"\n");
	ctl_request("reboot", reply, sizeof reply);
	return TRUE;
}

/* ---- the menu --------------------------------------------------------- */

/* Back in the System menu after a menu of its own closed (they leave
   UI_is_active FALSE). */
static void ui_back(void)
{
	UI_is_active = TRUE;
	UI_driver->fInit();
}

void KMS_SYSMENU_Run(void)
{
	static char boot_item[48], eject_item[48], vol_item[32], net_item[48], reboot_item[48], ver_item[48];
	int option = 1;
	int done = FALSE;
	int owner = KMS_INPUT_menu_owner;

	UI_is_active = TRUE;
	UI_driver->fInit();
	KMS_SYSMENU_HintSeen();
	KMS_INPUT_menu_owner = 8;

	/* F8 (or F1/F9) pressed inside closes every level: KMS_INPUT_close_menus */
	while (!done && !KMS_INPUT_close_menus) {
		char status[1024], buf[128], reply[1024];
		int have_ctl, vol;
		UI_tMenuItem menu[] = {
			UI_MENU_ACTION_ACCEL(1, "Library", "F9"),
			UI_MENU_ACTION(2, boot_item),
			UI_MENU_ACTION(3, eject_item),
			UI_MENU_SUBMENU(4, "Display..."),
			UI_MENU_ACTION(5, vol_item),
			UI_MENU_SUBMENU(6, "Controls..."),
			UI_MENU_SUBMENU(7, net_item),
			UI_MENU_ACTION(8, "Help: keys and menus"),
			UI_MENU_ACTION(9, "Hardware status"),
			UI_MENU_ACTION(17, "Report a problem..."),
			UI_MENU_ACTION(10, "Install onto this laptop's disk..."),
			UI_MENU_ACTION(11, "Save logs to the stick"),
			UI_MENU_ACTION(12, "Restart emulator"),
			UI_MENU_ACTION(13, reboot_item),
			UI_MENU_ACTION(14, "Shut down"),
			UI_MENU_SUBMENU(15, "Advanced: emulator menu..."),
			UI_MENU_ACTION_ACCEL(16, "Back to Atari", "Esc"),
			UI_MENU_LABEL(""),
			UI_MENU_LABEL(ver_item),
			UI_MENU_END
		};
		int efi, i;

		have_ctl = ctl_request("status", status, sizeof status);
		if (!have_ctl)
			Util_strlcpy(status, "", sizeof status);
		{
			/* the install item: only when running from a stick (or an
			   image file on one) and an internal disk is there */
			char media[24], inst[16];
			int show;
			status_get(status, "media", media, sizeof media, "none");
			status_get(status, "installable", inst, sizeof inst, "0");
			show = (strcmp(media, "stick") == 0 || strcmp(media, "image-file") == 0) && atoi(inst) > 0;
			for (i = 0; menu[i].flags != UI_ITEM_END; i++)
				if (menu[i].retval == 10 && !show)
					menu[i].flags = UI_ITEM_HIDDEN;
		}

		snprintf(boot_item, sizeof boot_item, "Boot into: %s", boot_label(boot_of(status, buf, sizeof buf)));
		snprintf(eject_item, sizeof eject_item, "Eject all, back to %s", boot_label(KMS_BOOT_Target()));
		vol = ALSA_GetVolume();
		if (vol >= 0)
			snprintf(vol_item, sizeof vol_item, "Sound: %d%%", vol);
		else
			snprintf(vol_item, sizeof vol_item, "Sound: no sound card");
		network_label(status, net_item, sizeof net_item);
		snprintf(ver_item, sizeof ver_item, "atari-os-x86 %.24s", KMS_SYSMENU_Version());
		efi = strcmp(status_get(status, "efi", buf, sizeof buf, "no"), "yes") == 0;
		/* on UEFI a popup asks: straight back here (BootNext) or normal */
		Util_strlcpy(reboot_item, efi ? "Reboot laptop..." : "Reboot laptop", sizeof reboot_item);

		option = UI_driver->fSelect("System", 0, option, menu, NULL);
		switch (option) {
		case 1:
			if (KMS_LIBRARY_Run())
				done = TRUE; /* a program was started */
			else
				ui_back();
			break;
		case 2:
			if (boot_menu(boot_of(status, buf, sizeof buf)))
				done = TRUE;
			break;
		case 3:
			KMS_BOOT_EjectAll();
			done = TRUE;
			break;
		case 4:
			display_menu();
			break;
		case 5:
			if (vol >= 0) {
				int v = UI_driver->fSelectSlider("Sound volume", vol, 100, percent_label, NULL);
				if (v >= 0) {
					ALSA_SetVolume(v);
					KMS_SYSMENU_volume = v;
					CFG_WriteConfig();
				}
			}
			break;
		case 6:
			controls_menu();
			break;
		case 7:
			network_menu();
			break;
		case 8:
			KMS_HELP_Run();
			break;
		case 9:
			hardware_page(status);
			break;
		case 17:
			report_menu(status);
			break;
		case 10:
			if (install_menu())
				done = TRUE;
			break;
		case 11:
			ctl_request("logs", reply, sizeof reply);
			UI_driver->fMessage("Logs written to atari/logs", TRUE);
			break;
		case 12:
			ctl_request("restart-emulator", reply, sizeof reply);
			done = TRUE;
			break;
		case 13:
			if (efi) {
				/* BootNext is a one-shot request for the next start only;
				   a normal reboot follows the machine's own boot order
				   (the other OS on a dual-boot laptop). */
				static const char *const hows[] = { "Straight back into Atari", "Normal reboot (boot order, other OS)" };
				int how = choose("Reboot", hows, 2, 0);
				if (how < 0)
					break;
				if (how == 0 && !ctl_request("bootnext", reply, sizeof reply)) {
					UI_driver->fMessage(reply[0] ? reply : "Could not set BootNext", TRUE);
					break;
				}
			}
			ctl_request("reboot", reply, sizeof reply);
			done = TRUE;
			break;
		case 14:
			ctl_request("poweroff", reply, sizeof reply);
			done = TRUE;
			break;
		case 15:
			if (KMS_SYSMENU_advanced_open) {
				done = TRUE; /* opened from that menu: back to it */
				break;
			}
			/* atari800's own menu (save states, machine type, cartridge
			   types, ...); its "System menu" entry comes back here,
			   anything else (Esc, a reset, a program) to the Atari */
			KMS_SYSMENU_advanced_open = TRUE;
			KMS_SYSMENU_reopen = FALSE;
			UI_Run();
			KMS_SYSMENU_advanced_open = FALSE;
			if (KMS_SYSMENU_reopen)
				ui_back();
			else
				done = TRUE;
			KMS_SYSMENU_reopen = FALSE;
			break;
		case 16:
		case -1:
		case -2:
			done = TRUE;
			break;
		default:
			break;
		}
	}

	UI_is_active = FALSE;
	/* flush keypresses; an F8 pressed inside the menu must not reopen it.
	   AKEY_EXIT is sticky (SIGTERM from init, e.g. after "fujinet on"):
	   leave it for the main loop. */
	{
		int k;
		while ((k = PLATFORM_Keyboard()) != AKEY_NONE && k != AKEY_EXIT)
			Atari800_Sync();
	}
	UI_alt_function = -1;
	KMS_INPUT_menu_owner = owner;
	if (!KMS_INPUT_close_menus)
		KMS_SYSMENU_requested = FALSE; /* a toggle may have asked for another menu */
}
