/*
 * kms/video.c - DRM/KMS dumb-buffer video backend for Atari800
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * Design: two dumb (CPU-mapped) XRGB8888 buffers on the first connected
 * connector's preferred mode. The 8-bit Atari screen is palette-expanded
 * and scaled on the CPU with precomputed row/column maps, then shown
 * with a vsync'd page flip. No GPU driver userspace (Mesa) is needed, so
 * this works on every KMS driver including simpledrm.
 *
 * Display options (F8 > Display): scaling Integer/Fit/Fill/Free, smoothing
 * (sharp-bilinear for Fit/Fill: each Atari pixel stays a solid block and
 * only the panel pixels on its edges are blended), scanlines (every
 * second panel row of an Atari line darker, from a second palette),
 * borders (384/336/320 columns) and the NTSC TV look (atari800's Blargg
 * NTSC filter into a 32-bit row buffer, then the same scaler).
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
#include <math.h>
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

#include "artifact.h"
#include "atari.h"
#include "colours.h"
#include "log.h"
#include "platform.h"
#include "screen.h"
#include "ui.h"
#include "util.h"
#include "kms/kms_video.h"
#include "font_atari.h"
#ifdef NTSC_FILTER
#include "filter_ntsc.h"
#endif

int KMS_VIDEO_scale_mode = KMS_SCALE_INTEGER;
double KMS_VIDEO_free_x = 3.0;
double KMS_VIDEO_free_y = 3.0;
int KMS_VIDEO_smooth = FALSE;
int KMS_VIDEO_scanlines = KMS_SCANLINES_OFF;
int KMS_VIDEO_borders = KMS_BORDERS_TV;
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

/* Destination rectangle on the panel and the scaler's maps. */
static int dst_x, dst_y, dst_w, dst_h;
static int *xmap = NULL;    /* dst column -> src column (nearest) */
static int *xidx = NULL;    /* dst column -> left src column (blended) */
static int *xwgt = NULL;    /* ... and the right one's weight, 0..256 */
static int *ya = NULL;      /* dst row -> src row */
static int *yb = NULL;      /* ... the row blended in (smooth) */
static int *ywgt = NULL;    /* ... and its weight, 0..256 */
static unsigned char *ydark = NULL; /* dst row is a scanline */
static int int_factor = 0;  /* >0 when integer scaling of the 8-bit screen (fast path) */
static int h_blend = FALSE; /* columns use xidx/xwgt instead of xmap */
static int lay_ntsc = FALSE; /* the layout reads the NTSC filter's output */
static int lay_x1 = 24, lay_y1 = 0, lay_sw = 336, lay_sh = 240;
static int need_clear = 0;  /* buffers to blank (the picture moved) */
static double vfactor = 1.0;

/* Rows of panel pixels built from one source row, reused for every panel
   row they cover (least recently used goes first). */
#define ROW_SLOTS 6
static struct {
	int row;  /* source row, -1 = empty */
	int dim;  /* built from palette_dim */
	unsigned long used;
	uint32_t *buf;
} rows[ROW_SLOTS];
static unsigned long row_clock = 0;
static uint32_t *outbuf = NULL;  /* a blended or dimmed row */

static uint32_t palette[256];
static uint32_t palette_dim[256]; /* scanline rows */
static unsigned int dim_level = 256; /* brightness of scanline rows, /256 */

/* NTSC TV look: atari800's filter writes 7 pixels per 4 Atari pixels. */
static uint32_t *ntsc_buf = NULL;
static int ntsc_w = 0;

/* -kms-debug: frame statistics every few seconds */
static int debug_stats = FALSE;
static unsigned long st_frames, st_flips, st_repeats, st_timeouts, st_busy;
static double st_last, st_render;
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

static const char *const scale_names[] = { "integer", "fit", "fill", "free" };
static const char *const scan_names[] = { "off", "light", "medium", "strong" };
static const char *const border_names[] = { "full", "tv", "tight" };

/* The NTSC TV look is on and applies (it is NTSC only). */
static int ntsc_wanted(void)
{
#ifdef NTSC_FILTER
	return Atari800_tv_mode == Atari800_TV_NTSC && ARTIFACT_mode == ARTIFACT_NTSC_FULL;
#else
	return FALSE;
#endif
}

static uint32_t dim_pixel(uint32_t p, unsigned int k)
{
	return ((((p & 0xff00ffu) * k) >> 8) & 0xff00ffu) | ((((p & 0x00ff00u) * k) >> 8) & 0x00ff00u);
}

static void update_dim_palette(void)
{
	static const unsigned int levels[] = { 256, 192, 128, 64 }; /* 100/75/50/25 % */
	int i;
	dim_level = levels[KMS_VIDEO_scanlines >= 0 && KMS_VIDEO_scanlines <= 3 ? KMS_VIDEO_scanlines : 0];
	for (i = 0; i < 256; i++)
		palette_dim[i] = dim_pixel(palette[i], dim_level);
}

/* Sharp-bilinear: position X of N destination pixels over M source pixels
   (scale S = N/M) -> left source index and the weight (0..256) of the
   next one. The weight only leaves 0/256 within one destination pixel of
   a source pixel edge, so blocks stay solid and edges get one blended
   pixel instead of uneven widths. SHARP false: plain linear. */
static void blend_map(int x, int n, int m, int sharp, int *idx, int *wgt)
{
	double s = (double)n / (double)m;
	double p = ((double)x + 0.5) / s - 0.5;
	int i = p >= 0 ? (int)p : (int)p - 1;
	double f = p - i;
	if (sharp) {
		f = (f - 0.5) * s + 0.5;
		if (f < 0) f = 0;
		if (f > 1) f = 1;
	}
	if (i < 0) {
		i = 0;
		f = 0;
	}
	if (i >= m - 1) {
		i = m > 1 ? m - 2 : 0;
		f = m > 1 ? 1 : 0;
	}
	*idx = i;
	*wgt = (int)(f * 256 + 0.5);
}

static void compute_layout(void)
{
	int W = mode.hdisplay, H = mode.vdisplay;
	int sw, sh, x, y, i, run = 0;
	int src_w, smooth;

	switch (KMS_VIDEO_borders) {
	case KMS_BORDERS_FULL:
		Screen_visible_x1 = 0;
		Screen_visible_x2 = Screen_WIDTH;
		break;
	case KMS_BORDERS_TIGHT:
		Screen_visible_x1 = 32;
		Screen_visible_x2 = 352;
		break;
	default:
		Screen_visible_x1 = 24;
		Screen_visible_x2 = 360;
		break;
	}
	sw = Screen_visible_x2 - Screen_visible_x1;
	sh = Screen_visible_y2 - Screen_visible_y1;

	int_factor = 0;
	switch (KMS_VIDEO_scale_mode) {
	case KMS_SCALE_STRETCH:
		dst_w = W;
		dst_h = H;
		break;
	case KMS_SCALE_FIT:
		/* the height, and the width the 336-column picture has at 4:3:
		   the Atari's pixel shape is kept whatever the borders */
		dst_h = H;
		dst_w = (int)((double)H * 4 * sw / (3.0 * 336) + 0.5);
		if (dst_w > W) {
			dst_w = W;
			dst_h = (int)((double)W * 3 * 336 / (4.0 * sw) + 0.5);
		}
		break;
	case KMS_SCALE_FREE:
		/* the user's factors, centred and clipped to the panel */
		dst_w = (int)(sw * KMS_VIDEO_free_x + 0.5);
		dst_h = (int)(sh * KMS_VIDEO_free_y + 0.5);
		if (dst_w < 1) dst_w = 1;
		if (dst_h < 1) dst_h = 1;
		if (dst_w > W) dst_w = W;
		if (dst_h > H) dst_h = H;
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
		if (dst_w > W) {
			dst_w = W;
			int_factor = 0; /* narrower than the screen: the general path */
		}
		if (dst_h > H) dst_h = H;
		break;
	}
	}
	dst_x = (W - dst_w) / 2;
	dst_y = (H - dst_h) / 2;
	vfactor = (double)dst_h / sh;
	lay_x1 = Screen_visible_x1;
	lay_y1 = Screen_visible_y1;
	lay_sw = sw;
	lay_sh = sh;

	lay_ntsc = ntsc_wanted();
	smooth = KMS_VIDEO_smooth && KMS_VIDEO_scale_mode != KMS_SCALE_INTEGER;
#ifdef NTSC_FILTER
	if (lay_ntsc) {
		ntsc_w = ATARI_NTSC_OUT_WIDTH(sw);
		free(ntsc_buf);
		ntsc_buf = Util_malloc((size_t)ntsc_w * sh * sizeof(uint32_t));
		if (FILTER_NTSC_emu == NULL) {
			FILTER_NTSC_emu = FILTER_NTSC_New();
			FILTER_NTSC_Update(FILTER_NTSC_emu);
		}
	}
#endif
	if (lay_ntsc)
		int_factor = 0; /* 7 filter pixels per 4 Atari pixels: no integer path */
	src_w = lay_ntsc ? ntsc_w : sw;
	/* the filter's output is a smooth signal: always blended across */
	h_blend = lay_ntsc || smooth;

	free(xmap); free(xidx); free(xwgt);
	free(ya); free(yb); free(ywgt); free(ydark);
	free(outbuf);
	xmap = Util_malloc(dst_w * sizeof(int));
	xidx = Util_malloc(dst_w * sizeof(int));
	xwgt = Util_malloc(dst_w * sizeof(int));
	ya = Util_malloc(dst_h * sizeof(int));
	yb = Util_malloc(dst_h * sizeof(int));
	ywgt = Util_malloc(dst_h * sizeof(int));
	ydark = Util_malloc(dst_h);
	outbuf = Util_malloc(dst_w * sizeof(uint32_t));
	for (i = 0; i < ROW_SLOTS; i++) {
		free(rows[i].buf);
		rows[i].buf = Util_malloc(dst_w * sizeof(uint32_t));
		rows[i].row = -1;
	}
	for (x = 0; x < dst_w; x++) {
		xmap[x] = x * src_w / dst_w;
		blend_map(x, dst_w, src_w, !lay_ntsc, &xidx[x], &xwgt[x]);
	}
	for (y = 0; y < dst_h; y++) {
		int near = y * sh / dst_h;
		if (y > 0 && near == (y - 1) * sh / dst_h)
			run++;
		else
			run = 0;
		/* scanlines: every second panel row of an Atari line, at 2x or more */
		ydark[y] = KMS_VIDEO_scanlines != KMS_SCANLINES_OFF && vfactor >= 2.0 && (run & 1);
		if (smooth) {
			int idx, w;
			blend_map(y, dst_h, sh, TRUE, &idx, &w);
			ya[y] = idx;
			yb[y] = idx + 1 < sh ? idx + 1 : idx;
			ywgt[y] = w;
			if (w >= 256) { /* the next row only */
				ya[y] = yb[y];
				ywgt[y] = 0;
			}
		}
		else {
			ya[y] = yb[y] = near;
			ywgt[y] = 0;
		}
	}
	update_dim_palette();
	need_clear = 2;

	Log_print("kms: picture %dx%d at %d,%d (%s%s, scanlines %s, borders %s%s)", dst_w, dst_h, dst_x, dst_y,
	          scale_names[KMS_VIDEO_scale_mode], smooth ? " smooth" : "",
	          vfactor >= 2.0 ? scan_names[KMS_VIDEO_scanlines] : "off (needs 2x)",
	          border_names[KMS_VIDEO_borders], lay_ntsc ? ", NTSC TV" : "");
}

void KMS_VIDEO_Relayout(void)
{
	if (drm_fd >= 0)
		compute_layout();
}

int KMS_VIDEO_ScanlinesPossible(void)
{
	return vfactor >= 2.0;
}

int KMS_VIDEO_NtscLook(void)
{
	return ntsc_wanted();
}

int KMS_VIDEO_SetNtscLook(int on)
{
#ifdef NTSC_FILTER
	if (Atari800_tv_mode != Atari800_TV_NTSC)
		return FALSE;
	ARTIFACT_Set(on ? ARTIFACT_NTSC_FULL : ARTIFACT_NONE);
	KMS_VIDEO_Relayout();
	return TRUE;
#else
	(void)on;
	return FALSE;
#endif
}

#ifdef NTSC_FILTER
static atari_ntsc_setup_t const *const ntsc_presets[] = {
	&atari_ntsc_composite, &atari_ntsc_svideo, &atari_ntsc_rgb, &atari_ntsc_monochrome
};
#endif

int KMS_VIDEO_NtscPreset(void)
{
#ifdef NTSC_FILTER
	/* the knobs that tell the presets apart; sharpness is the user's */
	int i;
	for (i = 0; i < 4; i++) {
		const atari_ntsc_setup_t *p = ntsc_presets[i];
		if (Util_almostequal(FILTER_NTSC_setup.resolution, p->resolution, 0.001) &&
		    Util_almostequal(FILTER_NTSC_setup.artifacts, p->artifacts, 0.001) &&
		    Util_almostequal(FILTER_NTSC_setup.fringing, p->fringing, 0.001) &&
		    Util_almostequal(FILTER_NTSC_setup.bleed, p->bleed, 0.001))
			return i;
	}
#endif
	return KMS_NTSC_CUSTOM;
}

const char *KMS_VIDEO_NtscPresetLabel(void)
{
	static const char *const labels[] = { "Composite", "S-Video", "RGB", "Monochrome", "Custom" };
	return labels[KMS_VIDEO_NtscPreset()];
}

static void ntsc_refilter(void)
{
#ifdef NTSC_FILTER
	if (FILTER_NTSC_emu != NULL)
		FILTER_NTSC_Update(FILTER_NTSC_emu);
#endif
}

int KMS_VIDEO_SetNtscPreset(int preset)
{
#ifdef NTSC_FILTER
	const atari_ntsc_setup_t *p;
	if (Atari800_tv_mode != Atari800_TV_NTSC || preset < 0 || preset >= KMS_NTSC_CUSTOM)
		return FALSE;
	/* only the filter's knobs: FILTER_NTSC_SetPreset would also reset the
	   colour controls (hue, saturation, ...) the Crisp picture uses */
	p = ntsc_presets[preset];
	FILTER_NTSC_setup.sharpness = p->sharpness;
	FILTER_NTSC_setup.resolution = p->resolution;
	FILTER_NTSC_setup.artifacts = p->artifacts;
	FILTER_NTSC_setup.fringing = p->fringing;
	FILTER_NTSC_setup.bleed = p->bleed;
	ntsc_refilter();
	if (ARTIFACT_mode != ARTIFACT_NTSC_FULL)
		ARTIFACT_Set(ARTIFACT_NTSC_FULL);
	KMS_VIDEO_Relayout();
	return TRUE;
#else
	(void)preset;
	return FALSE;
#endif
}

double KMS_VIDEO_NtscSharpness(void)
{
#ifdef NTSC_FILTER
	return FILTER_NTSC_setup.sharpness;
#else
	return 0.0;
#endif
}

void KMS_VIDEO_SetNtscSharpness(double s)
{
#ifdef NTSC_FILTER
	if (s < FILTER_NTSC_SHARPNESS_MIN) s = FILTER_NTSC_SHARPNESS_MIN;
	if (s > FILTER_NTSC_SHARPNESS_MAX) s = FILTER_NTSC_SHARPNESS_MAX;
	FILTER_NTSC_setup.sharpness = s;
	ntsc_refilter();
#else
	(void)s;
#endif
}

const char *KMS_VIDEO_ScaleLabel(void)
{
	static const char *const labels[] = { "Integer", "Fit", "Fill", "Free" };
	return labels[KMS_VIDEO_scale_mode >= 0 && KMS_VIDEO_scale_mode <= 3 ? KMS_VIDEO_scale_mode : 0];
}

double KMS_VIDEO_ScaleX(void)
{
	return drm_fd >= 0 && lay_sw > 0 ? (double)dst_w / lay_sw : 0.0;
}

double KMS_VIDEO_ScaleY(void)
{
	return drm_fd >= 0 && lay_sh > 0 ? (double)dst_h / lay_sh : 0.0;
}

double KMS_VIDEO_FreeMax(int vertical)
{
	int sw = Screen_visible_x2 - Screen_visible_x1, sh = Screen_visible_y2 - Screen_visible_y1;
	if (drm_fd < 0 || sw <= 0 || sh <= 0)
		return 1.0;
	return vertical ? (double)mode.vdisplay / sh : (double)mode.hdisplay / sw;
}

void KMS_VIDEO_ScaleFactors(char *buf, size_t size)
{
	if (KMS_VIDEO_scale_mode == KMS_SCALE_INTEGER && lay_sw > 0 && lay_sh > 0
	    && dst_w % lay_sw == 0 && dst_h % lay_sh == 0)
		snprintf(buf, size, "%d x %d", dst_w / lay_sw, dst_h / lay_sh);
	else
		snprintf(buf, size, "%.2f x %.2f", KMS_VIDEO_ScaleX(), KMS_VIDEO_ScaleY());
}

const char *KMS_VIDEO_ScanlinesLabel(void)
{
	static const char *const labels[] = { "Off", "Light", "Medium", "Strong" };
	return labels[KMS_VIDEO_scanlines >= 0 && KMS_VIDEO_scanlines <= 3 ? KMS_VIDEO_scanlines : 0];
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
		else if (strcmp(parameters, "STRETCH") == 0 || strcmp(parameters, "FILL") == 0) KMS_VIDEO_scale_mode = KMS_SCALE_STRETCH;
		else if (strcmp(parameters, "FREE") == 0) KMS_VIDEO_scale_mode = KMS_SCALE_FREE;
		else return FALSE;
	}
	else if (strcmp(option, "KMS_SCALE_X") == 0 || strcmp(option, "KMS_SCALE_Y") == 0) {
		double f;
		if (!Util_sscandouble(parameters, &f) || f < 0.5 || f > 32.0)
			return FALSE;
		if (option[10] == 'X')
			KMS_VIDEO_free_x = f;
		else
			KMS_VIDEO_free_y = f;
	}
	else if (strcmp(option, "KMS_SMOOTH") == 0)
		KMS_VIDEO_smooth = Util_sscanbool(parameters) == 1;
	else if (strcmp(option, "KMS_SCANLINES") == 0) {
		int i;
		for (i = 0; i < 4 && Util_stricmp(parameters, scan_names[i]) != 0; i++)
			;
		if (i == 4)
			return FALSE;
		KMS_VIDEO_scanlines = i;
	}
	else if (strcmp(option, "KMS_BORDERS") == 0) {
		int i;
		for (i = 0; i < 3 && Util_stricmp(parameters, border_names[i]) != 0; i++)
			;
		if (i == 3)
			return FALSE;
		KMS_VIDEO_borders = i;
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
	static const char *const scan_cfg[] = { "OFF", "LIGHT", "MEDIUM", "STRONG" };
	static const char *const border_cfg[] = { "FULL", "TV", "TIGHT" };
	static const char *const names[] = { "INTEGER", "FIT", "STRETCH", "FREE" };
	fprintf(fp, "KMS_SCALE=%s\n", names[KMS_VIDEO_scale_mode]);
	fprintf(fp, "KMS_SCALE_X=%.2f\n", KMS_VIDEO_free_x);
	fprintf(fp, "KMS_SCALE_Y=%.2f\n", KMS_VIDEO_free_y);
	fprintf(fp, "KMS_SMOOTH=%d\n", KMS_VIDEO_smooth);
	fprintf(fp, "KMS_SCANLINES=%s\n", scan_cfg[KMS_VIDEO_scanlines]);
	fprintf(fp, "KMS_BORDERS=%s\n", border_cfg[KMS_VIDEO_borders]);
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
				Log_print("\t-kms-scale integer|fit|fill|free");
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
	free(xidx); xidx = NULL;
	free(xwgt); xwgt = NULL;
	free(ya); ya = NULL;
	free(yb); yb = NULL;
	free(ywgt); ywgt = NULL;
	free(ydark); ydark = NULL;
	free(outbuf); outbuf = NULL;
	free(ntsc_buf); ntsc_buf = NULL;
	for (i = 0; i < ROW_SLOTS; i++) {
		free(rows[i].buf);
		rows[i].buf = NULL;
	}
}

void KMS_VIDEO_PaletteUpdate(void)
{
	int i;
	for (i = 0; i < 256; i++)
		palette[i] = (uint32_t)Colours_table[i] & 0x00ffffffu; /* XRGB8888 */
	update_dim_palette();
#ifdef NTSC_FILTER
	if (FILTER_NTSC_emu != NULL)
		FILTER_NTSC_Update(FILTER_NTSC_emu); /* the filter has its own copy of the palette */
#endif
}

/* ---- hint overlay ------------------------------------------------------ */

void KMS_VIDEO_SetHint(const char *text, double seconds)
{
	/* a hint that is taken down (text "") must be wiped from both
	   buffers: the band is cleared for a few frames as when it expires */
	int showing = hint_text[0] && Util_time() < hint_until;
	Util_strlcpy(hint_text, text, sizeof hint_text);
	hint_until = Util_time() + seconds;
	hint_clear_frames = showing ? 3 : 0;
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

static uint32_t mix(uint32_t a, uint32_t b, unsigned int w)
{
	unsigned int iw = 256 - w;
	return (((((a & 0xff00ffu) * iw) + ((b & 0xff00ffu) * w)) >> 8) & 0xff00ffu)
	       | (((((a & 0x00ff00u) * iw) + ((b & 0x00ff00u) * w)) >> 8) & 0x00ff00u);
}

/* Source row SY (0 = first visible line) across the panel's width; DIM:
   a scanline row from palette_dim (plain paths only, see get_row()). */
static void build_row(uint32_t *d, int sy, int dim)
{
	int x;
	if (lay_ntsc) {
		const uint32_t *s = ntsc_buf + (size_t)sy * ntsc_w;
		for (x = 0; x < dst_w; x++) {
			unsigned int w = (unsigned int)xwgt[x];
			const uint32_t *p = s + xidx[x];
			d[x] = w == 0 ? p[0] : w >= 256 ? p[1] : mix(p[0], p[1], w);
		}
	}
	else {
		const UBYTE *s = (const UBYTE *)Screen_atari + (size_t)(lay_y1 + sy) * Screen_WIDTH + lay_x1;
		const uint32_t *pal = dim ? palette_dim : palette;
		if (int_factor) {
			int sx, k;
			for (sx = 0; sx < lay_sw; sx++) {
				uint32_t p = pal[s[sx]];
				for (k = 0; k < int_factor; k++)
					*d++ = p;
			}
		}
		else if (h_blend) {
			for (x = 0; x < dst_w; x++) {
				unsigned int w = (unsigned int)xwgt[x];
				const UBYTE *p = s + xidx[x];
				d[x] = w == 0 ? pal[p[0]] : w >= 256 ? pal[p[1]] : mix(pal[p[0]], pal[p[1]], w);
			}
		}
		else
			for (x = 0; x < dst_w; x++)
				d[x] = pal[s[xmap[x]]];
	}
}

static const uint32_t *get_row(int sy, int dim)
{
	const uint32_t *bright = NULL;
	int i, victim = 0;
	for (i = 0; i < ROW_SLOTS; i++)
		if (rows[i].row == sy && rows[i].dim == dim) {
			rows[i].used = ++row_clock;
			return rows[i].buf;
		}
	/* blended or filtered rows: a scanline is the bright row dimmed */
	if (dim && (lay_ntsc || h_blend))
		bright = get_row(sy, FALSE); /* now the most recently used: not the victim */
	for (i = 1; i < ROW_SLOTS; i++)
		if (rows[i].used < rows[victim].used)
			victim = i;
	if (bright != NULL) {
		int x;
		for (x = 0; x < dst_w; x++)
			rows[victim].buf[x] = dim_pixel(bright[x], dim_level);
	}
	else
		build_row(rows[victim].buf, sy, dim);
	rows[victim].row = sy;
	rows[victim].dim = dim;
	rows[victim].used = ++row_clock;
	return rows[victim].buf;
}

static void render(struct dumb_fb *fb)
{
	uint8_t *base;
	const uint32_t *line = NULL;
	int pa = -1, pb = -1, pw = -1, pd = -1;
	int y, i;
	size_t rowbytes;

	if (lay_ntsc != ntsc_wanted() || lay_x1 != Screen_visible_x1 || lay_sw != Screen_visible_x2 - Screen_visible_x1)
		compute_layout(); /* the TV system changed, or someone moved the borders */
	if (need_clear > 0) {
		memset(fb->map, 0, fb->size);
		need_clear--;
	}
#ifdef NTSC_FILTER
	if (lay_ntsc)
		atari_ntsc_blit_argb32(FILTER_NTSC_emu,
		                       (const ATARI_NTSC_IN_T *)Screen_atari + (size_t)lay_y1 * Screen_WIDTH + lay_x1,
		                       Screen_WIDTH, lay_sw, lay_sh, ntsc_buf, (long)ntsc_w * 4);
#endif
	for (i = 0; i < ROW_SLOTS; i++)
		rows[i].row = -1; /* a new frame */
	base = fb->map + (size_t)dst_y * fb->pitch + (size_t)dst_x * 4;
	rowbytes = (size_t)dst_w * 4;

	/* The dumb buffer is write-combined memory: build each distinct panel
	   row once in cached memory, then stream it out with memcpy for every
	   panel row it covers. Never read back from the framebuffer. */
	for (y = 0; y < dst_h; y++) {
		int a = ya[y], b = yb[y], w = ywgt[y], dark = ydark[y];
		if (a != pa || b != pb || w != pw || dark != pd) {
			if (w == 0)
				line = get_row(a, dark);
			else { /* smooth: a row on the edge between two Atari lines */
				const uint32_t *ra = get_row(a, dark);
				const uint32_t *rb = get_row(b, dark);
				int x;
				for (x = 0; x < dst_w; x++)
					outbuf[x] = mix(ra[x], rb[x], (unsigned int)w);
				line = outbuf;
			}
			pa = a; pb = b; pw = w; pd = dark;
		}
		memcpy(base + (size_t)y * fb->pitch, line, rowbytes);
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
	if (debug_stats) {
		double t0 = Util_time();
		render(&fbs[back]);
		st_render += Util_time() - t0;
	}
	else
		render(&fbs[back]);
	draw_hint(&fbs[back]);
	st_frames++;
	if (debug_stats) {
		double now = Util_time();
		if (now - st_last >= 5.0) {
			Log_print("kms: %.1f fps, flips %lu, repeats %lu, timeouts %lu, busy %lu, pacing %s, render %.2f ms",
			          st_frames / (now - st_last), st_flips, st_repeats, st_timeouts, st_busy,
			          locked_now() ? "display" : "timer", st_frames ? st_render * 1000.0 / st_frames : 0.0);
			st_frames = st_flips = st_repeats = st_timeouts = st_busy = 0;
			st_render = 0;
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
