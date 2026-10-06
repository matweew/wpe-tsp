/*
 * Zero-copy page frames (see dmabuf.h).
 *
 * Each WebKit swap-chain buffer is a DMA-buf. We import it once into the SDL renderer's
 * GLES context as an EGLImage, and bind it as the storage of an SDL texture, so drawing a
 * frame is a plain SDL_RenderCopy of that texture. The PowerVR driver resolves EGL/GL
 * extension functions via eglGetProcAddress; core EGL functions come from libEGL itself.
 */
#include "dmabuf.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <stdio.h>

#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

static struct {
    gboolean loaded, usable;
    EGLDisplay display;
    guint generation; /* bumped whenever the renderer (and its GL context) goes away */
    EGLDisplay (*get_current_display)(void);
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLCLIENTWAITSYNCKHRPROC client_wait_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
} egl;

typedef struct {
    SDL_Texture *texture;
    EGLImageKHR image;
    guint generation;
} Frame;

static gboolean load(void)
{
    if (egl.loaded)
        return egl.usable;
    egl.loaded = TRUE;
    void *lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        fprintf(stderr, "[wpe-tsp] dmabuf: %s\n", dlerror());
        return FALSE;
    }
    egl.get_current_display = (EGLDisplay (*)(void))dlsym(lib, "eglGetCurrentDisplay");
    egl.create_image = (PFNEGLCREATEIMAGEKHRPROC)SDL_GL_GetProcAddress("eglCreateImageKHR");
    egl.destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)SDL_GL_GetProcAddress("eglDestroyImageKHR");
    egl.create_sync = (PFNEGLCREATESYNCKHRPROC)SDL_GL_GetProcAddress("eglCreateSyncKHR");
    egl.client_wait_sync = (PFNEGLCLIENTWAITSYNCKHRPROC)SDL_GL_GetProcAddress("eglClientWaitSyncKHR");
    egl.destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)SDL_GL_GetProcAddress("eglDestroySyncKHR");
    egl.image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)SDL_GL_GetProcAddress("glEGLImageTargetTexture2DOES");
    egl.usable = egl.get_current_display && egl.create_image && egl.destroy_image && egl.image_target_texture;
    if (!egl.usable)
        fprintf(stderr, "[wpe-tsp] dmabuf: EGL image functions missing\n");
    return egl.usable;
}

static void frame_free(gpointer data)
{
    Frame *frame = data;
    /* Textures and images of a previous renderer are already gone with it */
    if (frame->generation == egl.generation) {
        SDL_DestroyTexture(frame->texture);
        egl.destroy_image(egl.display, frame->image);
    }
    g_free(frame);
}

static Frame *import(SDL_Renderer *renderer, WPEBufferDMABuf *buffer)
{
    int width = wpe_buffer_get_width(WPE_BUFFER(buffer));
    int height = wpe_buffer_get_height(WPE_BUFFER(buffer));
    /* The buffer is ARGB8888 (BGRA bytes); the GL sampler returns proper RGBA for it, so the
     * texture must use SDL's unswizzled GLES format (ABGR8888 = RGBA bytes). */
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture)
        return NULL;

    SDL_RenderFlush(renderer);
    if (SDL_GL_BindTexture(texture, NULL, NULL) < 0) {
        fprintf(stderr, "[wpe-tsp] dmabuf: SDL_GL_BindTexture: %s\n", SDL_GetError());
        SDL_DestroyTexture(texture);
        return NULL;
    }
    egl.display = egl.get_current_display();
    EGLint attributes[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)wpe_buffer_dma_buf_get_format(buffer),
        EGL_DMA_BUF_PLANE0_FD_EXT, wpe_buffer_dma_buf_get_fd(buffer, 0),
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)wpe_buffer_dma_buf_get_offset(buffer, 0),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)wpe_buffer_dma_buf_get_stride(buffer, 0),
        EGL_NONE
    };
    EGLImageKHR image = egl.create_image(egl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
    if (image != EGL_NO_IMAGE_KHR)
        egl.image_target_texture(GL_TEXTURE_2D, image);
    SDL_GL_UnbindTexture(texture);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "[wpe-tsp] dmabuf: eglCreateImageKHR failed (%dx%d)\n", width, height);
        SDL_DestroyTexture(texture);
        return NULL;
    }

    Frame *frame = g_new0(Frame, 1);
    frame->texture = texture;
    frame->image = image;
    frame->generation = egl.generation;
    return frame;
}

SDL_Texture *dmabuf_texture(SDL_Renderer *renderer, WPEBuffer *buffer)
{
    if (!WPE_IS_BUFFER_DMA_BUF(buffer) || wpe_buffer_dma_buf_get_n_planes(WPE_BUFFER_DMA_BUF(buffer)) != 1
        || wpe_buffer_dma_buf_get_modifier(WPE_BUFFER_DMA_BUF(buffer)) != DRM_FORMAT_MOD_INVALID || !load())
        return NULL;
    Frame *frame = g_object_get_data(G_OBJECT(buffer), "wpe-tsp-frame");
    if (!frame || frame->generation != egl.generation) {
        frame = import(renderer, WPE_BUFFER_DMA_BUF(buffer));
        /* Replaces (and frees) a stale frame of a previous renderer */
        g_object_set_data_full(G_OBJECT(buffer), "wpe-tsp-frame", frame, frame ? frame_free : NULL);
    }
    return frame ? frame->texture : NULL;
}

void dmabuf_renderer_lost(void)
{
    egl.generation++;
}

void dmabuf_wait_presented(void)
{
    if (!egl.usable || !egl.create_sync || !egl.client_wait_sync || !egl.destroy_sync)
        return;
    EGLSyncKHR sync = egl.create_sync(egl.display, EGL_SYNC_FENCE_KHR, NULL);
    if (sync == EGL_NO_SYNC_KHR)
        return;
    egl.client_wait_sync(egl.display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 100 * 1000 * 1000);
    egl.destroy_sync(egl.display, sync);
}
