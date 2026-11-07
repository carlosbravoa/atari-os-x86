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
#include <linux/kd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "atari.h"
#include "colours.h"
#include "log.h"
#include "platform.h"
#include "screen.h"
#include "util.h"
#include "kms/kms_video.h"

int KMS_VIDEO_scale_mode = KMS_SCALE_INTEGER;
char KMS_VIDEO_device[256] = "";
int KMS_VIDEO_width = 0;
int KMS_VIDEO_height = 0;
double KMS_VIDEO_refresh = 60.0;

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
static unsigned long st_frames, st_flips, st_timeouts, st_busy;
static double st_last;

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
	mode = best->modes[0];
	for (i = 0; i < best->count_modes; i++) {
		if (best->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			mode = best->modes[i];
			break;
		}
	}

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
	Log_print("kms: using %s, %dx%d@%d", path, mode.hdisplay, mode.vdisplay, mode.vrefresh);
	return TRUE;
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
	if (mode.vrefresh)
		KMS_VIDEO_refresh = mode.vrefresh;
	else if (mode.htotal && mode.vtotal)
		KMS_VIDEO_refresh = mode.clock * 1000.0 / (mode.htotal * mode.vtotal);

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

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
	(void)fd; (void)frame; (void)sec; (void)usec; (void)data;
	flip_pending = 0;
}

static void wait_flip(void)
{
	drmEventContext ev;
	memset(&ev, 0, sizeof ev);
	ev.version = 2;
	ev.page_flip_handler = page_flip_handler;

	while (flip_pending) {
		fd_set fds;
		struct timeval tv;
		int r;
		FD_ZERO(&fds);
		FD_SET(drm_fd, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = 100000; /* a stuck flip must not hang the emulator */
		r = select(drm_fd + 1, &fds, NULL, NULL, &tv);
		if (r <= 0) {
			flip_pending = 0;
			st_timeouts++;
			break;
		}
		drmHandleEvent(drm_fd, &ev);
	}
}

void KMS_VIDEO_DisplayScreen(void)
{
	int back;
	if (drm_fd < 0)
		return;

	wait_flip();
	back = 1 - front;
	render(&fbs[back]);
	st_frames++;
	if (debug_stats) {
		double now = Util_time();
		if (now - st_last >= 5.0) {
			Log_print("kms: %.1f fps, flips %lu, timeouts %lu, busy %lu",
			          st_frames / (now - st_last), st_flips, st_timeouts, st_busy);
			st_frames = st_flips = st_timeouts = st_busy = 0;
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
			st_busy++;
			return; /* previous flip still in flight: drop this frame */
		}
		Log_print("kms: page flip unavailable (%s), falling back to setcrtc", strerror(errno));
		flip_supported = 0;
	}
	drmModeSetCrtc(drm_fd, crtc_id, fbs[back].fb_id, 0, 0, &conn_id, 1, &mode);
	front = back;
}
