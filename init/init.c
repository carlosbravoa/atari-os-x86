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
#include <ctype.h>
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
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/netlink.h>
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
#define CTL_SOCK     "/run/atari.ctl"       /* System menu <-> init */
#define WPA_CTRL_DIR "/run/wpa"
#define SYSCONF      MEDIA_USER "/config/system.conf"
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
static pid_t ssh_pid = -1;
static int emu_restarts = 0;
static int emu_restart_requested = 0;
static int fujinet_enabled = 0;   /* FujiNet-PC process available */
static int ctl_fd = -1;
static int uevent_fd = -1;
static int uevent_pending = 0;
static char wifi_iface[32] = "";

/* Persisted settings (system.conf on the stick). */
static struct {
	int fujinet_boot; /* emulator boots through FujiNet (-netsio) */
	int ssh;          /* dropbear running */
} conf = { 0, 0 };

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
	char buf[256];
	if (read_file(SYSCONF, buf, sizeof buf) < 0)
		return;
	conf.fujinet_boot = strstr(buf, "fujinet=1") != NULL;
	conf.ssh = strstr(buf, "ssh=1") != NULL;
}

static void save_sysconf(void)
{
	FILE *f;
	if (!media_mounted)
		return;
	f = fopen(SYSCONF, "w");
	if (!f)
		return;
	fprintf(f, "fujinet=%d\nssh=%d\n", conf.fujinet_boot, conf.ssh);
	fclose(f);
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

static void uevent_read(void)
{
	char buf[8192];
	for (;;) {
		ssize_t n = recv(uevent_fd, buf, sizeof buf - 1, 0);
		char *p, *end;
		int add = 0;
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
		}
	}
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
		/* -F foreground, -s no password logins, -r host key */
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
	if (fujinet_enabled && conf.fujinet_boot)
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

/* ---- control socket ------------------------------------------------------- */

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

	snprintf(line, sizeof line, "fujinet %s\nssh %s\n",
	         fujinet_enabled && conf.fujinet_boot ? "on" : "off", ssh_pid > 0 ? "on" : "off");
	reply(fd, line);

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

/* "wifi connect\t<ssid>\t<psk>" */
static void cmd_wifi_connect(int fd, char *args)
{
	char *ssid = args, *psk, buf[512], id[16], q[1100];
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
		char pq[1100];
		snprintf(pq, sizeof pq, "\"%s\"", psk);
		char *argv[] = { "/sbin/wpa_cli", "-p", WPA_CTRL_DIR, "-i", wifi_iface, "set_network", id, "psk", pq, NULL };
		run_output(argv, buf, sizeof buf);
	}
	else {
		char *argv[] = { "/sbin/wpa_cli", "-p", WPA_CTRL_DIR, "-i", wifi_iface, "set_network", id, "key_mgmt", "NONE", NULL };
		run_output(argv, buf, sizeof buf);
	}
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
	logmsg("ctl: %s", strncmp(line, "wifi connect", 12) == 0 ? "wifi connect" : line);

	if (strcmp(line, "status") == 0)
		cmd_status(fd);
	else if (strcmp(line, "fujinet on") == 0 || strcmp(line, "fujinet off") == 0) {
		if (!fujinet_enabled) {
			reply(fd, "ERR FujiNet not installed\n");
			return;
		}
		conf.fujinet_boot = strcmp(line, "fujinet on") == 0;
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
		reply(fd, "OK\n");
	}
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
		"for f in atari800 fujinet; do [ -f /run/$f.log ] && busybox cp /run/$f.log " MEDIA_USER "/logs/$f.log; done;"
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
	struct pollfd pfds[MAX_BUTTONS + 3];

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
	{
		/* System time from the RTC; the kernel only does this itself when
		   the driver is built in. Network time follows once DHCP is up. */
		char *hw[] = { BUSYBOX, "hwclock", "-s", "-u", NULL };
		run(hw);
	}
	if (!nomedia)
		find_media();
	if (media_mounted)
		mount_modloop();
	/* With the full module set: GPU, sound, network, odd input devices.
	   The uevent listener is open before this pass so devices that appear
	   while it runs are not lost. */
	uevent_open();
	coldplug();
	open_buttons();

	load_sysconf();
	ctl_open();
	start_network();
	fujinet_enabled = !nofujinet && exists(FUJINET_BIN);
	if (fujinet_enabled)
		start_fujinet();
	if (conf.ssh)
		ssh_start();

	if (emudelay > 0) {
		logmsg("waiting %d s before starting the emulator", emudelay);
		sleep_ms(emudelay * 1000);
	}
	if (!noemu)
		start_emulator();

	for (;;) {
		static int logs_saved = 0;
		int i, n = 0, r, ctl_slot = -1, uevent_slot = -1, timeout;
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
		for (i = 0; i < button_count; i++) {
			pfds[n].fd = button_fds[i];
			pfds[n].events = POLLIN;
			n++;
		}
		/* Batch modprobes a moment after the last uevent burst. */
		timeout = uevent_pending ? 300 : (logs_saved ? -1 : 30000);
		r = poll(pfds, n, timeout);
		if (r == 0) {
			if (uevent_pending) {
				uevent_pending = 0;
				flush_batch();
				continue;
			}
			if (!logs_saved) {
				save_logs(); /* 30 s after start: enough to see how the hardware came up */
				logs_saved = 1;
			}
			continue;
		}
		if (r < 0)
			continue;
		if (uevent_slot >= 0 && (pfds[uevent_slot].revents & POLLIN))
			uevent_read();
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
								logmsg("restarting emulator (fujinet boot %s)", conf.fujinet_boot ? "on" : "off");
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
				else if (si.ssi_signo == SIGUSR1)
					shutdown_machine(LINUX_REBOOT_CMD_RESTART);
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
			if (i != ctl_slot && i != uevent_slot && (pfds[i].revents & POLLIN))
				read_button(pfds[i].fd);
	}
}
