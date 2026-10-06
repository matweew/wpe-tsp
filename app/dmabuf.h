/*
 * Zero-copy page frames: the WebProcess renders into DMA-bufs (ION, see WebKit patch 0008)
 * and we show them as SDL textures bound to EGLImages of those buffers. No pixel copies.
 */
#pragma once

#include <SDL.h>
#include <wpe/wpe-platform.h>

/* The SDL texture showing a DMA-buf frame, created on first use and cached on the buffer.
 * NULL if the buffer can't be imported (the caller then has nothing to show). */
SDL_Texture *dmabuf_texture(SDL_Renderer *renderer, WPEBuffer *buffer);

/* The renderer is about to be destroyed: forget every cached texture/EGLImage (SDL frees
 * the textures with the renderer). Buffers are imported again for the next renderer. */
void dmabuf_renderer_lost(void);

/* Wait until the GPU has finished drawing what was presented, so a buffer handed back to
 * WebKit isn't overwritten while it's still being read. */
void dmabuf_wait_presented(void);
