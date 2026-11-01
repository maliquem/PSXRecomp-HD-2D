/* sgl.h — the GL 3.3 core subset the GL renderer uses, on SDL3's GPU API.
 *
 * [video] renderer = "sdl_gpu" (docs/SDL_GPU_RENDERER_PLAN.md). gl_api.h
 * routes every GL call of gpu_gl_renderer.c here instead of the driver's
 * OpenGL: the same renderer, the same GLSL, drawn by Vulkan / Direct3D 12 /
 * Metal. Calls are recorded and encoded on SDL_GPU in batches (a draw is a
 * copy into a list; the list becomes passes when the picture is swapped or
 * read back).
 *
 * The conventions of GL are kept in memory: row 0 of every surface is GL's
 * bottom row, so viewports, scissors, texture coordinates, gl_FragCoord and
 * readbacks use the GL numbers unchanged; only the swap to the window flips.
 *
 * Only one thread uses the layer at a time (the one that made the context
 * current), as with one GL context. */
#ifndef PSX_SGL_H
#define PSX_SGL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

/* GL entry points by name (glDisable, glUniform4f, ...); NULL for the ones
 * the layer does not offer (the timer queries: the renderer then runs
 * without GPU timing, as on a GL without ARB_timer_query). */
void *sgl_get_proc(const char *name);

/* The "context": the SDL_GPU device on the window. A second context (the GL
 * present thread's shared one) is refused: the renderer then presents from
 * its own thread. Returns an opaque non-NULL token. */
void *sgl_create_context(struct SDL_Window *win);
bool  sgl_make_current(struct SDL_Window *win, void *ctx);
void  sgl_delete_context(void *ctx);
bool  sgl_swap_window(struct SDL_Window *win);
/* 1 = vsync, 0 = immediate (mailbox when available), -1 = adaptive (vsync). */
bool  sgl_set_swap_interval(int interval);
bool  sgl_get_swap_interval(int *interval);

/* What the GL subset cannot do here (gl_api.h reports them to the renderer). */
typedef struct SglCaps {
    int dual_source_blend;   /* GL_SRC1_* blend factors: 0 */
    int timer_queries;       /* glQueryCounter / GL_TIME_ELAPSED: 0 */
    int wide_lines;          /* glLineWidth > 1: 0 */
    int max_texture_size;
} SglCaps;
const SglCaps *sgl_caps(void);

/* HDR for the next context: a float16 default framebuffer, shown as scRGB
 * (HDR_EXTENDED_LINEAR) where the window supports it (gl_hdr.inc), else as
 * HDR10 (ST 2084 PQ, BT.2020: what gamescope / the Steam Deck take).
 * PSX_HDR_FORMAT=hdr10 prefers HDR10. On Windows the output follows the
 * display live: HDR while Windows shows the window in HDR, SDR otherwise. */
void sgl_set_hdr(int on);
/* The output now: 0 SDR, 1 scRGB, 2 HDR10 (gl_hdr.inc runs its pass when
 * not 0; the swap encodes HDR10 itself). */
int sgl_hdr_output(void);

/* "sdl_gpu (vulkan)" etc.; "sdl_gpu" before a context exists. */
const char *sgl_impl_name(void);

/* Window flags the window needs for the driver sgl will use
 * (SDL_WINDOW_VULKAN / SDL_WINDOW_METAL / 0). [video] sdl_gpu_driver or
 * PSX_SGL_DRIVER picks the driver ("vulkan", "direct3d12", "metal"; empty =
 * the platform's default). */
void        sgl_set_driver(const char *name);
uint64_t    sgl_window_flags(void);

/* Counters for the debug server (TCP "sgl"): passes, draws, uploads... */
int sgl_stats_json(char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SGL_H */
