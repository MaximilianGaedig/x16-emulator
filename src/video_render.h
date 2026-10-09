// Commander X16 Emulator
// All rights reserved. License: 2-clause BSD

// VERA scanline renderer.
//
// The renderer keeps its own copy of everything the scanline renderer
// reads (VRAM, palette, sprite attributes, layer and composer registers).
// video.c forwards every change to that state and every "render this
// line" request through an ordered command queue. With the render thread
// enabled, the queue is drained on a separate thread while the CPU keeps
// running; without it, every command is executed immediately. Both modes
// execute the same commands in the same order and produce identical output.

#ifndef _VIDEO_RENDER_H_
#define _VIDEO_RENDER_H_

#include <stdbool.h>
#include <stdint.h>

void vr_init(bool threaded);
void vr_shutdown(void);

// state changes
void vr_vram_write(uint32_t address, uint8_t value);   // plain VRAM byte, address < 0x20000
void vr_vram_side_effect(uint32_t address, uint8_t value); // palette/sprite attribute update for a VRAM space write
void vr_layer_write(uint8_t layer, uint8_t reg, uint8_t value);
void vr_composer_write(uint8_t reg, uint8_t value, bool palette_dirty);
void vr_clear_framebuffer(void);
void vr_reset(const uint8_t *video_ram, const uint8_t *palette, const uint8_t *sprite_data, const uint8_t *reg_layer, const uint8_t *reg_composer);

// rendering
void vr_render_line(uint16_t y, uint16_t s_pos_x, bool skip_layers);

// Wait for the renderer to catch up, then return the sprite collisions
// accumulated since the last call and clear them.
uint8_t vr_take_collisions(void);

// Wait for the renderer to catch up and return the framebuffer (BGRA, 640x480).
// It stays stable until the next vr_render_line()/vr_clear_framebuffer().
uint8_t *vr_framebuffer(void);

#endif
