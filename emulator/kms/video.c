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
#include "kms/kms_input.h"
#include "kms/kms_sysmenu.h"
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
static uint32_t conn_type = 0; /* DRM_MODE_CONNECTOR_* of conn_id */
static uint32_t dpms_prop = 0; /* the connector's DPMS property, 0: not looked up / none */
static int dpms_looked = FALSE;
static int blanked = FALSE;    /* KMS_VIDEO_Blank(TRUE): the panel is off */
/* Flips after the panel came back on that give no input-lag or wake-up
   sample: the first lands whenever the restarted vblank does, not when
   the frame asked for it. */
static int settle_flips = 0;
static int flip_settling = FALSE; /* the last flip event was one of them */
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

/* Input lag (kms/main.c feeds it): the clock both the flip events and the
   evdev events use, the oldest input stamp of the frame being built and of
   the flip in flight, and rings of the last LAT_RING latency samples and
   frame times (emulate + blit, and time spent waiting). */
#define LAT_RING 120
static clockid_t lat_clock = CLOCK_REALTIME;
static double frame_input_ts = 0, flip_input_ts = 0;
static double lat_ring[LAT_RING];
static int lat_n = 0, lat_pos = 0;
static double fr_emu[LAT_RING], fr_wait[LAT_RING];
static int fr_n = 0, fr_pos = 0;
static double frame_waited = 0; /* seconds slept or waited this frame */
static int frame_paced = FALSE; /* the vblank wait came first (KMS_VIDEO_WaitFrame) */
static int frame_vbl = FALSE;   /* ... the same, until FrameDone */
static double st_lat_last = 0;
/* Wake-up error (scheduling jitter): how late a timed sleep returned past
   its request, or how long after the flip's vblank stamp the loop ran
   again after waiting for it; the last WAKE_RING samples. */
#define WAKE_RING 600
static double wake_ring[WAKE_RING];
static int wake_n = 0, wake_pos = 0;
static double last_flip_ts = 0; /* vblank stamp of the last flip event */

/* Just-in-time frame (Low latency, KMS_JIT): after the vblank the loop
   sleeps until LEAD before the next one, then reads the input, emulates,
   renders and flips. LEAD = p95 of the emulate + render time plus a
   margin, plus ADAPT, which grows on every late frame (its flip missed the
   next vblank) and shrinks after a long run of frames on time. Two frames
   longer than a period in a row: off until the display settings change or
   Low latency is switched (today's loop, read right after the vblank). */
int KMS_VIDEO_jit = TRUE;
#define PER_RING 60
static double per_ring[PER_RING]; /* measured vblank periods (s) */
static int per_n = 0, per_pos = 0;
static int jit_armed = FALSE;  /* the frame being built slept for the JIT target */
static int frame_jit = FALSE;  /* ... the same, for FrameDone */
static int flip_jit = FALSE;   /* the flip in flight is a JIT frame's */
static int jit_off = FALSE;    /* gave up: the machine is too slow for the mode */
static int jit_slow = 0;       /* consecutive JIT frames longer than a period */
static double jit_adapt = 0;   /* s, added to the lead after late frames */
static int jit_ontime = 0;     /* JIT flips on time in a row */
static double jit_lead_last = 0; /* s, the lead used last */
static int jit_ll_seen = -1;   /* Low latency as last seen (a toggle resets) */
static unsigned long st_late = 0;
static unsigned long jit_late_total = 0; /* misses since the last reset */
static double last_wake_late = 0;  /* s, the last JIT wake-up error */
static double last_submit_ts = 0;  /* s, when the flip in flight was queued */
static double last_submit_dur = 0; /* s, how long drmModePageFlip took */
static double late_log_ts = 0;     /* s, the last "jit late" line */
static double dbg_waited_ts = 0;   /* s, when wait_flip returned (frame start) */
static double dbg_target_ts = 0;   /* s, the JIT sleep target */
static double dbg_wake_ts = 0;     /* s, when the JIT sleep returned */
static int dbg_sleep_skipped = 0;

static int cmp_double(const void *a, const void *b);
static double jit_period(void);

static void wake_sample(double late)
{
	if (late < 0)
		late = 0;
	last_wake_late = late;
	if (late > 1.0) /* stopped for a menu or a suspend: not jitter */
		return;
	wake_ring[wake_pos] = late;
	wake_pos = (wake_pos + 1) % WAKE_RING;
	if (wake_n < WAKE_RING)
		wake_n++;
}

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

int KMS_VIDEO_InternalPanel(void)
{
	/* A virtual machine's screen (bochs-drm, virtio-gpu, QXL: "Virtual")
	   counts too: it is the machine's own and only one, and a lid in a VM
	   is only ever a test (scripts/qemu-test.sh, a uinput lid). */
	return drm_fd >= 0 && (connector_rank(conn_type) == 0 || conn_type == DRM_MODE_CONNECTOR_VIRTUAL);
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
	conn_type = best->connector_type;
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

static const char *const scale_names[] = { "integer", "fit", "fill", "integer-xy", "free" };
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

static void jit_reset(void)
{
	jit_off = FALSE;
	jit_slow = 0;
	jit_adapt = 0;
	jit_ontime = 0;
	jit_late_total = 0;
}

static void compute_layout(void)
{
	int W = mode.hdisplay, H = mode.vdisplay;
	int sw, sh, x, y, i, run = 0;
	int src_w, smooth;

	jit_reset(); /* the render time changes with the picture */

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
	case KMS_SCALE_INTEGER_XY: {
		/* the largest whole factor on each axis: 5 x 4 on a 1920x1080
		   panel ("a really sweet spot": wide, crisp, scanlines land on
		   whole rows), 4 x 3 on 1366x768, the same as Integer when the
		   panel is 4:3-ish */
		int fx = W / sw, fy = H / sh;
		if (fx < 1) fx = 1;
		if (fy < 1) fy = 1;
		dst_w = sw * fx;
		dst_h = sh * fy;
		if (dst_w > W) dst_w = W;
		if (dst_h > H) dst_h = H;
		break;
	}
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
	smooth = KMS_VIDEO_smooth && KMS_VIDEO_scale_mode != KMS_SCALE_INTEGER && KMS_VIDEO_scale_mode != KMS_SCALE_INTEGER_XY;
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
	jit_reset();
#else
	(void)s;
#endif
}

const char *KMS_VIDEO_ScaleLabel(void)
{
	static const char *const labels[] = { "Integer", "Fit", "Fill", "Integer+", "Free" };
	return labels[KMS_VIDEO_scale_mode >= 0 && KMS_VIDEO_scale_mode <= 4 ? KMS_VIDEO_scale_mode : 0];
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
	if ((KMS_VIDEO_scale_mode == KMS_SCALE_INTEGER || KMS_VIDEO_scale_mode == KMS_SCALE_INTEGER_XY) && lay_sw > 0 && lay_sh > 0
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
		else if (strcmp(parameters, "INTEGER_XY") == 0) KMS_VIDEO_scale_mode = KMS_SCALE_INTEGER_XY;
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
	else if (strcmp(option, "KMS_JIT") == 0) {
		int v = Util_sscanbool(parameters);
		if (v == -1)
			return FALSE;
		KMS_VIDEO_jit = v;
	}
	else
		return FALSE;
	return TRUE;
}

void KMS_VIDEO_WriteConfig(FILE *fp)
{
	static const char *const scan_cfg[] = { "OFF", "LIGHT", "MEDIUM", "STRONG" };
	static const char *const border_cfg[] = { "FULL", "TV", "TIGHT" };
	static const char *const names[] = { "INTEGER", "FIT", "STRETCH", "INTEGER_XY", "FREE" };
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
	if (!KMS_VIDEO_jit) /* a debugging switch: only written when off */
		fprintf(fp, "KMS_JIT=0\n");
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
				Log_print("\t-kms-scale integer|fit|fill|integer_xy|free");
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
	{
		/* Flip events carry CLOCK_MONOTONIC stamps on every modern kernel;
		   the input devices are switched to the same clock. */
		uint64_t mono = 0;
		lat_clock = drmGetCap(drm_fd, DRM_CAP_TIMESTAMP_MONOTONIC, &mono) == 0 && mono ? CLOCK_MONOTONIC : CLOCK_REALTIME;
		KMS_INPUT_SetClock(lat_clock);
		Log_print("kms: timestamps %s", lat_clock == CLOCK_MONOTONIC ? "monotonic" : "realtime");
	}

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

static void lat_sample(double shown)
{
	double d = shown - flip_input_ts;
	flip_input_ts = 0;
	if (d < 0 || d > 1.0) /* a clock mismatch or a pause: not a sample */
		return;
	lat_ring[lat_pos] = d;
	lat_pos = (lat_pos + 1) % LAT_RING;
	if (lat_n < LAT_RING)
		lat_n++;
}

static double clock_now(void)
{
	struct timespec ts;
	clock_gettime(lat_clock, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* FRAME is the vblank sequence the flip landed on: a gap of more than one
   since the previous flip means the panel showed a picture again (judder
   when the Atari and the panel run at different rates). */
static void jit_flip_done(unsigned int gap, double ts, double prev_ts)
{
	int late = gap >= 2 && gap < 120;
	if (late && ts - late_log_ts >= 1.0) {
		double emu = fr_n ? fr_emu[(fr_pos + LAT_RING - 1) % LAT_RING] : 0;
		double margin = prev_ts > 0 ? prev_ts + jit_period() - last_submit_ts : 0;
		late_log_ts = ts;
		Log_print("kms: jit late: gap %u, lead %.1f ms, woke %.2f ms late, emu %.2f ms,"
		          " flip queued %.2f ms before the vblank, call %.2f ms; from prev vblank (ms):"
		          " event %.2f, target %.2f, wake %.2f%s, submit %.2f, this %.2f, period %.3f",
		          gap, jit_lead_last * 1000.0, last_wake_late * 1000.0, emu * 1000.0,
		          margin * 1000.0, last_submit_dur * 1000.0,
		          (dbg_waited_ts - prev_ts) * 1000.0, (dbg_target_ts - prev_ts) * 1000.0,
		          (dbg_wake_ts - prev_ts) * 1000.0, dbg_sleep_skipped ? " (no sleep)" : "",
		          (last_submit_ts - prev_ts) * 1000.0, (ts - prev_ts) * 1000.0, jit_period() * 1000.0);
	}
	/* A miss moves the lead by half a millisecond, up to 4 ms; one step
	   is given back after 30 s without a miss. A rare miss (a stall)
	   repeats one picture; it must not buy lag for every frame after it. */
	if (late) {
		st_late++;
		jit_late_total++;
		if (jit_adapt < 0.004)
			jit_adapt += 0.0005;
		jit_ontime = 0;
	}
	else if (++jit_ontime >= 1800) {
		jit_ontime = 0;
		jit_adapt -= 0.0005;
		if (jit_adapt < 0)
			jit_adapt = 0;
	}
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
	double ts = (double)sec + (double)usec / 1e6, prev_ts = last_flip_ts;
	unsigned int gap = 0; /* vblanks since the previous flip, 0 = unknown */
	(void)fd; (void)data;
	flip_pending = 0;
	last_flip_ts = ts;
	flip_settling = settle_flips > 0;
	if (flip_settling)
		settle_flips--;
	if (flip_input_ts > 0) {
		if (flip_settling)
			flip_input_ts = 0;
		else
			lat_sample(ts);
	}
	/* Without sequence numbers (bochs-drm, simpledrm) the flip events
	   come when the flip is queued, not at a vblank: nothing to count. */
	if (frame != 0 && have_seq)
		gap = frame - last_seq;
	if (gap > 1 && gap < 120) /* longer: a pause (menu), not judder */
		st_repeats += gap - 1;
	if (gap >= 1 && gap <= 4 && prev_ts > 0 && ts > prev_ts) {
		per_ring[per_pos] = (ts - prev_ts) / gap;
		per_pos = (per_pos + 1) % PER_RING;
		if (per_n < PER_RING)
			per_n++;
	}
	/* the pacing never plans a repeat (one Atari frame per vblank while
	   locked): a gap after a JIT frame is that frame missing its vblank */
	if (flip_jit)
		jit_flip_done(gap, ts, prev_ts);
	flip_jit = FALSE;
	last_seq = frame;
	have_seq = frame != 0;
}

/* Wait up to MAX_S seconds for the pending flip; TRUE once it completed. */
static int flip_wait_for(double max_s)
{
	drmEventContext ev;
	double start = Util_time(), until = start + max_s;
	double start_clk = clock_now(); /* for the wake-up error */
	int done = TRUE;
	memset(&ev, 0, sizeof ev);
	ev.version = 2;
	ev.page_flip_handler = page_flip_handler;

	while (flip_pending) {
		fd_set fds;
		struct timeval tv;
		double left = until - Util_time();
		int r;
		if (left <= 0) {
			done = FALSE;
			break;
		}
		FD_ZERO(&fds);
		FD_SET(drm_fd, &fds);
		tv.tv_sec = (long)left;
		tv.tv_usec = (long)((left - (double)tv.tv_sec) * 1e6);
		r = select(drm_fd + 1, &fds, NULL, NULL, &tv);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			done = FALSE;
			break;
		}
		drmHandleEvent(drm_fd, &ev);
	}
	/* woken by the flip: the delay from its vblank to running again (not
	   when the flip had landed before we started waiting) */
	if (done && last_flip_ts >= start_clk && !flip_settling)
		wake_sample(clock_now() - last_flip_ts);
	frame_waited += Util_time() - start;
	return done;
}

/* FALSE when the flip got stuck and was given up. */
static int wait_flip(void)
{
	if (flip_pending && !flip_wait_for(0.1)) { /* a stuck flip must not hang the emulator */
		flip_pending = 0;
		have_seq = FALSE;
		st_timeouts++;
		flip_input_ts = 0;
		flip_jit = FALSE;
		return FALSE;
	}
	return TRUE;
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
	double t0;
	if (s <= 0)
		return;
	if (flip_pending && drm_fd >= 0 && locked_now()) {
		flip_wait_for(s);
		return;
	}
	if (frame_vbl && drm_fd >= 0 && locked_now()) {
		/* Inside a frame that started at a vblank: the sound buffer is
		   full (paced by a panel at 60.0x Hz the Atari makes its 59.92 Hz
		   worth of samples a little too fast). Sleep only until shortly
		   before the next vblank, so the picture is not repeated; with
		   no time left, the full sleep (the picture repeats once). */
		double left = last_flip_ts + jit_period() - 0.002 - clock_now();
		if (left > 0.0005 && left < s)
			s = left;
	}
	ts.tv_sec = (time_t)s;
	ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
	t0 = Util_time();
	nanosleep(&ts, NULL);
	t0 = Util_time() - t0;
	frame_waited += t0;
	wake_sample(t0 - s);
}

/* ---- lid closed: panel off -------------------------------------------- */

/* The connector's "DPMS" property id, looked up once; 0 when it has none. */
static uint32_t find_dpms(void)
{
	drmModeObjectProperties *props;
	uint32_t i;
	if (dpms_looked)
		return dpms_prop;
	dpms_looked = TRUE;
	props = drmModeObjectGetProperties(drm_fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
	if (!props)
		return 0;
	for (i = 0; i < props->count_props && !dpms_prop; i++) {
		drmModePropertyRes *p = drmModeGetProperty(drm_fd, props->props[i]);
		if (!p)
			continue;
		if (strcmp(p->name, "DPMS") == 0)
			dpms_prop = p->prop_id;
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	if (!dpms_prop)
		Log_print("kms: the connector has no DPMS property: the lid only pauses");
	return dpms_prop;
}

/* Read any page flip event still queued, without waiting. */
static void drain_events(void)
{
	drmEventContext ev;
	memset(&ev, 0, sizeof ev);
	ev.version = 2;
	ev.page_flip_handler = page_flip_handler;
	for (;;) {
		fd_set fds;
		struct timeval tv;
		FD_ZERO(&fds);
		FD_SET(drm_fd, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		if (select(drm_fd + 1, &fds, NULL, NULL, &tv) <= 0)
			break;
		if (drmHandleEvent(drm_fd, &ev) != 0)
			break;
	}
}

int KMS_VIDEO_Blank(int off)
{
	if (!KMS_VIDEO_InternalPanel())
		return FALSE;
	if (off) {
		if (blanked)
			return TRUE;
		/* the last flip lands first: its event must not be taken for the
		   first flip after the wake */
		wait_flip();
		if (find_dpms() && drmModeConnectorSetProperty(drm_fd, conn_id, dpms_prop, DRM_MODE_DPMS_OFF))
			Log_print("kms: DPMS off failed: %s", strerror(errno));
		blanked = TRUE;
		return TRUE;
	}
	if (!blanked)
		return TRUE;
	blanked = FALSE;
	if (dpms_prop && drmModeConnectorSetProperty(drm_fd, conn_id, dpms_prop, DRM_MODE_DPMS_ON))
		Log_print("kms: DPMS on failed: %s", strerror(errno));
	drain_events();
	/* a full modeset with the picture last shown restarts the flip chain
	   from a known state, whatever the driver did while the panel was off */
	if (drmModeSetCrtc(drm_fd, crtc_id, fbs[front].fb_id, 0, 0, &conn_id, 1, &mode))
		Log_print("kms: modeset after the lid failed: %s", strerror(errno));
	/* Pacing and the just-in-time frame start over: no flip in flight, no
	   vblank stamp or sequence to measure the next flip against (its gap
	   and its wake-up would be minutes long), no input stamp waiting for
	   a picture. The input-lag and wake rings keep their history; nothing
	   from before the pause can now land in them. */
	flip_pending = 0;
	have_seq = FALSE;
	last_seq = 0;
	last_flip_ts = 0;
	flip_input_ts = frame_input_ts = 0;
	flip_jit = jit_armed = frame_jit = FALSE;
	frame_paced = frame_vbl = FALSE;
	frame_waited = 0;
	jit_reset();
	settle_flips = 2; /* the first after the modeset, and one more to be sure */
	return TRUE;
}

/* ---- input lag ------------------------------------------------------- */

/* Low latency (kms/main.c): wait for the flip of the previous frame before
   the input is read, so the frame emulated next starts from fresh input
   right after a vblank. atari800's frame sleep then has nothing left to
   wait for (KMS_VIDEO_TakeFramePaced). */
/* JIT applies: Low latency on, the switch on, not given up, paced by the
   display's page flips (not in turbo or a menu). */
static int jit_wanted(void)
{
	if (jit_ll_seen != KMS_SYSMENU_low_latency) {
		jit_ll_seen = KMS_SYSMENU_low_latency;
		jit_reset();
	}
	return KMS_SYSMENU_low_latency && KMS_VIDEO_jit && !jit_off && flip_supported && locked_now();
}

/* One vblank period (s): the measured mean when it agrees with the mode's
   within 2%, else the mode's. */
static double jit_period(void)
{
	double mode_p = KMS_VIDEO_refresh > 0 ? 1.0 / KMS_VIDEO_refresh : 1.0 / 60.0, sum = 0;
	int i;
	if (per_n < 10)
		return mode_p;
	for (i = 0; i < per_n; i++)
		sum += per_ring[i];
	sum /= per_n;
	return sum > mode_p * 0.98 && sum < mode_p * 1.02 ? sum : mode_p;
}

/* The lead (s) before the vblank at which the frame starts. */
static double jit_lead(double period)
{
	static double sorted[LAT_RING];
	double p95 = 0, lead;
	int n = fr_n;
	if (n > 0) {
		memcpy(sorted, fr_emu, sizeof sorted[0] * (size_t)n);
		qsort(sorted, (size_t)n, sizeof sorted[0], cmp_double);
		p95 = sorted[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
	}
	/* The flip must be queued before the vblank starts, and the flip
	   timestamp is its end (the first line scanned out): the vblank's
	   length is part of the lead, plus 1.5 ms for the commit and the
	   odd frame that runs half a millisecond long. */
	lead = p95 + 0.0015;
	if (mode.vtotal > mode.vdisplay)
		lead += period * (double)(mode.vtotal - mode.vdisplay) / (double)mode.vtotal;
	if (lead < 0.002)
		lead = 0.002;
	lead += jit_adapt;
	if (lead > period - 0.001) /* the most: read right after the vblank */
		lead = period - 0.001;
	return lead;
}

/* Sleep until LEAD before the vblank after the one that just ended. */
static void jit_sleep(void)
{
	double period = jit_period(), lead = jit_lead(period);
	double target = last_flip_ts + period - lead, now = clock_now(), t0;
	struct timespec ts;
	jit_lead_last = lead;
	jit_armed = frame_jit = TRUE;
	dbg_target_ts = target;
	dbg_sleep_skipped = 0;
	if (target <= now || target - now > period) { /* no time left, or a stale stamp */
		dbg_sleep_skipped = 1;
		dbg_wake_ts = now;
		return;
	}
	ts.tv_sec = (time_t)target;
	ts.tv_nsec = (long)((target - (double)ts.tv_sec) * 1e9);
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	t0 = Util_time();
	while (clock_nanosleep(lat_clock, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
	frame_waited += Util_time() - t0;
	dbg_wake_ts = clock_now();
	wake_sample(dbg_wake_ts - target);
}

void KMS_VIDEO_WaitFrame(void)
{
	int jit;
	frame_paced = FALSE;
	jit_armed = frame_jit = FALSE;
	jit = jit_wanted();
	if (drm_fd < 0 || !flip_pending || !locked_now())
		return;
	if (!wait_flip())
		return;
	frame_paced = frame_vbl = TRUE;
	dbg_waited_ts = clock_now();
	if (jit && last_flip_ts > 0)
		jit_sleep();
}

int KMS_VIDEO_Jit(double *lead_ms, int *late)
{
	*lead_ms = 0;
	*late = 0;
	if (!(KMS_SYSMENU_low_latency && KMS_VIDEO_jit && !jit_off && !Atari800_turbo && KMS_VIDEO_PacingLocked()))
		return FALSE;
	*lead_ms = (jit_lead_last > 0 ? jit_lead_last : jit_lead(jit_period())) * 1000.0;
	*late = jit_late_total > 99999 ? 99999 : (int)jit_late_total;
	return TRUE;
}

int KMS_VIDEO_TakeFramePaced(void)
{
	int paced = frame_paced && locked_now();
	frame_paced = FALSE;
	return paced;
}

void KMS_VIDEO_FrameInput(double t)
{
	if (t > 0 && (frame_input_ts == 0 || t < frame_input_ts))
		frame_input_ts = t;
}

void KMS_VIDEO_FrameDone(double total, int valid)
{
	double wait = frame_waited;
	int was_jit = frame_jit;
	frame_waited = 0;
	frame_paced = frame_vbl = FALSE;
	frame_jit = FALSE;
	if (!valid)
		return;
	if (wait > total)
		wait = total;
	if (was_jit) {
		double period = jit_period();
		if (total - wait > period) {
			if (++jit_slow >= 2 && !jit_off) {
				jit_off = TRUE;
				Log_print("kms: jit off, frame %.1f ms > period", (total - wait) * 1000.0);
			}
		}
		else
			jit_slow = 0;
	}
	fr_emu[fr_pos] = total - wait;
	fr_wait[fr_pos] = wait;
	fr_pos = (fr_pos + 1) % LAT_RING;
	if (fr_n < LAT_RING)
		fr_n++;
	if (debug_stats) {
		double now = Util_time();
		if (st_lat_last == 0)
			st_lat_last = now;
		else if (now - st_lat_last >= 5.0) {
			double avg, max, emu, w, p50, p99, wmax;
			int n;
			st_lat_last = now;
			KMS_VIDEO_Latency(&avg, &max, &n);
			KMS_VIDEO_FrameTimes(&emu, &w);
			KMS_VIDEO_Wake(&p50, &p99, &wmax);
			Log_print("kms: input lag %.1f ms avg, %.1f max (%d), frame %.2f ms emu, %.2f ms wait,"
			          " wake %.3f ms p50, %.3f p99, %.3f max, alsa underruns %u",
			          avg, max, n, emu, w, p50, p99, wmax, ALSA_Underruns());
		}
	}
}

int KMS_VIDEO_Latency(double *avg_ms, double *max_ms, int *n)
{
	double sum = 0, max = 0;
	int i;
	for (i = 0; i < lat_n; i++) {
		sum += lat_ring[i];
		if (lat_ring[i] > max)
			max = lat_ring[i];
	}
	*avg_ms = lat_n ? sum * 1000.0 / lat_n : 0;
	*max_ms = max * 1000.0;
	*n = lat_n;
	return lat_n > 0;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

int KMS_VIDEO_Wake(double *p50_ms, double *p99_ms, double *max_ms)
{
	static double sorted[WAKE_RING];
	int n = wake_n;
	*p50_ms = *p99_ms = *max_ms = 0;
	if (n == 0)
		return FALSE;
	memcpy(sorted, wake_ring, sizeof sorted[0] * (size_t)n);
	qsort(sorted, (size_t)n, sizeof sorted[0], cmp_double);
	*p50_ms = sorted[n / 2] * 1000.0;
	*p99_ms = sorted[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1] * 1000.0;
	*max_ms = sorted[n - 1] * 1000.0;
	return TRUE;
}

int KMS_VIDEO_FrameTimes(double *emu_ms, double *wait_ms)
{
	double e = 0, w = 0;
	int i;
	for (i = 0; i < fr_n; i++) {
		e += fr_emu[i];
		w += fr_wait[i];
	}
	*emu_ms = fr_n ? e * 1000.0 / fr_n : 0;
	*wait_ms = fr_n ? w * 1000.0 / fr_n : 0;
	return fr_n > 0;
}

/* Queue the back buffer's flip; remembers when and how long it took. */
static int submit_flip(unsigned int fb_id)
{
	double t = clock_now();
	int r = drmModePageFlip(drm_fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
	int e = errno;
	last_submit_dur = clock_now() - t;
	last_submit_ts = t;
	errno = e;
	return r;
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
			char jit[48];
			double lead_ms;
			int late;
			if (KMS_VIDEO_Jit(&lead_ms, &late))
				snprintf(jit, sizeof jit, "jit lead %.1f ms, late %lu", lead_ms, st_late);
			else
				snprintf(jit, sizeof jit, "jit off");
			Log_print("kms: %.1f fps, flips %lu, repeats %lu, timeouts %lu, busy %lu, pacing %s, %s, render %.2f ms",
			          st_frames / (now - st_last), st_flips, st_repeats, st_timeouts, st_busy,
			          locked_now() ? "display" : "timer", jit, st_frames ? st_render * 1000.0 / st_frames : 0.0);
			st_frames = st_flips = st_repeats = st_timeouts = st_busy = st_late = 0;
			st_render = 0;
			st_last = now;
		}
	}

	if (flip_supported) {
		if (submit_flip(fbs[back].fb_id) == 0) {
			flip_pending = 1;
			front = back;
			st_flips++;
			flip_input_ts = frame_input_ts;
			frame_input_ts = 0;
			flip_jit = jit_armed && locked_now();
			jit_armed = FALSE;
			return;
		}
		if (errno == EBUSY) {
			/* A flip we gave up waiting for is still in flight. Paced by
			   the display, never drop the frame: wait for it, then retry. */
			if (locked_now()) {
				flip_pending = 1;
				if (flip_wait_for(0.1)
				    && submit_flip(fbs[back].fb_id) == 0) {
					flip_pending = 1;
					front = back;
					st_flips++;
					flip_input_ts = frame_input_ts;
					frame_input_ts = 0;
					flip_jit = jit_armed && locked_now();
					jit_armed = FALSE;
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
	if (frame_input_ts > 0) { /* no flip event: the picture is up now */
		flip_input_ts = frame_input_ts;
		frame_input_ts = 0;
		lat_sample(clock_now());
	}
}
