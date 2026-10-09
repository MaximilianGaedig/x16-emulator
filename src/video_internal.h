// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

// Definitions shared by the VERA register side (video.c) and the
// scanline renderer (video_render.c).

#ifndef _VIDEO_INTERNAL_H_
#define _VIDEO_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#define ADDR_VRAM_START     0x00000
#define ADDR_VRAM_END       0x20000
#define ADDR_PSG_START      0x1F9C0
#define ADDR_PSG_END        0x1FA00
#define ADDR_PALETTE_START  0x1FA00
#define ADDR_PALETTE_END    0x1FC00
#define ADDR_SPRDATA_START  0x1FC00
#define ADDR_SPRDATA_END    0x20000

#define NUM_SPRITES 128

// both VGA and NTSC
#define SCAN_HEIGHT 525
#define PIXEL_FREQ 25.0

// VGA
#define VGA_SCAN_WIDTH 800
#define VGA_Y_OFFSET 0

// NTSC: 262.5 lines per frame, lower field first
#define NTSC_HALF_SCAN_WIDTH 794
#define NTSC_X_OFFSET 270
#define NTSC_Y_OFFSET_LOW 42
#define NTSC_Y_OFFSET_HIGH 568
#define TITLE_SAFE_X 0.067
#define TITLE_SAFE_Y 0.05

// visible area we're drawing
#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480

#define SCREEN_RAM_OFFSET 0x00000

#ifdef __APPLE__
#define LSHORTCUT_KEY SDL_SCANCODE_LGUI
#define RSHORTCUT_KEY SDL_SCANCODE_RGUI
#else
#define LSHORTCUT_KEY SDL_SCANCODE_LCTRL
#define RSHORTCUT_KEY SDL_SCANCODE_RCTRL
#endif

// When rendering a layer line, we can amortize some of the cost by calculating multiple pixels at a time.
#define LAYER_PIXELS_PER_ITERATION 8

#define MAX(a,b) ((a) > (b) ? a : b)

struct video_layer_properties
{
	uint8_t color_depth;
	uint32_t map_base;
	uint32_t tile_base;

	bool text_mode;
	bool text_mode_256c;
	bool tile_mode;
	bool bitmap_mode;

	uint16_t hscroll;
	uint16_t vscroll;

	uint8_t  mapw_log2;
	uint8_t  maph_log2;
	uint16_t tilew;
	uint16_t tileh;
	uint8_t  tilew_log2;
	uint8_t  tileh_log2;

	uint16_t mapw_max;
	uint16_t maph_max;
	uint16_t tilew_max;
	uint16_t tileh_max;
	uint16_t layerw_max;
	uint16_t layerh_max;

	uint8_t tile_size_log2;

	int min_eff_x;
	int max_eff_x;

	uint8_t bits_per_pixel;
	uint8_t first_color_pos;
	uint8_t color_mask;
	uint8_t color_fields_max;
};

#define NUM_LAYERS 2

static inline int
calc_layer_eff_x(const struct video_layer_properties *props, const int x)
{
	return (x + props->hscroll) & (props->layerw_max);
}

static inline int
calc_layer_eff_y(const struct video_layer_properties *props, const int y)
{
	return (y + props->vscroll) & (props->layerh_max);
}

static inline uint32_t
calc_layer_map_addr_base2(const struct video_layer_properties *props, const int eff_x, const int eff_y)
{
	// Slightly faster on some platforms because we know that tilew and tileh are powers of 2.
	return props->map_base + ((((eff_y >> props->tileh_log2) << props->mapw_log2) + (eff_x >> props->tilew_log2)) << 1);
}

//TODO: Unused in all current cases. Delete? Or leave commented as a reminder?
//static uint32_t
//calc_layer_map_addr(struct video_layer_properties *props, int eff_x, int eff_y)
//{
//	return props->map_base + ((eff_y / props->tileh) * props->mapw + (eff_x / props->tilew)) * 2;
//}
static inline void
layer_properties_refresh(struct video_layer_properties *props, const uint8_t *regs)
{

	uint16_t prev_layerw_max = props->layerw_max;
	uint16_t prev_hscroll = props->hscroll;

	props->color_depth    = regs[0] & 0x3;
	props->map_base       = regs[1] << 9;
	props->tile_base      = (regs[2] & 0xFC) << 9;
	props->bitmap_mode    = (regs[0] & 0x4) != 0;
	props->text_mode      = (props->color_depth == 0) && !props->bitmap_mode;
	props->text_mode_256c = (regs[0] & 8) != 0;
	props->tile_mode      = !props->bitmap_mode && !props->text_mode;

	if (!props->bitmap_mode) {
		props->hscroll = regs[3] | (regs[4] & 0xf) << 8;
		props->vscroll = regs[5] | (regs[6] & 0xf) << 8;
	} else {
		props->hscroll = 0;
		props->vscroll = 0;
	}

	uint16_t mapw = 0;
	uint16_t maph = 0;
	props->tilew = 0;
	props->tileh = 0;

	if (props->tile_mode || props->text_mode) {
		props->mapw_log2 = 5 + ((regs[0] >> 4) & 3);
		props->maph_log2 = 5 + ((regs[0] >> 6) & 3);
		mapw      = 1 << props->mapw_log2;
		maph      = 1 << props->maph_log2;

		// Scale the tiles or text characters according to TILEW and TILEH.
		props->tilew_log2 = 3 + (regs[2] & 1);
		props->tileh_log2 = 3 + ((regs[2] >> 1) & 1);
		props->tilew      = 1 << props->tilew_log2;
		props->tileh      = 1 << props->tileh_log2;
	} else if (props->bitmap_mode) {
		// bitmap mode is basically tiled mode with a single huge tile
		props->tilew = (regs[2] & 1) ? 640 : 320;
		props->tileh = SCREEN_HEIGHT;
	}

	// We know mapw, maph, tilew, and tileh are powers of two in all cases except bitmap modes, and any products of that set will be powers of two,
	// so there's no need to modulo against them if we have bitmasks we can bitwise-and against.

	props->mapw_max = mapw - 1;
	props->maph_max = maph - 1;
	props->tilew_max = props->tilew - 1;
	props->tileh_max = props->tileh - 1;
	props->layerw_max = (mapw * props->tilew) - 1;
	props->layerh_max = (maph * props->tileh) - 1;

	// Find min/max eff_x for bulk reading in tile data during draw.
	if (prev_layerw_max != props->layerw_max || prev_hscroll != props->hscroll) {
		int min_eff_x = INT_MAX;
		int max_eff_x = INT_MIN;
		for (int x = 0; x < SCREEN_WIDTH; ++x) {
			int eff_x = calc_layer_eff_x(props, x);
			if (eff_x < min_eff_x) {
				min_eff_x = eff_x;
			}
			if (eff_x > max_eff_x) {
				max_eff_x = eff_x;
			}
		}
		props->min_eff_x = min_eff_x;
		props->max_eff_x = max_eff_x;
	}

	props->bits_per_pixel = 1 << props->color_depth;
	props->tile_size_log2 = props->tilew_log2 + props->tileh_log2 + props->color_depth - 3;

	props->first_color_pos  = 8 - props->bits_per_pixel;
	props->color_mask       = (1 << props->bits_per_pixel) - 1;
	props->color_fields_max = (8 >> props->color_depth) - 1;
}

#endif
