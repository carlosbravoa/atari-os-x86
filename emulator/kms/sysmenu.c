/*
 * kms/sysmenu.c - the F8 "System" menu
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * Drawn with atari800's own UI toolkit, so it looks like the F1 menu.
 * Anything that needs root-level system control (FujiNet boot mode,
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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/input.h>

#include "atari.h"
#include "akey.h"
#include "input.h"
#include "log.h"
#include "platform.h"
#include "ui.h"
#include "util.h"
#include "kms/kms_sysmenu.h"
#include "kms/kms_video.h"
#include <sys/utsname.h>

#define CTL_SOCK "/run/atari.ctl"

int KMS_SYSMENU_requested = FALSE;
int KMS_SYSMENU_volume = -1;
int KMS_SYSMENU_brightness = -1;
int KMS_SYSMENU_hint_done = FALSE;

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
		KMS_VIDEO_SetHint("F8 = System menu    F1 = Emulator menu", 25.0);
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

static void hardware_page(const char *status)
{
	static char page[2048];
	char line[200], v[128], w[128];
	size_t n = 0;
	FILE *f;

	read_line("/sys/class/dmi/id/product_name", v, sizeof v);
	read_line("/sys/class/dmi/id/sys_vendor", w, sizeof w);
	add_line(page, &n, sizeof page, "%.16s %.20s", w, v);
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
	add_line(page, &n, sizeof page, "CPU: %.34s", v);
	f = fopen("/proc/meminfo", "r");
	if (f) {
		long kb = 0;
		if (fscanf(f, "MemTotal: %ld", &kb) == 1)
			add_line(page, &n, sizeof page, "RAM: %ld MB", kb / 1024);
		fclose(f);
	}
	add_line(page, &n, sizeof page, "Video: %s %dx%d @ %.0f Hz", KMS_VIDEO_driver, KMS_VIDEO_width, KMS_VIDEO_height, KMS_VIDEO_refresh);
	if (read_line("/proc/asound/cards", v, sizeof v)) {
		char *p = strchr(v, ':');
		add_line(page, &n, sizeof page, "Sound: %.32s", p ? p + 2 : v);
	}
	else
		add_line(page, &n, sizeof page, "Sound: no card");
	status_get(status, "wifi", v, sizeof v, "no adapter");
	add_line(page, &n, sizeof page, "Wi-Fi: %.30s", v);
	status_get(status, "ip", v, sizeof v, "none");
	add_line(page, &n, sizeof page, "IP: %s", v);
	status_get(status, "battery", v, sizeof v, "");
	if (v[0])
		add_line(page, &n, sizeof page, "Battery: %s%%", v);
	status_get(status, "media", v, sizeof v, "?");
	status_get(status, "efi", w, sizeof w, "?");
	add_line(page, &n, sizeof page, "Boot: %s, UEFI %s", v, w);
	status_get(status, "fujinet", v, sizeof v, "?");
	status_get(status, "ssh", w, sizeof w, "?");
	add_line(page, &n, sizeof page, "FujiNet boot %s, SSH %s", v, w);
	{
		struct utsname u;
		if (uname(&u) == 0)
			add_line(page, &n, sizeof page, "Linux %.30s", u.release);
	}
	{
		double up = 0;
		f = fopen("/proc/uptime", "r");
		if (f) {
			if (fscanf(f, "%lf", &up) != 1) up = 0;
			fclose(f);
		}
		add_line(page, &n, sizeof page, "Up %d min", (int)(up / 60));
	}
	if (n + 2 < sizeof page) {
		page[n++] = '\n';
		page[n] = '\0';
	}
	UI_driver->fInfoScreen("Hardware", page);
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
				if (!UI_driver->fEditString("Password", psk, sizeof psk))
					continue;
			}
			snprintf(cmd, sizeof cmd, "wifi connect\t%s\t%s", ssids[choice], psk);
			UI_driver->fMessage("Connecting...", FALSE);
			if (!ctl_request(cmd, results, sizeof results)) {
				UI_driver->fMessage(results[0] ? results : "Connection failed", TRUE);
				continue;
			}
			wait_seconds(8.0);
			ctl_request("status", status, sizeof status);
			status_get(status, "wifi", state, sizeof state, "?");
			snprintf(line, sizeof line, "Wi-Fi: %s", state);
			UI_driver->fMessage(line, TRUE);
			return;
		}
	}
}

/* ---- the menu --------------------------------------------------------- */

void KMS_SYSMENU_Run(void)
{
	static char fuji_item[48], vol_item[32], bri_item[32], wifi_item[80], ssh_item[48], pw_item[40], info_item[80];
	static char boot_item[48];
	int option = 1;
	int done = FALSE;

	UI_is_active = TRUE;
	UI_driver->fInit();

	while (!done) {
		char status[1024], buf[128], reply[1024];
		int have_ctl, vol, bri;
		UI_tMenuItem menu[] = {
			UI_MENU_ACTION(1, fuji_item),
			UI_MENU_ACTION(2, vol_item),
			UI_MENU_ACTION(3, bri_item),
			UI_MENU_SUBMENU(4, wifi_item),
			UI_MENU_ACTION(5, ssh_item),
			UI_MENU_ACTION(11, pw_item),
			UI_MENU_LABEL(info_item),
			UI_MENU_ACTION(12, "Hardware status"),
			UI_MENU_LABEL(""),
			UI_MENU_ACTION(13, boot_item),
			UI_MENU_ACTION(14, "Make Atari the default boot"),
			UI_MENU_ACTION(6, "Save logs to the stick"),
			UI_MENU_ACTION(7, "Restart emulator"),
			UI_MENU_ACTION(8, "Reboot laptop"),
			UI_MENU_ACTION(9, "Shut down"),
			UI_MENU_ACTION_ACCEL(10, "Back to Atari", "Esc"),
			UI_MENU_END
		};
		int efi;

		have_ctl = ctl_request("status", status, sizeof status);
		if (!have_ctl)
			Util_strlcpy(status, "", sizeof status);

		snprintf(fuji_item, sizeof fuji_item, "FujiNet boot: %s",
		         strcmp(status_get(status, "fujinet", buf, sizeof buf, "?"), "on") == 0 ? "on" : "off");
		vol = ALSA_GetVolume();
		if (vol >= 0)
			snprintf(vol_item, sizeof vol_item, "Volume: %d%%", vol);
		else
			snprintf(vol_item, sizeof vol_item, "Volume: no sound card");
		bri = backlight_get();
		if (bri >= 0)
			snprintf(bri_item, sizeof bri_item, "Brightness: %d%%", bri);
		else
			snprintf(bri_item, sizeof bri_item, "Brightness: not controllable");
		snprintf(wifi_item, sizeof wifi_item, "Wi-Fi: %s", status_get(status, "wifi", buf, sizeof buf, "no adapter"));
		snprintf(ssh_item, sizeof ssh_item, "Remote access (SSH): %s",
		         strcmp(status_get(status, "ssh", buf, sizeof buf, "?"), "on") == 0 ? "on" : "off");
		snprintf(pw_item, sizeof pw_item, "SSH password: %s",
		         strcmp(status_get(status, "password", buf, sizeof buf, "none"), "set") == 0 ? "set" : "not set");
		efi = strcmp(status_get(status, "efi", buf, sizeof buf, "no"), "yes") == 0;
		snprintf(boot_item, sizeof boot_item, efi ? "Reboot straight back into Atari" : "Reboot (no UEFI boot control here)");
		if (!KMS_SYSMENU_hint_done) {
			KMS_SYSMENU_hint_done = TRUE; /* they found the key */
			KMS_VIDEO_SetHint("", 0);
		}
		{
			char ip[64], bat[16];
			status_get(status, "ip", ip, sizeof ip, "no network");
			status_get(status, "battery", bat, sizeof bat, "");
			if (bat[0])
				snprintf(info_item, sizeof info_item, "IP %.40s   Battery %.8s%%", ip, bat);
			else
				snprintf(info_item, sizeof info_item, "IP %s", ip);
		}

		option = UI_driver->fSelect("System", 0, option, menu, NULL);
		switch (option) {
		case 1: {
			int on = strcmp(status_get(status, "fujinet", buf, sizeof buf, "off"), "on") == 0;
			if (ctl_request(on ? "fujinet off" : "fujinet on", reply, sizeof reply)) {
				UI_driver->fMessage(on ? "Restarting without FujiNet..." : "Restarting into FujiNet...", FALSE);
				done = TRUE; /* init restarts us now */
			}
			else
				UI_driver->fMessage(reply[0] ? reply : "Not available", TRUE);
			break;
		}
		case 2:
			if (vol >= 0) {
				int v = UI_driver->fSelectSlider("Volume", vol, 100, percent_label, NULL);
				if (v >= 0) {
					ALSA_SetVolume(v);
					KMS_SYSMENU_volume = v;
				}
			}
			break;
		case 3:
			if (bri >= 0) {
				int v = UI_driver->fSelectSlider("Brightness", bri, 100, percent_label, NULL);
				if (v >= 0) {
					backlight_set(v);
					KMS_SYSMENU_brightness = backlight_get();
				}
			}
			break;
		case 4:
			wifi_menu();
			break;
		case 5: {
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
		case 6:
			ctl_request("logs", reply, sizeof reply);
			UI_driver->fMessage("Logs written to atari/logs", TRUE);
			break;
		case 11: {
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
		case 12:
			hardware_page(status);
			break;
		case 13:
			if (efi && !ctl_request("bootnext", reply, sizeof reply)) {
				UI_driver->fMessage(reply[0] ? reply : "Could not set BootNext", TRUE);
				break;
			}
			ctl_request("reboot", reply, sizeof reply);
			done = TRUE;
			break;
		case 14:
			if (ctl_request("bootdefault", reply, sizeof reply))
				UI_driver->fMessage("Atari is now first in the UEFI boot order", TRUE);
			else
				UI_driver->fMessage(reply[0] ? reply : "Not booted from UEFI", TRUE);
			break;
		case 7:
			ctl_request("restart-emulator", reply, sizeof reply);
			done = TRUE;
			break;
		case 8:
			ctl_request("reboot", reply, sizeof reply);
			done = TRUE;
			break;
		case 9:
			ctl_request("poweroff", reply, sizeof reply);
			done = TRUE;
			break;
		case 10:
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
	KMS_SYSMENU_requested = FALSE;
}
