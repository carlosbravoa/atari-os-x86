/*
 * init.c - PID 1 for atari-os-x86
 *
 * The whole userspace of the appliance is this program plus the emulator
 * (and, later, FujiNet-PC and the network helpers). It:
 *
 *   1. mounts the kernel filesystems,
 *   2. loads drivers for the hardware present (modalias coldplug, no udev),
 *   3. finds the boot stick, mounts it, and attaches the module/firmware
 *      squashfs ("modloop") so GPU, sound and network drivers can load,
 *   4. starts the emulator and keeps it running,
 *   5. powers the machine off on the power button, lid, or emulator exit.
 *
 * Kernel command line options:
 *   atari.debug=1      spawn a root shell on the serial console (ttyS0)
 *   atari.nomedia=1    do not wait for the boot stick
 *   atari.noemu=1      set everything up but do not start the emulator
 *   atari.emudelay=N   wait N seconds before starting the emulator (debugging)
 *   atari.nofujinet=1  do not start FujiNet-PC or networking
 *
 * Licence: GPL-2.0-or-later, like the emulator it serves.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <linux/loop.h>
#include <linux/reboot.h>

#define MEDIA        "/media"
#define HOSTFS       "/.host"   /* partition holding atari-os*.img (Ventoy) */
#define MEDIA_OS     MEDIA "/atari-os"
#define MEDIA_USER   MEDIA "/atari"
#define MODLOOP_IMG  MEDIA_OS "/modloop.sqfs"
#define MODLOOP_MNT  "/.modloop"
#define EMULATOR     "/usr/bin/atari800"
#define BUSYBOX      "/bin/busybox"
#define FUJINET_DIR  "/usr/lib/fujinet"      /* binary + data/ (web UI, autorun) */
#define FUJINET_BIN  FUJINET_DIR "/fujinet"
#define UDHCPC_SCRIPT "/etc/udhcpc.script"
#define MEDIA_WAIT_S 15

static int debug = 0;
static int nomedia = 0;
static int noemu = 0;
static int emudelay = 0;
static int nofujinet = 0;
static int media_mounted = 0;
static pid_t emu_pid = -1;
static pid_t shell_pid = -1;
static pid_t fuji_pid = -1;
static int emu_restarts = 0;
static int fujinet_enabled = 0;

/* ---- logging --------------------------------------------------------- */

static int kmsg_fd = -1;

static void logmsg(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof buf - 2)
		n = sizeof buf - 2;
	buf[n++] = '\n';
	buf[n] = '\0';
	if (kmsg_fd >= 0) {
		char kbuf[560];
		snprintf(kbuf, sizeof kbuf, "<6>init: %s", buf);
		if (write(kmsg_fd, kbuf, strlen(kbuf)) < 0) { /* ignore */ }
	}
	if (debug)
		fputs(buf, stderr);
}

/* ---- small helpers --------------------------------------------------- */

static void mkdir_p(const char *path, mode_t mode)
{
	char tmp[256];
	char *p;
	snprintf(tmp, sizeof tmp, "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, mode);
			*p = '/';
		}
	}
	mkdir(tmp, mode);
}

static int exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static int read_file(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;
	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return (int)n;
}

static void write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, val, strlen(val)) < 0) { /* ignore */ }
	close(fd);
}

/* Run a program to completion; returns its exit status or -1. */
static int run(char *const argv[])
{
	pid_t pid = fork();
	int status;
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			if (!debug) {
				dup2(null, 1);
				dup2(null, 2);
			}
			if (null > 2)
				close(null);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* ---- modules ---------------------------------------------------------- */

/* Aliases already handed to modprobe, so each coldplug pass only loads
   what is new. A plain list is fine for a few hundred entries. */
static char **seen_alias = NULL;
static int seen_count = 0, seen_cap = 0;

static int alias_seen(const char *a)
{
	int i;
	for (i = 0; i < seen_count; i++)
		if (strcmp(seen_alias[i], a) == 0)
			return 1;
	if (seen_count == seen_cap) {
		seen_cap = seen_cap ? seen_cap * 2 : 256;
		seen_alias = realloc(seen_alias, seen_cap * sizeof *seen_alias);
	}
	seen_alias[seen_count++] = strdup(a);
	return 0;
}

#define MAX_BATCH 400
static char *batch[MAX_BATCH + 8];
static int batch_n = 0;

static void flush_batch(void)
{
	char *argv[MAX_BATCH + 8];
	int i, n = 0;
	if (batch_n == 0)
		return;
	argv[n++] = BUSYBOX;
	argv[n++] = "modprobe";
	argv[n++] = "-a";
	argv[n++] = "-b";
	argv[n++] = "-q";
	for (i = 0; i < batch_n; i++)
		argv[n++] = batch[i];
	argv[n] = NULL;
	run(argv);
	for (i = 0; i < batch_n; i++)
		free(batch[i]);
	batch_n = 0;
}

static void queue_alias(const char *a)
{
	if (alias_seen(a))
		return;
	batch[batch_n++] = strdup(a);
	if (batch_n == MAX_BATCH)
		flush_batch();
}

static void walk_modalias(const char *dir, int depth)
{
	DIR *d;
	struct dirent *e;
	char path[512];
	char alias[256];

	if (depth > 16)
		return;
	d = opendir(dir);
	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (e->d_type == DT_DIR)
			walk_modalias(path, depth + 1);
		else if (strcmp(e->d_name, "modalias") == 0 && read_file(path, alias, sizeof alias) > 0)
			queue_alias(alias);
	}
	closedir(d);
}

/* Modules nothing announces through a modalias. */
static const char *const base_modules[] = {
	"evdev", "loop", "squashfs", "vfat", "exfat", "ntfs3", "nls_cp437", "nls_iso8859-1",
	"nls_utf8", "usb-storage", "uas", "sd_mod", "button", "af_packet", NULL
};

static void coldplug(void)
{
	int i;
	for (i = 0; base_modules[i]; i++)
		queue_alias(base_modules[i]);
	walk_modalias("/sys/devices", 0);
	flush_batch();
}

/* ---- boot media -------------------------------------------------------- */

static int try_image_file(const char *dev);

static int try_mount_media(const char *dev)
{
	static const char *const fstypes[] = { "vfat", "exfat", NULL };
	int i;
	for (i = 0; fstypes[i]; i++) {
		const char *opts = i == 0 ? "flush,shortname=mixed,errors=continue" : "";
		if (mount(dev, MEDIA, fstypes[i], MS_NOATIME | MS_NODEV | MS_NOSUID, opts) == 0) {
			if (exists(MODLOOP_IMG))
				return 1;
			umount2(MEDIA, MNT_DETACH);
		}
	}
	return 0;
}

static int scan_block_devices(void)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;
	char dev[320];
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' || strncmp(e->d_name, "loop", 4) == 0
		    || strncmp(e->d_name, "ram", 3) == 0 || strncmp(e->d_name, "zram", 4) == 0)
			continue;
		snprintf(dev, sizeof dev, "/dev/%s", e->d_name);
		if (!exists(dev))
			continue;
		found = try_mount_media(dev);
	}
	closedir(d);
	if (found)
		return 1;
	/* Second pass: our image as a file on somebody else's partition. */
	d = opendir("/sys/class/block");
	if (!d)
		return 0;
	while (!found && (e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' || strncmp(e->d_name, "loop", 4) == 0
		    || strncmp(e->d_name, "ram", 3) == 0 || strncmp(e->d_name, "zram", 4) == 0)
			continue;
		snprintf(dev, sizeof dev, "/dev/%s", e->d_name);
		if (!exists(dev))
			continue;
		found = try_image_file(dev);
	}
	closedir(d);
	return found;
}

static void find_media(void)
{
	int waited = 0;
	mkdir_p(MEDIA, 0755);
	while (waited < MEDIA_WAIT_S * 1000) {
		if (scan_block_devices()) {
			media_mounted = 1;
			logmsg("boot media mounted after %d ms", waited);
			return;
		}
		/* USB enumeration is still going on: new devices keep appearing. */
		walk_modalias("/sys/devices", 0);
		flush_batch();
		sleep_ms(250);
		waited += 250;
	}
	logmsg("no boot media found; continuing with the built-in system");
}

static int attach_loop(const char *image, int partscan, int writable)
{
	int ctl, img, dev, num;
	char path[32];

	ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		return -1;
	num = ioctl(ctl, LOOP_CTL_GET_FREE);
	close(ctl);
	if (num < 0)
		return -1;
	snprintf(path, sizeof path, "/dev/loop%d", num);
	img = open(image, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	if (img < 0)
		return -1;
	dev = open(path, O_RDWR | O_CLOEXEC);
	if (dev < 0) {
		close(img);
		return -1;
	}
	if (ioctl(dev, LOOP_SET_FD, img) < 0) {
		close(dev);
		close(img);
		return -1;
	}
	if (partscan) {
		struct loop_info64 info;
		memset(&info, 0, sizeof info);
		info.lo_flags = LO_FLAGS_PARTSCAN;
		ioctl(dev, LOOP_SET_STATUS64, &info);
	}
	close(img);
	close(dev);
	return num;
}

/* Ventoy (or any stick with our image copied onto it as a file): the
   kernel and initramfs were loaded by the firmware-side loader, but the
   FAT inside the image is what we want as /media. Find the image file on
   a mountable partition, loop-attach it, mount its first partition. */
static int try_image_file(const char *dev)
{
	static const char *const fstypes[] = { "vfat", "exfat", "ntfs3", NULL };
	int i, found = 0;

	mkdir_p(HOSTFS, 0755);
	for (i = 0; fstypes[i] && !found; i++) {
		DIR *d;
		struct dirent *e;
		if (mount(dev, HOSTFS, fstypes[i], MS_NOATIME | MS_NODEV | MS_NOSUID, "") != 0)
			continue;
		d = opendir(HOSTFS);
		while (d && !found && (e = readdir(d)) != NULL) {
			char img[400], part[48];
			size_t len = strlen(e->d_name);
			int num, tries;
			if (strncmp(e->d_name, "atari-os", 8) != 0 || len < 4
			    || strcmp(e->d_name + len - 4, ".img") != 0)
				continue;
			snprintf(img, sizeof img, HOSTFS "/%s", e->d_name);
			num = attach_loop(img, 1, 1);
			if (num < 0)
				continue;
			snprintf(part, sizeof part, "/dev/loop%dp1", num);
			for (tries = 0; tries < 20 && !exists(part); tries++)
				sleep_ms(50);
			if (mount(part, MEDIA, "vfat", MS_NOATIME | MS_NODEV | MS_NOSUID,
			          "flush,shortname=mixed,errors=continue") == 0) {
				if (exists(MODLOOP_IMG)) {
					logmsg("using image file %s on %s (%s)", e->d_name, dev, fstypes[i]);
					found = 1;
				}
				else
					umount2(MEDIA, MNT_DETACH);
			}
		}
		if (d)
			closedir(d);
		if (!found)
			umount2(HOSTFS, MNT_DETACH);
	}
	return found;
}

static void mount_modloop(void)
{
	char dev[32];
	int num = attach_loop(MODLOOP_IMG, 0, 0);
	if (num < 0) {
		logmsg("modloop: cannot attach loop device: %s", strerror(errno));
		return;
	}
	snprintf(dev, sizeof dev, "/dev/loop%d", num);
	mkdir_p(MODLOOP_MNT, 0755);
	if (mount(dev, MODLOOP_MNT, "squashfs", MS_RDONLY, "") < 0) {
		logmsg("modloop: mount failed: %s", strerror(errno));
		return;
	}
	/* The initramfs carries a module subset at /lib/modules; the full set
	   goes over the top of it. Firmware is only in the modloop. */
	mkdir_p("/lib/modules", 0755);
	mkdir_p("/lib/firmware", 0755);
	if (mount(MODLOOP_MNT "/modules", "/lib/modules", NULL, MS_BIND, NULL) < 0)
		logmsg("modloop: bind modules failed: %s", strerror(errno));
	if (mount(MODLOOP_MNT "/firmware", "/lib/firmware", NULL, MS_BIND, NULL) < 0)
		logmsg("modloop: bind firmware failed: %s", strerror(errno));
	/* Aliases that found no module in the initramfs must be retried now. */
	{
		int i;
		for (i = 0; i < seen_count; i++)
			free(seen_alias[i]);
		seen_count = 0;
	}
	logmsg("modloop attached");
}

/* ---- networking ---------------------------------------------------------- */

/* No network manager: every wired interface gets DHCP in the background.
   Wi-Fi comes later (wpa_supplicant driven from FujiNet's CONFIG). */
static void start_network(void)
{
	DIR *d;
	struct dirent *e;
	char *up_lo[] = { BUSYBOX, "ip", "link", "set", "lo", "up", NULL };

	run(up_lo);
	d = opendir("/sys/class/net");
	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		char wireless[320];
		char *up[] = { BUSYBOX, "ip", "link", "set", NULL, "up", NULL };
		char *dhcp[] = { BUSYBOX, "udhcpc", "-i", NULL, "-b", "-q", "-n", "-t", "0",
		                 "-s", UDHCPC_SCRIPT, "-x", "hostname:atari", NULL };
		char *dhcp_retry[] = { BUSYBOX, "udhcpc", "-i", NULL, "-b", "-s", UDHCPC_SCRIPT,
		                       "-x", "hostname:atari", NULL };
		if (e->d_name[0] == '.' || strcmp(e->d_name, "lo") == 0)
			continue;
		snprintf(wireless, sizeof wireless, "/sys/class/net/%s/wireless", e->d_name);
		if (exists(wireless))
			continue; /* needs association first; later phase */
		up[4] = e->d_name;
		run(up);
		dhcp_retry[3] = e->d_name;
		(void)dhcp;
		/* -b: background on failure and keep trying, so a cable plugged in
		   later still gets an address. */
		run(dhcp_retry);
		logmsg("dhcp started on %s", e->d_name);
	}
	closedir(d);
}

/* ---- FujiNet-PC ---------------------------------------------------------- */

static void start_fujinet(void)
{
	const char *base = media_mounted ? MEDIA_USER "/fujinet" : "/run/fujinet";
	char cfg[320], sd[320];

	snprintf(cfg, sizeof cfg, "%s/fnconfig.ini", base);
	snprintf(sd, sizeof sd, "%s/SD", base);
	mkdir_p(sd, 0755);
	if (!exists(cfg)) {
		/* Seed from the distribution default; FujiNet rewrites it on change. */
		char *cp[] = { BUSYBOX, "cp", FUJINET_DIR "/fnconfig.ini", (char *)cfg, NULL };
		run(cp);
	}

	fuji_pid = fork();
	if (fuji_pid < 0)
		return;
	if (fuji_pid == 0) {
		int null = open("/dev/null", O_RDWR);
		int log = open("/run/fujinet.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		setsid();
		if (null >= 0)
			dup2(null, 0);
		if (log >= 0) {
			dup2(log, 1);
			dup2(log, 2);
		}
		setenv("HOME", base, 1);
		setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
		if (chdir(FUJINET_DIR) < 0) { /* data/ is found relative to cwd */ }
		execl(FUJINET_BIN, FUJINET_BIN, "-c", cfg, "-s", sd, (char *)NULL);
		_exit(127);
	}
	logmsg("fujinet started, pid %d, config %s", (int)fuji_pid, cfg);
}

/* ---- emulator ---------------------------------------------------------- */

static void start_emulator(void)
{
	char *argv[64];
	static char argbuf[4096];
	int n = 0;
	const char *home = media_mounted ? MEDIA_USER "/config" : "/run/atari";

	mkdir_p(home, 0755);
	if (media_mounted) {
		mkdir_p(MEDIA_USER "/disks", 0755);
		mkdir_p(MEDIA_USER "/carts", 0755);
		mkdir_p(MEDIA_USER "/roms", 0755);
	}

	argv[n++] = EMULATOR;
	/* atari800 otherwise keeps its config next to the binary (initramfs). */
	argv[n++] = "-config";
	argv[n++] = (char *)(media_mounted ? MEDIA_USER "/config/atari800.cfg" : "/run/atari/atari800.cfg");
	/* First boot: seed the config so the machine behaves like a real XL,
	   which powers up into BASIC. atari800 fills in the rest on save. */
	if (!exists(argv[n - 1])) {
		FILE *f = fopen(argv[n - 1], "w");
		if (f) {
			fputs("Atari 800 Emulator, Version 7.x\nDISABLE_BASIC=0\n", f);
			fclose(f);
		}
	}
	if (debug) {
		argv[n++] = "-evdev-debug";
		argv[n++] = "-kms-debug";
	}
	if (fujinet_enabled)
		argv[n++] = "-netsio"; /* the emulator is the NetSIO hub FujiNet-PC connects to */
	/* Extra options, one per line, from the stick. */
	{
		char path[256];
		snprintf(path, sizeof path, "%s/atari800.args", home);
		if (read_file(path, argbuf, sizeof argbuf) > 0) {
			char *s = argbuf;
			while (*s && n < 60) {
				char *e = strchr(s, '\n');
				if (e)
					*e = '\0';
				if (*s && *s != '#')
					argv[n++] = s;
				if (!e)
					break;
				s = e + 1;
			}
		}
	}
	argv[n] = NULL;

	emu_pid = fork();
	if (emu_pid < 0) {
		logmsg("fork failed: %s", strerror(errno));
		return;
	}
	if (emu_pid == 0) {
		int null = open("/dev/null", O_RDWR);
		int log = open("/run/atari800.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		sigset_t none;
		/* Undo PID 1's signal setup: the child gets a clean slate. */
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		setsid();
		if (null >= 0)
			dup2(null, 0);
		if (log >= 0) {
			dup2(log, 1);
			dup2(log, 2);
		}
		setenv("HOME", home, 1);
		setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
		setenv("TERM", "linux", 1);
		if (media_mounted)
			if (chdir(MEDIA_USER) < 0) { /* stay in / */ }
		execv(argv[0], argv);
		_exit(127);
	}
	logmsg("emulator started, pid %d, HOME=%s", (int)emu_pid, home);
}

static void start_debug_shell(void)
{
	shell_pid = fork();
	if (shell_pid == 0) {
		int fd;
		setsid();
		fd = open("/dev/ttyS0", O_RDWR);
		if (fd < 0)
			_exit(1);
		ioctl(fd, TIOCSCTTY, 1);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
		setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
		setenv("HOME", "/root", 1);
		execl(BUSYBOX, "sh", "-l", (char *)NULL);
		_exit(1);
	}
}

/* ---- shutdown ---------------------------------------------------------- */

static void shutdown_machine(int how)
{
	int i;
	logmsg("shutting down (%s)", how == LINUX_REBOOT_CMD_RESTART ? "reboot" : "power off");
	if (emu_pid > 0) {
		kill(emu_pid, SIGTERM);
		for (i = 0; i < 30; i++) {
			if (waitpid(emu_pid, NULL, WNOHANG) == emu_pid) {
				emu_pid = -1;
				break;
			}
			sleep_ms(100);
		}
		if (emu_pid > 0) {
			kill(emu_pid, SIGKILL);
			waitpid(emu_pid, NULL, 0);
		}
	}
	kill(-1, SIGTERM);
	sleep_ms(300);
	kill(-1, SIGKILL);
	sync();
	if (media_mounted)
		umount2(MEDIA, 0);
	umount2(HOSTFS, 0); /* image-file (Ventoy) case; harmless otherwise */
	sync();
	reboot(how);
	for (;;)
		pause();
}

/* ---- power button / lid ------------------------------------------------ */

#define MAX_BUTTONS 8
static int button_fds[MAX_BUTTONS];
static int button_count = 0;

static void open_buttons(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	char path[320];
	if (!d)
		return;
	while ((e = readdir(d)) != NULL && button_count < MAX_BUTTONS) {
		unsigned long keybits[KEY_CNT / (8 * sizeof(long)) + 1];
		int fd;
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(keybits, 0, sizeof keybits);
		if (ioctl(fd, (int)EVIOCGBIT(EV_KEY, sizeof keybits), keybits) >= 0
		    && (keybits[KEY_POWER / (8 * sizeof(long))] & (1UL << (KEY_POWER % (8 * sizeof(long)))))
		    && !(keybits[KEY_A / (8 * sizeof(long))] & (1UL << (KEY_A % (8 * sizeof(long)))))) {
			button_fds[button_count++] = fd;
			logmsg("watching %s for the power button", e->d_name);
		}
		else
			close(fd);
	}
	closedir(d);
}

static void read_button(int fd)
{
	struct input_event ev[8];
	ssize_t n = read(fd, ev, sizeof ev);
	int i;
	if (n <= 0)
		return;
	for (i = 0; i < (int)(n / sizeof ev[0]); i++) {
		if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1)
			shutdown_machine(LINUX_REBOOT_CMD_POWER_OFF);
	}
}

/* ---- main --------------------------------------------------------------- */

static void parse_cmdline(void)
{
	char buf[1024];
	if (read_file("/proc/cmdline", buf, sizeof buf) < 0)
		return;
	if (strstr(buf, "atari.debug=1"))
		debug = 1;
	if (strstr(buf, "atari.nomedia=1"))
		nomedia = 1;
	if (strstr(buf, "atari.noemu=1"))
		noemu = 1;
	if (strstr(buf, "atari.nofujinet=1"))
		nofujinet = 1;
	{
		const char *p = strstr(buf, "atari.emudelay=");
		if (p)
			emudelay = atoi(p + strlen("atari.emudelay="));
	}
}

int main(void)
{
	sigset_t mask;
	int sfd;
	struct pollfd pfds[MAX_BUTTONS + 1];

	mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
	mkdir_p("/dev/pts", 0755);
	mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "mode=0620,gid=5");

	kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	parse_cmdline();
	logmsg("atari-os-x86 init starting%s", debug ? " (debug)" : "");

	/* Ctrl+Alt+Del and SIGTERM come to us; children are reaped below. */
	reboot(LINUX_REBOOT_CMD_CAD_OFF);
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGUSR2);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

	write_file("/proc/sys/kernel/printk", debug ? "7 4 1 7" : "3 3 1 3");
	write_file("/proc/sys/kernel/modprobe", "/bin/modprobe");

	if (debug)
		start_debug_shell();

	coldplug();
	if (!nomedia)
		find_media();
	if (media_mounted)
		mount_modloop();
	/* With the full module set: GPU, sound, network, odd input devices. */
	coldplug();
	open_buttons();

	fujinet_enabled = !nofujinet && exists(FUJINET_BIN);
	if (fujinet_enabled) {
		start_network();
		start_fujinet();
	}

	if (emudelay > 0) {
		logmsg("waiting %d s before starting the emulator", emudelay);
		sleep_ms(emudelay * 1000);
	}
	if (!noemu)
		start_emulator();

	for (;;) {
		int i, n = 0, r;
		pfds[n].fd = sfd;
		pfds[n].events = POLLIN;
		n++;
		for (i = 0; i < button_count; i++) {
			pfds[n].fd = button_fds[i];
			pfds[n].events = POLLIN;
			n++;
		}
		r = poll(pfds, n, -1);
		if (r < 0)
			continue;
		if (pfds[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			while (read(sfd, &si, sizeof si) == sizeof si) {
				if (si.ssi_signo == SIGCHLD) {
					int status;
					pid_t pid;
					while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
						if (pid == emu_pid) {
							emu_pid = -1;
							if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && !debug) {
								logmsg("emulator exited normally");
								shutdown_machine(LINUX_REBOOT_CMD_POWER_OFF);
							}
							logmsg("emulator died (status 0x%x), restarting", status);
							if (++emu_restarts > 5) {
								logmsg("too many restarts, powering off");
								shutdown_machine(LINUX_REBOOT_CMD_POWER_OFF);
							}
							sleep_ms(1000);
							start_emulator();
						}
						else if (pid == shell_pid) {
							shell_pid = -1;
							start_debug_shell();
						}
						else if (pid == fuji_pid) {
							fuji_pid = -1;
							/* Exit 75 is FujiNet asking to be restarted (it does so
							   on every Atari cold reset): relaunch at once so the
							   Atari's disk boot still finds it. */
							if (!(WIFEXITED(status) && WEXITSTATUS(status) == 75)) {
								logmsg("fujinet died (status 0x%x), restarting", status);
								sleep_ms(1000);
							}
							start_fujinet();
						}
					}
				}
				else if (si.ssi_signo == SIGINT)
					shutdown_machine(LINUX_REBOOT_CMD_RESTART);
				else if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGUSR2)
					shutdown_machine(LINUX_REBOOT_CMD_POWER_OFF);
				else if (si.ssi_signo == SIGUSR1)
					shutdown_machine(LINUX_REBOOT_CMD_RESTART);
			}
		}
		for (i = 1; i < n; i++)
			if (pfds[i].revents & POLLIN)
				read_button(pfds[i].fd);
	}
}
