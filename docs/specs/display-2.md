# Spec: free scaling with the numbers shown; TV look in between

Status: from Carlos's session 2026-11-08: "Scaling should also have free,
so i can adjust horizontal and vertical sizes and it should show me the
current scaling number so i can see it and manually decide (i.e. it is
different 1.33 vs 1.5)" and "Can the tv look have a slider or options in
between? My crt tv is way sharper than your ntsc tv look option".
Design: Claude (Fable); implementer: an Opus agent; verification: Claude.
Rules as in `docs/specs/menus.md` (patch regeneration, `-Werror`, docs in
the same change). `scripts/flash.sh` and `flash-it.sh` untouched.

## A. Scaling: show the factors, add Free

Today (`emulator/kms/video.c` ~552, `KMS_VIDEO_scale_mode`): Integer
(same integer factor both axes), Fit (fill the height, 4:3 width), Fill
(whole panel). Every mode ends in `dst_w x dst_h` for a source of
`sw x sh` (sw = 320/336/384 after Borders, sh = 240).

- **Numbers everywhere.** The Display menu line reads
  `Scaling: Fit  3.33 x 3.00` (horizontal factor = dst_w / sw, vertical =
  dst_h / sh, two decimals, in that order), `Scaling: Integer  3 x 3`,
  `Scaling: Fill  4.00 x 3.33`, `Scaling: Free  3.50 x 3.00`. The Hardware
  page's display line gets the same `3.33 x 3.00`. `KMS_VIDEO_ScaleX()` /
  `KMS_VIDEO_ScaleY()` (doubles) computed from the current layout.
- **Free** (`KMS_SCALE_FREE`, config `KMS_SCALE=FREE`, plus
  `KMS_SCALE_X=3.50` and `KMS_SCALE_Y=3.00` saved with two decimals):
  dst_w = sw * x, dst_h = sh * y, centred, clipped to the panel. Picking
  Free in the Scaling popup starts from the current mode's factors (so
  Fit -> Free changes nothing until adjusted). Smoothing applies as for
  Fit/Fill.
- **Adjusting.** Choosing `Scaling` when Free is active (or right after
  picking Free) opens a popup:
  ```
  Free scaling
    Horizontal: 3.50      (slider, 1.00 .. panel max, step 0.05)
    Vertical:   3.00      (slider)
    Both:       3.00      (slider; sets both, keeps them equal)
    Pixel aspect: 1.17 (4:3 TV)   (label: x/y relative to the 4:3 shape, see below)
    Back                  Esc
  ```
  Sliders use `UI_driver->fSelectSlider` with a label callback that prints
  the factor (`3.50`); the slider's integer value is factor * 20 (0.05
  steps), range 20 .. floor(panel / source * 20). Live: every change
  recomputes the layout at once (`KMS_VIDEO_*` setter + the same path the
  other display settings use) and saves (`CFG_WriteConfig`).
- **Pixel aspect label**: the Atari's 4:3 TV picture at 336 columns has
  pixels 1.17 times wider than tall (4 * 240 / (3 * 336)); show
  `Pixel aspect: 1.17 (4:3 TV)` when x/y is within 2 % of that, `1.00
  (square)` within 2 % of 1, else the bare number. Purely informative.
- Hardware page: `  Free 3.50 x 3.00, scanlines Off` as today's second
  display line pattern.

## B. TV look: presets and a sharpness slider

Today `TV look: Crisp | NTSC TV` toggles `ARTIFACT_NTSC=NTSC-FULL` with
atari800's filter at `atari_ntsc_composite` (soft, bleeding, artifacts).
atari800's filter (`src/filter_ntsc.c`) has presets Composite, S-Video,
RGB, Monochrome and the knobs sharpness, resolution, artifacts, fringing,
bleed, burst phase (`FILTER_NTSC_SHARPNESS` ... in the config, range
-1.0 .. 1.0).

- `TV look` becomes a popup:
  ```
  TV look
    Crisp (no filter)
    Composite (soft, colour bleed)      = preset composite
    S-Video (sharper, less bleed)       = preset svideo
    RGB monitor (sharp, artifacts only) = preset rgb
    Sharpness: +0.40                    slider -1.00 .. +1.00, step 0.05, on top of the preset
    Back                                Esc
  ```
  Choosing a preset sets `FILTER_NTSC_setup` from it and turns the filter
  on (`ARTIFACT_NTSC`); Crisp turns it off and keeps the preset for later.
  Sharpness changes `FILTER_NTSC_setup.sharpness` (then
  `FILTER_NTSC_Update(FILTER_NTSC_emu)` so the live picture follows) and is
  saved by atari800's own `FILTER_NTSC_SHARPNESS` config line; the preset
  by `FILTER_NTSC_PRESET` (atari800 writes it already; make sure the KMS
  config path round-trips it: look at `cfg.c` and `filter_ntsc.c`
  Read/WriteConfig). NTSC only, as today ("NTSC only (TV system is PAL)").
- Display menu line: `TV look: S-Video, sharp +0.40` (`Crisp` when off).
  Hardware page: `, S-Video` instead of `, NTSC TV`.
- Keep the stock Advanced > Display Settings consistent: it edits the same
  variables, nothing to do but check it does not fight the KMS line.

## C. Docs

README Display paragraph (Free with numbers, TV look presets +
sharpness), CHANGELOG. KNOWN-ISSUES only if something is left out.

## D. Verification (Claude, QEMU, 1280x800 panel in QEMU)

1. Display menu shows `Scaling: Integer  3 x 3` at boot; Fit shows
   `3.33 x 3.00`-ish numbers consistent with the panel; Fill `4.00 x 3.33`.
2. Free: Horizontal 3.50 / Vertical 3.00 -> picture visibly wider; the
   config has `KMS_SCALE=FREE`, `KMS_SCALE_X=3.50`, `KMS_SCALE_Y=3.00`;
   survives an emulator restart (kill atari800 on the debug image).
3. TV look: S-Video and RGB visibly sharper than Composite (screenshots
   of READY text at the same zoom); sharpness +1.00 vs -1.00 differ; the
   config carries the preset and sharpness and they survive a restart.
