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
 *   5. powers the machine off on the power button, lid, or emulator exit,
 *   6. mounts USB drives under /media/usb for the emulator's Library (F9),
 *   7. installs the system onto the laptop's own disk when asked (F8).
 *
 * Kernel command line options:
 *   atari.debug=1      spawn a root shell on the serial console (ttyS0)
 *   atari.nomedia=1    do not wait for the boot stick
 *   atari.noemu=1      set everything up but do not start the emulator
 *   atari.emudelay=N   wait N seconds before starting the emulator (debugging)
 *   atari.nofujinet=1  do not start FujiNet-PC or networking
 *   atari.hwsim=1      load mac80211_hwsim (two virtual Wi-Fi radios) before
 *                      networking, to exercise the Wi-Fi code in QEMU
 *   atari.perf=1       log CPU use, package power, temperature and clock
 *                      every 10 s to atari/logs/perf.log (socket: perf on)
 *   atari.media=XXXX-XXXX  the FAT32 volume serial of the copy we were
 *                      booted from (set by the installer on an internal
 *                      disk): that partition is used as /media first
 *
 * Licence: GPL-2.0-or-later, like the emulator it serves.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/netlink.h>
#include <linux/fs.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include "font_atari.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
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
#define CTL_SOCK     "/run/atari.ctl"       /* System menu <-> init */
#define WPA_CTRL_DIR "/run/wpa"
#define WPA_LOG      "/run/wpa.log"        /* wpa_supplicant events (monitor) */
#define WPA_MON_SOCK "/run/wpa-mon.sock"   /* our end of the event monitor */
#define SYSCONF      MEDIA_USER "/config/system.conf"
#define MEDIA_WAIT_S 15
#define USB_DIR      MEDIA "/usb"           /* USB drives, one folder each */
#define ASH_CART     MEDIA_USER "/ash/ASH.CAR" /* the Atari shell (boot=ash) */
#define PERF_LOG     MEDIA_USER "/logs/perf.log"

static int debug = 0;
static int nomedia = 0;
static int noemu = 0;
static int emudelay = 0;
static int nofujinet = 0;
static int hwsim = 0;
static int perf_logging = 0;       /* atari.perf=1 or "perf on" */
static int media_mounted = 0;
static pid_t emu_pid = -1;
static pid_t shell_pid = -1;
static pid_t fuji_pid = -1;
static pid_t ssh_pid = -1;
static int emu_restarts = 0;
static int emu_restart_requested = 0;
static int fujinet_enabled = 0;   /* FujiNet-PC process available */
static int ctl_fd = -1;
static int uevent_fd = -1;
static int uevent_pending = 0;
static int usb_pending = 0;       /* block devices changed: rescan USB drives */
static char wifi_iface[32] = "";
static int wpa_mon_fd = -1;        /* attached to wpa_supplicant's events */
static char wifi_err[48] = "";     /* last failure since the last connect */
static int wifi_scanned = 1;       /* a scan finished since the last connect */

/* What the Atari boots into when nothing is mounted: BASIC (the plain
   XL), No BASIC (the XL self test; the emulator handles it, kms/boot.c),
   the ASH shell cartridge, or FujiNet's CONFIG through NetSIO (-netsio). */
enum { BOOT_BASIC, BOOT_ASH, BOOT_FUJINET, BOOT_NOBASIC, BOOT_COUNT };
static const char *const boot_names[] = { "basic", "ash", "fujinet", "nobasic" };

/* Persisted settings (system.conf on the stick). */
static struct {
	int boot;         /* BOOT_* */
	int ssh;          /* dropbear running */
	char rootpw[160]; /* crypt(3) hash, empty = keys only */
} conf = { 0, 0, "" };
static int efi_ok = 0;
static char media_kind[32] = "none"; /* stick, image-file, disk (installed) or none */
static char media_serial[16] = "";   /* atari.media=: FAT32 serial to prefer */
static pid_t install_pid = -1;       /* the installer child, one at a time */
/* Build version, one line in /etc/atari-os-release (scripts/mkimage.sh). */
static char os_version[48] = "unknown";

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

/* write_file() that says whether the kernel took the value. */
static int write_sysfs(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;
	if (fd < 0)
		return 0;
	n = write(fd, val, strlen(val));
	close(fd);
	return n == (ssize_t)strlen(val);
}

static double mono_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Milliseconds from now until T (monotonic), for poll(); never negative. */
static int ms_until(double t)
{
	double d = (t - mono_now()) * 1000.0;
	if (d <= 0)
		return 0;
	if (d > 600000)
		return 600000;
	return (int)d + 1;
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

/* Run a program, capture its stdout (truncated to size-1). */
static int run_output(char *const argv[], char *buf, size_t size)
{
	int fds[2];
	pid_t pid;
	int status;
	size_t used = 0;

	buf[0] = '\0';
	if (pipe(fds) < 0)
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		dup2(fds[1], 1);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 2);
		}
		close(fds[0]);
		close(fds[1]);
		execv(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	for (;;) {
		ssize_t n = read(fds[0], buf + used, size - 1 - used);
		if (n <= 0)
			break;
		used += (size_t)n;
		if (used >= size - 1)
			break;
	}
	buf[used] = '\0';
	close(fds[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void load_sysconf(void)
{
	char buf[512];
	if (read_file(SYSCONF, buf, sizeof buf) < 0)
		return;
	(void)0;
	/* "boot=" replaced the boolean "fujinet=" (read as boot=fujinet). */
	conf.boot = BOOT_BASIC;
	if (strstr(buf, "boot=ash"))
		conf.boot = BOOT_ASH;
	else if (strstr(buf, "boot=fujinet") || strstr(buf, "fujinet=1"))
		conf.boot = BOOT_FUJINET;
	else if (strstr(buf, "boot=nobasic"))
		conf.boot = BOOT_NOBASIC;
	conf.ssh = strstr(buf, "ssh=1") != NULL;
	{
		char *p = strstr(buf, "rootpw=");
		if (p) {
			char *e;
			p += 7;
			e = strchr(p, '\n');
			if (e)
				*e = '\0';
			snprintf(conf.rootpw, sizeof conf.rootpw, "%s", p);
		}
	}
}

static void save_sysconf(void)
{
	FILE *f;
	if (!media_mounted)
		return;
	f = fopen(SYSCONF, "w");
	if (!f)
		return;
	fprintf(f, "boot=%s\nssh=%d\nrootpw=%s\n", boot_names[conf.boot], conf.ssh, conf.rootpw);
	fclose(f);
}

/* The boot target in effect: FujiNet needs FujiNet-PC, ASH its cartridge. */
static int boot_effective(void)
{
	if (conf.boot == BOOT_FUJINET && !fujinet_enabled)
		return BOOT_BASIC;
	if (conf.boot == BOOT_ASH && !exists(ASH_CART))
		return BOOT_BASIC;
	return conf.boot;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* ---- boot splash ------------------------------------------------------- */

/* A few lines of Atari-font text on whatever DRM device the firmware left
   us, so the screen is not black while drivers load. Released before the
   emulator starts; re-created after the native GPU driver replaces the
   firmware framebuffer. */
static struct {
	int fd;
	uint32_t crtc, conn, fb, handle, w, h, pitch;
	uint64_t size;
	uint8_t *map;
	struct drm_mode_modeinfo mode;
	int nlines;
	char lines[10][64];
} splash = { .fd = -1 };

#define SPLASH_BG 0x00001a4a
#define SPLASH_FG 0x009ad6ff

static void splash_text(int x, int y, int scale, const char *text, uint32_t color)
{
	for (; *text; text++, x += 8 * scale) {
		int c = (unsigned char)*text;
		const unsigned char *g;
		int gy, gx, sy, sx;
		if (c < 32 || c > 127)
			c = 32;
		g = font_atari + (c < 96 ? c - 32 : c) * 8;
		for (gy = 0; gy < 8; gy++)
			for (gx = 0; gx < 8; gx++) {
				if (!(g[gy] & (0x80 >> gx)))
					continue;
				for (sy = 0; sy < scale; sy++)
					for (sx = 0; sx < scale; sx++) {
						uint32_t px = x + gx * scale + sx, py = y + gy * scale + sy;
						if (px < splash.w && py < splash.h)
							*(uint32_t *)(splash.map + py * splash.pitch + px * 4) = color;
					}
			}
	}
}

static void splash_redraw(void)
{
	int i, y;
	uint32_t x, yy;
	if (splash.fd < 0)
		return;
	for (yy = 0; yy < splash.h; yy++)
		for (x = 0; x < splash.w; x++)
			*(uint32_t *)(splash.map + yy * splash.pitch + x * 4) = SPLASH_BG;
	splash_text(64, 48, 3, "atari-os-x86", SPLASH_FG);
	{
		char ver[64];
		snprintf(ver, sizeof ver, "atari-os-x86 %s", os_version);
		splash_text(64, 48 + 24 + 24, 2, ver, SPLASH_FG);
	}
	y = 48 + 24 + 24 + 16 + 24;
	for (i = 0; i < splash.nlines; i++, y += 20)
		splash_text(64, y, 2, splash.lines[i], SPLASH_FG);
	/* Shadow-buffered drivers (simpledrm, bochs) only show changes on a
	   commit: mark the whole framebuffer dirty. */
	if (splash.fb) {
		struct drm_mode_fb_dirty_cmd dirty;
		memset(&dirty, 0, sizeof dirty);
		dirty.fb_id = splash.fb;
		ioctl(splash.fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
	}
}

static void splash_close(void)
{
	if (splash.fd < 0)
		return;
	if (splash.map)
		munmap(splash.map, splash.size);
	if (splash.fb)
		ioctl(splash.fd, DRM_IOCTL_MODE_RMFB, &splash.fb);
	if (splash.handle) {
		struct drm_mode_destroy_dumb d = { splash.handle };
		ioctl(splash.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	close(splash.fd);
	splash.fd = -1;
	splash.map = NULL;
	splash.fb = splash.handle = 0;
}

static int splash_try_card(const char *path)
{
	struct drm_mode_card_res res;
	uint32_t conns[16], crtcs[16], encs[16];
	struct drm_mode_get_connector conn;
	struct drm_mode_modeinfo modes[32];
	struct drm_mode_get_encoder enc;
	struct drm_mode_create_dumb creq;
	struct drm_mode_fb_cmd fb;
	struct drm_mode_map_dumb mreq;
	struct drm_mode_crtc crtc;
	unsigned i;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return 0;
	memset(&res, 0, sizeof res);
	res.connector_id_ptr = (uintptr_t)conns; res.count_connectors = 16;
	res.crtc_id_ptr = (uintptr_t)crtcs; res.count_crtcs = 16;
	res.encoder_id_ptr = (uintptr_t)encs; res.count_encoders = 16;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || res.count_connectors == 0 || res.count_crtcs == 0) {
		logmsg("splash: %s: getresources: %s (%u conns, %u crtcs)", path, strerror(errno), res.count_connectors, res.count_crtcs);
		goto fail;
	}
	for (i = 0; i < res.count_connectors && i < 16; i++) {
		/* First call with count_modes == 0 makes the kernel probe the
		   connector (that is what libdrm does); the second fetches modes. */
		memset(&conn, 0, sizeof conn);
		conn.connector_id = conns[i];
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;
		if (conn.connection != 1 || conn.count_modes == 0)
			continue;
		conn.modes_ptr = (uintptr_t)modes;
		if (conn.count_modes > 32)
			conn.count_modes = 32;
		conn.count_props = conn.count_encoders = 0;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;
		if (conn.count_modes > 0)
			break;
	}
	if (i >= res.count_connectors || i >= 16) {
		logmsg("splash: %s: no connected connector with modes", path);
		goto fail;
	}
	splash.conn = conns[i];
	splash.mode = modes[0];
	splash.crtc = crtcs[0];
	if (conn.encoder_id) {
		memset(&enc, 0, sizeof enc);
		enc.encoder_id = conn.encoder_id;
		if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
			splash.crtc = enc.crtc_id;
	}
	memset(&creq, 0, sizeof creq);
	creq.width = splash.mode.hdisplay; creq.height = splash.mode.vdisplay; creq.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
		logmsg("splash: %s: create_dumb: %s", path, strerror(errno));
		goto fail;
	}
	splash.fd = fd;
	splash.handle = creq.handle; splash.pitch = creq.pitch; splash.size = creq.size;
	splash.w = creq.width; splash.h = creq.height;
	memset(&fb, 0, sizeof fb);
	fb.width = creq.width; fb.height = creq.height; fb.pitch = creq.pitch;
	fb.bpp = 32; fb.depth = 24; fb.handle = creq.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
		logmsg("splash: addfb: %s", strerror(errno));
		goto fail2;
	}
	splash.fb = fb.fb_id;
	memset(&mreq, 0, sizeof mreq);
	mreq.handle = creq.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
		goto fail2;
	splash.map = mmap(0, splash.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
	if (splash.map == MAP_FAILED) {
		splash.map = NULL;
		goto fail2;
	}
	splash_redraw();
	memset(&crtc, 0, sizeof crtc);
	crtc.crtc_id = splash.crtc; crtc.fb_id = splash.fb;
	crtc.set_connectors_ptr = (uintptr_t)&splash.conn; crtc.count_connectors = 1;
	crtc.mode = splash.mode; crtc.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
		logmsg("splash: setcrtc: %s", strerror(errno));
		goto fail2;
	}
	return 1;
fail2:
	splash_close();
	return 0;
fail:
	close(fd);
	return 0;
}

static void splash_open(void)
{
	char path[32];
	int i;
	splash_close();
	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof path, "/dev/dri/card%d", i);
		if (splash_try_card(path)) {
			logmsg("splash on %s, %ux%u", path, splash.w, splash.h);
			return;
		}
	}
	logmsg("splash: no usable DRM device yet");
}

static void splash_line(const char *text)
{
	if (splash.nlines < 10)
		snprintf(splash.lines[splash.nlines++], 64, "%s", text);
	splash_redraw();
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
	/* The x86 CPU alias lists every feature flag: ~700 bytes. */
	char alias[2048];

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
		else if (strcmp(e->d_name, "uevent") == 0 && read_file(path, alias, sizeof alias) > 0) {
			/* Some devices only announce themselves here (the CPUs:
			   cpu:type:x86,... matches coretemp, k10temp, the RAPL and
			   cpufreq drivers). The same string as a modalias file is
			   deduplicated by alias_seen(). */
			char *m = strstr(alias, "MODALIAS=");
			if (m != NULL && (m == alias || m[-1] == '\n')) {
				char *nl = strchr(m, '\n');
				if (nl != NULL)
					*nl = '\0';
				queue_alias(m + 9);
			}
		}
	}
	closedir(d);
}

/* Modules nothing announces through a modalias. */
static const char *const base_modules[] = {
	"evdev", "loop", "squashfs", "vfat", "exfat", "ntfs3", "nls_cp437", "nls_iso8859-1",
	"nls_utf8", "usb-storage", "uas", "sd_mod", "button", "af_packet",
	/* CPU temperature for the hardware page and perf log; the one that
	   does not fit this CPU fails quietly (-q). */
	"coretemp", "k10temp", NULL
};

static void coldplug(void)
{
	int i;
	for (i = 0; base_modules[i]; i++)
		queue_alias(base_modules[i]);
	walk_modalias("/sys/devices", 0);
	flush_batch();
}

/* Hotplug without udev: kernel uevents carry MODALIAS for every device
   that appears later (SOF's audio machine driver, USB gadgets, i2c
   touchpads). Queue them and modprobe in a batch shortly afterwards. */
static void uevent_open(void)
{
	struct sockaddr_nl addr;
	int sz = 1 << 20;
	uevent_fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (uevent_fd < 0)
		return;
	setsockopt(uevent_fd, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof sz);
	memset(&addr, 0, sizeof addr);
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = 1;
	if (bind(uevent_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(uevent_fd);
		uevent_fd = -1;
	}
}

static void usb_remove(const char *name);

static void uevent_read(void)
{
	char buf[8192];
	for (;;) {
		ssize_t n = recv(uevent_fd, buf, sizeof buf - 1, 0);
		char *p, *end;
		const char *devname = NULL;
		int add = 0, block = 0;
		if (n <= 0)
			return;
		buf[n] = '\0';
		end = buf + n;
		if (strncmp(buf, "add@", 4) == 0 || strncmp(buf, "bind@", 5) == 0)
			add = 1;
		for (p = buf; p < end; p += strlen(p) + 1) {
			if (add && strncmp(p, "MODALIAS=", 9) == 0) {
				queue_alias(p + 9);
				uevent_pending = 1;
			}
			else if (strcmp(p, "SUBSYSTEM=block") == 0)
				block = 1;
			else if (strncmp(p, "DEVNAME=", 8) == 0)
				devname = p + 8;
		}
		if (block) {
			/* USB drives: unmount at once on removal, (re)scan shortly
			   after anything else (the partitions follow the disk). */
			if (strncmp(buf, "remove@", 7) == 0 && devname)
				usb_remove(devname);
			usb_pending = 1;
			uevent_pending = 1;
		}
	}
}

/* ---- boot media -------------------------------------------------------- */

static int try_image_file(const char *dev);
static int disk_of(const char *link, char *out, size_t size);

static int try_mount_media(const char *dev)
{
	static const char *const fstypes[] = { "vfat", "exfat", NULL };
	int i;
	for (i = 0; fstypes[i]; i++) {
		const char *opts = i == 0 ? "flush,shortname=mixed,errors=continue" : "";
		if (mount(dev, MEDIA, fstypes[i], MS_NOATIME | MS_NODEV | MS_NOSUID, opts) == 0) {
			if (exists(MODLOOP_IMG)) {
				logmsg("media: %s (%s)", dev, fstypes[i]);
				return 1;
			}
			umount2(MEDIA, MNT_DETACH);
		}
	}
	return 0;
}

/* An internal disk ("sda", "nvme0n1", "mmcblk0"): not removable, not on
   USB, not an SD card (those are the USB code's, see usb_candidate). */
static int disk_internal(const char *disk)
{
	char path[300], real[PATH_MAX], val[16];
	snprintf(path, sizeof path, "/sys/block/%s", disk);
	if (!realpath(path, real) || strstr(real, "/usb"))
		return 0;
	snprintf(path, sizeof path, "/sys/block/%s/removable", disk);
	if (read_file(path, val, sizeof val) <= 0 || strcmp(val, "0") != 0)
		return 0;
	if (strncmp(disk, "mmcblk", 6) == 0) {
		snprintf(path, sizeof path, "/sys/block/%s/device/type", disk);
		if (read_file(path, val, sizeof val) > 0 && strcmp(val, "SD") == 0)
			return 0;
	}
	return 1;
}

/* Is block device NAME (a disk or a partition) on an internal disk? */
static int on_internal_disk(const char *name)
{
	char link[300], disk[64];
	snprintf(link, sizeof link, "/sys/class/block/%s", name);
	return disk_of(link, disk, sizeof disk) && disk_internal(disk);
}

/* The volume serial of a FAT32 filesystem, as blkid prints it
   ("1A2B-3C4D"): the boot sector's volume ID at 0x43. */
static int fat_serial(const char *dev, char *out, size_t size)
{
	unsigned char bs[512];
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	ssize_t n;
	out[0] = '\0';
	if (fd < 0)
		return 0;
	n = pread(fd, bs, sizeof bs, 0);
	close(fd);
	if (n != (ssize_t)sizeof bs || bs[510] != 0x55 || bs[511] != 0xaa || memcmp(bs + 0x52, "FAT32   ", 8) != 0)
		return 0;
	snprintf(out, size, "%02X%02X-%02X%02X", bs[0x46], bs[0x45], bs[0x44], bs[0x43]);
	return 1;
}

static int skip_blockdev(const char *name)
{
	return name[0] == '.' || strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0
	       || strncmp(name, "zram", 4) == 0;
}

/* WAITED: milliseconds since the search began. */
static int scan_block_devices(int waited)
{
	DIR *d;
	struct dirent *e;
	char dev[320], serial[16];
	int pass, found = 0;

	/* atari.media=<serial> (an installed disk's limine.conf): the copy we
	   were booted from comes first. For the first seconds nothing else
	   will do, the disk may not have shown up yet. */
	if (media_serial[0]) {
		d = opendir("/sys/class/block");
		while (d && !found && (e = readdir(d)) != NULL) {
			if (skip_blockdev(e->d_name))
				continue;
			snprintf(dev, sizeof dev, "/dev/%s", e->d_name);
			if (!exists(dev) || !fat_serial(dev, serial, sizeof serial) || strcmp(serial, media_serial) != 0)
				continue;
			logmsg("media: %s has serial %s (atari.media)", dev, serial);
			found = try_mount_media(dev);
		}
		if (d)
			closedir(d);
		if (found || waited < 5000)
			return found;
		logmsg("media: no filesystem with serial %s, looking further", media_serial);
	}
	/* Sticks before internal disks: a copy installed there earlier must
	   not hide the stick we booted from (its kernel would run on the
	   disk's modules, and the stick could not install again). USB comes
	   up later than SATA/NVMe, so internal disks only count after a few
	   seconds without a stick. Within each, a FAT of our own before our
	   image as a file on somebody else's partition. */
	for (pass = 0; pass < (waited < 5000 ? 2 : 4) && !found; pass++) {
		d = opendir("/sys/class/block");
		while (d && !found && (e = readdir(d)) != NULL) {
			if (skip_blockdev(e->d_name) || on_internal_disk(e->d_name) != (pass >= 2))
				continue;
			snprintf(dev, sizeof dev, "/dev/%s", e->d_name);
			if (!exists(dev))
				continue;
			found = (pass & 1) ? try_image_file(dev) : try_mount_media(dev);
		}
		if (d)
			closedir(d);
	}
	return found;
}

static void find_media(void)
{
	int waited = 0;
	mkdir_p(MEDIA, 0755);
	while (waited < MEDIA_WAIT_S * 1000) {
		if (scan_block_devices(waited)) {
			media_mounted = 1;
			if (strcmp(media_kind, "none") == 0)
				snprintf(media_kind, sizeof media_kind, "stick");
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
					snprintf(media_kind, sizeof media_kind, "image-file");
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

/* ---- USB drives ---------------------------------------------------------- */

/* Every filesystem on a USB drive (or SD card) other than the boot stick
   is mounted at /media/usb/<label> for the emulator's Library (F9), which
   finds them by listing that directory. A tmpfs over /media/usb keeps the
   mount points off the stick's FAT. Internal disks are left alone. */
#define MAX_USB      8
#define MAX_IGNORED  16

static struct { char dev[32]; char dir[96]; } usb_vols[MAX_USB];
static int usb_count = 0;
static char usb_ignored[MAX_IGNORED][32]; /* failed mounts, other atari-os sticks */
static int usb_n_ignored = 0;
static char boot_disks[2][32];            /* disk(s) holding MEDIA and HOSTFS */

/* Whole-disk name ("sdb") for a /sys/class/block or /sys/dev/block link. */
static int disk_of(const char *link, char *out, size_t size)
{
	char real[PATH_MAX], probe[PATH_MAX + 16];
	char *last;
	if (!realpath(link, real) || !(last = strrchr(real, '/')))
		return 0;
	snprintf(probe, sizeof probe, "%s/partition", real);
	if (exists(probe)) {
		*last = '\0';
		if (!(last = strrchr(real, '/')))
			return 0;
	}
	snprintf(out, size, "%s", last + 1);
	return 1;
}

static void usb_find_boot_disks(void)
{
	const char *const mnts[2] = { MEDIA, HOSTFS };
	int i;
	for (i = 0; i < 2; i++) {
		struct stat st;
		char link[64];
		boot_disks[i][0] = '\0';
		if (stat(mnts[i], &st) != 0)
			continue;
		snprintf(link, sizeof link, "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
		if (exists(link))
			disk_of(link, boot_disks[i], sizeof boot_disks[i]);
	}
}

static int has_partitions(const char *disk)
{
	char path[300];
	DIR *d;
	struct dirent *e;
	int found = 0;
	snprintf(path, sizeof path, "/sys/class/block/%s", disk);
	d = opendir(path);
	while (d && !found && (e = readdir(d)) != NULL) {
		char probe[600];
		if (strncmp(e->d_name, disk, strlen(disk)) != 0)
			continue;
		snprintf(probe, sizeof probe, "%s/%s/partition", path, e->d_name);
		found = exists(probe);
	}
	if (d)
		closedir(d);
	return found;
}

static int usb_ignored_index(const char *name)
{
	int i;
	for (i = 0; i < usb_n_ignored; i++)
		if (strcmp(usb_ignored[i], name) == 0)
			return i;
	return -1;
}

static void usb_ignore(const char *name)
{
	if (usb_ignored_index(name) < 0 && usb_n_ignored < MAX_IGNORED)
		snprintf(usb_ignored[usb_n_ignored++], sizeof usb_ignored[0], "%.31s", name);
}

static int usb_index(const char *name)
{
	int i;
	for (i = 0; i < usb_count; i++)
		if (strcmp(usb_vols[i].dev, name) == 0)
			return i;
	return -1;
}

/* A block device we should try: on USB or an SD card, not the boot disk,
   not a disk whose partitions are the real candidates. */
static int usb_candidate(const char *name)
{
	char link[300], real[PATH_MAX], disk[64], path[400], type[16];
	int i;
	if (strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0
	    || strncmp(name, "zram", 4) == 0 || strncmp(name, "sr", 2) == 0)
		return 0;
	if (usb_index(name) >= 0 || usb_ignored_index(name) >= 0)
		return 0;
	snprintf(link, sizeof link, "/sys/class/block/%s", name);
	if (!disk_of(link, disk, sizeof disk) || !realpath(link, real))
		return 0;
	for (i = 0; i < 2; i++)
		if (boot_disks[i][0] && strcmp(disk, boot_disks[i]) == 0)
			return 0;
	if (!strstr(real, "/usb")) {
		snprintf(path, sizeof path, "/sys/class/block/%s/device/type", disk);
		if (strncmp(disk, "mmcblk", 6) != 0 || read_file(path, type, sizeof type) <= 0
		    || strcmp(type, "SD") != 0)
			return 0;
	}
	if (strcmp(disk, name) == 0 && has_partitions(name))
		return 0;
	return 1;
}

/* Value of KEY="..." in a blkid line. */
static void blkid_tag(const char *line, const char *key, char *out, size_t size)
{
	char pat[16];
	const char *p, *q;
	size_t len;
	out[0] = '\0';
	snprintf(pat, sizeof pat, " %s=\"", key);
	p = strstr(line, pat);
	if (!p)
		return;
	p += strlen(pat);
	q = strchr(p, '"');
	len = q ? (size_t)(q - p) : strlen(p);
	if (len >= size)
		len = size - 1;
	memcpy(out, p, len);
	out[len] = '\0';
}

static void usb_mount(const char *name)
{
	char dev[48], out[512], label[64], type[16], dir[96], probe[160];
	char *blkid[] = { BUSYBOX, "blkid", dev, NULL };
	const char *fstype, *opts = "";
	int i;

	snprintf(dev, sizeof dev, "/dev/%.31s", name);
	if (strlen(name) > 31 || !exists(dev) || usb_count >= MAX_USB)
		return;
	run_output(blkid, out, sizeof out);
	blkid_tag(out, "TYPE", type, sizeof type);
	blkid_tag(out, "LABEL", label, sizeof label);
	if (strcmp(type, "vfat") == 0) {
		fstype = "vfat";
		opts = "flush,shortname=mixed,errors=continue";
	}
	else if (strcmp(type, "exfat") == 0)
		fstype = "exfat";
	else if (strcmp(type, "ntfs") == 0)
		fstype = "ntfs3";
	else
		return; /* no filesystem we can share (or a partition table) */

	/* The folder name: the label, sanitised, else the device name. */
	for (i = 0; label[i]; i++)
		if (!isalnum((unsigned char)label[i]) && label[i] != '_' && label[i] != '-')
			label[i] = '_';
	while (i > 0 && label[i - 1] == '_')
		label[--i] = '\0';
	snprintf(dir, sizeof dir, USB_DIR "/%.31s", label[0] ? label : name);
	if (exists(dir))
		snprintf(dir, sizeof dir, USB_DIR "/%.31s-%.20s", label[0] ? label : name, name);
	mkdir(dir, 0755);
	if (mount(dev, dir, fstype, MS_NOATIME | MS_NOSUID | MS_NODEV, opts) != 0) {
		logmsg("usb: cannot mount %s (%s): %s", dev, fstype, strerror(errno));
		rmdir(dir);
		usb_ignore(name);
		return;
	}
	snprintf(probe, sizeof probe, "%s/atari-os/modloop.sqfs", dir);
	if (exists(probe)) {
		logmsg("usb: %s is another atari-os stick, not used", dev);
		umount2(dir, MNT_DETACH);
		rmdir(dir);
		usb_ignore(name);
		return;
	}
	snprintf(usb_vols[usb_count].dev, sizeof usb_vols[0].dev, "%.31s", name);
	snprintf(usb_vols[usb_count].dir, sizeof usb_vols[0].dir, "%s", dir);
	usb_count++;
	logmsg("usb: %s (%s) mounted at %s", dev, fstype, dir);
}

static void usb_unmount(int i, int flags)
{
	if (umount2(usb_vols[i].dir, flags) != 0)
		umount2(usb_vols[i].dir, MNT_DETACH);
	rmdir(usb_vols[i].dir);
	logmsg("usb: %s unmounted", usb_vols[i].dir);
	usb_vols[i] = usb_vols[--usb_count];
}

static void usb_remove(const char *name)
{
	int i = usb_index(name);
	if (i >= 0)
		usb_unmount(i, MNT_DETACH);
	i = usb_ignored_index(name);
	if (i >= 0) /* plugged in again later: try afresh */
		memcpy(usb_ignored[i], usb_ignored[--usb_n_ignored], sizeof usb_ignored[0]);
}

static void usb_scan(void)
{
	DIR *d;
	struct dirent *e;
	int i;
	char dev[320];

	for (i = usb_count - 1; i >= 0; i--) {
		snprintf(dev, sizeof dev, "/dev/%s", usb_vols[i].dev);
		if (!exists(dev))
			usb_unmount(i, MNT_DETACH);
	}
	d = opendir("/sys/class/block");
	while (d && (e = readdir(d)) != NULL)
		if (e->d_name[0] != '.' && usb_candidate(e->d_name))
			usb_mount(e->d_name);
	if (d)
		closedir(d);
}

static void usb_setup(void)
{
	mkdir_p(USB_DIR, 0755);
	if (mount("tmpfs", USB_DIR, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0755,size=64k") != 0)
		logmsg("usb: tmpfs on %s failed: %s", USB_DIR, strerror(errno));
	usb_find_boot_disks();
	/* Booted from an internal disk (an installed system), not a stick. */
	if (strcmp(media_kind, "stick") == 0 && boot_disks[0][0] && disk_internal(boot_disks[0])) {
		snprintf(media_kind, sizeof media_kind, "disk");
		logmsg("media: on internal disk %s", boot_disks[0]);
	}
	usb_scan();
}

static void usb_unmount_all(void)
{
	while (usb_count > 0)
		usb_unmount(usb_count - 1, 0);
	umount2(USB_DIR, MNT_DETACH);
}

/* ---- laptop power defaults ------------------------------------------------ */

/* Write VAL to every file matching PATTERN unless skip(path) says not
   to; returns how many took it. */
static int write_glob(const char *pattern, const char *val, int (*skip)(const char *), int *skipped)
{
	glob_t g;
	size_t i;
	int n = 0;
	if (glob(pattern, 0, NULL, &g) != 0)
		return 0;
	for (i = 0; i < g.gl_pathc; i++) {
		if (skip && skip(g.gl_pathv[i])) {
			if (skipped)
				(*skipped)++;
			continue;
		}
		n += write_sysfs(g.gl_pathv[i], val);
	}
	globfree(&g);
	return n;
}

/* USB autosuspend stays off for the boot stick and the hubs it hangs from
   (realpath of /sys/block/<disk>/device below the USB device), and for HID
   devices (interface class 03): keyboards drop keys on resume. */
static int usb_keep_on(const char *control)
{
	char dev[256], real[PATH_MAX], stick[PATH_MAX], link[96], pattern[600];
	const char *name;
	char *slash;
	glob_t g;
	size_t i, len;
	int hid = 0;

	snprintf(dev, sizeof dev, "%s", control);
	if (!(slash = strstr(dev, "/power/control")))
		return 1;
	*slash = '\0';
	name = strrchr(dev, '/') ? strrchr(dev, '/') + 1 : dev;
	if (strchr(name, ':') || !realpath(dev, real))
		return 1; /* interfaces have no autosuspend of their own */
	len = strlen(real);
	for (i = 0; i < 2; i++) {
		if (!boot_disks[i][0])
			continue;
		snprintf(link, sizeof link, "/sys/block/%s/device", boot_disks[i]);
		if (realpath(link, stick) && strncmp(stick, real, len) == 0 && stick[len] == '/')
			return 1;
	}
	snprintf(pattern, sizeof pattern, "%s/%s:*/bInterfaceClass", dev, name);
	if (glob(pattern, 0, NULL, &g) == 0) {
		for (i = 0; i < g.gl_pathc && !hid; i++) {
			char cls[8];
			hid = read_file(g.gl_pathv[i], cls, sizeof cls) > 0 && strcmp(cls, "03") == 0;
		}
		globfree(&g);
	}
	return hid;
}

/* /sys/class/drm/card0/device/..., not the connectors (card0-eDP-1). */
static int drm_not_card(const char *path)
{
	const char *p = path + strlen("/sys/class/drm/");
	return strncmp(p, "card", 4) != 0 || strchr(p, '/') == NULL
	       || memchr(p, '-', (size_t)(strchr(p, '/') - p)) != NULL;
}

/* Cheap, reversible laptop power settings, applied once after the full
   driver set is loaded. Every file is optional (QEMU has almost none). */
static void power_defaults(void)
{
	int aspm, sata, usb, usb_kept = 0, epp, gpu;

	aspm = write_sysfs("/sys/module/pcie_aspm/parameters/policy", "powersave");
	sata = write_glob("/sys/class/scsi_host/host*/link_power_management_policy", "med_power_with_dipm", NULL, NULL);
	usb = write_glob("/sys/bus/usb/devices/*/power/control", "auto", usb_keep_on, &usb_kept);
	epp = write_glob("/sys/devices/system/cpu/cpufreq/policy*/energy_performance_preference", "balance_power",
	                 NULL, NULL);
	gpu = write_glob("/sys/class/drm/card*/device/power/control", "auto", drm_not_card, NULL);
	logmsg("power: pcie aspm %s, sata lpm %d host(s), usb autosuspend %d device(s) (%d kept on: stick, hid),"
	       " epp balance_power %d policies, gpu runtime pm %d",
	       aspm ? "powersave" : "unchanged", sata, usb, usb_kept, epp, gpu);
}

/* ---- networking ---------------------------------------------------------- */

/* ---- wpa_supplicant events ----------------------------------------------- */

/* Alpine's wpa_supplicant has no -f (debug file), and its stdout is not
   flushed line by line, so init attaches to the control interface as an
   event monitor instead: events go to WPA_LOG and the last failure since
   the last connect request becomes the "wifierr" status line. */
static void wpa(const char *a1, const char *a2, const char *a3, char *out, size_t size);

/* Is any network configured? NETWORK-NOT-FOUND also comes after scans
   with nothing configured, which is no failure. */
static int wpa_has_network(void)
{
	char buf[2048];
	char *nl;
	wpa("list_networks", NULL, NULL, buf, sizeof buf);
	nl = strchr(buf, '\n'); /* header: network id / ssid / bssid / flags */
	return nl && isdigit((unsigned char)nl[1]);
}

static void wpa_mon_open(void)
{
	struct sockaddr_un addr;
	if (wpa_mon_fd >= 0 || !wifi_iface[0])
		return;
	wpa_mon_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (wpa_mon_fd < 0)
		return;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, WPA_MON_SOCK, sizeof addr.sun_path - 1);
	unlink(WPA_MON_SOCK);
	if (bind(wpa_mon_fd, (struct sockaddr *)&addr, sizeof addr) < 0)
		goto fail;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", WPA_CTRL_DIR, wifi_iface);
	if (connect(wpa_mon_fd, (struct sockaddr *)&addr, sizeof addr) < 0
	    || send(wpa_mon_fd, "ATTACH", 6, 0) != 6)
		goto fail;
	logmsg("wifi: watching wpa_supplicant events on %s", wifi_iface);
	return;
fail:
	close(wpa_mon_fd);
	wpa_mon_fd = -1;
	unlink(WPA_MON_SOCK);
}

static void wpa_mon_read(void)
{
	char buf[1024];
	ssize_t n;
	while ((n = recv(wpa_mon_fd, buf, sizeof buf - 1, 0)) > 0) {
		char *ev = buf, *nl;
		struct timespec ts;
		FILE *f;
		struct stat st;
		buf[n] = '\0';
		if (ev[0] != '<')
			continue; /* "OK" for ATTACH */
		ev = strchr(ev, '>');
		if (!ev)
			continue;
		ev++;
		nl = strchr(ev, '\n');
		if (nl)
			*nl = '\0';
		if (strstr(ev, "CTRL-EVENT-SCAN-RESULTS"))
			wifi_scanned = 1;
		if (strstr(ev, "SCAN-STARTED") || strstr(ev, "SCAN-RESULTS") || strstr(ev, "BSS-ADDED")
		    || strstr(ev, "BSS-REMOVED"))
			continue;
		if (strstr(ev, "CTRL-EVENT-SSID-TEMP-DISABLED")) {
			if (strstr(ev, "reason=WRONG_KEY"))
				snprintf(wifi_err, sizeof wifi_err, "wrong password");
			else if (strstr(ev, "reason=CONN_FAILED") || strstr(ev, "reason=AUTH_FAILED"))
				snprintf(wifi_err, sizeof wifi_err, "connection failed");
		}
		else if (strstr(ev, "CTRL-EVENT-NETWORK-NOT-FOUND")) {
			/* right after select_network wpa_supplicant judges by the
			   old scan results: wait for a fresh scan */
			if (wifi_scanned && wpa_has_network())
				snprintf(wifi_err, sizeof wifi_err, "network not found");
		}
		else if (strstr(ev, "CTRL-EVENT-CONNECTED"))
			wifi_err[0] = '\0';
		/* keep the log small: start over past 256 KB */
		f = fopen(WPA_LOG, stat(WPA_LOG, &st) == 0 && st.st_size > 256 * 1024 ? "w" : "a");
		if (f) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			fprintf(f, "%5ld.%03ld %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000L, ev);
			fclose(f);
		}
	}
}

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
		up[4] = e->d_name;
		run(up);
		if (exists(wireless)) {
			/* wpa_supplicant with its config on the stick; the System menu
			   drives it through wpa_cli. DHCP keeps retrying until a link. */
			char *wpaconf = (char *)(media_mounted ? MEDIA_USER "/config/wpa_supplicant.conf"
			                                        : "/run/wpa_supplicant.conf");
			char *wpa[] = { "/sbin/wpa_supplicant", "-B", "-i", e->d_name, "-c", wpaconf, NULL };
			if (wifi_iface[0] || !exists("/sbin/wpa_supplicant"))
				continue;
			if (!exists(wpaconf)) {
				FILE *f = fopen(wpaconf, "w");
				if (f) {
					fputs("ctrl_interface=" WPA_CTRL_DIR "\nupdate_config=1\n", f);
					fclose(f);
				}
			}
			mkdir_p(WPA_CTRL_DIR, 0755);
			snprintf(wifi_iface, sizeof wifi_iface, "%.31s", e->d_name);
			run(wpa);
			dhcp_retry[3] = e->d_name;
			(void)dhcp;
			run(dhcp_retry);
			logmsg("wifi: wpa_supplicant and dhcp started on %s", e->d_name);
			wpa_mon_open();
			continue;
		}
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

/* ---- SSH (dropbear) ---------------------------------------------------- */

/* /etc/shadow lives in the initramfs; the hash is kept in system.conf. */
static void apply_rootpw(void)
{
	FILE *f = fopen("/etc/shadow", "w");
	if (!f)
		return;
	fprintf(f, "root:%s:0:0:99999:7:::\n", conf.rootpw[0] ? conf.rootpw : "!");
	fclose(f);
	chmod("/etc/shadow", 0600);
}

/* Keys live on the stick: a host key so clients don't see a new identity
   every boot, and a client key pair generated on first use whose private
   half is written to the stick in OpenSSH format. Holding the stick means
   you may log in; that is the device's trust model. */
static int ssh_prepare(void)
{
	const char *dir = media_mounted ? MEDIA_USER "/ssh" : "/run/ssh";
	char host[320], db[320], priv[320], pub[320], auth[320], pubkey[600];
	FILE *f;

	mkdir_p(dir, 0700);
	snprintf(host, sizeof host, "%s/host_ed25519", dir);
	snprintf(db, sizeof db, "%s/client_ed25519.dropbear", dir);
	snprintf(priv, sizeof priv, "%s/id_ed25519", dir);
	snprintf(pub, sizeof pub, "%s/id_ed25519.pub", dir);
	snprintf(auth, sizeof auth, "%s/authorized_keys", dir);

	if (!exists(host)) {
		char *gen[] = { "/usr/bin/dropbearkey", "-t", "ed25519", "-f", host, NULL };
		if (run(gen) != 0)
			return 0;
	}
	if (!exists(auth)) {
		char *gen[] = { "/usr/bin/dropbearkey", "-t", "ed25519", "-f", db, NULL };
		char *conv[] = { "/usr/bin/dropbearconvert", "dropbear", "openssh", db, priv, NULL };
		char *show[] = { "/usr/bin/dropbearkey", "-y", "-f", db, NULL };
		char *p;
		if (run(gen) != 0 || run(conv) != 0 || run_output(show, pubkey, sizeof pubkey) != 0)
			return 0;
		p = strstr(pubkey, "ssh-ed25519 ");
		if (!p)
			return 0;
		{
			char *nl = strchr(p, '\n');
			if (nl)
				nl[1] = '\0';
		}
		f = fopen(auth, "w");
		if (!f)
			return 0;
		fputs(p, f);
		fclose(f);
		f = fopen(pub, "w");
		if (f) {
			fputs(p, f);
			fclose(f);
		}
		unlink(db);
	}
	mkdir_p("/root/.ssh", 0700);
	chmod("/root", 0700);      /* dropbear refuses keys under group-writable dirs */
	chmod("/root/.ssh", 0700);
	{
		char *cp[] = { BUSYBOX, "cp", auth, "/root/.ssh/authorized_keys", NULL };
		run(cp);
		chmod("/root/.ssh/authorized_keys", 0600);
	}
	return 1;
}

static void ssh_start(void)
{
	const char *dir = media_mounted ? MEDIA_USER "/ssh" : "/run/ssh";
	char host[320];
	if (ssh_pid > 0 || !exists("/usr/sbin/dropbear"))
		return;
	if (!ssh_prepare()) {
		logmsg("ssh: key setup failed");
		return;
	}
	snprintf(host, sizeof host, "%s/host_ed25519", dir);
	ssh_pid = fork();
	if (ssh_pid == 0) {
		int null = open("/dev/null", O_RDWR);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
		/* -F foreground, -r host key, -s (no password logins) unless one is set */
		if (conf.rootpw[0])
			execl("/usr/sbin/dropbear", "dropbear", "-F", "-r", host, (char *)NULL);
		else
			execl("/usr/sbin/dropbear", "dropbear", "-F", "-s", "-r", host, (char *)NULL);
		_exit(127);
	}
	logmsg("ssh: dropbear started, pid %d", (int)ssh_pid);
}

static void ssh_stop(void)
{
	if (ssh_pid <= 0)
		return;
	kill(ssh_pid, SIGTERM);
	waitpid(ssh_pid, NULL, 0);
	ssh_pid = -1;
	logmsg("ssh: stopped");
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
		mkdir_p(MEDIA_USER "/library", 0755);
	}

	argv[n++] = EMULATOR;
	/* atari800 otherwise keeps its config next to the binary (initramfs). */
	argv[n++] = "-config";
	argv[n++] = (char *)(media_mounted ? MEDIA_USER "/config/atari800.cfg" : "/run/atari/atari800.cfg");
	/* First boot: seed the config so the machine behaves like a real XL,
	   which powers up into BASIC, and an NTSC one (60 Hz: smooth on
	   laptop panels; F8 "TV system" switches). atari800 fills in the rest
	   on save. An existing config is the user's and is left alone. */
	if (!exists(argv[n - 1])) {
		FILE *f = fopen(argv[n - 1], "w");
		if (f) {
			fputs("Atari 800 Emulator, Version 7.x\nDISABLE_BASIC=0\nDEFAULT_TV_MODE=NTSC\n", f);
			fclose(f);
		}
	}
	if (debug) {
		argv[n++] = "-evdev-debug";
		argv[n++] = "-kms-debug";
	}
	if (fujinet_enabled && conf.boot == BOOT_FUJINET)
		argv[n++] = "-netsio"; /* the emulator is the NetSIO hub FujiNet-PC connects to */
	if (conf.boot == BOOT_ASH && exists(ASH_CART)) {
		argv[n++] = "-cart"; /* atari800 reads the type from the CART header */
		argv[n++] = ASH_CART;
	}
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

/* ---- EFI boot order ---------------------------------------------------------- */

#define EFIVARS "/sys/firmware/efi/efivars"
#define EFI_GLOBAL "8be4df61-93ca-11d2-aa0d-00e098032b8c"

static int efi_read(const char *name, uint8_t *data, size_t size)
{
	char path[200];
	int fd;
	ssize_t n;
	snprintf(path, sizeof path, EFIVARS "/%s-" EFI_GLOBAL, name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, data, size);
	close(fd);
	return n < 4 ? -1 : (int)(n - 4); /* strip the 4-byte attributes */
}

static int efi_write(const char *name, const uint8_t *data, size_t len)
{
	char path[200];
	uint8_t buf[4 + 256];
	int fd, attr;
	if (len > 256)
		return -1;
	snprintf(path, sizeof path, EFIVARS "/%s-" EFI_GLOBAL, name);
	/* efivarfs marks existing variables immutable; clear that first */
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (ioctl(fd, FS_IOC_GETFLAGS, &attr) == 0 && (attr & FS_IMMUTABLE_FL)) {
			attr &= ~FS_IMMUTABLE_FL;
			ioctl(fd, FS_IOC_SETFLAGS, &attr);
		}
		close(fd);
	}
	buf[0] = 0x07; buf[1] = buf[2] = buf[3] = 0; /* NV | BS | RT */
	memcpy(buf + 4, data, len);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	if (write(fd, buf, len + 4) != (ssize_t)(len + 4)) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static void efi_setup(void)
{
	uint8_t cur[8];
	if (!exists("/sys/firmware/efi"))
		return;
	mount("efivarfs", EFIVARS, "efivarfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	efi_ok = efi_read("BootCurrent", cur, sizeof cur) >= 2;
}

/* Boot this same entry once more on the next start (no boot menu key).
   Only BootNext is written: the user's BootOrder is not ours to change
   (the one exception is the installer, efi_install_entry below). */
static int efi_bootnext(void)
{
	uint8_t cur[8];
	if (!efi_ok || efi_read("BootCurrent", cur, sizeof cur) < 2)
		return -1;
	return efi_write("BootNext", cur + 4, 2);
}

static void put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v & 0xffff);
	put16(p + 2, v >> 16);
}

static void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

#define EFI_ATARI_DESC "Atari"
#define EFI_ATARI_FILE "\\EFI\\BOOT\\BOOTX64.EFI"

/* Is this load option (without the attribute prefix) one of ours? */
static int efi_option_is_atari(const uint8_t *opt, int len)
{
	static const char desc[] = EFI_ATARI_DESC;
	int i;
	if (len < 6 + 2 * (int)sizeof desc)
		return 0;
	for (i = 0; i < (int)sizeof desc; i++)
		if (opt[6 + 2 * i] != (uint8_t)desc[i] || opt[7 + 2 * i] != 0)
			return 0;
	return 1;
}

/* The installer's firmware entry: Boot#### "Atari" =
   HD(1,MBR,<signature>,<start>,<size>)/\EFI\BOOT\BOOTX64.EFI, put first
   in BootOrder. This is the one place the boot order is changed, and only
   because the user has just typed ERASE for a disk that will hold no
   other system. An "Atari" entry from an earlier install is reused. */
static int efi_install_entry(uint32_t sig, uint64_t start, uint64_t size, char *err, size_t esize)
{
	uint8_t opt[256], order[4 + 256], cur[4 + 256], neworder[256];
	char name[16], path[200];
	const char *file = EFI_ATARI_FILE;
	int n, dp, i, k, len, cnt, num = -1;

	put32(opt, 1); /* LOAD_OPTION_ACTIVE */
	n = 6;         /* the device path length goes at 4 */
	for (i = 0; EFI_ATARI_DESC[i]; i++, n += 2)
		put16(opt + n, (unsigned char)EFI_ATARI_DESC[i]);
	put16(opt + n, 0);
	n += 2;
	dp = n;
	opt[n] = 4; opt[n + 1] = 1; put16(opt + n + 2, 42); /* media, hard drive */
	put32(opt + n + 4, 1);
	put64(opt + n + 8, start);
	put64(opt + n + 16, size);
	memset(opt + n + 24, 0, 16);
	put32(opt + n + 24, sig);
	opt[n + 40] = 1; /* MBR partition table */
	opt[n + 41] = 1; /* 32-bit MBR signature */
	n += 42;
	len = 4 + 2 * ((int)strlen(file) + 1);
	opt[n] = 4; opt[n + 1] = 4; put16(opt + n + 2, (unsigned)len); /* media, file path */
	for (i = 0; file[i]; i++)
		put16(opt + n + 4 + 2 * i, (unsigned char)file[i]);
	put16(opt + n + 4 + 2 * i, 0);
	n += len;
	opt[n] = 0x7f; opt[n + 1] = 0xff; put16(opt + n + 2, 4); /* end */
	n += 4;
	put16(opt + 4, (unsigned)(n - dp));

	cnt = efi_read("BootOrder", order, sizeof order);
	cnt = cnt > 0 ? cnt / 2 : 0;
	for (i = 0; i < cnt && num < 0; i++) {
		int e = order[4 + 2 * i] | order[5 + 2 * i] << 8;
		snprintf(name, sizeof name, "Boot%04X", e);
		len = efi_read(name, cur, sizeof cur);
		if (len > 0 && efi_option_is_atari(cur + 4, len))
			num = e;
	}
	for (i = 0; num < 0 && i < 0x10000; i++) {
		snprintf(path, sizeof path, EFIVARS "/Boot%04X-" EFI_GLOBAL, i);
		if (!exists(path))
			num = i;
	}
	if (num < 0) {
		snprintf(err, esize, "no free Boot#### number");
		return -1;
	}
	snprintf(name, sizeof name, "Boot%04X", num);
	if (efi_write(name, opt, (size_t)n) != 0) {
		snprintf(err, esize, "cannot write %s: %s", name, strerror(errno));
		return -1;
	}
	put16(neworder, (unsigned)num);
	for (i = 0, k = 1; i < cnt && k < 128; i++) {
		int e = order[4 + 2 * i] | order[5 + 2 * i] << 8;
		if (e != num)
			put16(neworder + 2 * k++, (unsigned)e);
	}
	if (efi_write("BootOrder", neworder, (size_t)(2 * k)) != 0) {
		snprintf(err, esize, "cannot write BootOrder: %s", strerror(errno));
		return -1;
	}
	if (efi_read("BootOrder", order, sizeof order) < 2 || (order[4] | order[5] << 8) != num) {
		snprintf(err, esize, "the firmware did not keep the boot order");
		return -1;
	}
	logmsg("install: firmware entry %s \"" EFI_ATARI_DESC "\" first in BootOrder (%d entries)", name, k);
	return 0;
}

/* ---- install onto an internal disk ------------------------------------------ */

/* F8 "Install onto this laptop's disk": the whole disk becomes one FAT32
   partition holding a copy of the running media, bootable on BIOS (Limine
   in the MBR) and UEFI (the removable-media path plus a firmware entry).
   The work happens in a child of init that streams "step", "progress" and
   finally "OK"/"ERR" lines to the menu's socket. */
#define INSTALL_MNT      "/mnt/install"
#define INSTALL_OUT      "/run/install.out"   /* output of the current tool */
#define INSTALL_SFDISK   "/run/install.sfdisk"
#define INSTALL_MIN_SECT (8UL * 1024 * 1024)  /* 4 GiB in 512-byte sectors */

/* NAME is a whole disk we may offer: internal, not what we booted from,
   at least 4 GiB. Fills in its size and a model name when asked. */
static int install_candidate(const char *name, uint64_t *bytes, char *model, size_t msize)
{
	static const char *const skip[] = { "loop", "ram", "zram", "sr", "fd", "dm-", "md", "nbd", "zd", NULL };
	char path[300], val[128];
	uint64_t sectors = 0;
	int i;

	if (name[0] == '.' || strlen(name) > 31 || strchr(name, '/'))
		return 0;
	for (i = 0; skip[i]; i++)
		if (strncmp(name, skip[i], strlen(skip[i])) == 0)
			return 0;
	if (strncmp(name, "mmcblk", 6) == 0 && (strstr(name, "boot") || strstr(name, "rpmb")))
		return 0;
	for (i = 0; i < 2; i++)
		if (boot_disks[i][0] && strcmp(name, boot_disks[i]) == 0)
			return 0;
	snprintf(path, sizeof path, "/sys/block/%s", name);
	if (!exists(path) || !disk_internal(name))
		return 0;
	snprintf(path, sizeof path, "/sys/block/%s/hidden", name);
	if (read_file(path, val, sizeof val) > 0 && strcmp(val, "1") == 0)
		return 0;
	snprintf(path, sizeof path, "/sys/block/%s/ro", name);
	if (read_file(path, val, sizeof val) > 0 && strcmp(val, "1") == 0)
		return 0;
	snprintf(path, sizeof path, "/sys/block/%s/size", name);
	if (read_file(path, val, sizeof val) <= 0 || sscanf(val, "%" SCNu64, &sectors) != 1
	    || sectors < INSTALL_MIN_SECT)
		return 0;
	if (bytes)
		*bytes = sectors * 512;
	if (model) {
		char *p;
		snprintf(path, sizeof path, "/sys/block/%s/device/model", name);
		if (read_file(path, val, sizeof val) <= 0) {
			snprintf(path, sizeof path, "/sys/block/%s/device/name", name); /* eMMC */
			if (read_file(path, val, sizeof val) <= 0)
				snprintf(val, sizeof val, "%s", strncmp(name, "vd", 2) == 0 ? "Virtio disk" : name);
		}
		for (p = val; *p == ' '; p++)
			;
		snprintf(model, msize, "%.*s", (int)msize - 1, p);
		for (p = model; *p; p++)
			if ((unsigned char)*p < 32 || (unsigned char)*p > 126)
				*p = '?';
	}
	return 1;
}

static int install_count(void)
{
	DIR *d = opendir("/sys/block");
	struct dirent *e;
	int n = 0;
	while (d && (e = readdir(d)) != NULL)
		n += install_candidate(e->d_name, NULL, NULL, 0);
	if (d)
		closedir(d);
	return n;
}

static void reply(int fd, const char *s);

/* "disks": "disk <name> <bytes> <model>" per candidate, then OK */
static void cmd_disks(int fd)
{
	DIR *d = opendir("/sys/block");
	struct dirent *e;
	char line[200], model[96];
	uint64_t bytes;
	while (d && (e = readdir(d)) != NULL) {
		if (!install_candidate(e->d_name, &bytes, model, sizeof model))
			continue;
		snprintf(line, sizeof line, "disk %.31s %" PRIu64 " %.95s\n", e->d_name, bytes, model);
		reply(fd, line);
	}
	if (d)
		closedir(d);
	reply(fd, "OK\n");
}

/* -- the installer child -- */

static int inst_sock = -1;       /* the menu's connection; -1 once it is gone */
static const char *inst_disk;    /* "sda" */
static uint64_t inst_total = 0;  /* bytes to copy (rounded to clusters) */
static uint64_t inst_base = 0;   /* bytes written to the disk before the copy */
static int inst_pct = -1;

static void inst_send(const char *fmt, ...)
{
	char buf[400];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
	va_end(ap);
	if (n < 0 || inst_sock < 0)
		return;
	if (n > (int)sizeof buf - 2)
		n = sizeof buf - 2;
	buf[n++] = '\n';
	if (send(inst_sock, buf, (size_t)n, MSG_NOSIGNAL) < 0)
		inst_sock = -1; /* the menu went away: carry on regardless */
}

static void inst_step(const char *text)
{
	logmsg("install: %s", text);
	inst_send("step %s", text);
}

static int inst_fail(const char *step, const char *why)
{
	int len = (int)strcspn(step, "."); /* "Partitioning: ...", not "Partitioning...: ..." */
	logmsg("install: %.*s failed: %s", len, step, why);
	inst_send("ERR %.*s: %s", len, step, why);
	return 1;
}

/* Run a tool with stdin from IN (or /dev/null), its output to INSTALL_OUT;
   TICK, if any, is called about once a second while it runs. Returns the
   exit status (0 = success); on failure ERR gets the last line it printed. */
static int inst_run(char *const argv[], const char *in, void (*tick)(void), char *err, size_t esize)
{
	char out[2048];
	int status = 0, out_fd, waited = 0, rc;
	pid_t pid;

	err[0] = '\0';
	out_fd = open(INSTALL_OUT, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	pid = fork();
	if (pid < 0) {
		if (out_fd >= 0)
			close(out_fd);
		snprintf(err, esize, "fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		int fd = open(in ? in : "/dev/null", O_RDONLY);
		if (fd >= 0)
			dup2(fd, 0);
		if (out_fd >= 0) {
			dup2(out_fd, 1);
			dup2(out_fd, 2);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	if (out_fd >= 0)
		close(out_fd);
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid)
			break;
		if (r < 0 && errno != EINTR) {
			status = 0x7f00;
			break;
		}
		sleep_ms(50);
		if (tick && (waited += 50) % 1000 == 0)
			tick();
	}
	rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	if (rc == 0)
		return 0;
	if (read_file(INSTALL_OUT, out, sizeof out) > 0) {
		char *last = strrchr(out, '\n');
		snprintf(err, esize, "%.*s", (int)esize - 1, last ? last + 1 : out);
	}
	if (!err[0])
		snprintf(err, esize, "%s failed (status %d)", argv[0], rc);
	logmsg("install: %s: status %d: %s", argv[0], rc, err);
	return rc;
}

/* Bytes the tree at PATH takes on a FAT with CLUSTER-byte clusters
   (one cluster per directory); other filesystems mounted below are left out. */
static uint64_t inst_tree_bytes(const char *path, dev_t dev, uint64_t cluster)
{
	struct stat st;
	DIR *d;
	struct dirent *e;
	char sub[PATH_MAX];
	uint64_t sum;

	if (lstat(path, &st) != 0 || st.st_dev != dev)
		return 0;
	if (!S_ISDIR(st.st_mode))
		return ((uint64_t)st.st_size + cluster - 1) / cluster * cluster;
	sum = cluster;
	d = opendir(path);
	while (d && (e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
		sum += inst_tree_bytes(sub, dev, cluster);
	}
	if (d)
		closedir(d);
	return sum;
}

/* Bytes written to the target disk so far (/sys/block/X/stat, field 7).
   Used for progress instead of the filesystem's used space, which runs
   ahead by whatever the page cache holds and then stalls in the final
   sync. */
static uint64_t inst_written(void)
{
	char path[64], buf[256];
	uint64_t f[7];
	snprintf(path, sizeof path, "/sys/block/%.31s/stat", inst_disk);
	if (read_file(path, buf, sizeof buf) <= 0
	    || sscanf(buf, "%" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
	              &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6]) != 7)
		return 0;
	return f[6] * 512;
}

/* While cp and the final sync run: "progress <pct>". */
static void inst_copy_tick(void)
{
	uint64_t done = inst_written();
	int pct = 0;
	if (done > inst_base && inst_total > 0)
		pct = (int)((done - inst_base) * 100 / inst_total);
	if (pct > 99)
		pct = 99;
	if (pct != inst_pct) {
		inst_pct = pct;
		inst_send("progress %d", pct);
	}
}

/* Append atari.media=<serial> to every cmdline: of the copy's limine.conf. */
static int inst_edit_conf(const char *serial, char *err, size_t esize)
{
	static char in[16384], out[20000];
	char *line, *next;
	size_t used = 0;
	int edited = 0;
	FILE *f;

	if (read_file(INSTALL_MNT "/limine.conf", in, sizeof in) <= 0) {
		snprintf(err, esize, "no limine.conf on the copy");
		return -1;
	}
	for (line = in; line; line = next) {
		const char *p = line;
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		while (*p == ' ' || *p == '\t')
			p++;
		if (used + strlen(line) + 40 >= sizeof out)
			break;
		if (strncmp(p, "cmdline:", 8) == 0 && !strstr(line, "atari.media=")) {
			used += (size_t)sprintf(out + used, "%s atari.media=%s\n", line, serial);
			edited++;
		}
		else
			used += (size_t)sprintf(out + used, "%s\n", line);
	}
	if (edited == 0) {
		snprintf(err, esize, "no cmdline: in limine.conf");
		return -1;
	}
	f = fopen(INSTALL_MNT "/limine.conf", "w");
	if (!f || fwrite(out, 1, used, f) != used || fflush(f) != 0 || fsync(fileno(f)) != 0) {
		snprintf(err, esize, "cannot write limine.conf: %s", strerror(errno));
		if (f)
			fclose(f);
		return -1;
	}
	fclose(f);
	logmsg("install: limine.conf: atari.media=%s on %d entries", serial, edited);
	return 0;
}

/* Is anything on DISK mounted? */
static int inst_disk_busy(const char *dev)
{
	char line[512];
	size_t len = strlen(dev);
	int busy = 0;
	FILE *f = fopen("/proc/mounts", "r");
	while (f && !busy && fgets(line, sizeof line, f))
		busy = strncmp(line, dev, len) == 0
		       && (line[len] == ' ' || line[len] == 'p' || isdigit((unsigned char)line[len]));
	if (f)
		fclose(f);
	return busy;
}

static int install_run(const char *disk)
{
	static const char *const S_PART = "Partitioning...", *const S_FORMAT = "Formatting...",
		*const S_COPY = "Copying system...", *const S_BOOT = "Installing boot loader...",
		*const S_EFI = "Registering with the firmware...";
	char dev[48], part[48], partname[40], path[300], val[64], err[300], serial[16], spc[8];
	unsigned char mbr[512];
	uint64_t sectors = 0, pstart = 0, psize = 0, lsect, maxsect, free_bytes;
	uint32_t sig = 0;
	unsigned lbs = 512, cl = 8;
	int fd, i;
	FILE *f;
	struct stat st;
	struct statvfs sv;
	DIR *d;
	struct dirent *e;

	snprintf(dev, sizeof dev, "/dev/%.31s", disk);
	snprintf(partname, sizeof partname, "%.31s%s1", disk, isdigit((unsigned char)disk[strlen(disk) - 1]) ? "p" : "");
	snprintf(part, sizeof part, "/dev/%.39s", partname);
	logmsg("install: onto %s (%s)", dev, part);

	/* 1. one MBR partition, type 0c, bootable, 1 MiB to the end (as mkimage.sh) */
	inst_step(S_PART);
	if (inst_disk_busy(dev))
		return inst_fail(S_PART, "the disk is in use (mounted)");
	snprintf(path, sizeof path, "/sys/block/%.31s/size", disk);
	if (read_file(path, val, sizeof val) > 0)
		sscanf(val, "%" SCNu64, &sectors);
	snprintf(path, sizeof path, "/sys/block/%.31s/queue/logical_block_size", disk);
	if (read_file(path, val, sizeof val) > 0 && atoi(val) >= 512)
		lbs = (unsigned)atoi(val);
	lsect = sectors / (lbs / 512); /* in logical sectors, as sfdisk counts */
	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	while (fd >= 0 && sig == 0)
		if (read(fd, &sig, sizeof sig) != (ssize_t)sizeof sig)
			break;
	if (fd >= 0)
		close(fd);
	if (sig == 0)
		sig = (uint32_t)time(NULL) ^ 0x41544152u;
	f = fopen(INSTALL_SFDISK, "w");
	if (!f)
		return inst_fail(S_PART, strerror(errno));
	fprintf(f, "label: dos\nlabel-id: 0x%08x\nunit: sectors\n\n", (unsigned)sig);
	/* MBR counts sectors in 32 bits: larger disks get a 2 TiB partition */
	maxsect = 0xffffffffUL;
	if (lsect > maxsect)
		fprintf(f, "start=2048, size=%" PRIu64 ", type=c, bootable\n", maxsect - 2048);
	else
		fprintf(f, "start=2048, type=c, bootable\n");
	fclose(f);
	{
		char *argv[] = { "/sbin/sfdisk", "--wipe", "always", "--wipe-partitions", "always", "-q", dev, NULL };
		if (inst_run(argv, INSTALL_SFDISK, NULL, err, sizeof err) != 0)
			return inst_fail(S_PART, err);
	}
	fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (ioctl(fd, BLKRRPART) != 0 && errno != EBUSY)
			logmsg("install: BLKRRPART %s: %s", dev, strerror(errno));
		close(fd);
	}
	snprintf(path, sizeof path, "/sys/class/block/%s/start", partname);
	for (i = 0; i < 100 && !(exists(part) && exists(path)); i++)
		sleep_ms(50);
	if (!exists(part) || read_file(path, val, sizeof val) <= 0)
		return inst_fail(S_PART, "the new partition did not appear");
	sscanf(val, "%" SCNu64, &pstart);
	snprintf(path, sizeof path, "/sys/class/block/%s/size", partname);
	if (read_file(path, val, sizeof val) > 0)
		sscanf(val, "%" SCNu64, &psize);
	pstart /= lbs / 512; /* sysfs counts 512-byte units; LBAs are logical sectors */
	psize /= lbs / 512;
	fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || pread(fd, mbr, sizeof mbr, 0) != (ssize_t)sizeof mbr) {
		if (fd >= 0)
			close(fd);
		return inst_fail(S_PART, "cannot read the new partition table back");
	}
	close(fd);
	if ((mbr[440] | mbr[441] << 8 | mbr[442] << 16 | (uint32_t)mbr[443] << 24) != sig
	    || mbr[446] != 0x80 || mbr[450] != 0x0c)
		return inst_fail(S_PART, "the partition table did not come out as written");
	logmsg("install: %s: start %" PRIu64 ", %" PRIu64 " sectors of %u bytes, disk signature %08x",
	       part, pstart, psize, lbs, (unsigned)sig);

	/* 2. FAT32, 8-sector clusters like the stick (bigger past FAT32's
	   cluster count, i.e. on disks over 1 TiB) */
	inst_step(S_FORMAT);
	while (psize / cl > 0x0ffffff0UL && cl < 128)
		cl *= 2;
	snprintf(spc, sizeof spc, "%u", cl);
	{
		char *argv[] = { "/sbin/mkfs.vfat", "-F", "32", "-n", "ATARI", "-s", spc, part, NULL };
		if (inst_run(argv, NULL, NULL, err, sizeof err) != 0)
			return inst_fail(S_FORMAT, err);
	}
	if (!fat_serial(part, serial, sizeof serial))
		return inst_fail(S_FORMAT, "no FAT32 on the new partition");
	logmsg("install: %s formatted, serial %s, %u-sector clusters", part, serial, cl);

	/* 3. copy the running media (the FAT, not /media/usb or /.host) */
	inst_step(S_COPY);
	inst_send("progress %d", inst_pct = 0);
	mkdir_p(INSTALL_MNT, 0755);
	umount2(INSTALL_MNT, MNT_DETACH); /* a failed earlier attempt */
	if (mount(part, INSTALL_MNT, "vfat", MS_NOATIME | MS_NODEV | MS_NOSUID, "shortname=mixed") != 0) {
		snprintf(err, sizeof err, "mount %s: %s", part, strerror(errno));
		return inst_fail(S_COPY, err);
	}
	if (stat(MEDIA, &st) != 0 || statvfs(INSTALL_MNT, &sv) != 0) {
		umount2(INSTALL_MNT, MNT_DETACH);
		return inst_fail(S_COPY, "cannot stat the media");
	}
	sync();
	inst_disk = disk;
	inst_base = inst_written();
	inst_total = 0;
	d = opendir(MEDIA);
	while (d && (e = readdir(d)) != NULL) {
		char src[300];
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || strcmp(e->d_name, "usb") == 0)
			continue;
		snprintf(src, sizeof src, MEDIA "/%s", e->d_name);
		inst_total += inst_tree_bytes(src, st.st_dev, (uint64_t)sv.f_bsize);
	}
	if (d)
		closedir(d);
	free_bytes = (uint64_t)sv.f_bavail * sv.f_frsize;
	logmsg("install: copying %" PRIu64 " KiB, %" PRIu64 " KiB free", inst_total / 1024, free_bytes / 1024);
	if (inst_total + (8u << 20) > free_bytes) {
		umount2(INSTALL_MNT, MNT_DETACH);
		return inst_fail(S_COPY, "the disk is too small");
	}
	d = opendir(MEDIA);
	while (d && (e = readdir(d)) != NULL) {
		char src[300];
		struct stat est;
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || strcmp(e->d_name, "usb") == 0)
			continue;
		snprintf(src, sizeof src, MEDIA "/%s", e->d_name);
		if (lstat(src, &est) != 0 || est.st_dev != st.st_dev)
			continue; /* a mount point below /media */
		{
			char *argv[] = { BUSYBOX, "cp", "-a", src, INSTALL_MNT "/", NULL };
			if (inst_run(argv, NULL, inst_copy_tick, err, sizeof err) != 0) {
				closedir(d);
				umount2(INSTALL_MNT, MNT_DETACH);
				return inst_fail(S_COPY, err);
			}
		}
		inst_copy_tick();
	}
	if (d)
		closedir(d);
	if (!exists(INSTALL_MNT "/limine-bios.sys") || !exists(INSTALL_MNT "/EFI/BOOT/BOOTX64.EFI")
	    || !exists(INSTALL_MNT "/atari-os/modloop.sqfs")) {
		umount2(INSTALL_MNT, MNT_DETACH);
		return inst_fail(S_COPY, "the copy lacks the boot files");
	}
	if (inst_edit_conf(serial, err, sizeof err) != 0) {
		umount2(INSTALL_MNT, MNT_DETACH);
		return inst_fail(S_COPY, err);
	}
	{
		/* most of the copy is still in the page cache: the progress
		   keeps moving while it goes to the disk */
		char *argv[] = { BUSYBOX, "sync", NULL };
		inst_run(argv, NULL, inst_copy_tick, err, sizeof err);
	}
	if (umount(INSTALL_MNT) != 0) {
		snprintf(err, sizeof err, "unmount: %s", strerror(errno));
		umount2(INSTALL_MNT, MNT_DETACH);
		return inst_fail(S_COPY, err);
	}
	inst_send("progress 100");
	logmsg("install: copy done (%" PRIu64 " KiB)", inst_total / 1024);

	/* 4. Limine's BIOS code into the MBR and the gap before 1 MiB */
	inst_step(S_BOOT);
	{
		char *argv[] = { "/usr/bin/limine", "bios-install", dev, NULL };
		if (inst_run(argv, NULL, NULL, err, sizeof err) != 0)
			return inst_fail(S_BOOT, err);
	}

	/* 5. UEFI: a firmware entry, first in the boot order */
	if (exists("/sys/firmware/efi") && exists(EFIVARS "/BootCurrent-" EFI_GLOBAL)) {
		inst_step(S_EFI);
		if (efi_install_entry(sig, pstart, psize, err, sizeof err) != 0)
			return inst_fail(S_EFI, err);
	}
	else
		logmsg("install: BIOS boot, no firmware entry needed");

	/* 6. */
	sync();
	logmsg("install: done, %s boots on its own", dev);
	inst_send("OK");
	return 0;
}

/* "install <disk>": the work happens in a child; the socket stays with it. */
static void cmd_install(int fd, const char *name)
{
	pid_t pid;
	char line[160];
	if (install_pid > 0) {
		reply(fd, "ERR an install is already running\n");
		return;
	}
	if (strcmp(media_kind, "stick") != 0 && strcmp(media_kind, "image-file") != 0) {
		snprintf(line, sizeof line, "ERR install needs a running stick (media %s)\n", media_kind);
		reply(fd, line);
		return;
	}
	if (!install_candidate(name, NULL, NULL, 0)) {
		snprintf(line, sizeof line, "ERR %.40s is not an internal disk that can be installed onto\n", name);
		reply(fd, line);
		return;
	}
	pid = fork();
	if (pid < 0) {
		reply(fd, "ERR fork failed\n");
		return;
	}
	if (pid == 0) {
		signal(SIGPIPE, SIG_IGN);
		if (ctl_fd >= 0)
			close(ctl_fd);
		if (uevent_fd >= 0)
			close(uevent_fd);
		inst_sock = fd;
		_exit(install_run(name));
	}
	install_pid = pid;
	logmsg("install: /dev/%s, pid %d", name, (int)pid);
}

/* ---- control socket ------------------------------------------------------- */

/* ---- performance sampling ------------------------------------------------- */

/* CPU use of our processes (% of one core, from /proc/<pid>/stat deltas),
   package power (RAPL energy counter), CPU temperature (coretemp/k10temp)
   and cpu0's clock. Two independent samplers: the perf log (every 10 s)
   and the "status" reply (at most once a second, when asked). */
#define PERF_NAMES 5
#define PERF_PIDS  64
static const char *const perf_names[PERF_NAMES] = { "atari800", "fujinet", "wpa_supplicant", "dropbear", "init" };

struct perf_state {
	int have;                       /* a previous sample exists */
	double t;                       /* its time (CLOCK_MONOTONIC) */
	int npid;
	int pid[PERF_PIDS];
	unsigned long ticks[PERF_PIDS]; /* utime + stime */
	double energy_uj;               /* < 0: no RAPL */
	/* results of the last sample */
	double cpu[PERF_NAMES];
	double watts;                   /* < 0: n/a */
	double temp;                    /* < -100: n/a */
	int mhz;                        /* 0: n/a */
};
static struct perf_state perf_log_st, perf_status_st;
static double perf_next = 0;        /* next perf log line */

static double perf_read_double(const char *path, double def)
{
	char buf[64];
	if (read_file(path, buf, sizeof buf) <= 0)
		return def;
	return strtod(buf, NULL);
}

static double perf_temp(void)
{
	static char path[300] = "";
	static int searched = 0;
	if (!searched) {
		glob_t g;
		size_t i;
		searched = 1;
		if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &g) == 0) {
			for (i = 0; i < g.gl_pathc && !path[0]; i++) {
				char name[32];
				if (read_file(g.gl_pathv[i], name, sizeof name) > 0
				    && (strcmp(name, "coretemp") == 0 || strcmp(name, "k10temp") == 0)) {
					snprintf(path, sizeof path, "%.*s/temp1_input",
					         (int)(strrchr(g.gl_pathv[i], '/') - g.gl_pathv[i]), g.gl_pathv[i]);
				}
			}
			globfree(&g);
		}
	}
	if (!path[0])
		return -1000;
	return perf_read_double(path, -1000000) / 1000.0;
}

static void perf_sample(struct perf_state *st)
{
	double now = mono_now(), dt = st->have ? now - st->t : 0, uptime;
	long hz = sysconf(_SC_CLK_TCK);
	int npid = 0, i, k;
	int pid[PERF_PIDS];
	unsigned long ticks[PERF_PIDS];
	DIR *d;
	struct dirent *e;
	double energy;

	if (hz <= 0)
		hz = 100;
	uptime = perf_read_double("/proc/uptime", 0);
	for (k = 0; k < PERF_NAMES; k++)
		st->cpu[k] = 0;
	d = opendir("/proc");
	while (d && (e = readdir(d)) != NULL) {
		char path[300], buf[512], *open_p, *close_p;
		unsigned long ut, stime, start;
		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
		if (read_file(path, buf, sizeof buf) <= 0)
			continue;
		open_p = strchr(buf, '(');
		close_p = strrchr(buf, ')');
		if (!open_p || !close_p || close_p < open_p)
			continue;
		*close_p = '\0';
		for (k = 0; k < PERF_NAMES && strcmp(open_p + 1, perf_names[k]) != 0; k++)
			;
		if (k == PERF_NAMES)
			continue;
		/* after "(comm)": state ppid pgrp session tty tpgid flags minflt
		   cminflt majflt cmajflt utime stime cutime cstime priority nice
		   threads itrealvalue starttime */
		if (sscanf(close_p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %lu",
		           &ut, &stime, &start) != 3)
			continue;
		if (npid < PERF_PIDS) {
			pid[npid] = atoi(e->d_name);
			ticks[npid] = ut + stime;
			if (st->have) {
				unsigned long prev = 0;
				for (i = 0; i < st->npid; i++)
					if (st->pid[i] == pid[npid])
						prev = st->ticks[i];
				/* a process that is new since the last sample counts in full */
				if (dt > 0 && ticks[npid] >= prev)
					st->cpu[k] += (double)(ticks[npid] - prev) / ((double)hz * dt) * 100.0;
			}
			else if (uptime * hz > (double)start + hz)
				/* first sample: the average since the process started */
				st->cpu[k] += (double)ticks[npid] / (uptime * hz - (double)start) * 100.0;
			npid++;
		}
	}
	if (d)
		closedir(d);

	energy = perf_read_double("/sys/class/powercap/intel-rapl:0/energy_uj", -1);
	st->watts = -1;
	if (energy >= 0 && st->have && st->energy_uj >= 0 && dt > 0) {
		double delta = energy - st->energy_uj;
		if (delta < 0) /* the counter wrapped */
			delta += perf_read_double("/sys/class/powercap/intel-rapl:0/max_energy_range_uj", 0);
		if (delta >= 0)
			st->watts = delta / 1e6 / dt;
	}
	st->temp = perf_temp();
	st->mhz = (int)(perf_read_double("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 0) / 1000.0);

	st->have = 1;
	st->t = now;
	st->npid = npid;
	memcpy(st->pid, pid, sizeof pid[0] * (size_t)npid);
	memcpy(st->ticks, ticks, sizeof ticks[0] * (size_t)npid);
	st->energy_uj = energy;
}

static void perf_fmt_w(char *out, size_t size, const struct perf_state *st)
{
	if (st->watts >= 0)
		snprintf(out, size, "%.1f", st->watts);
	else
		snprintf(out, size, "n/a");
}

static void perf_fmt_t(char *out, size_t size, const struct perf_state *st)
{
	if (st->temp > -100)
		snprintf(out, size, "%.0f", st->temp);
	else
		snprintf(out, size, "n/a");
}

/* One perf log line, every 10 s while perf logging is on. */
static void perf_tick(void)
{
	char line[400], w[16], t[16], mhz[16];
	size_t n = 0;
	int k, fd;
	const char *path = media_mounted ? PERF_LOG : "/run/perf.log";

	if (!perf_logging || mono_now() < perf_next)
		return;
	perf_next = mono_now() + 10.0;
	perf_sample(&perf_log_st);
	n += (size_t)snprintf(line + n, sizeof line - n, "%.0f s:", perf_read_double("/proc/uptime", 0));
	for (k = 0; k < PERF_NAMES; k++)
		n += (size_t)snprintf(line + n, sizeof line - n, " %s %.1f%%", perf_names[k], perf_log_st.cpu[k]);
	perf_fmt_w(w, sizeof w, &perf_log_st);
	perf_fmt_t(t, sizeof t, &perf_log_st);
	if (perf_log_st.mhz > 0)
		snprintf(mhz, sizeof mhz, "%d", perf_log_st.mhz);
	else
		snprintf(mhz, sizeof mhz, "n/a");
	snprintf(line + n, sizeof line - n, ", package %s W, cpu %s C, cpu0 %s MHz\n", w, t, mhz);
	if (media_mounted)
		mkdir_p(MEDIA_USER "/logs", 0755);
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd >= 0) {
		if (write(fd, line, strlen(line)) < 0) { /* ignore */ }
		close(fd);
	}
	if (debug) {
		line[strcspn(line, "\n")] = '\0';
		logmsg("perf: %s", line);
	}
}

static void perf_start(void)
{
	if (perf_logging)
		return;
	perf_logging = 1;
	perf_log_st.have = 0;
	perf_sample(&perf_log_st); /* baseline: the first line covers 10 s */
	perf_next = mono_now() + 10.0;
	logmsg("perf: logging every 10 s to %s", media_mounted ? PERF_LOG : "/run/perf.log");
}

static void shutdown_machine(int how);

static void ctl_open(void)
{
	struct sockaddr_un addr;
	ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (ctl_fd < 0)
		return;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CTL_SOCK, sizeof addr.sun_path - 1);
	unlink(CTL_SOCK);
	if (bind(ctl_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(ctl_fd, 4) < 0) {
		close(ctl_fd);
		ctl_fd = -1;
		return;
	}
	chmod(CTL_SOCK, 0600);
}

static void reply(int fd, const char *s)
{
	if (write(fd, s, strlen(s)) < 0) { /* client gone */ }
}

static void wpa(const char *a1, const char *a2, const char *a3, char *out, size_t size)
{
	char *argv[] = { "/sbin/wpa_cli", "-p", WPA_CTRL_DIR, "-i", wifi_iface,
	                 (char *)a1, (char *)a2, (char *)a3, NULL };
	int n = 5;
	if (!a1) argv[n] = NULL;
	else if (!a2) argv[n + 1] = NULL;
	else if (!a3) argv[n + 2] = NULL;
	run_output(argv, out, size);
}

static void cmd_status(int fd)
{
	char buf[2048], line[300], val[128];
	char *p;

	snprintf(line, sizeof line, "boot %s\nfujinet %s\nssh %s\npassword %s\nefi %s\nmedia %s\n",
	         boot_names[boot_effective()], boot_effective() == BOOT_FUJINET ? "on" : "off",
	         ssh_pid > 0 ? "on" : "off",
	         conf.rootpw[0] ? "set" : "none", efi_ok ? "yes" : "no", media_kind);
	reply(fd, line);
	snprintf(line, sizeof line, "usb %d\ninstallable %d\n", usb_count, install_count());
	reply(fd, line);
	snprintf(line, sizeof line, "version %s\n", os_version);
	reply(fd, line);
	{
		/* perf <emu%> <fuji%> <tempC> <watts>; sampled at most once a second */
		char w[16], t[16];
		if (!perf_status_st.have || mono_now() - perf_status_st.t >= 1.0)
			perf_sample(&perf_status_st);
		perf_fmt_w(w, sizeof w, &perf_status_st);
		perf_fmt_t(t, sizeof t, &perf_status_st);
		snprintf(line, sizeof line, "perf %.0f %.0f %s %s\n", perf_status_st.cpu[0], perf_status_st.cpu[1], t, w);
		reply(fd, line);
	}

	/* first non-loopback IPv4 address */
	{
		char *ip[] = { BUSYBOX, "ip", "-4", "-o", "addr", "show", "scope", "global", NULL };
		run_output(ip, buf, sizeof buf);
		p = strstr(buf, "inet ");
		if (p && sscanf(p + 5, "%127s", val) == 1) {
			char *slash = strchr(val, '/');
			if (slash)
				*slash = '\0';
			snprintf(line, sizeof line, "ip %s\n", val);
			reply(fd, line);
		}
	}
	if (wifi_iface[0]) {
		char state[64] = "?", ssid[128] = "";
		wpa("status", NULL, NULL, buf, sizeof buf);
		p = strstr(buf, "wpa_state=");
		if (p)
			sscanf(p + 10, "%63s", state);
		p = strstr(buf, "\nssid=");
		if (p)
			sscanf(p + 6, "%127[^\n]", ssid);
		if (strcmp(state, "COMPLETED") == 0)
			snprintf(line, sizeof line, "wifi connected %s\n", ssid);
		else if (ssid[0])
			snprintf(line, sizeof line, "wifi %s %s\n", state, ssid);
		else
			snprintf(line, sizeof line, "wifi not connected\n");
		reply(fd, line);
		wpa_mon_open(); /* in case wpa_supplicant was not up at boot */
		if (strcmp(state, "COMPLETED") != 0 && wifi_err[0]) {
			snprintf(line, sizeof line, "wifierr %s\n", wifi_err);
			reply(fd, line);
		}
	}
	{
		DIR *d = opendir("/sys/class/power_supply");
		struct dirent *e;
		while (d && (e = readdir(d)) != NULL) {
			char path[300];
			if (strncmp(e->d_name, "BAT", 3) != 0)
				continue;
			snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", e->d_name);
			if (read_file(path, val, sizeof val) > 0) {
				snprintf(line, sizeof line, "battery %s\n", val);
				reply(fd, line);
				break;
			}
		}
		if (d)
			closedir(d);
	}
	reply(fd, "OK\n");
}

static void cmd_wifi_results(int fd)
{
	char buf[8192], line[400];
	char *p;
	if (!wifi_iface[0]) {
		reply(fd, "ERR no wifi adapter\n");
		return;
	}
	wpa("scan_results", NULL, NULL, buf, sizeof buf);
	/* bssid / frequency / signal level / flags / ssid */
	p = strchr(buf, '\n');
	while (p && *++p) {
		char bssid[32], flags[200], ssid[200];
		int freq, sig;
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		if (sscanf(p, "%31s %d %d %199s %199[^\n]", bssid, &freq, &sig, flags, ssid) == 5 && ssid[0] != '\0') {
			const char *kind = strstr(flags, "WPA") ? "wpa" : strstr(flags, "WEP") ? "wep" : "open";
			snprintf(line, sizeof line, "net %d %s %s\n", sig, kind, ssid);
			reply(fd, line);
		}
		p = nl;
	}
	reply(fd, "OK\n");
}

/* A WPA2 passphrase is 8..63 printable characters; 64 hex digits are a
   raw PSK and go to wpa_supplicant without quotes. */
static int is_hex_psk(const char *psk)
{
	int i;
	for (i = 0; psk[i]; i++)
		if (!isxdigit((unsigned char)psk[i]))
			return 0;
	return i == 64;
}

/* "wifi connect\t<ssid>\t<psk>" */
static void cmd_wifi_connect(int fd, char *args)
{
	char *ssid = args, *psk, buf[512], id[16], q[1100], pq[1100];
	if (!wifi_iface[0]) {
		reply(fd, "ERR no wifi adapter\n");
		return;
	}
	psk = strchr(ssid, '\t');
	if (!psk) {
		reply(fd, "ERR usage\n");
		return;
	}
	*psk++ = '\0';
	logmsg("wifi: ssid \"%s\" psk_len %d", ssid, (int)strlen(psk));
	wifi_err[0] = '\0'; /* a fresh attempt: forget earlier failures */
	wpa_mon_open();
	wpa("remove_network", "all", NULL, buf, sizeof buf);
	wpa("add_network", NULL, NULL, buf, sizeof buf);
	if (sscanf(buf, "%15s", id) != 1 || !isdigit((unsigned char)id[0])) {
		reply(fd, "ERR wpa_supplicant not responding\n");
		return;
	}
	snprintf(q, sizeof q, "\"%s\"", ssid);
	wpa("set_network", id, NULL, buf, sizeof buf); /* no-op, keeps argv shape */
	{
		char *argv[] = { "/sbin/wpa_cli", "-p", WPA_CTRL_DIR, "-i", wifi_iface, "set_network", id, "ssid", q, NULL };
		run_output(argv, buf, sizeof buf);
	}
	if (psk[0]) {
		char *argv[] = { "/sbin/wpa_cli", "-p", WPA_CTRL_DIR, "-i", wifi_iface, "set_network", id, "psk", pq, NULL };
		if (is_hex_psk(psk))
			snprintf(pq, sizeof pq, "%s", psk);
		else
			snprintf(pq, sizeof pq, "\"%s\"", psk);
		run_output(argv, buf, sizeof buf);
		buf[strcspn(buf, "\n")] = '\0';
		logmsg("wifi: set_network psk: %s", buf[0] ? buf : "(no reply)");
		if (strcmp(buf, "OK") != 0) {
			wpa("remove_network", id, NULL, buf, sizeof buf);
			wpa("save_config", NULL, NULL, buf, sizeof buf);
			reply(fd, "ERR password rejected by wpa_supplicant (8-63 characters)\n");
			return;
		}
	}
	else {
		char *argv[] = { "/sbin/wpa_cli", "-p", WPA_CTRL_DIR, "-i", wifi_iface, "set_network", id, "key_mgmt", "NONE", NULL };
		run_output(argv, buf, sizeof buf);
	}
	/* events about the old network or from before the request: logged,
	   then forgotten */
	if (wpa_mon_fd >= 0)
		wpa_mon_read();
	wifi_err[0] = '\0';
	wifi_scanned = 0;
	wpa("enable_network", id, NULL, buf, sizeof buf);
	wpa("select_network", id, NULL, buf, sizeof buf);
	wpa("save_config", NULL, NULL, buf, sizeof buf);
	logmsg("wifi: connecting to \"%s\"", ssid);
	reply(fd, "OK\n");
}

static void save_logs(void);

static void ctl_handle(int fd)
{
	char line[1024];
	ssize_t n;
	struct timeval tv = { 2, 0 };
	char buf[512];

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	n = read(fd, line, sizeof line - 1);
	if (n <= 0)
		return;
	line[n] = '\0';
	{
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
	}
	/* "status" is polled (FujiNet's network bridge, the menus): not logged */
	if (strcmp(line, "status") != 0)
		logmsg("ctl: %s", strncmp(line, "wifi connect", 12) == 0 ? "wifi connect" : line);

	if (strcmp(line, "status") == 0)
		cmd_status(fd);
	else if (strncmp(line, "boot ", 5) == 0 || strcmp(line, "fujinet on") == 0 ||
	         strcmp(line, "fujinet off") == 0) {
		/* "fujinet on|off" are the old spellings of boot fujinet|basic */
		int b;
		const char *want = strncmp(line, "boot ", 5) == 0 ? line + 5 :
		                   strcmp(line, "fujinet on") == 0 ? "fujinet" : "basic";
		for (b = 0; b < BOOT_COUNT && strcmp(want, boot_names[b]) != 0; b++)
			;
		if (b == BOOT_COUNT) {
			reply(fd, "ERR usage: boot basic|nobasic|ash|fujinet\n");
			return;
		}
		if (b == BOOT_FUJINET && !fujinet_enabled) {
			reply(fd, "ERR FujiNet not installed\n");
			return;
		}
		if (b == BOOT_ASH && !exists(ASH_CART)) {
			reply(fd, "ERR ASH not installed (atari/ash/ASH.CAR)\n");
			return;
		}
		conf.boot = b;
		save_sysconf();
		reply(fd, "OK\n");
		emu_restart_requested = 1;
		if (emu_pid > 0)
			kill(emu_pid, SIGTERM);
	}
	else if (strcmp(line, "ssh on") == 0) {
		ssh_start();
		conf.ssh = ssh_pid > 0;
		save_sysconf();
		reply(fd, ssh_pid > 0 ? "OK\n" : "ERR could not start dropbear\n");
	}
	else if (strcmp(line, "ssh off") == 0) {
		ssh_stop();
		conf.ssh = 0;
		save_sysconf();
		reply(fd, "OK\n");
	}
	else if (strcmp(line, "wifi scan") == 0) {
		if (!wifi_iface[0]) {
			reply(fd, "ERR no wifi adapter\n");
			return;
		}
		wpa("scan", NULL, NULL, buf, sizeof buf);
		reply(fd, "OK\n");
	}
	else if (strcmp(line, "wifi results") == 0)
		cmd_wifi_results(fd);
	else if (strncmp(line, "wifi connect\t", 13) == 0)
		cmd_wifi_connect(fd, line + 13);
	else if (strcmp(line, "wifi forget") == 0) {
		if (wifi_iface[0]) {
			wpa("remove_network", "all", NULL, buf, sizeof buf);
			wpa("save_config", NULL, NULL, buf, sizeof buf);
		}
		wifi_err[0] = '\0';
		reply(fd, "OK\n");
	}
	else if (strncmp(line, "password ", 9) == 0) {
		const char *pw = line + 9;
		if (strcmp(pw, "clear") == 0 || pw[0] == '\0')
			conf.rootpw[0] = '\0';
		else {
			char *argv[] = { BUSYBOX, "cryptpw", "-m", "sha512", (char *)pw, NULL };
			char hash[200];
			if (run_output(argv, hash, sizeof hash) != 0 || hash[0] != '$') {
				reply(fd, "ERR could not hash the password\n");
				return;
			}
			hash[strcspn(hash, "\n")] = '\0';
			snprintf(conf.rootpw, sizeof conf.rootpw, "%.159s", hash);
		}
		save_sysconf();
		apply_rootpw();
		if (ssh_pid > 0) { /* restart so the password policy changes */
			ssh_stop();
			ssh_start();
		}
		reply(fd, "OK\n");
	}
	else if (strcmp(line, "perf") == 0 || strcmp(line, "perf on") == 0) {
		perf_start();
		reply(fd, "OK\n");
	}
	else if (strcmp(line, "perf off") == 0) {
		if (perf_logging)
			logmsg("perf: logging stopped");
		perf_logging = 0;
		reply(fd, "OK\n");
	}
	else if (strcmp(line, "disks") == 0)
		cmd_disks(fd);
	else if (strncmp(line, "install ", 8) == 0)
		cmd_install(fd, line + 8);
	else if (strcmp(line, "bootnext") == 0)
		reply(fd, efi_bootnext() == 0 ? "OK\n" : "ERR not booted from UEFI\n");
	else if (strcmp(line, "logs") == 0) {
		save_logs();
		reply(fd, "OK\n");
	}
	else if (strcmp(line, "restart-emulator") == 0) {
		reply(fd, "OK\n");
		emu_restart_requested = 1;
		if (emu_pid > 0)
			kill(emu_pid, SIGTERM);
	}
	else if (strcmp(line, "reboot") == 0) {
		reply(fd, "OK\n");
		close(fd);
		shutdown_machine(LINUX_REBOOT_CMD_RESTART);
	}
	else if (strcmp(line, "poweroff") == 0) {
		reply(fd, "OK\n");
		close(fd);
		shutdown_machine(LINUX_REBOOT_CMD_POWER_OFF);
	}
	else
		reply(fd, "ERR unknown command\n");
}

/* ---- logs on the stick ---------------------------------------------------- */

/* Real laptops have no serial console: keep the kernel, emulator and
   FujiNet logs on the stick, written shortly after boot and at shutdown. */
static void save_logs(void)
{
	char *dmesg[] = { BUSYBOX, "sh", "-c",
		"mkdir -p " MEDIA_USER "/logs && busybox dmesg > " MEDIA_USER "/logs/dmesg.txt 2>/dev/null;"
		"for f in atari800 fujinet wpa; do [ -f /run/$f.log ] && busybox cp /run/$f.log " MEDIA_USER "/logs/$f.log; done;"
		"busybox cat /proc/cpuinfo /proc/meminfo > " MEDIA_USER "/logs/system.txt 2>/dev/null;"
		"busybox lsmod > " MEDIA_USER "/logs/modules.txt 2>/dev/null;"
		"busybox ip addr > " MEDIA_USER "/logs/network.txt 2>/dev/null;"
		"busybox sync", NULL };
	if (!media_mounted)
		return;
	run(dmesg);
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
	save_logs();
	kill(-1, SIGTERM);
	sleep_ms(300);
	kill(-1, SIGKILL);
	sync();
	usb_unmount_all();
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
	if (strstr(buf, "atari.hwsim=1"))
		hwsim = 1;
	if (strstr(buf, "atari.perf=1"))
		perf_logging = -1; /* started with the main loop */
	{
		const char *p = strstr(buf, "atari.media=");
		if (p) {
			sscanf(p + strlen("atari.media="), "%15[0-9A-Fa-f-]", media_serial);
			for (p = media_serial; *p; p++)
				if (*p >= 'a' && *p <= 'f')
					media_serial[p - media_serial] = (char)(*p - 'a' + 'A');
		}
	}
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
	struct pollfd pfds[MAX_BUTTONS + 4];

	mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
	mkdir_p("/dev/pts", 0755);
	mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "mode=0620,gid=5");

	kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	parse_cmdline();
	if (read_file("/etc/atari-os-release", os_version, sizeof os_version) <= 0)
		snprintf(os_version, sizeof os_version, "unknown");
	logmsg("atari-os-x86 %s init starting%s", os_version, debug ? " (debug)" : "");

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
	/* no rate limit on our own log lines (the installer writes a burst) */
	write_file("/proc/sys/kernel/printk_devkmsg", "on");
	write_file("/proc/sys/kernel/modprobe", "/bin/modprobe");
	splash_open();
	splash_line("Loading drivers...");

	if (debug)
		start_debug_shell();

	coldplug();
	{
		/* System time from the RTC; the kernel only does this itself when
		   the driver is built in. Network time follows once DHCP is up. */
		char *hw[] = { BUSYBOX, "hwclock", "-s", "-u", NULL };
		run(hw);
	}
	if (!nomedia)
		find_media();
	splash_line(media_mounted ? "Stick found" : "No stick: built-in system");
	if (media_mounted)
		mount_modloop();
	/* With the full module set: GPU, sound, network, odd input devices.
	   The uevent listener is open before this pass so devices that appear
	   while it runs are not lost. */
	uevent_open();
	coldplug();
	usb_setup(); /* drives plugged in at boot; later ones arrive as uevents */
	power_defaults();
	/* The native GPU driver may have replaced the firmware framebuffer. */
	splash_open();
	splash_line("Hardware ready");
	efi_setup();
	open_buttons();

	load_sysconf();
	apply_rootpw();
	ctl_open();
	if (hwsim) {
		char *mp[] = { BUSYBOX, "modprobe", "mac80211_hwsim", "radios=2", NULL };
		run(mp);
		sleep_ms(500);
		logmsg("hwsim: virtual Wi-Fi radios loaded");
	}
	start_network();
	splash_line("Network starting");
	fujinet_enabled = !nofujinet && exists(FUJINET_BIN);
	if (fujinet_enabled)
		start_fujinet();
	if (conf.ssh)
		ssh_start();

	if (emudelay > 0) {
		logmsg("waiting %d s before starting the emulator", emudelay);
		sleep_ms(emudelay * 1000);
	}
	splash_line("Starting the Atari");
	sleep_ms(300);
	splash_close(); /* the emulator takes the display from here */
	if (!noemu)
		start_emulator();
	if (perf_logging) {
		perf_logging = 0;
		perf_start();
	}

	for (;;) {
		static int logs_saved = 0;
		static double logs_due = 0;
		int i, n = 0, r, ctl_slot = -1, uevent_slot = -1, mon_slot = -1, timeout;
		if (logs_due == 0)
			logs_due = mono_now() + 30.0;
		pfds[n].fd = sfd;
		pfds[n].events = POLLIN;
		n++;
		if (ctl_fd >= 0) {
			ctl_slot = n;
			pfds[n].fd = ctl_fd;
			pfds[n].events = POLLIN;
			n++;
		}
		if (uevent_fd >= 0) {
			uevent_slot = n;
			pfds[n].fd = uevent_fd;
			pfds[n].events = POLLIN;
			n++;
		}
		if (wpa_mon_fd >= 0) {
			mon_slot = n;
			pfds[n].fd = wpa_mon_fd;
			pfds[n].events = POLLIN;
			n++;
		}
		for (i = 0; i < button_count; i++) {
			pfds[n].fd = button_fds[i];
			pfds[n].events = POLLIN;
			n++;
		}
		/* Batch modprobes a moment after the last uevent burst. */
		timeout = uevent_pending ? 300 : (logs_saved ? -1 : ms_until(logs_due));
		if (perf_logging && (timeout < 0 || ms_until(perf_next) < timeout))
			timeout = ms_until(perf_next);
		r = poll(pfds, n, timeout);
		perf_tick();
		if (r == 0) {
			if (uevent_pending) {
				uevent_pending = 0;
				flush_batch();
				if (usb_pending) {
					usb_pending = 0;
					usb_scan();
				}
				continue;
			}
			if (!logs_saved && mono_now() >= logs_due) {
				save_logs(); /* 30 s after start: enough to see how the hardware came up */
				logs_saved = 1;
			}
			continue;
		}
		if (r < 0)
			continue;
		if (uevent_slot >= 0 && (pfds[uevent_slot].revents & POLLIN))
			uevent_read();
		if (mon_slot >= 0 && (pfds[mon_slot].revents & POLLIN))
			wpa_mon_read();
		if (mon_slot >= 0 && (pfds[mon_slot].revents & (POLLERR | POLLHUP)) && wpa_mon_fd >= 0) {
			close(wpa_mon_fd); /* wpa_supplicant went away; re-attached on demand */
			wpa_mon_fd = -1;
		}
		if (pfds[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			while (read(sfd, &si, sizeof si) == sizeof si) {
				if (si.ssi_signo == SIGCHLD) {
					int status;
					pid_t pid;
					while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
						if (pid == emu_pid) {
							emu_pid = -1;
							if (emu_restart_requested) {
								emu_restart_requested = 0;
								logmsg("restarting emulator (boot %s)", boot_names[boot_effective()]);
								start_emulator();
								continue;
							}
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
						else if (pid == install_pid) {
							install_pid = -1;
							logmsg("install: finished (status 0x%x)", status);
						}
						else if (pid == ssh_pid) {
							ssh_pid = -1;
							logmsg("ssh: dropbear exited");
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
				else if (si.ssi_signo == SIGUSR1) {
					/* "reboot back into this system": used for remote updates */
					efi_bootnext();
					shutdown_machine(LINUX_REBOOT_CMD_RESTART);
				}
			}
		}
		if (ctl_slot >= 0 && (pfds[ctl_slot].revents & POLLIN)) {
			int c = accept(ctl_fd, NULL, NULL);
			if (c >= 0) {
				ctl_handle(c);
				close(c);
			}
		}
		for (i = 1; i < n; i++)
			if (i != ctl_slot && i != uevent_slot && i != mon_slot && (pfds[i].revents & POLLIN))
				read_button(pfds[i].fd);
	}
}
