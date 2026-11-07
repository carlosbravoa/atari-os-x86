/*
 * kms/video.c - DRM/KMS dumb-buffer video backend for Atari800
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * Design: two dumb (CPU-mapped) XRGB8888 buffers on the first connected
 * connector's preferred mode. The 8-bit Atari screen is palette-expanded
 * and scaled on the CPU with nearest-neighbour row/column maps, then shown
 * with a vsync'd page flip. No GPU driver userspace (Mesa) is needed, so
 * this works on every KMS driver including simpledrm.
 *
 * Pacing: among the modes at the panel's preferred resolution the one
 * closest to the Atari's frame rate is used (PAL 49.86, NTSC 59.92 Hz).
 * When the panel's refresh is within KMS_VIDEO_lock_hz of that rate the
 * page flips are the emulator's clock (PLATFORM_Sleep below), so every
 * Atari frame is shown for exactly one vblank; otherwise atari800's timer
 * paces it as on every other port.
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <linux/kd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "atari.h"
#include "colours.h"
#include "log.h"
#include "platform.h"
#include "screen.h"
#include "ui.h"
#include "util.h"
#include "kms/kms_video.h"
#include "font_atari.h"

int KMS_VIDEO_scale_mode = KMS_SCALE_INTEGER;
char KMS_VIDEO_device[256] = "";
char KMS_VIDEO_driver[32] = "?";
static char hint_text[80] = "";
static double hint_until = 0;
static int hint_clear_frames = 0;
int KMS_VIDEO_width = 0;
int KMS_VIDEO_height = 0;
double KMS_VIDEO_refresh = 60.0;
/* Pace by page flips when |refresh - Atari fps| is below this (0 = never). */
double KMS_VIDEO_lock_hz = 0.75;
/* -kms-mode / KMS_MODE: "WxH" or "WxH@R"; "" = automatic */
char KMS_VIDEO_mode_request[32] = "";

struct dumb_fb {
	uint32_t handle;
	uint32_t pitch;
	uint32_t fb_id;
	uint64_t size;
	uint8_t *map;
};

static int drm_fd = -1;
static struct dumb_fb fbs[2];
static int front = 0;
static int flip_pending = 0;
static int flip_supported = 1;

static uint32_t crtc_id = 0;
static uint32_t conn_id = 0;
static drmModeModeInfo mode;
static drmModeCrtc *saved_crtc = NULL;

static int tty_fd = -1;
static long tty_saved_mode = -1;

/* Destination rectangle on the panel and nearest-neighbour maps. */
static int dst_x, dst_y, dst_w, dst_h;
static int *xmap = NULL; /* dst column -> src column */
static int *ymap = NULL; /* dst row -> src row */
static int int_factor = 0; /* >0 when integer scaling (fast path) */
static uint32_t *rowbuf = NULL;

static uint32_t palette[256];

/* -kms-debug: frame statistics every few seconds */
static int debug_stats = FALSE;
static unsigned long st_frames, st_flips, st_repeats, st_timeouts, st_busy;
static double st_last;
/* vblank sequence of the last completed flip, for st_repeats */
static unsigned int last_seq = 0;
static int have_seq = FALSE;

/* Refresh rates of the modes at the chosen resolution (TV system menu). */
#define MAX_CAND 16
static double cand_hz[MAX_CAND];
static int n_cand = 0;
static char modes_log[400];

/* Pacing state last logged: -1 none yet, 0 timer, 1 display; and for
   which TV system. */
static int pacing_logged = -1;
static int pacing_logged_tv = -1;

/* ---- dumb buffers --------------------------------------------------- */

static int create_fb(struct dumb_fb *fb, int width, int height)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	struct drm_mode_destroy_dumb dreq;

	memset(&creq, 0, sizeof creq);
	creq.width = width;
	creq.height = height;
	creq.bpp = 32;
	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
		Log_print("kms: cannot create dumb buffer: %s", strerror(errno));
		return FALSE;
	}
	fb->handle = creq.handle;
	fb->pitch = creq.pitch;
	fb->size = creq.size;

	if (drmModeAddFB(drm_fd, width, height, 24, 32, fb->pitch, fb->handle, &fb->fb_id)) {
		Log_print("kms: cannot create framebuffer: %s", strerror(errno));
		goto err_destroy;
	}

	memset(&mreq, 0, sizeof mreq);
	mreq.handle = fb->handle;
	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
		Log_print("kms: cannot map dumb buffer: %s", strerror(errno));
		goto err_fb;
	}
	fb->map = mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, mreq.offset);
	if (fb->map == MAP_FAILED) {
		Log_print("kms: mmap failed: %s", strerror(errno));
		fb->map = NULL;
		goto err_fb;
	}
	memset(fb->map, 0, fb->size);
	return TRUE;

err_fb:
	drmModeRmFB(drm_fd, fb->fb_id);
err_destroy:
	memset(&dreq, 0, sizeof dreq);
	dreq.handle = fb->handle;
	drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return FALSE;
}

static void destroy_fb(struct dumb_fb *fb)
{
	struct drm_mode_destroy_dumb dreq;
	if (fb->map)
		munmap(fb->map, fb->size);
	if (fb->fb_id)
		drmModeRmFB(drm_fd, fb->fb_id);
	if (fb->handle) {
		memset(&dreq, 0, sizeof dreq);
		dreq.handle = fb->handle;
		drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	}
	memset(fb, 0, sizeof *fb);
}

/* ---- connector / mode selection ------------------------------------- */

double KMS_VIDEO_AtariFps(void)
{
	return Atari800_tv_mode == Atari800_TV_PAL ? Atari800_FPS_PAL : Atari800_FPS_NTSC;
}

static double hz_diff(double a, double b)
{
	return a > b ? a - b : b - a;
}

/* The exact refresh of a mode; vrefresh is rounded to whole Hz. */
static double mode_hz(const drmModeModeInfo *m)
{
	double hz;
	if (m->clock == 0 || m->htotal == 0 || m->vtotal == 0)
		return m->vrefresh;
	hz = m->clock * 1000.0 / ((double)m->htotal * m->vtotal);
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		hz *= 2;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		hz /= 2;
	if (m->vscan > 1)
		hz /= m->vscan;
	return hz;
}

/* Index of the mode to use: -kms-mode if it matches, else among the modes
   at the preferred mode's resolution the one closest to the Atari's frame
   rate; a mode must be clearly closer (0.1 Hz) to beat the preferred one. */
static int choose_mode(const drmModeConnector *c)
{
	int i, pref = 0, best, req_w = 0, req_h = 0, forced = FALSE;
	double req_hz = 0, want = KMS_VIDEO_AtariFps(), best_d;
	size_t n = 0;

	for (i = 0; i < c->count_modes; i++)
		if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			pref = i;
			break;
		}
	if (KMS_VIDEO_mode_request[0]
	    && sscanf(KMS_VIDEO_mode_request, "%dx%d@%lf", &req_w, &req_h, &req_hz) < 2) {
		Log_print("kms: ignoring mode \"%s\" (want WxH or WxH@Hz)", KMS_VIDEO_mode_request);
		req_w = 0;
	}
	if (req_w > 0) {
		if (req_hz > 0)
			want = req_hz;
		best = -1;
		best_d = 1e9;
		for (i = 0; i < c->count_modes; i++) {
			const drmModeModeInfo *m = &c->modes[i];
			if (m->hdisplay == req_w && m->vdisplay == req_h && !(m->flags & DRM_MODE_FLAG_INTERLACE)
			    && hz_diff(mode_hz(m), want) < best_d) {
				best = i;
				best_d = hz_diff(mode_hz(m), want);
			}
		}
		if (best >= 0 && (req_hz <= 0 || best_d < 0.5)) {
			pref = best;
			forced = TRUE;
		}
		else
			Log_print("kms: no mode %s on this display, choosing automatically", KMS_VIDEO_mode_request);
		want = KMS_VIDEO_AtariFps();
	}

	best = pref;
	best_d = hz_diff(mode_hz(&c->modes[pref]), want);
	n_cand = 0;
	n = (size_t)snprintf(modes_log, sizeof modes_log, "kms: modes:");
	for (i = 0; i < c->count_modes; i++) {
		const drmModeModeInfo *m = &c->modes[i];
		double d;
		if (m->hdisplay != c->modes[pref].hdisplay || m->vdisplay != c->modes[pref].vdisplay
		    || (m->flags & DRM_MODE_FLAG_INTERLACE))
			continue;
		if (n_cand < MAX_CAND)
			cand_hz[n_cand++] = mode_hz(m);
		d = hz_diff(mode_hz(m), want);
		if (!forced && d < best_d - 0.1) {
			best = i;
			best_d = d;
		}
	}
	for (i = 0; i < c->count_modes; i++) {
		const drmModeModeInfo *m = &c->modes[i];
		if (m->hdisplay != c->modes[pref].hdisplay || m->vdisplay != c->modes[pref].vdisplay
		    || (m->flags & DRM_MODE_FLAG_INTERLACE) || n + 24 >= sizeof modes_log)
			continue;
		n += (size_t)snprintf(modes_log + n, sizeof modes_log - n, " %dx%d@%.2f%s",
		                      m->hdisplay, m->vdisplay, mode_hz(m), i == best ? "*" : "");
	}
	return best;
}

/* Internal panels first: a laptop with an external monitor plugged in
   should still come up on its own screen until we have a display menu. */
static int connector_rank(uint32_t type)
{
	switch (type) {
	case DRM_MODE_CONNECTOR_eDP:
	case DRM_MODE_CONNECTOR_LVDS:
	case DRM_MODE_CONNECTOR_DSI:
		return 0;
	default:
		return 1;
	}
}

static int pick_output(drmModeRes *res)
{
	int i, j;
	int best_rank = 99;
	drmModeConnector *best = NULL;

	for (i = 0; i < res->count_connectors; i++) {
		drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[i]);
		if (!conn)
			continue;
		if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0
		    && connector_rank(conn->connector_type) < best_rank) {
			if (best)
				drmModeFreeConnector(best);
			best = conn;
			best_rank = connector_rank(conn->connector_type);
		}
		else
			drmModeFreeConnector(conn);
	}
	if (!best)
		return FALSE;

	conn_id = best->connector_id;
	mode = best->modes[choose_mode(best)];

	/* CRTC: the one currently driving this connector, else any possible one. */
	crtc_id = 0;
	if (best->encoder_id) {
		drmModeEncoder *enc = drmModeGetEncoder(drm_fd, best->encoder_id);
		if (enc) {
			crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);
		}
	}
	for (i = 0; crtc_id == 0 && i < best->count_encoders; i++) {
		drmModeEncoder *enc = drmModeGetEncoder(drm_fd, best->encoders[i]);
		if (!enc)
			continue;
		for (j = 0; j < res->count_crtcs; j++) {
			if (enc->possible_crtcs & (1u << j)) {
				crtc_id = res->crtcs[j];
				break;
			}
		}
		drmModeFreeEncoder(enc);
	}
	drmModeFreeConnector(best);
	return crtc_id != 0;
}

static int open_card(const char *path)
{
	drmModeRes *res;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return FALSE;
	drm_fd = fd;
	res = drmModeGetResources(fd);
	if (!res) {
		close(fd);
		drm_fd = -1;
		return FALSE;
	}
	if (!pick_output(res)) {
		drmModeFreeResources(res);
		close(fd);
		drm_fd = -1;
		return FALSE;
	}
	drmModeFreeResources(res);
	drmSetMaster(fd); /* best effort; we are usually the only client */
	{
		drmVersionPtr v = drmGetVersion(fd);
		if (v) {
			Util_strlcpy(KMS_VIDEO_driver, v->name ? v->name : "?", sizeof KMS_VIDEO_driver);
			drmFreeVersion(v);
		}
	}
	Log_print("%s", modes_log);
	Log_print("kms: using %s (%s), %dx%d@%d (%.3f Hz)", path, KMS_VIDEO_driver, mode.hdisplay, mode.vdisplay,
	          mode.vrefresh, mode_hz(&mode));
	return TRUE;
}

/* ---- pacing ---------------------------------------------------------- */

/* The panel runs close enough to the Atari's rate for the page flips to
   be the clock (whenever the emulation runs: not in turbo or a menu). */
int KMS_VIDEO_PacingLocked(void)
{
	return drm_fd >= 0 && flip_supported && KMS_VIDEO_lock_hz > 0
	       && hz_diff(KMS_VIDEO_refresh, KMS_VIDEO_AtariFps()) < KMS_VIDEO_lock_hz;
}

/* Some mode at this resolution would lock FPS (it is used after a restart:
   the mode is chosen at start-up). */
int KMS_VIDEO_HasModeFor(double fps)
{
	int i;
	for (i = 0; i < n_cand; i++)
		if (KMS_VIDEO_lock_hz > 0 && hz_diff(cand_hz[i], fps) < KMS_VIDEO_lock_hz)
			return TRUE;
	return FALSE;
}

static int locked_now(void)
{
	return !Atari800_turbo && !UI_is_active && KMS_VIDEO_PacingLocked();
}

/* Log when the TV system (or the start) changes how we are paced. */
static void pacing_check(void)
{
	int locked = KMS_VIDEO_PacingLocked();
	if (locked == pacing_logged && Atari800_tv_mode == pacing_logged_tv)
		return;
	pacing_logged = locked;
	pacing_logged_tv = Atari800_tv_mode;
	if (locked)
		Log_print("kms: pacing locked to the display (%.2f Hz)", KMS_VIDEO_refresh);
	else
		Log_print("kms: pacing by timer (panel %.2f Hz, Atari %.2f Hz)", KMS_VIDEO_refresh, KMS_VIDEO_AtariFps());
}

/* ---- scaling layout -------------------------------------------------- */

static void compute_layout(void)
{
	int W = mode.hdisplay, H = mode.vdisplay;
	int sw = Screen_visible_x2 - Screen_visible_x1;
	int sh = Screen_visible_y2 - Screen_visible_y1;
	int x, y;

	int_factor = 0;
	switch (KMS_VIDEO_scale_mode) {
	case KMS_SCALE_STRETCH:
		dst_w = W;
		dst_h = H;
		break;
	case KMS_SCALE_FIT:
		dst_h = H;
		dst_w = H * 4 / 3;
		if (dst_w > W) {
			dst_w = W;
			dst_h = W * 3 / 4;
		}
		break;
	case KMS_SCALE_INTEGER:
	default: {
		int fx = W / sw, fy = H / sh;
		int f = fx < fy ? fx : fy;
		if (f < 1)
			f = 1;
		int_factor = f;
		dst_w = sw * f;
		dst_h = sh * f;
		if (dst_w > W) dst_w = W;
		if (dst_h > H) dst_h = H;
		break;
	}
	}
	dst_x = (W - dst_w) / 2;
	dst_y = (H - dst_h) / 2;

	free(xmap);
	free(ymap);
	free(rowbuf);
	xmap = Util_malloc(dst_w * sizeof(int));
	ymap = Util_malloc(dst_h * sizeof(int));
	rowbuf = Util_malloc(dst_w * sizeof(uint32_t));
	for (x = 0; x < dst_w; x++)
		xmap[x] = Screen_visible_x1 + x * sw / dst_w;
	for (y = 0; y < dst_h; y++)
		ymap[y] = Screen_visible_y1 + y * sh / dst_h;

	Log_print("kms: picture %dx%d at %d,%d (%s)", dst_w, dst_h, dst_x, dst_y,
	          int_factor ? "integer" : KMS_VIDEO_scale_mode == KMS_SCALE_FIT ? "fit" : "stretch");
}

/* ---- console mode ---------------------------------------------------- */

static void tty_graphics(void)
{
	static const char *const names[] = { "/dev/tty0", "/dev/tty", NULL };
	int i;
	for (i = 0; names[i]; i++) {
		tty_fd = open(names[i], O_RDWR | O_CLOEXEC);
		if (tty_fd >= 0)
			break;
	}
	if (tty_fd < 0)
		return; /* no VT (appliance boot): nothing to silence */
	if (ioctl(tty_fd, KDGETMODE, &tty_saved_mode) == 0) {
		if (ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) < 0)
			Log_print("kms: KDSETMODE failed: %s", strerror(errno));
	}
	else
		Log_print("kms: KDGETMODE failed: %s", strerror(errno));
}

static void tty_restore(void)
{
	if (tty_fd < 0)
		return;
	if (tty_saved_mode >= 0)
		ioctl(tty_fd, KDSETMODE, tty_saved_mode);
	close(tty_fd);
	tty_fd = -1;
}

/* ---- public API ------------------------------------------------------ */

int KMS_VIDEO_ReadConfig(char *option, char *parameters)
{
	if (strcmp(option, "KMS_SCALE") == 0) {
		if (strcmp(parameters, "INTEGER") == 0) KMS_VIDEO_scale_mode = KMS_SCALE_INTEGER;
		else if (strcmp(parameters, "FIT") == 0) KMS_VIDEO_scale_mode = KMS_SCALE_FIT;
		else if (strcmp(parameters, "STRETCH") == 0) KMS_VIDEO_scale_mode = KMS_SCALE_STRETCH;
		else return FALSE;
	}
	else if (strcmp(option, "KMS_DEVICE") == 0)
		Util_strlcpy(KMS_VIDEO_device, parameters, sizeof KMS_VIDEO_device);
	else if (strcmp(option, "KMS_MODE") == 0)
		Util_strlcpy(KMS_VIDEO_mode_request, parameters, sizeof KMS_VIDEO_mode_request);
	else
		return FALSE;
	return TRUE;
}

void KMS_VIDEO_WriteConfig(FILE *fp)
{
	static const char *const names[] = { "INTEGER", "FIT", "STRETCH" };
	fprintf(fp, "KMS_SCALE=%s\n", names[KMS_VIDEO_scale_mode]);
	if (KMS_VIDEO_device[0])
		fprintf(fp, "KMS_DEVICE=%s\n", KMS_VIDEO_device);
	if (KMS_VIDEO_mode_request[0])
		fprintf(fp, "KMS_MODE=%s\n", KMS_VIDEO_mode_request);
}

int KMS_VIDEO_Initialise(int *argc, char *argv[])
{
	int i, j;
	int help_only = FALSE;

	for (i = j = 1; i < *argc; i++) {
		int i_a = (i + 1 < *argc); /* is argument available? */
		int a_m = FALSE;            /* error, argument missing! */
		if (strcmp(argv[i], "-kms-device") == 0) {
			if (i_a) Util_strlcpy(KMS_VIDEO_device, argv[++i], sizeof KMS_VIDEO_device);
			else a_m = TRUE;
		}
		else if (strcmp(argv[i], "-kms-debug") == 0)
			debug_stats = TRUE;
		else if (strcmp(argv[i], "-kms-mode") == 0) {
			if (i_a) Util_strlcpy(KMS_VIDEO_mode_request, argv[++i], sizeof KMS_VIDEO_mode_request);
			else a_m = TRUE;
		}
		else if (strcmp(argv[i], "-kms-lock") == 0) {
			if (i_a) KMS_VIDEO_lock_hz = atof(argv[++i]);
			else a_m = TRUE;
		}
		else if (strcmp(argv[i], "-kms-scale") == 0) {
			if (i_a) {
				char buf[16];
				Util_strlcpy(buf, argv[++i], sizeof buf);
				Util_strupper(buf);
				if (!KMS_VIDEO_ReadConfig("KMS_SCALE", buf)) {
					Log_print("Invalid value for -kms-scale");
					return FALSE;
				}
			}
			else a_m = TRUE;
		}
		else {
			if (strcmp(argv[i], "-help") == 0) {
				help_only = TRUE;
				Log_print("\t-kms-device <path>        Use this DRM device instead of probing");
				Log_print("\t-kms-debug                Log frame statistics");
				Log_print("\t-kms-mode WxH[@Hz]        Display mode (default: preferred, rate closest to the Atari)");
				Log_print("\t-kms-lock <Hz>            Pace by the display within this of the Atari rate (0.75; 0 = off)");
				Log_print("\t-kms-scale integer|fit|stretch");
				Log_print("\t                          How to scale the picture to the panel");
			}
			argv[j++] = argv[i];
		}
		if (a_m) {
			Log_print("Missing argument for '%s'", argv[i]);
			return FALSE;
		}
	}
	*argc = j;
	if (help_only)
		return TRUE;

	if (KMS_VIDEO_device[0]) {
		if (!open_card(KMS_VIDEO_device)) {
			Log_print("kms: cannot use %s", KMS_VIDEO_device);
			return FALSE;
		}
	}
	else {
		char path[32];
		for (i = 0; i < 16; i++) {
			snprintf(path, sizeof path, "/dev/dri/card%d", i);
			if (open_card(path))
				break;
		}
		if (drm_fd < 0) {
			Log_print("kms: no DRM device with a connected display found");
			return FALSE;
		}
	}

	KMS_VIDEO_width = mode.hdisplay;
	KMS_VIDEO_height = mode.vdisplay;
	if (mode_hz(&mode) > 0)
		KMS_VIDEO_refresh = mode_hz(&mode);

	saved_crtc = drmModeGetCrtc(drm_fd, crtc_id);

	for (i = 0; i < 2; i++) {
		if (!create_fb(&fbs[i], mode.hdisplay, mode.vdisplay)) {
			KMS_VIDEO_Exit();
			return FALSE;
		}
	}

	tty_graphics();
	compute_layout();
	KMS_VIDEO_PaletteUpdate();

	front = 0;
	if (drmModeSetCrtc(drm_fd, crtc_id, fbs[front].fb_id, 0, 0, &conn_id, 1, &mode)) {
		Log_print("kms: modeset failed: %s", strerror(errno));
		KMS_VIDEO_Exit();
		return FALSE;
	}
	pacing_check();
	return TRUE;
}

void KMS_VIDEO_Exit(void)
{
	int i;
	if (drm_fd < 0)
		return;
	if (saved_crtc) {
		drmModeSetCrtc(drm_fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
		               saved_crtc->x, saved_crtc->y, &conn_id, 1, &saved_crtc->mode);
		drmModeFreeCrtc(saved_crtc);
		saved_crtc = NULL;
	}
	for (i = 0; i < 2; i++)
		destroy_fb(&fbs[i]);
	drmDropMaster(drm_fd);
	close(drm_fd);
	drm_fd = -1;
	tty_restore();
	free(xmap); xmap = NULL;
	free(ymap); ymap = NULL;
	free(rowbuf); rowbuf = NULL;
}

void KMS_VIDEO_PaletteUpdate(void)
{
	int i;
	for (i = 0; i < 256; i++)
		palette[i] = (uint32_t)Colours_table[i] & 0x00ffffffu; /* XRGB8888 */
}

/* ---- hint overlay ------------------------------------------------------ */

void KMS_VIDEO_SetHint(const char *text, double seconds)
{
	Util_strlcpy(hint_text, text, sizeof hint_text);
	hint_until = Util_time() + seconds;
	hint_clear_frames = 0;
}

/* Text in the black border below (or above) the picture, Atari font at
   2x, centred. The band is cleared for a few frames once the hint ends
   so both buffers are clean again. */
static void draw_hint(struct dumb_fb *fb)
{
	int scale = 2, band = 8 * scale + 8;
	int y0, x0, len, i;
	int active = hint_text[0] && Util_time() < hint_until;

	if (!active) {
		if (hint_clear_frames <= 0)
			return;
		hint_clear_frames--;
	}
	if ((int)mode.vdisplay - (dst_y + dst_h) >= band)
		y0 = dst_y + dst_h + 4;
	else if (dst_y >= band)
		y0 = dst_y - band + 4;
	else
		return;
	for (i = 0; i < 8 * scale; i++)
		memset(fb->map + (size_t)(y0 + i) * fb->pitch, 0, (size_t)mode.hdisplay * 4);
	if (!active)
		return;
	hint_clear_frames = 3;
	len = (int)strlen(hint_text);
	x0 = ((int)mode.hdisplay - len * 8 * scale) / 2;
	if (x0 < 0)
		x0 = 0;
	for (i = 0; i < len; i++) {
		int c = (unsigned char)hint_text[i], gy, gx, sy, sx;
		const unsigned char *g;
		if (c < 32 || c > 127)
			c = 32;
		g = font_atari + (c < 96 ? c - 32 : c) * 8;
		for (gy = 0; gy < 8; gy++)
			for (gx = 0; gx < 8; gx++) {
				if (!(g[gy] & (0x80 >> gx)))
					continue;
				for (sy = 0; sy < scale; sy++)
					for (sx = 0; sx < scale; sx++) {
						int px = x0 + i * 8 * scale + gx * scale + sx;
						int py = y0 + gy * scale + sy;
						if (px >= 0 && px < (int)mode.hdisplay)
							*(uint32_t *)(fb->map + (size_t)py * fb->pitch + (size_t)px * 4) = 0x00ffffff;
					}
			}
	}
}

/* ---- rendering ------------------------------------------------------- */

static void render(struct dumb_fb *fb)
{
	const UBYTE *src = (const UBYTE *)Screen_atari;
	uint8_t *base = fb->map + (size_t)dst_y * fb->pitch + (size_t)dst_x * 4;
	int prev_src_row = -1;
	int y;
	size_t rowbytes = (size_t)dst_w * 4;

	/* The dumb buffer is write-combined memory: render each distinct source
	   row once into a cached row buffer, then stream it out with memcpy for
	   every panel row it covers. Never read back from the framebuffer. */
	for (y = 0; y < dst_h; y++) {
		int sy = ymap[y];
		if (sy != prev_src_row) {
			const UBYTE *s = src + (size_t)sy * Screen_WIDTH;
			uint32_t *d = rowbuf;
			if (int_factor) {
				int sx, k;
				for (sx = Screen_visible_x1; sx < Screen_visible_x2; sx++) {
					uint32_t p = palette[s[sx]];
					for (k = 0; k < int_factor; k++)
						*d++ = p;
				}
			}
			else {
				int x;
				for (x = 0; x < dst_w; x++)
					d[x] = palette[s[xmap[x]]];
			}
			prev_src_row = sy;
		}
		memcpy(base + (size_t)y * fb->pitch, rowbuf, rowbytes);
	}
}

/* FRAME is the vblank sequence the flip landed on: a gap of more than one
   since the previous flip means the panel showed a picture again (judder
   when the Atari and the panel run at different rates). */
static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
	(void)fd; (void)sec; (void)usec; (void)data;
	flip_pending = 0;
	if (frame != 0 && have_seq) {
		unsigned int gap = frame - last_seq;
		if (gap > 1 && gap < 120) /* longer: a pause (menu), not judder */
			st_repeats += gap - 1;
	}
	last_seq = frame;
	have_seq = frame != 0;
}

/* Wait up to MAX_S seconds for the pending flip; TRUE once it completed. */
static int flip_wait_for(double max_s)
{
	drmEventContext ev;
	double until = Util_time() + max_s;
	memset(&ev, 0, sizeof ev);
	ev.version = 2;
	ev.page_flip_handler = page_flip_handler;

	while (flip_pending) {
		fd_set fds;
		struct timeval tv;
		double left = until - Util_time();
		int r;
		if (left <= 0)
			return FALSE;
		FD_ZERO(&fds);
		FD_SET(drm_fd, &fds);
		tv.tv_sec = (long)left;
		tv.tv_usec = (long)((left - (double)tv.tv_sec) * 1e6);
		r = select(drm_fd + 1, &fds, NULL, NULL, &tv);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return FALSE;
		drmHandleEvent(drm_fd, &ev);
	}
	return TRUE;
}

static void wait_flip(void)
{
	if (flip_pending && !flip_wait_for(0.1)) { /* a stuck flip must not hang the emulator */
		flip_pending = 0;
		have_seq = FALSE;
		st_timeouts++;
	}
}

/* Util_sleep() ends up here (SUPPORTS_PLATFORM_SLEEP). Paced by the
   display, the clock is the vblank: atari800's frame sleep returns when
   the flip queued for the previous frame has completed (at most S later).
   The sound code's "buffer full" sleeps get the same treatment for their
   first wait and a plain sleep after that, so they never spin. Turbo, the
   menus and every other case sleep by the clock as usual. */
void PLATFORM_Sleep(double s)
{
	struct timespec ts;
	if (s <= 0)
		return;
	if (flip_pending && drm_fd >= 0 && locked_now()) {
		flip_wait_for(s);
		return;
	}
	ts.tv_sec = (time_t)s;
	ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
	nanosleep(&ts, NULL);
}

void KMS_VIDEO_DisplayScreen(void)
{
	int back;
	if (drm_fd < 0)
		return;

	/* wait -> render -> flip: the buffer drawn into is never on screen */
	wait_flip();
	pacing_check();
	back = 1 - front;
	render(&fbs[back]);
	draw_hint(&fbs[back]);
	st_frames++;
	if (debug_stats) {
		double now = Util_time();
		if (now - st_last >= 5.0) {
			Log_print("kms: %.1f fps, flips %lu, repeats %lu, timeouts %lu, busy %lu, pacing %s",
			          st_frames / (now - st_last), st_flips, st_repeats, st_timeouts, st_busy,
			          locked_now() ? "display" : "timer");
			st_frames = st_flips = st_repeats = st_timeouts = st_busy = 0;
			st_last = now;
		}
	}

	if (flip_supported) {
		if (drmModePageFlip(drm_fd, crtc_id, fbs[back].fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) {
			flip_pending = 1;
			front = back;
			st_flips++;
			return;
		}
		if (errno == EBUSY) {
			/* A flip we gave up waiting for is still in flight. Paced by
			   the display, never drop the frame: wait for it, then retry. */
			if (locked_now()) {
				flip_pending = 1;
				if (flip_wait_for(0.1)
				    && drmModePageFlip(drm_fd, crtc_id, fbs[back].fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0) {
					flip_pending = 1;
					front = back;
					st_flips++;
					return;
				}
				flip_pending = 0;
			}
			st_busy++;
			return; /* timer-paced: drop this frame */
		}
		Log_print("kms: page flip unavailable (%s), falling back to setcrtc", strerror(errno));
		flip_supported = 0;
	}
	drmModeSetCrtc(drm_fd, crtc_id, fbs[back].fb_id, 0, 0, &conn_id, 1, &mode);
	front = back;
}
