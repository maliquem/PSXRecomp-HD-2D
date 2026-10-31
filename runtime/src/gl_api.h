/* gl_api.h — the OpenGL the GL renderer calls, through one table.
 *
 * Included by gpu_gl_renderer.c (and so by its .inc files) right after the GL
 * headers. Every GL entry point the renderer uses goes through a pointer:
 * the modern ones already did (p_gl*, loaded by load_modern_gl), the GL 1.1
 * ones here. The SDL_GL_* context calls go through small wrappers.
 *
 * Two implementations behind the table ([video] renderer):
 *   - "opengl" (default): the driver's OpenGL. The pointers are the real
 *     functions and the wrappers call SDL_GL_* — the same calls as before, one
 *     indirection more.
 *   - "sdl_gpu": the sgl layer (runtime/src/sgl/), the same GL 3.3 core subset
 *     recorded and encoded on SDL3's GPU API (Vulkan / Direct3D 12 / Metal).
 *     docs/SDL_GPU_RENDERER_PLAN.md.
 *
 * The choice is made once, before the context is created
 * (gl_renderer_set_impl); PSX_GL_IMPL=sgl|opengl overrides it (tests). */
#ifndef PSX_GL_API_H
#define PSX_GL_API_H

#if defined(PSX_HAVE_SGL)
#include "sgl/sgl.h"
#endif

/* The GL 1.1 functions the renderer calls (exported by the GL library). */
#define PSX_GL11_FUNCS(X) \
    X(glDisable) X(glEnable) X(glIsEnabled) X(glBindTexture) X(glTexParameteri) \
    X(glGetTexParameteriv) X(glGetTexLevelParameteriv) X(glGetIntegerv) \
    X(glGetBooleanv) X(glGetFloatv) X(glGetString) X(glGetError) X(glViewport) \
    X(glScissor) X(glDrawArrays) X(glPixelStorei) X(glGenTextures) \
    X(glDeleteTextures) X(glTexImage2D) X(glTexSubImage2D) X(glCopyTexImage2D) \
    X(glCopyTexSubImage2D) X(glGetTexImage) X(glReadPixels) X(glReadBuffer) \
    X(glDrawBuffer) X(glClear) X(glClearColor) X(glClearDepth) X(glClearStencil) \
    X(glColorMask) X(glDepthMask) X(glDepthFunc) X(glStencilMask) \
    X(glStencilFunc) X(glStencilOp) X(glBlendFunc) X(glLineWidth) X(glFinish) \
    X(glFlush)

#define PSX_GL_DECL_PTR(f) static __typeof__(&f) p_##f;
PSX_GL11_FUNCS(PSX_GL_DECL_PTR)
#undef PSX_GL_DECL_PTR

/* 0 = the driver's OpenGL, 1 = sgl. */
static int s_gl_impl = -1;

static int gl_api_impl(void) {
    if (s_gl_impl < 0) {
        int impl = 0;
#if defined(PSX_HAVE_SGL)
        const char *e = getenv("PSX_GL_IMPL");
        if (e && (strcmp(e, "sgl") == 0 || strcmp(e, "sdl_gpu") == 0)) impl = 1;
#endif
        s_gl_impl = impl;
    }
    return s_gl_impl;
}

static void *gl_api_get_proc(const char *name) {
#if defined(PSX_HAVE_SGL)
    if (gl_api_impl() == 1) return sgl_get_proc(name);
#endif
    return (void *)SDL_GL_GetProcAddress(name);
}

static void gl_api_load_gl11(void) {
#if defined(PSX_HAVE_SGL)
    if (gl_api_impl() == 1) {
#define PSX_GL_LOAD_SGL(f) p_##f = (__typeof__(&f))sgl_get_proc(#f);
        PSX_GL11_FUNCS(PSX_GL_LOAD_SGL)
#undef PSX_GL_LOAD_SGL
        return;
    }
#endif
#define PSX_GL_LOAD_NATIVE(f) p_##f = f;
    PSX_GL11_FUNCS(PSX_GL_LOAD_NATIVE)
#undef PSX_GL_LOAD_NATIVE
}

/* Context wrappers: SDL3 return conventions as the renderer used them
 * (MakeCurrent / SetSwapInterval through psx_sdl.h: 0 = success). */
static SDL_GLContext gl_api_create_context(SDL_Window *w) {
    SDL_GLContext c;
#if defined(PSX_HAVE_SGL)
    if (gl_api_impl() == 1) c = (SDL_GLContext)sgl_create_context(w);
    else
#endif
    c = SDL_GL_CreateContext(w);
    if (c) gl_api_load_gl11();
    return c;
}
static int gl_api_make_current(SDL_Window *w, SDL_GLContext c) {
#if defined(PSX_HAVE_SGL)
    if (gl_api_impl() == 1) return sgl_make_current(w, (void *)c) ? 0 : -1;
#endif
    return SDL_GL_MakeCurrent(w, c);
}
static void gl_api_delete_context(SDL_GLContext c) {
#if defined(PSX_HAVE_SGL)
    if (gl_api_impl() == 1) { sgl_delete_context((void *)c); return; }
#endif
    SDL_GL_DeleteContext(c);
}
static bool gl_api_swap_window(SDL_Window *w) {
#if defined(PSX_HAVE_SGL)
    if (gl_api_impl() == 1) return sgl_swap_window(w);
#endif
#if defined(PSX_SDL3)
    return SDL_GL_SwapWindow(w);
#else
    SDL_GL_SwapWindow(w);
    return true;
#endif
}
static int gl_api_set_swap_interval(int interval) {
#if defined(PSX_HAVE_SGL)
    if (gl_api_impl() == 1) return sgl_set_swap_interval(interval) ? 0 : -1;
#endif
    return SDL_GL_SetSwapInterval(interval);
}
#if defined(PSX_SDL3)
static bool gl_api_get_swap_interval(int *interval) {
#if defined(PSX_HAVE_SGL)
    if (gl_api_impl() == 1) return sgl_get_swap_interval(interval);
#endif
    return SDL_GL_GetSwapInterval(interval);
}
#else
static int gl_api_get_swap_interval(void) { return SDL_GL_GetSwapInterval(); }
#endif

/* From here on the renderer's calls land on the table. */
#define glDisable(...)                p_glDisable(__VA_ARGS__)
#define glEnable(...)                 p_glEnable(__VA_ARGS__)
#define glIsEnabled(...)              p_glIsEnabled(__VA_ARGS__)
#define glBindTexture(...)            p_glBindTexture(__VA_ARGS__)
#define glTexParameteri(...)          p_glTexParameteri(__VA_ARGS__)
#define glGetTexParameteriv(...)      p_glGetTexParameteriv(__VA_ARGS__)
#define glGetTexLevelParameteriv(...) p_glGetTexLevelParameteriv(__VA_ARGS__)
#define glGetIntegerv(...)            p_glGetIntegerv(__VA_ARGS__)
#define glGetBooleanv(...)            p_glGetBooleanv(__VA_ARGS__)
#define glGetFloatv(...)              p_glGetFloatv(__VA_ARGS__)
#define glGetString(...)              p_glGetString(__VA_ARGS__)
#define glGetError()                  p_glGetError()
#define glViewport(...)               p_glViewport(__VA_ARGS__)
#define glScissor(...)                p_glScissor(__VA_ARGS__)
#define glDrawArrays(...)             p_glDrawArrays(__VA_ARGS__)
#define glPixelStorei(...)            p_glPixelStorei(__VA_ARGS__)
#define glGenTextures(...)            p_glGenTextures(__VA_ARGS__)
#define glDeleteTextures(...)         p_glDeleteTextures(__VA_ARGS__)
#define glTexImage2D(...)             p_glTexImage2D(__VA_ARGS__)
#define glTexSubImage2D(...)          p_glTexSubImage2D(__VA_ARGS__)
#define glCopyTexImage2D(...)         p_glCopyTexImage2D(__VA_ARGS__)
#define glCopyTexSubImage2D(...)      p_glCopyTexSubImage2D(__VA_ARGS__)
#define glGetTexImage(...)            p_glGetTexImage(__VA_ARGS__)
#define glReadPixels(...)             p_glReadPixels(__VA_ARGS__)
#define glReadBuffer(...)             p_glReadBuffer(__VA_ARGS__)
#define glDrawBuffer(...)             p_glDrawBuffer(__VA_ARGS__)
#define glClear(...)                  p_glClear(__VA_ARGS__)
#define glClearColor(...)             p_glClearColor(__VA_ARGS__)
#define glClearDepth(...)             p_glClearDepth(__VA_ARGS__)
#define glClearStencil(...)           p_glClearStencil(__VA_ARGS__)
#define glColorMask(...)              p_glColorMask(__VA_ARGS__)
#define glDepthMask(...)              p_glDepthMask(__VA_ARGS__)
#define glDepthFunc(...)              p_glDepthFunc(__VA_ARGS__)
#define glStencilMask(...)            p_glStencilMask(__VA_ARGS__)
#define glStencilFunc(...)            p_glStencilFunc(__VA_ARGS__)
#define glStencilOp(...)              p_glStencilOp(__VA_ARGS__)
#define glBlendFunc(...)              p_glBlendFunc(__VA_ARGS__)
#define glLineWidth(...)              p_glLineWidth(__VA_ARGS__)
#define glFinish()                    p_glFinish()
#define glFlush()                     p_glFlush()

#undef SDL_GL_GetProcAddress
#define SDL_GL_GetProcAddress(n)      gl_api_get_proc(n)
#undef SDL_GL_CreateContext
#define SDL_GL_CreateContext(w)       gl_api_create_context(w)
#undef SDL_GL_MakeCurrent
#define SDL_GL_MakeCurrent(w, c)      gl_api_make_current((w), (c))
#undef SDL_GL_DeleteContext
#define SDL_GL_DeleteContext(c)       gl_api_delete_context(c)
#undef SDL_GL_SwapWindow
#define SDL_GL_SwapWindow(w)          gl_api_swap_window(w)
#undef SDL_GL_SetSwapInterval
#define SDL_GL_SetSwapInterval(i)     gl_api_set_swap_interval(i)
#undef SDL_GL_GetSwapInterval
#if defined(PSX_SDL3)
#define SDL_GL_GetSwapInterval(p)     gl_api_get_swap_interval(p)
#else
#define SDL_GL_GetSwapInterval()      gl_api_get_swap_interval()
#endif

#endif /* PSX_GL_API_H */
