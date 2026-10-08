/*
 * A stand-in for libGL.so.1 (desktop GL/GLX), which Debian's GStreamer GL library links for its X11
 * backend. The device has no X11 and no desktop GL: GStreamer uses EGL + GLES (the PowerVR
 * drivers), so these are never called. Shipping glvnd's real libGL would put its gl* dispatch
 * stubs into the process next to the PowerVR GLES library. Built by package.sh.
 */
#include <stddef.h>

#define STUB(name) void *name(void) { return NULL; }
STUB(glXChooseFBConfig)
STUB(glXChooseVisual)
STUB(glXCreateContext)
STUB(glXDestroyContext)
STUB(glXGetCurrentContext)
STUB(glXGetFBConfigAttrib)
STUB(glXGetFBConfigs)
STUB(glXGetProcAddressARB)
STUB(glXGetVisualFromFBConfig)
STUB(glXMakeCurrent)
STUB(glXQueryContext)
STUB(glXQueryExtension)
STUB(glXQueryExtensionsString)
STUB(glXQueryVersion)
STUB(glXSwapBuffers)
