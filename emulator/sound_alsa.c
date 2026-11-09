/*
 * sound_alsa.c - ALSA PCM sound driver for Atari800
 *
 * Part of atari-os-x86: Atari800 running directly on Linux DRM/KMS,
 * ALSA and evdev with no display server or SDL in between.
 *
 * Non-callback driver, same contract as sound_oss.c: the core asks how
 * many bytes it can write without blocking, then writes them. alsa-lib's
 * "default" device (plug layer, no sound server) handles format and rate
 * conversion for whatever the HDA codec wants.
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

#include <stdio.h>
#include <string.h>
#include <alsa/asoundlib.h>

#include "atari.h"
#include "log.h"
#include "platform.h"
#include "sound.h"
#include "kms/kms_sysmenu.h"

static snd_pcm_t *pcm = NULL;
static unsigned int frame_bytes = 0;
static snd_pcm_uframes_t period_frames = 0;
static snd_pcm_uframes_t buffer_frames = 0;
static int can_pause = 0;
static unsigned int underruns = 0;

/* Card selection: laptops often have the HDMI codec as card 0 and the
   analogue one (speakers, headphones) as card 1, so "default" would be
   silent. Pick the first card with a playback PCM whose name is not
   HDMI/DisplayPort-only. */
static int pick_card(void)
{
	int card = -1, fallback = -1;
	while (snd_card_next(&card) == 0 && card >= 0) {
		snd_ctl_t *ctl;
		snd_ctl_card_info_t *info;
		char name[16];
		int dev = -1, has_pcm = 0;
		const char *cname;
		snprintf(name, sizeof name, "hw:%d", card);
		if (snd_ctl_open(&ctl, name, 0) < 0)
			continue;
		snd_ctl_card_info_alloca(&info);
		snd_ctl_card_info(ctl, info);
		cname = snd_ctl_card_info_get_name(info);
		if (strcmp(snd_ctl_card_info_get_id(info), "pcsp") == 0) {
			snd_ctl_close(ctl); /* the PC speaker driver is not a sound card */
			continue;
		}
		while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
			snd_pcm_info_t *pi;
			snd_pcm_info_alloca(&pi);
			snd_pcm_info_set_device(pi, dev);
			snd_pcm_info_set_subdevice(pi, 0);
			snd_pcm_info_set_stream(pi, SND_PCM_STREAM_PLAYBACK);
			if (snd_ctl_pcm_info(ctl, pi) == 0) {
				const char *pname = snd_pcm_info_get_name(pi);
				has_pcm = 1;
				if (!strstr(pname, "HDMI") && !strstr(pname, "DP") && !strstr(pname, "DisplayPort")) {
					Log_print("alsa: card %d \"%s\" device %d \"%s\"", card, cname, dev, pname);
					snd_ctl_close(ctl);
					return card * 100 + dev;
				}
			}
		}
		if (has_pcm && fallback < 0)
			fallback = card * 100;
		snd_ctl_close(ctl);
	}
	return fallback;
}

static char pcm_name[32] = "default";
static char ctl_name[16] = "default";

static int open_pcm(Sound_setup_t *setup)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_format_t fmt = setup->sample_size == 2 ? SND_PCM_FORMAT_S16 : SND_PCM_FORMAT_U8;
	unsigned int rate = setup->freq;
	unsigned int channels = setup->channels;
	int err, i;

	{
		int pick = pick_card();
		if (pick >= 0) {
			snprintf(pcm_name, sizeof pcm_name, "plughw:%d,%d", pick / 100, pick % 100);
			snprintf(ctl_name, sizeof ctl_name, "hw:%d", pick / 100);
		}
	}
	err = snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (err < 0)
		err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (err < 0) {
		Log_print("alsa: no playback device: %s", snd_strerror(err));
		return FALSE;
	}
	i = 0;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_format(pcm, hw, fmt)) < 0) {
		fmt = fmt == SND_PCM_FORMAT_S16 ? SND_PCM_FORMAT_U8 : SND_PCM_FORMAT_S16;
		if ((err = snd_pcm_hw_params_set_format(pcm, hw, fmt)) < 0)
			goto fail;
	}
	if ((err = snd_pcm_hw_params_set_channels_near(pcm, hw, &channels)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0)
		goto fail;

	/* Low latency (F8 > Controls): 10 ms periods, 5 of them (50 ms; with
	   3 QEMU's HDA underran ~20 times a second when the timer paces the
	   emulation, with 4 about once a minute once the just-in-time frame
	   moved the writes to just before the vblank); otherwise the core's
	   20 ms periods, 4 of them (80 ms). */
	if (KMS_SYSMENU_low_latency)
		period_frames = rate / 100;
	else
		period_frames = setup->buffer_frames ? setup->buffer_frames : rate / 50;
	if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period_frames, NULL)) < 0)
		goto fail;
	buffer_frames = period_frames * (KMS_SYSMENU_low_latency ? 5 : 4);
	if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer_frames)) < 0)
		goto fail;
	if ((err = snd_pcm_hw_params(pcm, hw)) < 0)
		goto fail;
	snd_pcm_hw_params_get_period_size(hw, &period_frames, NULL);
	snd_pcm_hw_params_get_buffer_size(hw, &buffer_frames);
	can_pause = snd_pcm_hw_params_can_pause(hw);

	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	/* Start as soon as we have one period queued; the core keeps it topped up. */
	snd_pcm_sw_params_set_start_threshold(pcm, sw, period_frames);
	snd_pcm_sw_params_set_avail_min(pcm, sw, period_frames);
	snd_pcm_sw_params(pcm, sw);

	setup->freq = rate;
	setup->channels = channels;
	setup->sample_size = fmt == SND_PCM_FORMAT_S16 ? 2 : 1;
	setup->buffer_frames = (unsigned int)period_frames;
	frame_bytes = setup->channels * setup->sample_size;

	(void)i;
	Log_print("alsa: %s, %u Hz, %u ch, %u-bit, period %lu, buffer %lu frames (%.0f ms%s)",
	          pcm_name, rate, channels, setup->sample_size * 8,
	          (unsigned long)period_frames, (unsigned long)buffer_frames,
	          buffer_frames * 1000.0 / rate, KMS_SYSMENU_low_latency ? ", low latency" : "");
	return TRUE;

fail:
	Log_print("alsa: configuration failed: %s", snd_strerror(err));
	snd_pcm_close(pcm);
	pcm = NULL;
	return FALSE;
}

/* There is no alsactl on the appliance to restore mixer state, and HDA
   codecs frequently come up muted or at zero volume. Switch every playback
   control on and set a sane level, once, on card 0. */
static void unmute_mixer(void)
{
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;
	int unmuted = 0;

	if (snd_mixer_open(&mixer, 0) < 0)
		return;
	if (snd_mixer_attach(mixer, ctl_name) < 0 && snd_mixer_attach(mixer, "default") < 0) {
		snd_mixer_close(mixer);
		return;
	}
	if (snd_mixer_selem_register(mixer, NULL, NULL) < 0 || snd_mixer_load(mixer) < 0) {
		snd_mixer_close(mixer);
		return;
	}
	for (elem = snd_mixer_first_elem(mixer); elem; elem = snd_mixer_elem_next(elem)) {
		long min, max;
		if (!snd_mixer_selem_is_active(elem))
			continue;
		if (snd_mixer_selem_has_playback_switch(elem)) {
			snd_mixer_selem_set_playback_switch_all(elem, 1);
			unmuted++;
		}
		if (snd_mixer_selem_has_playback_volume(elem)
		    && snd_mixer_selem_get_playback_volume_range(elem, &min, &max) == 0 && max > min) {
			/* 80% of the range: audible on every laptop tried, not painful */
			snd_mixer_selem_set_playback_volume_all(elem, min + (max - min) * 4 / 5);
		}
	}
	snd_mixer_close(mixer);
	Log_print("alsa: mixer: %d playback switches enabled", unmuted);
}

/* Master volume for the System menu: the first playback element with a
   volume, preferring "Master" then "PCM"/"Speaker". */
static snd_mixer_elem_t *volume_elem(snd_mixer_t **mixer_out)
{
	static const char *const prefer[] = { "Master", "PCM", "Speaker", "Headphone", NULL };
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem, *first = NULL;
	int i;

	if (snd_mixer_open(&mixer, 0) < 0)
		return NULL;
	if ((snd_mixer_attach(mixer, ctl_name) < 0 && snd_mixer_attach(mixer, "default") < 0)
	    || snd_mixer_selem_register(mixer, NULL, NULL) < 0 || snd_mixer_load(mixer) < 0) {
		snd_mixer_close(mixer);
		return NULL;
	}
	for (i = 0; prefer[i]; i++) {
		for (elem = snd_mixer_first_elem(mixer); elem; elem = snd_mixer_elem_next(elem)) {
			if (!snd_mixer_selem_has_playback_volume(elem))
				continue;
			if (!first)
				first = elem;
			if (strcmp(snd_mixer_selem_get_name(elem), prefer[i]) == 0) {
				*mixer_out = mixer;
				return elem;
			}
		}
	}
	if (first) {
		*mixer_out = mixer;
		return first;
	}
	snd_mixer_close(mixer);
	return NULL;
}

int ALSA_GetVolume(void)
{
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem = volume_elem(&mixer);
	long min, max, cur;
	int pct = -1;
	if (!elem)
		return -1;
	if (snd_mixer_selem_get_playback_volume_range(elem, &min, &max) == 0 && max > min
	    && snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &cur) == 0)
		pct = (int)((cur - min) * 100 / (max - min));
	snd_mixer_close(mixer);
	return pct;
}

void ALSA_SetVolume(int percent)
{
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem = volume_elem(&mixer);
	long min, max;
	if (!elem)
		return;
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	if (snd_mixer_selem_get_playback_volume_range(elem, &min, &max) == 0 && max > min)
		snd_mixer_selem_set_playback_volume_all(elem, min + (max - min) * percent / 100);
	if (snd_mixer_selem_has_playback_switch(elem))
		snd_mixer_selem_set_playback_switch_all(elem, percent > 0);
	snd_mixer_close(mixer);
}

int PLATFORM_SoundSetup(Sound_setup_t *setup)
{
	static int mixer_done = 0;
	if (pcm) {
		snd_pcm_close(pcm);
		pcm = NULL;
	}
	if (!open_pcm(setup))
		return FALSE;
	if (!mixer_done) {
		unmute_mixer();
		mixer_done = 1;
	}
	return TRUE;
}

void PLATFORM_SoundExit(void)
{
	if (pcm) {
		snd_pcm_close(pcm);
		pcm = NULL;
	}
}

void PLATFORM_SoundPause(void)
{
	if (!pcm)
		return;
	if (can_pause && snd_pcm_state(pcm) == SND_PCM_STATE_RUNNING)
		snd_pcm_pause(pcm, 1);
	else
		snd_pcm_drop(pcm);
}

void PLATFORM_SoundContinue(void)
{
	if (!pcm)
		return;
	if (snd_pcm_state(pcm) == SND_PCM_STATE_PAUSED)
		snd_pcm_pause(pcm, 0);
	else
		snd_pcm_prepare(pcm);
}

unsigned int ALSA_Underruns(void)
{
	return underruns;
}

static int recover(int err)
{
	if (err == -EPIPE)
		underruns++;
	if (err == -EPIPE || err == -ESTRPIPE) {
		if (snd_pcm_recover(pcm, err, 1) == 0)
			return TRUE;
	}
	return FALSE;
}

unsigned int PLATFORM_SoundAvailable(void)
{
	snd_pcm_sframes_t avail;
	if (!pcm)
		return 0;
	avail = snd_pcm_avail_update(pcm);
	if (avail < 0) {
		if (!recover((int)avail))
			return 0;
		avail = snd_pcm_avail_update(pcm);
		if (avail < 0)
			return 0;
	}
	/* Keep one period of headroom so a late frame does not underrun;
	   the core's Sound_AdjustSpeed keeps the fill level steady. */
	if ((snd_pcm_uframes_t)avail > buffer_frames)
		avail = buffer_frames;
	return (unsigned int)avail * frame_bytes;
}

void PLATFORM_SoundWrite(UBYTE const *buffer, unsigned int size)
{
	snd_pcm_uframes_t frames = size / frame_bytes;
	if (!pcm)
		return;
	while (frames > 0) {
		snd_pcm_sframes_t n = snd_pcm_writei(pcm, buffer, frames);
		if (n < 0) {
			if (n == -EAGAIN)
				return;
			if (!recover((int)n))
				return;
			continue;
		}
		buffer += n * frame_bytes;
		frames -= n;
	}
}
