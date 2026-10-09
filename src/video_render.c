// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

// VERA scanline renderer, optionally running on its own thread.
// See video_render.h for an overview.

#include "video_render.h"
#include "video_internal.h"

#include <SDL.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

//
// Renderer-side copy of the VERA state the scanline renderer reads.
// Only the renderer (the render thread, if enabled) touches these.
//

#define COMPOSER_SLOTS 4*64

static uint8_t video_ram[0x20000];
static uint8_t palette[256 * 2];
static uint8_t sprite_data[128][8];
static uint8_t reg_layer[2][7];
static uint8_t reg_composer[COMPOSER_SLOTS];
static uint8_t prev_reg_composer[2][COMPOSER_SLOTS];

static uint8_t layer_line[2][SCREEN_WIDTH];
static uint8_t sprite_line_col[SCREEN_WIDTH];
static uint8_t sprite_line_z[SCREEN_WIDTH];
static uint8_t sprite_line_mask[SCREEN_WIDTH];
static uint8_t sprite_line_collisions;
static bool layer_line_enable[2];
static bool old_layer_line_enable[2];
static bool old_sprite_line_enable;
static bool sprite_line_enable;

static uint8_t framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT * 4];

static struct video_layer_properties layer_properties[NUM_LAYERS];
static struct video_layer_properties prev_layer_properties[2][NUM_LAYERS];

static void
refresh_layer_properties(const uint8_t layer)
{
	layer_properties_refresh(&layer_properties[layer], reg_layer[layer]);
}

static inline uint8_t
vram_read(uint32_t address)
{
	return video_ram[address & 0x1FFFF];
}

static void
vram_read_range(uint8_t* dest, uint32_t address, uint32_t size)
{
	if (address >= ADDR_VRAM_START && (address+size) <= ADDR_VRAM_END) {
		memcpy(dest, &video_ram[address], size);
	} else {
		for(int i = 0; i < size; ++i) {
			*dest++ = vram_read(address + i);
		}
	}
}

struct video_sprite_properties
{
	int8_t sprite_zdepth;
	uint8_t sprite_collision_mask;

	int16_t sprite_x;
	int16_t sprite_y;
	uint8_t sprite_width_log2;
	uint8_t sprite_height_log2;
	uint8_t sprite_width;
	uint8_t sprite_height;

	bool hflip;
	bool vflip;

	uint8_t color_mode;
	uint32_t sprite_address;

	uint16_t palette_offset;
};

static struct video_sprite_properties sprite_properties[128];

static void
refresh_sprite_properties(const uint16_t sprite)
{
	struct video_sprite_properties* props = &sprite_properties[sprite];

	props->sprite_zdepth = (sprite_data[sprite][6] >> 2) & 3;
	props->sprite_collision_mask = sprite_data[sprite][6] & 0xf0;

	props->sprite_x = sprite_data[sprite][2] | (sprite_data[sprite][3] & 3) << 8;
	props->sprite_y = sprite_data[sprite][4] | (sprite_data[sprite][5] & 3) << 8;
	props->sprite_width_log2  = (((sprite_data[sprite][7] >> 4) & 3) + 3);
	props->sprite_height_log2 = ((sprite_data[sprite][7] >> 6) + 3);
	props->sprite_width       = 1 << props->sprite_width_log2;
	props->sprite_height      = 1 << props->sprite_height_log2;

	// fix up negative coordinates
	if (props->sprite_x >= 0x400 - props->sprite_width) {
		props->sprite_x -= 0x400;
	}
	if (props->sprite_y >= 0x400 - props->sprite_height) {
		props->sprite_y -= 0x400;
	}

	props->hflip = sprite_data[sprite][6] & 1;
	props->vflip = (sprite_data[sprite][6] >> 1) & 1;

	props->color_mode     = (sprite_data[sprite][1] >> 7) & 1;
	props->sprite_address = sprite_data[sprite][0] << 5 | (sprite_data[sprite][1] & 0xf) << 13;

	props->palette_offset = (sprite_data[sprite][7] & 0x0f) << 4;
}

struct video_palette
{
	uint32_t entries[256];
	bool dirty;
};

static struct video_palette video_palette;

static void
refresh_palette() {
	const uint8_t out_mode = reg_composer[0] & 3;
	const bool chroma_disable = ((reg_composer[0] & 0x07) == 6);
	for (int i = 0; i < 256; ++i) {
		uint8_t r;
		uint8_t g;
		uint8_t b;
		if (out_mode == 0) {
			// video generation off
			// -> show blue screen
			r = 0;
			g = 0;
			b = 255;
		} else {
			uint16_t entry = palette[i * 2] | palette[i * 2 + 1] << 8;
			r = ((entry >> 8) & 0xf) << 4 | ((entry >> 8) & 0xf);
			g = ((entry >> 4) & 0xf) << 4 | ((entry >> 4) & 0xf);
			b = (entry & 0xf) << 4 | (entry & 0xf);
			if (chroma_disable) {
				r = g = b = (r + b + g) / 3;
			}
		}

		video_palette.entries[i] = (uint32_t)(r << 16) | ((uint32_t)g << 8) | ((uint32_t)b);
	}
	video_palette.dirty = false;
}

static void
expand_4bpp_data(uint8_t *dst, const uint8_t *src, int dst_size)
{
	while (dst_size >= 2) {
		*dst = (*src) >> 4;
		++dst;
		*dst = (*src) & 0xf;
		++dst;

		++src;
		dst_size -= 2;
	}
}

static void
render_sprite_line(const uint16_t y)
{
	memset(sprite_line_col, 0, SCREEN_WIDTH);
	memset(sprite_line_z, 0, SCREEN_WIDTH);
	memset(sprite_line_mask, 0, SCREEN_WIDTH);

	uint16_t sprite_budget = 800 + 1;
	for (int i = 0; i < NUM_SPRITES; i++) {
		// one clock per lookup
		sprite_budget--; if (sprite_budget == 0) break;
		const struct video_sprite_properties *props = &sprite_properties[i];

		if (props->sprite_zdepth == 0) {
			continue;
		}

		// check whether this line falls within the sprite
		if (y < props->sprite_y || y >= props->sprite_y + props->sprite_height) {
			continue;
		}

		const uint16_t eff_sy = props->vflip ? ((props->sprite_height - 1) - (y - props->sprite_y)) : (y - props->sprite_y);

		int16_t       eff_sx      = (props->hflip ? (props->sprite_width - 1) : 0);
		const int16_t eff_sx_incr = props->hflip ? -1 : 1;

		const uint8_t *bitmap_data = video_ram + props->sprite_address + (eff_sy << (props->sprite_width_log2 - (1 - props->color_mode)));

		uint8_t unpacked_sprite_line[64];
		const uint16_t width = (props->sprite_width<64? props->sprite_width : 64);
		const uint8_t vram_fetch_mask = ((2 - props->color_mode) << 2) - 1;
		if (props->color_mode == 0) {
			// 4bpp
			expand_4bpp_data(unpacked_sprite_line, bitmap_data, width);
		} else {
			// 8bpp
			memcpy(unpacked_sprite_line, bitmap_data, width);
		}

		for (uint16_t sx = 0; sx < props->sprite_width; ++sx) {
			const uint16_t line_x = props->sprite_x + sx;
			if (line_x >= SCREEN_WIDTH) {
				eff_sx += eff_sx_incr;
				continue;
			}

			// one clock per fetched 32 bits
			if (!(sx & vram_fetch_mask)) {
				sprite_budget--; if (sprite_budget == 0) break;
			}

			// one clock per rendered pixel
			sprite_budget--; if (sprite_budget == 0) break;

			uint8_t col_index = unpacked_sprite_line[eff_sx];
			eff_sx += eff_sx_incr;

			// palette offset
			if (col_index > 0) {
				sprite_line_collisions |= sprite_line_mask[line_x] & props->sprite_collision_mask;
				sprite_line_mask[line_x] |= props->sprite_collision_mask;

				if (props->sprite_zdepth > sprite_line_z[line_x]) {
					if (col_index < 16) {
						col_index += props->palette_offset;
					}
					sprite_line_col[line_x] = col_index;
					sprite_line_z[line_x] = props->sprite_zdepth;
				}
			}
		}
	}
}

static void
render_layer_line_text(uint8_t layer, uint16_t y)
{
	const struct video_layer_properties *props = &prev_layer_properties[1][layer];
	const struct video_layer_properties *props0 = &prev_layer_properties[0][layer];

	const uint8_t max_pixels_per_byte = (8 >> props->color_depth) - 1;
	const int     eff_y               = calc_layer_eff_y(props0, y);
	const int     yy                  = eff_y & props->tileh_max;

	// additional bytes to reach the correct line of the tile
	const uint32_t y_add = (yy << props->tilew_log2) >> 3;

	const uint32_t map_addr_begin = calc_layer_map_addr_base2(props, props->min_eff_x, eff_y);
	const uint32_t map_addr_end   = calc_layer_map_addr_base2(props, props->max_eff_x, eff_y);
	const int      size           = (map_addr_end - map_addr_begin) + 2;

	uint8_t tile_bytes[512]; // max 256 tiles, 2 bytes each.
	vram_read_range(tile_bytes, map_addr_begin, size);

	uint32_t tile_start;

	uint8_t  fg_color;
	uint8_t  bg_color;
	uint8_t  s;
	uint8_t  color_shift;

	{
		const int eff_x = calc_layer_eff_x(props, 0);
		const int xx    = eff_x & props->tilew_max;

		// extract all information from the map
		const uint32_t map_addr = calc_layer_map_addr_base2(props, eff_x, eff_y) - map_addr_begin;

		const uint8_t tile_index = tile_bytes[map_addr];
		const uint8_t byte1      = tile_bytes[map_addr + 1];

		if (!props->text_mode_256c) {
			fg_color = byte1 & 15;
			bg_color = byte1 >> 4;
		} else {
			fg_color = byte1;
			bg_color = 0;
		}

		// offset within tilemap of the current tile
		tile_start = tile_index << props->tile_size_log2;

		// additional bytes to reach the correct column of the tile
		const uint16_t x_add       = xx >> 3;
		const uint32_t tile_offset = tile_start + y_add + x_add;

		s           = vram_read(props->tile_base + tile_offset);
		color_shift = max_pixels_per_byte - (xx & 0x7);
	}

	// Render tile line.
	for (int x = 0; x < SCREEN_WIDTH; x++) {
		// Scrolling
		const int eff_x = calc_layer_eff_x(props, x);
		const int xx = eff_x & props->tilew_max;

		if ((eff_x & 0x7) == 0) {
			if ((eff_x & props->tilew_max) == 0) {
				// extract all information from the map
				const uint32_t map_addr = calc_layer_map_addr_base2(props, eff_x, eff_y) - map_addr_begin;

				const uint8_t tile_index = tile_bytes[map_addr];
				const uint8_t byte1      = tile_bytes[map_addr + 1];

				if (!props->text_mode_256c) {
					fg_color = byte1 & 15;
					bg_color = byte1 >> 4;
				} else {
					fg_color = byte1;
					bg_color = 0;
				}

				// offset within tilemap of the current tile
				tile_start = tile_index << props->tile_size_log2;
			}

			// additional bytes to reach the correct column of the tile
			const uint16_t x_add       = xx >> 3;
			const uint32_t tile_offset = tile_start + y_add + x_add;

			s           = vram_read(props->tile_base + tile_offset);
			color_shift = max_pixels_per_byte;
		}

		// convert tile byte to indexed color
		const uint8_t col_index = (s >> color_shift) & 1;
		--color_shift;
		layer_line[layer][x] = col_index ? fg_color : bg_color;
	}
}

static void
render_layer_line_tile(uint8_t layer, uint16_t y)
{
	const struct video_layer_properties *props = &prev_layer_properties[1][layer];
	const struct video_layer_properties *props0 = &prev_layer_properties[0][layer];

	const uint8_t max_pixels_per_byte = (8 >> props->color_depth) - 1;
	const int     eff_y               = calc_layer_eff_y(props0, y);
	const uint8_t yy                  = eff_y & props->tileh_max;
	const uint8_t yy_flip             = yy ^ props->tileh_max;
	const uint32_t y_add              = (yy << ((props->tilew_log2 + props->color_depth - 3) & 31));
	const uint32_t y_add_flip         = (yy_flip << ((props->tilew_log2 + props->color_depth - 3) & 31));

	const uint32_t map_addr_begin = calc_layer_map_addr_base2(props, props->min_eff_x, eff_y);
	const uint32_t map_addr_end   = calc_layer_map_addr_base2(props, props->max_eff_x, eff_y);
	const int      size           = (map_addr_end - map_addr_begin) + 2;

	uint8_t tile_bytes[512]; // max 256 tiles, 2 bytes each.
	vram_read_range(tile_bytes, map_addr_begin, size);

	uint8_t  palette_offset;
	bool     vflip;
	bool     hflip;
	uint32_t tile_start;
	uint8_t  s;
	uint8_t  color_shift;
	int8_t   color_shift_incr;

	{
		const int eff_x = calc_layer_eff_x(props, 0);

		// extract all information from the map
		const uint32_t map_addr = calc_layer_map_addr_base2(props, eff_x, eff_y) - map_addr_begin;

		const uint8_t byte0 = tile_bytes[map_addr];
		const uint8_t byte1 = tile_bytes[map_addr + 1];

		// Tile Flipping
		vflip = (byte1 >> 3) & 1;
		hflip = (byte1 >> 2) & 1;

		palette_offset = byte1 & 0xf0;

		// offset within tilemap of the current tile
		const uint16_t tile_index = byte0 | ((byte1 & 3) << 8);
		tile_start                = tile_index << props->tile_size_log2;

		color_shift_incr = hflip ? props->bits_per_pixel : -props->bits_per_pixel;

		int xx = eff_x & props->tilew_max;
		if (hflip) {
			xx          = xx ^ (props->tilew_max);
			color_shift = 0;
		} else {
			color_shift = props->first_color_pos;
		}

		// additional bytes to reach the correct column of the tile
		uint16_t x_add       = (xx << props->color_depth) >> 3;
		uint32_t tile_offset = tile_start + (vflip ? y_add_flip : y_add) + x_add;

		s = vram_read(props->tile_base + tile_offset);
	}


	// Render tile line.
	for (int x = 0; x < SCREEN_WIDTH; x++) {
		const int eff_x = calc_layer_eff_x(props, x);

		if ((eff_x & max_pixels_per_byte) == 0) {
			if ((eff_x & props->tilew_max) == 0) {
				// extract all information from the map
				const uint32_t map_addr = calc_layer_map_addr_base2(props, eff_x, eff_y) - map_addr_begin;

				const uint8_t byte0 = tile_bytes[map_addr];
				const uint8_t byte1 = tile_bytes[map_addr + 1];

				// Tile Flipping
				vflip = (byte1 >> 3) & 1;
				hflip = (byte1 >> 2) & 1;

				palette_offset = byte1 & 0xf0;

				// offset within tilemap of the current tile
				const uint16_t tile_index = byte0 | ((byte1 & 3) << 8);
				tile_start                = tile_index << props->tile_size_log2;

				color_shift_incr = hflip ? props->bits_per_pixel : -props->bits_per_pixel;
			}

			int xx = eff_x & props->tilew_max;
			if (hflip) {
				xx = xx ^ (props->tilew_max);
				color_shift = 0;
			} else {
				color_shift = props->first_color_pos;
			}

			// additional bytes to reach the correct column of the tile
			const uint16_t x_add       = (xx << props->color_depth) >> 3;
			const uint32_t tile_offset = tile_start + (vflip ? y_add_flip : y_add) + x_add;

			s = vram_read(props->tile_base + tile_offset);
		}

		// convert tile byte to indexed color
		uint8_t col_index = (s >> color_shift) & props->color_mask;
		color_shift += color_shift_incr;

		// Apply Palette Offset
		if (col_index > 0 && col_index < 16) {
			col_index += palette_offset;
			if (props->text_mode_256c) {
				col_index |= 0x80;
			}
		}
		layer_line[layer][x] = col_index;
	}
}


static void
render_layer_line_bitmap(uint8_t layer, uint16_t y)
{
	const struct video_layer_properties *props = &prev_layer_properties[1][layer];
//	const struct video_layer_properties *props0 = &prev_layer_properties[0][layer];

	int yy = y % props->tileh;
	// additional bytes to reach the correct line of the tile
	uint32_t y_add = (yy * props->tilew * props->bits_per_pixel) >> 3;

	// Render tile line.
	for (int x = 0; x < SCREEN_WIDTH; x++) {
		int xx = x % props->tilew;

		// extract all information from the map
		uint8_t palette_offset = reg_layer[layer][4] & 0xf;

		// additional bytes to reach the correct column of the tile
		uint16_t x_add = (xx * props->bits_per_pixel) >> 3;
		uint32_t tile_offset = y_add + x_add;
		uint8_t s = vram_read(props->tile_base + tile_offset);

		// convert tile byte to indexed color
		uint8_t col_index = (s >> (props->first_color_pos - ((xx & props->color_fields_max) << props->color_depth))) & props->color_mask;

		// Apply Palette Offset
		if (col_index > 0 && col_index < 16) {
			col_index += palette_offset << 4;
			if (props->text_mode_256c) {
				col_index |= 0x80;
			}
		}
		layer_line[layer][x] = col_index;
	}
}

static uint8_t calculate_line_col_index(uint8_t spr_zindex, uint8_t spr_col_index, uint8_t l1_col_index, uint8_t l2_col_index)
{
	uint8_t col_index = 0;
	switch (spr_zindex) {
		case 3:
			col_index = spr_col_index ? spr_col_index : (l2_col_index ? l2_col_index : l1_col_index);
			break;
		case 2:
			col_index = l2_col_index ? l2_col_index : (spr_col_index ? spr_col_index : l1_col_index);
			break;
		case 1:
			col_index = l2_col_index ? l2_col_index : (l1_col_index ? l1_col_index : spr_col_index);
			break;
		case 0:
			col_index = l2_col_index ? l2_col_index : l1_col_index;
			break;
	}
	return col_index;
}

static void
render_line(uint16_t y, uint16_t s_pos_x, bool skip_layers)
{
	static uint16_t y_prev;
	static uint16_t s_pos_x_p;
	static uint32_t eff_y_fp; // 16.16 fixed point
	static uint32_t eff_x_fp; // 16.16 fixed point

	static uint8_t col_line[SCREEN_WIDTH];

	uint8_t dc_video = reg_composer[0];
	uint16_t vstart = reg_composer[6] << 1;
	uint16_t vstop = reg_composer[7] << 1;

	if (y != y_prev) {
		y_prev = y;
		s_pos_x_p = 0;

		// Copy the composer array to 2-line history buffer
		// so that the raster effects that happen on a delay take effect
		// at exactly the right time

		// This simulates different effects happening at render,
		// render but delayed until the next line, or applied mid-line
		// at scan-out

		memcpy(prev_reg_composer[1], prev_reg_composer[0], sizeof(*reg_composer) * COMPOSER_SLOTS);
		memcpy(prev_reg_composer[0], reg_composer, sizeof(*reg_composer) * COMPOSER_SLOTS);

		// Same with the layer properties

		memcpy(prev_layer_properties[1], prev_layer_properties[0], sizeof(*layer_properties) * NUM_LAYERS);
		memcpy(prev_layer_properties[0], layer_properties, sizeof(*layer_properties) * NUM_LAYERS);

		if ((dc_video & 3) > 1) { // 480i or 240p
			if ((y >> 1) == 0) {
				eff_y_fp = y*(prev_reg_composer[1][2] << 9);
			} else if ( ((y & 0xfffe) >= vstart) && ((y & 0xfffe) < vstop) ) {
				eff_y_fp += (prev_reg_composer[1][2] << 10);
			}
		} else {
			if (y == 0) {
				eff_y_fp = 0;
			} else if ( (y >= vstart) && (y < vstop) ) {
				eff_y_fp += (prev_reg_composer[1][2] << 9);
			}
		}
	}

	if ((dc_video & 8) && (dc_video & 3) > 1) { // progressive NTSC/RGB mode
		y &= 0xfffe;
	}

	// refresh palette for next entry
	if (video_palette.dirty) {
		refresh_palette();
	}

	if (y >= SCREEN_HEIGHT) {
		return;
	}

	if (s_pos_x_p == 0) {
		eff_x_fp = 0;
	}

	uint8_t out_mode = reg_composer[0] & 3;

	uint8_t border_color = reg_composer[3];
	uint16_t hstart = reg_composer[4] << 2;
	uint16_t hstop = reg_composer[5] << 2;

	uint16_t eff_y = (eff_y_fp >> 16);
	if (eff_y >= 480) eff_y = 480 - (y & 1);

	layer_line_enable[0] = dc_video & 0x10;
	layer_line_enable[1] = dc_video & 0x20;
	sprite_line_enable   = dc_video & 0x40;

	// clear layer_line if layer gets disabled
	for (uint8_t layer = 0; layer < 2; layer++) {
		if (!layer_line_enable[layer] && old_layer_line_enable[layer]) {
			for (uint16_t i = s_pos_x_p; i < SCREEN_WIDTH; i++) {
				layer_line[layer][i] = 0;
			}
		}
		if (s_pos_x_p == 0)
			old_layer_line_enable[layer] = layer_line_enable[layer];
	}

	// clear sprite_line if sprites get disabled
	if (!sprite_line_enable && old_sprite_line_enable) {
		for (uint16_t i = s_pos_x_p; i < SCREEN_WIDTH; i++) {
			sprite_line_col[i] = 0;
			sprite_line_z[i] = 0;
			sprite_line_mask[i] = 0;
		}
	}

	if (s_pos_x_p == 0)
		old_sprite_line_enable = sprite_line_enable;



	if (sprite_line_enable) {
		render_sprite_line(eff_y);
	}

	if (skip_layers) {
		// sprites were needed for the collision IRQ, but we can skip
		// everything else if we're in warp mode, most of the time
		return;
	}

	if (layer_line_enable[0]) {
		if (prev_layer_properties[1][0].text_mode) {
			render_layer_line_text(0, eff_y);
		} else if (prev_layer_properties[1][0].bitmap_mode) {
			render_layer_line_bitmap(0, eff_y);
		} else {
			render_layer_line_tile(0, eff_y);
		}
	}
	if (layer_line_enable[1]) {
		if (prev_layer_properties[1][1].text_mode) {
			render_layer_line_text(1, eff_y);
		} else if (prev_layer_properties[1][1].bitmap_mode) {
			render_layer_line_bitmap(1, eff_y);
		} else {
			render_layer_line_tile(1, eff_y);
		}
	}

	// If video output is enabled, calculate color indices for line.
	if (out_mode != 0) {
		// Add border after if required.
		if (y < vstart || y >= vstop) {
			uint32_t border_fill = border_color;
			border_fill = border_fill | (border_fill << 8);
			border_fill = border_fill | (border_fill << 16);
			memset(col_line, border_fill, SCREEN_WIDTH);
		} else {
			hstart = hstart < 640 ? hstart : 640;
			hstop = hstop < 640 ? hstop : 640;

			for (uint16_t x = s_pos_x_p; x < hstart && x < s_pos_x; ++x) {
				col_line[x] = border_color;
			}

			const uint32_t scale = reg_composer[1];
			for (uint16_t x = MAX(hstart, s_pos_x_p); x < hstop && x < s_pos_x; ++x) {
				uint16_t eff_x = eff_x_fp >> 16;
				col_line[x] = (eff_x < SCREEN_WIDTH) ? calculate_line_col_index(sprite_line_z[eff_x], sprite_line_col[eff_x], layer_line[0][eff_x], layer_line[1][eff_x]) : 0;
				eff_x_fp += (scale << 9);
			}
			for (uint16_t x = hstop; x < s_pos_x; ++x) {
				col_line[x] = border_color;
			}
		}
	}

	// Look up all color indices.
	uint32_t* framebuffer4_begin = ((uint32_t*)framebuffer) + (y * SCREEN_WIDTH) + s_pos_x_p;
	{
		uint32_t* framebuffer4 = framebuffer4_begin;
		for (uint16_t x = s_pos_x_p; x < s_pos_x; x++) {
			*framebuffer4++ = video_palette.entries[col_line[x]];
		}
	}

	// NTSC overscan
	if (out_mode == 2) {
		uint32_t* framebuffer4 = framebuffer4_begin;
		for (uint16_t x = s_pos_x_p; x < s_pos_x; x++)
		{
			if (x < SCREEN_WIDTH * TITLE_SAFE_X ||
				x > SCREEN_WIDTH * (1 - TITLE_SAFE_X) ||
				y < SCREEN_HEIGHT * TITLE_SAFE_Y ||
				y > SCREEN_HEIGHT * (1 - TITLE_SAFE_Y)) {

				// Divide RGB elements by 4.
				*framebuffer4 &= 0x00fcfcfc;
				*framebuffer4 >>= 2;
			}
			framebuffer4++;
		}
	}

	s_pos_x_p = s_pos_x;
}
//
// Command queue
//
// A single-producer (emulation thread), single-consumer (render thread)
// ring of 32 bit commands: the top 4 bits are the command type, the rest
// is its payload.
//

enum {
	CMD_VRAM = 1,     // [24:8] address, [7:0] value
	CMD_SIDE_EFFECT,  // [24:8] address, [7:0] value
	CMD_LAYER,        // [11] layer, [10:8] register, [7:0] value
	CMD_COMPOSER,     // [16] palette dirty, [15:8] register, [7:0] value
	CMD_CLEAR_FB,
	CMD_LINE,         // [26] skip layers, [25:16] scan position x, [15:0] y
};

#define CMD(type, payload) (((uint32_t)(type) << 28) | (payload))

#define QUEUE_SIZE (1 << 20) // entries, must be a power of two
#define QUEUE_MASK (QUEUE_SIZE - 1)

// spin this many times before going to sleep
#define SPIN_COUNT 2000

static bool threaded;
static uint32_t *queue;
static uint32_t queue_wr_local; // producer's copy of queue_wr
static _Atomic uint32_t queue_wr;
static _Atomic uint32_t queue_rd;
static _Atomic bool renderer_sleeping;
static _Atomic bool producer_waiting;
static _Atomic bool quit;
static SDL_Thread *thread;
static SDL_mutex *mutex;
static SDL_cond *cond_work; // renderer waits for commands
static SDL_cond *cond_idle; // producer waits for the renderer to catch up

static inline void
cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
	__asm__ volatile("yield");
#endif
}

static void
execute(uint32_t cmd)
{
	const uint32_t p = cmd & 0x0fffffff;
	switch (cmd >> 28) {
		case CMD_VRAM:
			video_ram[p >> 8] = p & 0xff;
			break;
		case CMD_SIDE_EFFECT: {
			const uint32_t address = p >> 8;
			const uint8_t value = p & 0xff;
			if (address >= ADDR_PALETTE_START && address < ADDR_PALETTE_END) {
				palette[address & 0x1ff] = value;
				video_palette.dirty = true;
			} else if (address >= ADDR_SPRDATA_START && address < ADDR_SPRDATA_END) {
				sprite_data[(address >> 3) & 0x7f][address & 0x7] = value;
				refresh_sprite_properties((address >> 3) & 0x7f);
			}
			break;
		}
		case CMD_LAYER: {
			const uint8_t layer = (p >> 11) & 1;
			reg_layer[layer][(p >> 8) & 7] = p & 0xff;
			refresh_layer_properties(layer);
			break;
		}
		case CMD_COMPOSER:
			reg_composer[(p >> 8) & 0xff] = p & 0xff;
			if (p & 0x10000) {
				video_palette.dirty = true;
			}
			break;
		case CMD_CLEAR_FB:
			memset(framebuffer, 0, sizeof(framebuffer));
			break;
		case CMD_LINE:
			render_line(p & 0xffff, (p >> 16) & 0x3ff, (p >> 26) & 1);
			break;
	}
}

static int
render_thread(void *data)
{
	uint32_t rd = atomic_load(&queue_rd);
	for (;;) {
		uint32_t wr = atomic_load_explicit(&queue_wr, memory_order_acquire);
		if (rd == wr) {
			// Idle. Wake up the producer if it is waiting for us.
			if (atomic_load(&producer_waiting)) {
				SDL_LockMutex(mutex);
				SDL_CondSignal(cond_idle);
				SDL_UnlockMutex(mutex);
			}
			for (int i = 0; i < SPIN_COUNT && rd == wr; i++) {
				cpu_relax();
				wr = atomic_load_explicit(&queue_wr, memory_order_acquire);
			}
			if (rd == wr) {
				SDL_LockMutex(mutex);
				atomic_store(&renderer_sleeping, true);
				while (rd == atomic_load(&queue_wr) && !atomic_load(&quit)) {
					SDL_CondWait(cond_work, mutex);
				}
				atomic_store(&renderer_sleeping, false);
				SDL_UnlockMutex(mutex);
				if (rd == atomic_load(&queue_wr)) {
					return 0; // quit
				}
				continue;
			}
		}
		while (rd != wr) {
			execute(queue[rd & QUEUE_MASK]);
			rd++;
			atomic_store_explicit(&queue_rd, rd, memory_order_release);
		}
		atomic_store(&queue_rd, rd);
	}
}

static void
wake_renderer(void)
{
	atomic_thread_fence(memory_order_seq_cst);
	if (atomic_load(&renderer_sleeping)) {
		SDL_LockMutex(mutex);
		SDL_CondSignal(cond_work);
		SDL_UnlockMutex(mutex);
	}
}

// Wait until the renderer has executed every queued command.
static void
sync_renderer(void)
{
	if (!threaded) {
		return;
	}
	const uint32_t wr = queue_wr_local;
	if (atomic_load_explicit(&queue_rd, memory_order_acquire) == wr) {
		return;
	}
	wake_renderer();
	for (int i = 0; i < SPIN_COUNT; i++) {
		if (atomic_load_explicit(&queue_rd, memory_order_acquire) == wr) {
			return;
		}
		cpu_relax();
	}
	SDL_LockMutex(mutex);
	atomic_store(&producer_waiting, true);
	while (atomic_load(&queue_rd) != wr) {
		SDL_CondWait(cond_idle, mutex);
	}
	atomic_store(&producer_waiting, false);
	SDL_UnlockMutex(mutex);
}

static inline void
push(uint32_t cmd)
{
	if (!threaded) {
		execute(cmd);
		return;
	}
	if (queue_wr_local - atomic_load_explicit(&queue_rd, memory_order_acquire) >= QUEUE_SIZE) {
		sync_renderer(); // queue full
	}
	queue[queue_wr_local & QUEUE_MASK] = cmd;
	queue_wr_local++;
	atomic_store_explicit(&queue_wr, queue_wr_local, memory_order_release);
}

//
// API
//

void
vr_init(bool use_thread)
{
	if (!use_thread || threaded) {
		return;
	}
	queue = malloc(QUEUE_SIZE * sizeof(*queue));
	mutex = SDL_CreateMutex();
	cond_work = SDL_CreateCond();
	cond_idle = SDL_CreateCond();
	if (!queue || !mutex || !cond_work || !cond_idle) {
		goto fail;
	}
	atomic_store(&quit, false);
	thread = SDL_CreateThread(render_thread, "x16emu render", NULL);
	if (!thread) {
		goto fail;
	}
	threaded = true;
	return;

fail:
	fprintf(stderr, "Warning: could not start render thread, rendering on the main thread.\n");
	free(queue);
	queue = NULL;
}

void
vr_shutdown(void)
{
	if (!threaded) {
		return;
	}
	sync_renderer();
	SDL_LockMutex(mutex);
	atomic_store(&quit, true);
	SDL_CondSignal(cond_work);
	SDL_UnlockMutex(mutex);
	SDL_WaitThread(thread, NULL);
	threaded = false;
	SDL_DestroyCond(cond_idle);
	SDL_DestroyCond(cond_work);
	SDL_DestroyMutex(mutex);
	free(queue);
	queue = NULL;
}

void
vr_vram_write(uint32_t address, uint8_t value)
{
	push(CMD(CMD_VRAM, (address & 0x1ffff) << 8 | value));
}

void
vr_vram_side_effect(uint32_t address, uint8_t value)
{
	if (address >= ADDR_PALETTE_START && address < ADDR_SPRDATA_END) {
		push(CMD(CMD_SIDE_EFFECT, address << 8 | value));
	}
}

void
vr_layer_write(uint8_t layer, uint8_t reg, uint8_t value)
{
	push(CMD(CMD_LAYER, (layer & 1) << 11 | (reg & 7) << 8 | value));
}

void
vr_composer_write(uint8_t reg, uint8_t value, bool palette_dirty)
{
	push(CMD(CMD_COMPOSER, palette_dirty << 16 | reg << 8 | value));
}

void
vr_clear_framebuffer(void)
{
	push(CMD(CMD_CLEAR_FB, 0));
}

void
vr_render_line(uint16_t y, uint16_t s_pos_x, bool skip_layers)
{
	push(CMD(CMD_LINE, (uint32_t)skip_layers << 26 | (s_pos_x & 0x3ff) << 16 | y));
	if (threaded) {
		wake_renderer();
	}
}

void
vr_reset(const uint8_t *vram, const uint8_t *pal, const uint8_t *sprites, const uint8_t *layer_regs, const uint8_t *composer_regs)
{
	sync_renderer();
	memcpy(video_ram, vram, sizeof(video_ram));
	memcpy(palette, pal, sizeof(palette));
	memcpy(sprite_data, sprites, sizeof(sprite_data));
	memcpy(reg_layer, layer_regs, sizeof(reg_layer));
	memcpy(reg_composer, composer_regs, sizeof(reg_composer));
	refresh_palette();
	sprite_line_collisions = 0;
}

uint8_t
vr_take_collisions(void)
{
	sync_renderer();
	const uint8_t collisions = sprite_line_collisions;
	sprite_line_collisions = 0;
	return collisions;
}

uint8_t *
vr_framebuffer(void)
{
	sync_renderer();
	return framebuffer;
}
