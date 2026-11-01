/* sgl_gl.c — the GL entry points of the sgl layer: objects, state, recording.
 *
 * Every function here has the signature of the GL function it stands for
 * (gl_api.h takes their addresses through sgl_get_proc). Drawing, clears,
 * uploads, copies and readbacks become ops of the stream (sgl_internal.h);
 * sgl_gpu.c encodes them on SDL_GPU. Only the GL the renderer uses is here
 * (docs/SDL_GPU_RENDERER_PLAN.md, 1.2); anything else is a no-op that sets
 * GL_INVALID_ENUM. */
#include "sgl_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SglState  g_sgls;
SglStream g_sgl_stream;

/* ---- name tables -------------------------------------------------------- */
typedef struct { void *items; int n, cap; size_t sz; } SglTable;
static SglTable s_tex = {0, 0, 0, sizeof(SglImage)};
static SglTable s_rb  = {0, 0, 0, sizeof(SglImage)};
static SglTable s_fbo = {0, 0, 0, sizeof(SglFbo)};
static SglTable s_buf = {0, 0, 0, sizeof(SglBuffer)};
static SglTable s_vao = {0, 0, 0, sizeof(SglVao)};
static SglTable s_obj = {0, 0, 0, sizeof(SglObj)};
static SglTable s_sync = {0, 0, 0, sizeof(SglSync)};
/* A timer query: the GPU busy clock (sgl_gpu_clock_at) at the streams it
 * was placed after (TIME_ELAPSED: begin and end; TIMESTAMP: one). */
typedef struct { int used; GLenum target; uint64_t begin, end; int placed; } SglQuery;
static SglTable s_query = {0, 0, 0, sizeof(SglQuery)};

static void *tab_get(SglTable *t, GLuint name) {
    if (name == 0 || (int)name >= t->n) return NULL;
    char *p = (char *)t->items + (size_t)name * t->sz;
    return *(int *)p ? p : NULL;      /* every item starts with "int used" */
}
static GLuint tab_new(SglTable *t) {
    if (t->n == 0) t->n = 1;          /* name 0 is never handed out */
    for (int i = 1; i < t->n; i++) {
        char *p = (char *)t->items + (size_t)i * t->sz;
        if (!*(int *)p) { memset(p, 0, t->sz); *(int *)p = 1; return (GLuint)i; }
    }
    if (t->n >= t->cap) {
        int nc = t->cap ? t->cap * 2 : 64;
        void *ni = realloc(t->items, (size_t)nc * t->sz);
        if (!ni) return 0;
        memset((char *)ni + (size_t)t->cap * t->sz, 0, (size_t)(nc - t->cap) * t->sz);
        t->items = ni; t->cap = nc;
    }
    char *p = (char *)t->items + (size_t)t->n * t->sz;
    memset(p, 0, t->sz); *(int *)p = 1;
    return (GLuint)t->n++;
}

SglImage *sgl_image(GLuint name, int rb) { return (SglImage *)tab_get(rb ? &s_rb : &s_tex, name); }
SglObj   *sgl_obj(GLuint name) { return (SglObj *)tab_get(&s_obj, name); }
static SglFbo    *fbo_get(GLuint n) { return (SglFbo *)tab_get(&s_fbo, n); }
static SglBuffer *buf_get(GLuint n) { return (SglBuffer *)tab_get(&s_buf, n); }
static SglVao    *vao_get(GLuint n) { return (SglVao *)tab_get(&s_vao, n); }

void sgl_set_error(GLenum e) { if (g_sgls.error == GL_NO_ERROR) g_sgls.error = e; }

/* Set a state field that goes into the pipeline (sgl_glDrawArrays keys its
 * pipeline memory by these fields; pipe_epoch only follows the VAO layout). */
#define PSET(field, v) do { (field) = (v); } while (0)

/* ---- half floats -------------------------------------------------------- */
uint16_t sgl_f2h(float f) {
    union { float f; uint32_t u; } v = { f };
    uint32_t x = v.u, sign = (x >> 16) & 0x8000u;
    int32_t e = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00u | (m ? 0x200u : 0));
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;
        m |= 0x800000u;
        uint32_t t = 14 - e;
        uint32_t h = m >> t, rem = m & ((1u << t) - 1), half = 1u << (t - 1);
        if (rem > half || (rem == half && (h & 1))) h++;
        return (uint16_t)(sign | h);
    }
    uint32_t h = sign | ((uint32_t)e << 10) | (m >> 13);
    uint32_t rem = m & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) h++;
    return (uint16_t)h;
}
float sgl_h2f(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, u;
    if (e == 0) {
        if (m == 0) u = sign;
        else { e = 127 - 15 + 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3FF; u = sign | (e << 23) | (m << 13); }
    } else if (e == 31) u = sign | 0x7F800000u | (m << 13);
    else u = sign | ((e - 15 + 127) << 23) | (m << 13);
    union { uint32_t u; float f; } v = { u };
    return v.f;
}

/* ---- pixel transfer ----------------------------------------------------- */
static int gl_px_size(GLenum format, GLenum type) {
    int comps;
    switch (format) {
    case GL_RED: case 0x8D94 /* RED_INTEGER */: case GL_DEPTH_COMPONENT: case GL_ALPHA: comps = 1; break;
    case 0x8227 /* RG */: case 0x8228 /* RG_INTEGER */: comps = 2; break;
    case GL_RGB: case 0x80E0 /* BGR */: comps = 3; break;
    default: comps = 4; break;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return comps;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case 0x140B /* HALF_FLOAT */: return comps * 2;
    case 0x8367 /* UNSIGNED_INT_8_8_8_8_REV */: case 0x8035 /* 8_8_8_8 */: return 4;
    case 0x8366 /* UNSIGNED_SHORT_1_5_5_5_REV */: case 0x8034: return 2;
    default: return comps * 4;   /* FLOAT, INT, UNSIGNED_INT */
    }
}

static inline float u8f(uint8_t v) { return (float)v / 255.0f; }
static inline uint8_t fu8(float f) {
    if (!(f > 0.0f)) return 0;
    if (f >= 1.0f) return 255;
    return (uint8_t)lrintf(f * 255.0f);
}

/* One row of GL client pixels (format/type) -> the GPU layout of im. */
static void row_to_gpu(const SglImage *im, uint8_t *dst, const uint8_t *src, int w,
                       GLenum format, GLenum type) {
    switch (im->fmt) {
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
        if (type == GL_FLOAT) {
            const float *f = (const float *)src;
            int c = format == GL_RGB ? 3 : 4;
            for (int i = 0; i < w; i++) {
                dst[4*i+0] = fu8(f[c*i+0]); dst[4*i+1] = fu8(f[c*i+1]);
                dst[4*i+2] = fu8(f[c*i+2]); dst[4*i+3] = c == 4 ? fu8(f[c*i+3]) : 255;
            }
        } else if (format == 0x80E1 /* BGRA */) {
            for (int i = 0; i < w; i++) {
                dst[4*i+0] = src[4*i+2]; dst[4*i+1] = src[4*i+1];
                dst[4*i+2] = src[4*i+0]; dst[4*i+3] = src[4*i+3];
            }
        } else if (format == GL_RGB) {
            for (int i = 0; i < w; i++) {
                dst[4*i+0] = src[3*i+0]; dst[4*i+1] = src[3*i+1];
                dst[4*i+2] = src[3*i+2]; dst[4*i+3] = 255;
            }
        } else if (format == 0x80E0 /* BGR */) {
            for (int i = 0; i < w; i++) {
                dst[4*i+0] = src[3*i+2]; dst[4*i+1] = src[3*i+1];
                dst[4*i+2] = src[3*i+0]; dst[4*i+3] = 255;
            }
        } else {
            memcpy(dst, src, (size_t)w * 4);
        }
        break;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT: {
        uint16_t *h = (uint16_t *)dst;
        if (type == GL_FLOAT) {
            const float *f = (const float *)src;
            int c = format == GL_RGB ? 3 : 4;
            for (int i = 0; i < w; i++) {
                h[4*i+0] = sgl_f2h(f[c*i+0]); h[4*i+1] = sgl_f2h(f[c*i+1]);
                h[4*i+2] = sgl_f2h(f[c*i+2]); h[4*i+3] = sgl_f2h(c == 4 ? f[c*i+3] : 1.0f);
            }
        } else if (type == 0x140B) {
            memcpy(dst, src, (size_t)w * 8);
        } else {
            int bgra = format == 0x80E1, c = (format == GL_RGB) ? 3 : 4;
            for (int i = 0; i < w; i++) {
                const uint8_t *s = src + (size_t)c * i;
                h[4*i+0] = sgl_f2h(u8f(s[bgra ? 2 : 0])); h[4*i+1] = sgl_f2h(u8f(s[1]));
                h[4*i+2] = sgl_f2h(u8f(s[bgra ? 0 : 2])); h[4*i+3] = sgl_f2h(c == 4 ? u8f(s[3]) : 1.0f);
            }
        }
        break;
    }
    case SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT:
    case SDL_GPU_TEXTUREFORMAT_R32_FLOAT:
    case SDL_GPU_TEXTUREFORMAT_R16_UINT:
    case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
    default:
        memcpy(dst, src, (size_t)w * (size_t)im->bpp);
        break;
    }
}

/* One row of the GPU layout of fmt -> GL client pixels. */
static void row_from_gpu(SDL_GPUTextureFormat fmt, uint8_t *dst, const uint8_t *src, int w,
                         GLenum format, GLenum type) {
    if (fmt == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ||
        fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) {
        const int sb = fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        for (int i = 0; i < w; i++) {
            const uint8_t *s = src + 4 * i;
            uint8_t r = s[sb ? 2 : 0], g = s[1], b = s[sb ? 0 : 2], a = s[3];
            if (type == GL_FLOAT) {
                float *f = (float *)dst;
                int c = format == GL_RGB ? 3 : 4;
                f[c*i+0] = u8f(r); f[c*i+1] = u8f(g); f[c*i+2] = u8f(b);
                if (c == 4) f[c*i+3] = u8f(a);
            } else if (format == 0x80E1) {
                dst[4*i+0] = b; dst[4*i+1] = g; dst[4*i+2] = r; dst[4*i+3] = a;
            } else if (format == GL_RGB) {
                dst[3*i+0] = r; dst[3*i+1] = g; dst[3*i+2] = b;
            } else if (format == 0x80E0) {
                dst[3*i+0] = b; dst[3*i+1] = g; dst[3*i+2] = r;
            } else {
                dst[4*i+0] = r; dst[4*i+1] = g; dst[4*i+2] = b; dst[4*i+3] = a;
            }
        }
        return;
    }
    if (fmt == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) {
        const uint16_t *h = (const uint16_t *)src;
        for (int i = 0; i < w; i++) {
            float r = sgl_h2f(h[4*i+0]), g = sgl_h2f(h[4*i+1]), b = sgl_h2f(h[4*i+2]), a = sgl_h2f(h[4*i+3]);
            if (type == GL_FLOAT) {
                float *f = (float *)dst;
                int c = format == GL_RGB ? 3 : 4;
                f[c*i+0] = r; f[c*i+1] = g; f[c*i+2] = b;
                if (c == 4) f[c*i+3] = a;
            } else if (type == 0x140B) {
                memcpy(dst + 8 * (size_t)i, h + 4 * (size_t)i, 8);
            } else {
                int c = (format == GL_RGB || format == 0x80E0) ? 3 : 4;
                int sw = format == 0x80E1 || format == 0x80E0;
                dst[c*i+0] = fu8(sw ? b : r); dst[c*i+1] = fu8(g); dst[c*i+2] = fu8(sw ? r : b);
                if (c == 4) dst[c*i+3] = fu8(a);
            }
        }
        return;
    }
    int bpp = fmt == SDL_GPU_TEXTUREFORMAT_R16_UINT ? 2 :
              fmt == SDL_GPU_TEXTUREFORMAT_R8_UNORM ? 1 :
              fmt == SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT ? 8 :
              fmt == SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ? 16 : 4;
    if (fmt == SDL_GPU_TEXTUREFORMAT_R8_UNORM && type == GL_FLOAT) {
        float *f = (float *)dst;
        for (int i = 0; i < w; i++) f[i] = u8f(src[i]);
        return;
    }
    memcpy(dst, src, (size_t)w * (size_t)bpp);
}

static size_t gl_row_stride(int w, int bpp, int row_len, int align) {
    size_t row = (size_t)(row_len > 0 ? row_len : w) * (size_t)bpp;
    if (align < 1) align = 1;
    return (row + (size_t)align - 1) / (size_t)align * (size_t)align;
}

/* ---- framebuffers ------------------------------------------------------- */
static SglImage *fbo_color_image(GLuint fbo, int for_read) {
    if (fbo == 0) return g_sgl.back.tex ? &g_sgl.back : NULL;
    SglFbo *f = fbo_get(fbo);
    if (!f) return NULL;
    GLenum b = for_read ? f->read_buffer : f->draw_buffer;
    if (b == GL_NONE || !f->color) return NULL;
    SglImage *im = sgl_image(f->color, f->color_rb);
    return im && im->tex ? im : NULL;
}
static SglImage *fbo_ds_image(GLuint fbo) {
    if (fbo == 0) return g_sgl.back_ds.tex ? &g_sgl.back_ds : NULL;
    SglFbo *f = fbo_get(fbo);
    if (!f || !f->ds) return NULL;
    SglImage *im = sgl_image(f->ds, f->ds_rb);
    return im && im->tex ? im : NULL;
}

/* The draw framebuffer as a target. 0 when nothing can be drawn. */
static int resolve_target(SglTarget *t) {
    memset(t, 0, sizeof(*t));
    SglImage *c = fbo_color_image(g_sgls.draw_fbo, 0);
    SglImage *d = fbo_ds_image(g_sgls.draw_fbo);
    if (c) { t->color = c->tex; t->color_fmt = c->fmt; t->w = c->w; t->h = c->h; }
    if (d) {
        t->ds = d->tex; t->ds_fmt = d->fmt;
        t->ds_has_stencil = d->kind == SGL_KIND_DEPTH_STENCIL;
        if (!c || d->w < t->w) t->w = d->w;
        if (!c || d->h < t->h) t->h = d->h;
    }
    return t->color || t->ds;
}

/* Default framebuffer follows the window's pixel size (like GL's). */
static void back_follow_window(void) {
    int w = 0, h = 0;
    if (!g_sgl.win || !SDL_GetWindowSizeInPixels(g_sgl.win, &w, &h)) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (g_sgl.back.tex && g_sgl.back.w == w && g_sgl.back.h == h) return;
    GLenum f = g_sgl.hdr ? 0x881A /* RGBA16F */ : GL_RGBA8;
    g_sgl.back.used = 1;
    g_sgl.back_ds.used = 1;
    sgl_gpu_alloc_image(&g_sgl.back, f, w, h);
    sgl_gpu_alloc_image(&g_sgl.back_ds, 0x88F0 /* DEPTH24_STENCIL8 */, w, h);
}

static void clip_rect(int *x, int *y, int *w, int *h, int tw, int th) {
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = tw;
    if (y1 > th) y1 = th;
    *x = x0; *y = y0; *w = x1 > x0 ? x1 - x0 : 0; *h = y1 > y0 ? y1 - y0 : 0;
}

/* ---- textures ----------------------------------------------------------- */
static void APIENTRY sgl_glGenTextures(GLsizei n, GLuint *out) {
    for (int i = 0; i < n; i++) {
        out[i] = tab_new(&s_tex);
        SglImage *im = sgl_image(out[i], 0);
        if (im) {
            im->min_filter = GL_NEAREST_MIPMAP_LINEAR; im->mag_filter = GL_LINEAR;
            im->wrap_s = GL_REPEAT; im->wrap_t = GL_REPEAT;
        }
    }
}
static void image_free(SglImage *im) {
    if (im->tex) sgl_release_texture(im->tex);
    im->tex = NULL;
}
static void APIENTRY sgl_glDeleteTextures(GLsizei n, const GLuint *names) {
    for (int i = 0; i < n; i++) {
        SglImage *im = sgl_image(names[i], 0);
        if (!im) continue;
        image_free(im);
        im->used = 0;
        for (int u = 0; u < SGL_UNITS; u++)
            if (g_sgls.unit_tex[u] == names[i]) g_sgls.unit_tex[u] = 0;
    }
}
static void APIENTRY sgl_glBindTexture(GLenum target, GLuint name) {
    (void)target;
    if (name && (int)name < s_tex.n && !sgl_image(name, 0)) {
        /* GL lets a name made elsewhere be bound; ours come from glGenTextures. */
        sgl_set_error(GL_INVALID_OPERATION);
        return;
    }
    g_sgls.unit_tex[g_sgls.active_unit] = name;
}
static SglImage *bound_tex(void) { return sgl_image(g_sgls.unit_tex[g_sgls.active_unit], 0); }

static void APIENTRY sgl_glActiveTexture(GLenum unit) {
    int u = (int)unit - 0x84C0;
    if (u < 0 || u >= SGL_UNITS) { sgl_set_error(GL_INVALID_ENUM); return; }
    g_sgls.active_unit = u;
}
static void APIENTRY sgl_glTexParameteri(GLenum target, GLenum pname, GLint v) {
    (void)target;
    SglImage *im = bound_tex();
    if (!im) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: im->min_filter = (GLenum)v; break;
    case GL_TEXTURE_MAG_FILTER: im->mag_filter = (GLenum)v; break;
    case GL_TEXTURE_WRAP_S: im->wrap_s = (GLenum)v; break;
    case GL_TEXTURE_WRAP_T: im->wrap_t = (GLenum)v; break;
    default: break;   /* max level, LOD, swizzle ... : not used */
    }
}
static void APIENTRY sgl_glGetTexParameteriv(GLenum target, GLenum pname, GLint *v) {
    (void)target;
    SglImage *im = bound_tex();
    if (!im || !v) return;
    if (pname == GL_TEXTURE_MIN_FILTER) *v = (GLint)im->min_filter;
    else if (pname == GL_TEXTURE_MAG_FILTER) *v = (GLint)im->mag_filter;
    else if (pname == GL_TEXTURE_WRAP_S) *v = (GLint)im->wrap_s;
    else if (pname == GL_TEXTURE_WRAP_T) *v = (GLint)im->wrap_t;
}
static void APIENTRY sgl_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *v) {
    (void)target; (void)level;
    SglImage *im = bound_tex();
    if (!v) return;
    if (pname == GL_TEXTURE_WIDTH) *v = im ? im->w : 0;
    else if (pname == GL_TEXTURE_HEIGHT) *v = im ? im->h : 0;
    else if (pname == 0x1003 /* INTERNAL_FORMAT */) *v = im ? (GLint)im->ifmt : 0;
}

/* Record an upload of client (or unpack-buffer) pixels into im. */
static void upload(SglImage *im, int x, int y, int w, int h, GLenum format, GLenum type,
                   const void *pixels) {
    if (!im || !im->tex || w <= 0 || h <= 0) return;
    const uint8_t *base;
    SglBuffer *ub = buf_get(g_sgls.unpack_buffer);
    if (g_sgls.unpack_buffer) {
        if (!ub || !ub->data) return;
        base = ub->data + (size_t)(uintptr_t)pixels;
    } else {
        if (!pixels) return;
        base = (const uint8_t *)pixels;
    }
    if (im->kind == SGL_KIND_DEPTH || im->kind == SGL_KIND_DEPTH_STENCIL) return;
    int ox = x, oy = y, ow = w, oh = h;
    clip_rect(&x, &y, &w, &h, im->w, im->h);
    if (w <= 0 || h <= 0) return;
    const int spx = gl_px_size(format, type);
    const size_t sstride = gl_row_stride(ow, spx, g_sgls.unpack_row_len, g_sgls.unpack_align);
    const uint8_t *src = base + (size_t)g_sgls.unpack_skip_rows * sstride +
                         (size_t)g_sgls.unpack_skip_px * (size_t)spx +
                         (size_t)(y - oy) * sstride + (size_t)(x - ox) * (size_t)spx;
    uint32_t off;
    uint8_t *dst = (uint8_t *)sgl_arena_reserve(&g_sgl_stream.upl, (size_t)w * h * im->bpp, 16, &off);
    if (!dst) { sgl_set_error(GL_OUT_OF_MEMORY); return; }
    for (int r = 0; r < h; r++)
        row_to_gpu(im, dst + (size_t)r * w * im->bpp, src + (size_t)r * sstride, w, format, type);
    SglOp *op = sgl_push_op(SGL_OP_UPLOAD);
    if (!op) return;
    op->u.up.tex = im->tex; op->u.up.x = x; op->u.up.y = y; op->u.up.w = w; op->u.up.h = h;
    op->u.up.off = off;
    sgl_stats_note(SGL_ST_UPLOAD);
}

/* New storage for an image is cleared (GL drivers hand out zeroed memory in
 * practice; Vulkan memory is garbage). */
static void clear_new_image(SglImage *im) {
    SglOp *op = sgl_push_op(SGL_OP_CLEAR);
    if (!op) return;
    if (im->kind == SGL_KIND_DEPTH || im->kind == SGL_KIND_DEPTH_STENCIL) {
        op->tgt.ds = im->tex; op->tgt.ds_fmt = im->fmt;
        op->tgt.ds_has_stencil = im->kind == SGL_KIND_DEPTH_STENCIL;
        op->u.clear.depth = 1; op->u.clear.stencil = op->tgt.ds_has_stencil;
        op->u.clear.d = 1.0f;
    } else {
        op->tgt.color = im->tex; op->tgt.color_fmt = im->fmt;
        op->u.clear.color = 1;
    }
    op->tgt.w = im->w; op->tgt.h = im->h;
}

static int alloc_storage(SglImage *im, GLenum ifmt, int w, int h) {
    if (im->tex && im->ifmt == ifmt && im->w == w && im->h == h) return 1;
    if (!sgl_gpu_alloc_image(im, ifmt, w, h)) { sgl_set_error(GL_OUT_OF_MEMORY); return 0; }
    clear_new_image(im);
    return 1;
}

static void APIENTRY sgl_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                                      GLint border, GLenum format, GLenum type, const void *pixels) {
    (void)target; (void)border;
    if (level != 0) return;
    SglImage *im = bound_tex();
    if (!im) { sgl_set_error(GL_INVALID_OPERATION); return; }
    GLenum f = (GLenum)ifmt;
    if (f == GL_RGBA || f == GL_RGB || f == 0x8051 /* RGB8 */) f = GL_RGBA8;
    if (f == GL_DEPTH_COMPONENT || f == 0x81A5 /* DEPTH16 */) f = 0x81A6;
    if (!alloc_storage(im, f, w, h)) return;
    upload(im, 0, 0, w, h, format, type, pixels);
}
static void APIENTRY sgl_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                                         GLsizei h, GLenum format, GLenum type, const void *pixels) {
    (void)target;
    if (level != 0) return;
    upload(bound_tex(), x, y, w, h, format, type, pixels);
}

static void push_copy(SDL_GPUTexture *src, SDL_GPUTexture *dst, int sx, int sy, int dx, int dy,
                      int w, int h) {
    SglOp *op = sgl_push_op(SGL_OP_COPY);
    if (!op) return;
    op->u.copy.src = src; op->u.copy.dst = dst;
    op->u.copy.sx = sx; op->u.copy.sy = sy; op->u.copy.dx = dx; op->u.copy.dy = dy;
    op->u.copy.w = w; op->u.copy.h = h;
    sgl_stats_note(SGL_ST_COPY);
}

/* A 1:1 copy of colour between two images; through a blit when the formats
 * differ (the GL renderer copies RGBA8 into float surfaces). */
static void copy_color(SglImage *src, int sx, int sy, SglImage *dst, int dx, int dy, int w, int h) {
    if (!src || !dst || w <= 0 || h <= 0) return;
    if (src->fmt == dst->fmt) {
        if (src == dst) {
            /* Overlapping copies within one texture go through a scratch. */
            SglImage tmp; memset(&tmp, 0, sizeof(tmp)); tmp.used = 1;
            if (!sgl_gpu_alloc_image(&tmp, src->ifmt, w, h)) return;
            push_copy(src->tex, tmp.tex, sx, sy, 0, 0, w, h);
            push_copy(tmp.tex, dst->tex, 0, 0, dx, dy, w, h);
            sgl_release_texture(tmp.tex);
            return;
        }
        push_copy(src->tex, dst->tex, sx, sy, dx, dy, w, h);
        return;
    }
    SglOp *op = sgl_push_op(SGL_OP_BLIT);
    if (!op) return;
    op->u.blit.src = src->tex; op->u.blit.dst = dst->tex;
    op->u.blit.sx = sx; op->u.blit.sy = sy; op->u.blit.sw = w; op->u.blit.sh = h;
    op->u.blit.dx = dx; op->u.blit.dy = dy; op->u.blit.dw = w; op->u.blit.dh = h;
    op->u.blit.src_w = src->w; op->u.blit.src_h = src->h;
    op->u.blit.dst_w = dst->w; op->u.blit.dst_h = dst->h;
    sgl_stats_note(SGL_ST_BLIT);
}

static void APIENTRY sgl_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff,
                                             GLint x, GLint y, GLsizei w, GLsizei h) {
    (void)target;
    if (level != 0) return;
    SglImage *src = fbo_color_image(g_sgls.read_fbo, 1);
    SglImage *dst = bound_tex();
    if (!src || !dst) return;
    /* Clip to both images, moving the destination with the source. */
    int sx = x, sy = y, cw = w, ch = h;
    clip_rect(&sx, &sy, &cw, &ch, src->w, src->h);
    int dx = xoff + (sx - x), dy = yoff + (sy - y);
    int ddx = dx, ddy = dy;
    clip_rect(&ddx, &ddy, &cw, &ch, dst->w, dst->h);
    sx += ddx - dx; sy += ddy - dy;
    copy_color(src, sx, sy, dst, ddx, ddy, cw, ch);
}
static void APIENTRY sgl_glCopyTexImage2D(GLenum target, GLint level, GLenum ifmt, GLint x, GLint y,
                                          GLsizei w, GLsizei h, GLint border) {
    (void)border;
    if (level != 0) return;
    SglImage *dst = bound_tex();
    if (!dst) return;
    GLenum f = ifmt;
    if (f == GL_RGBA || f == GL_RGB) f = GL_RGBA8;
    if (!alloc_storage(dst, f, w, h)) return;
    sgl_glCopyTexSubImage2D(target, 0, 0, 0, x, y, w, h);
}

/* ---- readbacks ---------------------------------------------------------- */
static void convert_rows_out(SDL_GPUTextureFormat fmt, const uint8_t *src, int w, int h,
                             uint8_t *dst, GLenum format, GLenum type, int row_len, int align,
                             int skip_px, int skip_rows) {
    const int dpx = gl_px_size(format, type);
    const size_t dstride = gl_row_stride(w, dpx, row_len, align);
    int sbpp = fmt == SDL_GPU_TEXTUREFORMAT_R16_UINT ? 2 :
               fmt == SDL_GPU_TEXTUREFORMAT_R8_UNORM ? 1 :
               fmt == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT ? 8 :
               fmt == SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT ? 8 :
              fmt == SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT ? 16 : 4;
    uint8_t *d0 = dst + (size_t)skip_rows * dstride + (size_t)skip_px * dpx;
    for (int r = 0; r < h; r++)
        row_from_gpu(fmt, d0 + (size_t)r * dstride, src + (size_t)r * w * sbpp, w, format, type);
}

/* Read a region now (blocking). Returns 1 with the GPU layout in *out. */
extern int sgl_gpu_read_now(SDL_GPUTexture *tex, SDL_GPUTextureFormat fmt, int x, int y, int w,
                            int h, const uint8_t **out);

static void APIENTRY sgl_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                                      GLenum type, void *pixels) {
    SglImage *src = fbo_color_image(g_sgls.read_fbo, 1);
    if (!src || w <= 0 || h <= 0) return;
    int cx = x, cy = y, cw = w, ch = h;
    clip_rect(&cx, &cy, &cw, &ch, src->w, src->h);
    if (cw <= 0 || ch <= 0) return;
    /* Outside the surface GL leaves the client memory undefined; ours stays. */
    const int dpx = gl_px_size(format, type);
    const size_t dstride = gl_row_stride(w, dpx, g_sgls.pack_row_len, g_sgls.pack_align);
    const size_t inner = (size_t)(cy - y) * dstride + (size_t)(cx - x) * dpx;
    if (g_sgls.pack_buffer) {
        SglBuffer *b = buf_get(g_sgls.pack_buffer);
        if (!b) return;
        const size_t need = (size_t)cw * ch * (size_t)src->bpp;
        if (!b->xfer || b->xfer_size < need) {
            extern SDL_GPUTransferBuffer *sgl_gpu_download_buffer(size_t size);
            if (b->xfer) {
                SglStream *s = &g_sgl_stream;
                if (s->nrelease_xfer >= s->cap_release_xfer) {
                    int nc = s->cap_release_xfer ? s->cap_release_xfer * 2 : 16;
                    void *p = realloc(s->release_xfer, sizeof(*s->release_xfer) * nc);
                    if (p) { s->release_xfer = p; s->cap_release_xfer = nc; }
                }
                if (s->nrelease_xfer < s->cap_release_xfer) s->release_xfer[s->nrelease_xfer++] = b->xfer;
            }
            b->xfer = sgl_gpu_download_buffer(need);
            b->xfer_size = b->xfer ? need : 0;
            if (!b->xfer) return;
        }
        SglOp *op = sgl_push_op(SGL_OP_DOWNLOAD);
        if (!op) return;
        op->u.down.src = src->tex; op->u.down.x = cx; op->u.down.y = cy;
        op->u.down.w = cw; op->u.down.h = ch; op->u.down.xfer = b->xfer; op->u.down.off = 0;
        b->job.active = 1;
        b->job.stream = g_sgl_stream.id;
        b->job.src_fmt = src->fmt; b->job.w = cw; b->job.h = ch;
        b->job.format = format; b->job.type = type;
        b->job.row_len = g_sgls.pack_row_len ? g_sgls.pack_row_len : w;
        b->job.align = g_sgls.pack_align;
        b->job.skip_px = g_sgls.pack_skip_px; b->job.skip_rows = g_sgls.pack_skip_rows;
        b->job.dst_off = (size_t)(uintptr_t)pixels + inner;
        sgl_stats_note(SGL_ST_DOWNLOAD);
        return;
    }
    if (!pixels) return;
    const uint8_t *data = NULL;
    if (!sgl_gpu_read_now(src->tex, src->fmt, cx, cy, cw, ch, &data) || !data) return;
    convert_rows_out(src->fmt, data, cw, ch, (uint8_t *)pixels + inner, format, type,
                     g_sgls.pack_row_len ? g_sgls.pack_row_len : w, g_sgls.pack_align,
                     g_sgls.pack_skip_px, g_sgls.pack_skip_rows);
}
static void APIENTRY sgl_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type,
                                       void *pixels) {
    (void)target;
    SglImage *im = bound_tex();
    if (!im || !im->tex || level != 0 || !pixels) return;
    if (im->kind == SGL_KIND_DEPTH || im->kind == SGL_KIND_DEPTH_STENCIL) return;
    const uint8_t *data = NULL;
    if (!sgl_gpu_read_now(im->tex, im->fmt, 0, 0, im->w, im->h, &data) || !data) return;
    convert_rows_out(im->fmt, data, im->w, im->h, (uint8_t *)pixels, format, type,
                     g_sgls.pack_row_len, g_sgls.pack_align, g_sgls.pack_skip_px,
                     g_sgls.pack_skip_rows);
}

/* ---- framebuffer objects ------------------------------------------------ */
static void APIENTRY sgl_glGenFramebuffers(GLsizei n, GLuint *out) {
    for (int i = 0; i < n; i++) {
        out[i] = tab_new(&s_fbo);
        SglFbo *f = fbo_get(out[i]);
        if (f) { f->draw_buffer = GL_COLOR_ATTACHMENT0; f->read_buffer = GL_COLOR_ATTACHMENT0; }
    }
}
static void APIENTRY sgl_glDeleteFramebuffers(GLsizei n, const GLuint *names) {
    for (int i = 0; i < n; i++) {
        SglFbo *f = fbo_get(names[i]);
        if (!f) continue;
        f->used = 0;
        if (g_sgls.draw_fbo == names[i]) g_sgls.draw_fbo = 0;
        if (g_sgls.read_fbo == names[i]) g_sgls.read_fbo = 0;
    }
}
static void APIENTRY sgl_glBindFramebuffer(GLenum target, GLuint name) {
    if (name == 0) back_follow_window();
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) g_sgls.draw_fbo = name;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) g_sgls.read_fbo = name;
}
static SglFbo *bound_fbo(GLenum target) {
    return fbo_get(target == GL_READ_FRAMEBUFFER ? g_sgls.read_fbo : g_sgls.draw_fbo);
}
static void attach(GLenum target, GLenum attachment, GLuint name, int rb) {
    SglFbo *f = bound_fbo(target);
    if (!f) { sgl_set_error(GL_INVALID_OPERATION); return; }
    if (attachment == GL_COLOR_ATTACHMENT0) { f->color = name; f->color_rb = rb; }
    else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT || attachment == GL_DEPTH_ATTACHMENT ||
             attachment == GL_STENCIL_ATTACHMENT) { f->ds = name; f->ds_rb = rb; }
}
static void APIENTRY sgl_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                                                GLuint tex, GLint level) {
    (void)textarget; (void)level;
    attach(target, attachment, tex, 0);
}
static void APIENTRY sgl_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbt,
                                                   GLuint rb) {
    (void)rbt;
    attach(target, attachment, rb, 1);
}
static GLenum APIENTRY sgl_glCheckFramebufferStatus(GLenum target) {
    GLuint n = target == GL_READ_FRAMEBUFFER ? g_sgls.read_fbo : g_sgls.draw_fbo;
    if (n == 0) return GL_FRAMEBUFFER_COMPLETE;
    SglFbo *f = fbo_get(n);
    if (!f) return 0x8CDD; /* UNSUPPORTED */
    SglImage *c = f->color ? sgl_image(f->color, f->color_rb) : NULL;
    SglImage *d = f->ds ? sgl_image(f->ds, f->ds_rb) : NULL;
    if (f->color && (!c || !c->tex)) return 0x8CD6; /* INCOMPLETE_ATTACHMENT */
    if (f->ds && (!d || !d->tex)) return 0x8CD6;
    if (!c && !d) return 0x8CD7; /* MISSING_ATTACHMENT */
    return GL_FRAMEBUFFER_COMPLETE;
}
static void APIENTRY sgl_glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment,
                                                               GLenum pname, GLint *v) {
    (void)target; (void)attachment;
    if (!v) return;
    const int f16 = g_sgl.back.fmt == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    switch (pname) {
    case 0x8211: *v = f16 ? GL_FLOAT : 0x8C17 /* UNSIGNED_NORMALIZED */; break;
    case 0x8212: case 0x8213: case 0x8214: case 0x8215: *v = f16 ? 16 : 8; break;
    case 0x8216: *v = 24; break;
    case 0x8217: *v = 8; break;
    default: *v = 0; break;
    }
}
static void APIENTRY sgl_glReadBuffer(GLenum mode) {
    SglFbo *f = fbo_get(g_sgls.read_fbo);
    if (f) f->read_buffer = mode;
}
static void APIENTRY sgl_glDrawBuffer(GLenum mode) {
    SglFbo *f = fbo_get(g_sgls.draw_fbo);
    if (f) f->draw_buffer = mode;
}

/* ---- renderbuffers ------------------------------------------------------ */
static void APIENTRY sgl_glGenRenderbuffers(GLsizei n, GLuint *out) {
    for (int i = 0; i < n; i++) out[i] = tab_new(&s_rb);
}
static void APIENTRY sgl_glDeleteRenderbuffers(GLsizei n, const GLuint *names) {
    for (int i = 0; i < n; i++) {
        SglImage *im = sgl_image(names[i], 1);
        if (!im) continue;
        image_free(im);
        im->used = 0;
        if (g_sgls.renderbuffer == names[i]) g_sgls.renderbuffer = 0;
    }
}
static void APIENTRY sgl_glBindRenderbuffer(GLenum target, GLuint rb) { (void)target; g_sgls.renderbuffer = rb; }
static void APIENTRY sgl_glRenderbufferStorage(GLenum target, GLenum ifmt, GLsizei w, GLsizei h) {
    (void)target;
    SglImage *im = sgl_image(g_sgls.renderbuffer, 1);
    if (!im) { sgl_set_error(GL_INVALID_OPERATION); return; }
    GLenum f = ifmt;
    if (f == GL_RGBA || f == GL_RGB) f = GL_RGBA8;
    alloc_storage(im, f, w, h);
}

/* ---- buffers ------------------------------------------------------------ */
static void APIENTRY sgl_glGenBuffers(GLsizei n, GLuint *out) {
    for (int i = 0; i < n; i++) out[i] = tab_new(&s_buf);
}
static void APIENTRY sgl_glDeleteBuffers(GLsizei n, const GLuint *names) {
    for (int i = 0; i < n; i++) {
        SglBuffer *b = buf_get(names[i]);
        if (!b) continue;
        free(b->data);
        if (b->xfer) {
            SglStream *s = &g_sgl_stream;
            if (s->nrelease_xfer >= s->cap_release_xfer) {
                int nc = s->cap_release_xfer ? s->cap_release_xfer * 2 : 16;
                void *p = realloc(s->release_xfer, sizeof(*s->release_xfer) * nc);
                if (p) { s->release_xfer = p; s->cap_release_xfer = nc; }
            }
            if (s->nrelease_xfer < s->cap_release_xfer) s->release_xfer[s->nrelease_xfer++] = b->xfer;
        }
        memset(b, 0, sizeof(*b));
        if (g_sgls.array_buffer == names[i]) g_sgls.array_buffer = 0;
        if (g_sgls.pack_buffer == names[i]) g_sgls.pack_buffer = 0;
        if (g_sgls.unpack_buffer == names[i]) g_sgls.unpack_buffer = 0;
    }
}
static SglVao *cur_vao(void);
static GLuint *buffer_slot(GLenum target) {
    switch (target) {
    case 0x8892: return &g_sgls.array_buffer;
    case 0x8893: return &cur_vao()->element;
    case 0x88EB: return &g_sgls.pack_buffer;
    case 0x88EC: return &g_sgls.unpack_buffer;
    default: return NULL;
    }
}
static void APIENTRY sgl_glBindBuffer(GLenum target, GLuint name) {
    GLuint *s = buffer_slot(target);
    if (s) *s = name;
}
static void APIENTRY sgl_glBufferData(GLenum target, ptrdiff_t size, const void *data, GLenum usage) {
    (void)usage;
    GLuint *s = buffer_slot(target);
    SglBuffer *b = s ? buf_get(*s) : NULL;
    if (!b || size < 0) { sgl_set_error(GL_INVALID_OPERATION); return; }
    if ((size_t)size > b->size || !b->data) {
        uint8_t *p = (uint8_t *)realloc(b->data, (size_t)size ? (size_t)size : 1);
        if (!p) { sgl_set_error(GL_OUT_OF_MEMORY); return; }
        b->data = p;
    }
    b->size = (size_t)size;
    if (data && size) memcpy(b->data, data, (size_t)size);
    b->version++;
    b->job.active = 0;
}
static void APIENTRY sgl_glBufferSubData(GLenum target, ptrdiff_t off, ptrdiff_t size, const void *data) {
    GLuint *s = buffer_slot(target);
    SglBuffer *b = s ? buf_get(*s) : NULL;
    if (!b || off < 0 || size < 0 || (size_t)(off + size) > b->size) { sgl_set_error(GL_INVALID_VALUE); return; }
    if (data && size) memcpy(b->data + off, data, (size_t)size);
    b->version++;
}
static void *APIENTRY sgl_glMapBuffer(GLenum target, GLenum access) {
    (void)access;
    GLuint *s = buffer_slot(target);
    SglBuffer *b = s ? buf_get(*s) : NULL;
    if (!b) return NULL;
    if (b->job.active) {
        extern int sgl_gpu_wait_stream(uint64_t id);
        if (b->job.stream == g_sgl_stream.id) sgl_flush(1, 0);
        else sgl_gpu_wait_stream(b->job.stream);
        const uint8_t *src = (const uint8_t *)SDL_MapGPUTransferBuffer(g_sgl.dev, b->xfer, false);
        if (src) {
            if (b->job.dst_off < b->size)
                convert_rows_out(b->job.src_fmt, src, b->job.w, b->job.h, b->data + b->job.dst_off,
                                 b->job.format, b->job.type, b->job.row_len, b->job.align,
                                 b->job.skip_px, b->job.skip_rows);
            SDL_UnmapGPUTransferBuffer(g_sgl.dev, b->xfer);
        }
        b->job.active = 0;
    }
    return b->data;
}
static GLboolean APIENTRY sgl_glUnmapBuffer(GLenum target) { (void)target; return GL_TRUE; }

/* ---- vertex arrays ------------------------------------------------------ */
static void APIENTRY sgl_glGenVertexArrays(GLsizei n, GLuint *out) {
    for (int i = 0; i < n; i++) out[i] = tab_new(&s_vao);
}
static void APIENTRY sgl_glDeleteVertexArrays(GLsizei n, const GLuint *names) {
    for (int i = 0; i < n; i++) {
        SglVao *v = vao_get(names[i]);
        if (v) v->used = 0;
        if (g_sgls.vao == names[i]) g_sgls.vao = 0;
        g_sgls.pipe_epoch++;
    }
}
static void APIENTRY sgl_glBindVertexArray(GLuint name) { PSET(g_sgls.vao, name); }
static SglVao s_vao0;     /* VAO 0: core GL forbids drawing with it, but keep one */
static SglVao *cur_vao(void) {
    SglVao *v = vao_get(g_sgls.vao);
    return v ? v : &s_vao0;
}
static void APIENTRY sgl_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm,
                                               GLsizei stride, const void *ptr) {
    if (index >= SGL_MAX_ATTRIBS) { sgl_set_error(GL_INVALID_VALUE); return; }
    SglAttrib *a = &cur_vao()->a[index];
    a->buffer = g_sgls.array_buffer; a->size = size; a->type = type; a->normalized = norm;
    a->stride = stride; a->offset = (size_t)(uintptr_t)ptr;
    g_sgls.pipe_epoch++;
}
static void APIENTRY sgl_glDisableVertexAttribArray(GLuint index) {
    if (index < SGL_MAX_ATTRIBS && cur_vao()->a[index].enabled) {
        cur_vao()->a[index].enabled = 0;
        g_sgls.pipe_epoch++;
    }
}
static void APIENTRY sgl_glGetVertexAttribiv(GLuint index, GLenum pname, GLint *v) {
    if (index >= SGL_MAX_ATTRIBS || !v) return;
    const SglAttrib *a = &cur_vao()->a[index];
    switch (pname) {
    case 0x8622: *v = a->enabled; break;                /* ENABLED */
    case 0x8623: *v = a->size ? a->size : 4; break;     /* SIZE */
    case 0x8624: *v = a->stride; break;                 /* STRIDE */
    case 0x8625: *v = a->type ? (GLint)a->type : GL_FLOAT; break;
    case 0x886A: *v = a->normalized; break;
    case 0x889F: *v = (GLint)a->buffer; break;          /* BUFFER_BINDING */
    default: *v = 0; break;
    }
}
static void APIENTRY sgl_glGetVertexAttribPointerv(GLuint index, GLenum pname, void **v) {
    (void)pname;
    if (index < SGL_MAX_ATTRIBS && v) *v = (void *)(uintptr_t)cur_vao()->a[index].offset;
}
static void APIENTRY sgl_glEnableVertexAttribArray(GLuint index) {
    if (index < SGL_MAX_ATTRIBS && !cur_vao()->a[index].enabled) {
        cur_vao()->a[index].enabled = 1;
        g_sgls.pipe_epoch++;
    }
}

/* ---- shaders and programs ----------------------------------------------- */
static uint32_t s_prog_ids;
static GLuint APIENTRY sgl_glCreateShader(GLenum type) {
    GLuint n = tab_new(&s_obj);
    SglObj *o = sgl_obj(n);
    if (o) o->shader_type = type;
    return n;
}
static void APIENTRY sgl_glShaderSource(GLuint sh, GLsizei count, const char *const *strs, const GLint *lens) {
    SglObj *o = sgl_obj(sh);
    if (!o || o->is_program) return;
    size_t total = 0;
    for (int i = 0; i < count; i++) total += lens && lens[i] >= 0 ? (size_t)lens[i] : strlen(strs[i]);
    char *s = (char *)malloc(total + 1);
    if (!s) return;
    size_t at = 0;
    for (int i = 0; i < count; i++) {
        size_t l = lens && lens[i] >= 0 ? (size_t)lens[i] : strlen(strs[i]);
        memcpy(s + at, strs[i], l); at += l;
    }
    s[at] = 0;
    free(o->source);
    o->source = s;
}
/* The GLSL is compiled with its partner stage at link time (varyings are
 * matched by name across the two stages there). */
static void APIENTRY sgl_glCompileShader(GLuint sh) { (void)sh; }
static void APIENTRY sgl_glGetShaderiv(GLuint sh, GLenum pname, GLint *v) {
    SglObj *o = sgl_obj(sh);
    if (!v) return;
    if (pname == 0x8B81 /* COMPILE_STATUS */) *v = o && o->source ? GL_TRUE : GL_FALSE;
    else if (pname == 0x8B84 /* INFO_LOG_LENGTH */) *v = 0;
    else if (pname == 0x8B4F /* SHADER_TYPE */) *v = o ? (GLint)o->shader_type : 0;
    else *v = 0;
}
static void APIENTRY sgl_glGetShaderInfoLog(GLuint sh, GLsizei max, GLsizei *len, char *log) {
    (void)sh;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}
static void free_program_link(SglObj *o) {
    extern void sgl_gpu_forget_program(SglObj *prog);
    sgl_gpu_forget_program(o);
    free(o->u); o->u = NULL; o->nu = 0; o->nlocs = 0;
    free(o->loc_u); o->loc_u = NULL;
    free(o->block[0]); free(o->block[1]); o->block[0] = o->block[1] = NULL;
    o->block_size[0] = o->block_size[1] = 0;
    free(o->log); o->log = NULL;
    o->linked = 0;
}
static void APIENTRY sgl_glDeleteShader(GLuint sh) {
    SglObj *o = sgl_obj(sh);
    if (!o || o->is_program) return;
    free(o->source);
    memset(o, 0, sizeof(*o));
}
static GLuint APIENTRY sgl_glCreateProgram(void) {
    GLuint n = tab_new(&s_obj);
    SglObj *o = sgl_obj(n);
    if (o) o->is_program = 1;
    return n;
}
static void APIENTRY sgl_glDeleteProgram(GLuint p) {
    SglObj *o = sgl_obj(p);
    if (!o || !o->is_program) return;
    free_program_link(o);
    memset(o, 0, sizeof(*o));
    if (g_sgls.program == p) g_sgls.program = 0;
}
static void APIENTRY sgl_glAttachShader(GLuint p, GLuint sh) {
    SglObj *o = sgl_obj(p);
    if (!o || !o->is_program || o->nattached >= 4) return;
    o->attached[o->nattached++] = sh;
}
static void APIENTRY sgl_glBindFragDataLocationIndexed(GLuint p, GLuint color, GLuint index, const char *name) {
    SglObj *o = sgl_obj(p);
    if (!o || !o->is_program || !name) return;
    for (int i = 0; i < o->nfrag_bind; i++)
        if (strcmp(o->frag_bind[i].name, name) == 0) { o->frag_bind[i].color = color; o->frag_bind[i].index = index; return; }
    if (o->nfrag_bind >= 4) return;
    snprintf(o->frag_bind[o->nfrag_bind].name, sizeof(o->frag_bind[0].name), "%s", name);
    o->frag_bind[o->nfrag_bind].color = color;
    o->frag_bind[o->nfrag_bind].index = index;
    o->nfrag_bind++;
}
static void APIENTRY sgl_glLinkProgram(GLuint p) {
    SglObj *o = sgl_obj(p);
    if (!o || !o->is_program) return;
    free_program_link(o);
    const char *vs = NULL, *fs = NULL;
    for (int i = 0; i < o->nattached; i++) {
        SglObj *s = sgl_obj(o->attached[i]);
        if (!s || !s->source) continue;
        if (s->shader_type == 0x8B31) vs = s->source;
        else if (s->shader_type == 0x8B30) fs = s->source;
    }
    if (!vs || !fs) { o->log = strdup("sgl: program needs a vertex and a fragment shader"); return; }
    o->id = ++s_prog_ids;
    o->linked = sgl_shader_link(o, vs, fs);
    if (!o->linked) {
        fprintf(stdout, "psxrecomp: sgl: link failed: %s\n", o->log ? o->log : "?");
        fflush(stdout);
    }
}
static void APIENTRY sgl_glGetProgramiv(GLuint p, GLenum pname, GLint *v) {
    SglObj *o = sgl_obj(p);
    if (!v) return;
    if (!o) { *v = 0; return; }
    switch (pname) {
    case 0x8B82: *v = o->linked ? GL_TRUE : GL_FALSE; break;               /* LINK_STATUS */
    case 0x8B84: *v = o->log ? (GLint)strlen(o->log) + 1 : 0; break;      /* INFO_LOG_LENGTH */
    case 0x8B86: *v = o->nu; break;                                       /* ACTIVE_UNIFORMS */
    case 0x8B87: {                                                        /* ACTIVE_UNIFORM_MAX_LENGTH */
        int m = 0;
        for (int i = 0; i < o->nu; i++) { int l = (int)strlen(o->u[i].name) + 4; if (l > m) m = l; }
        *v = m;
        break;
    }
    default: *v = 0; break;
    }
}
static void APIENTRY sgl_glGetProgramInfoLog(GLuint p, GLsizei max, GLsizei *len, char *log) {
    SglObj *o = sgl_obj(p);
    const char *s = o && o->log ? o->log : "";
    int n = (int)strlen(s);
    if (max <= 0) { if (len) *len = 0; return; }
    if (n > max - 1) n = max - 1;
    if (log) { memcpy(log, s, (size_t)n); log[n] = 0; }
    if (len) *len = n;
}
static void APIENTRY sgl_glUseProgram(GLuint p) { PSET(g_sgls.program, p); }
static GLboolean APIENTRY sgl_glIsProgram(GLuint p) { return sgl_obj(p) ? GL_TRUE : GL_FALSE; }
static void APIENTRY sgl_glDetachShader(GLuint p, GLuint sh) { (void)p; (void)sh; }
static GLint APIENTRY sgl_glGetAttribLocation(GLuint p, const char *name) {
    SglObj *o = sgl_obj(p);
    if (!o || !o->linked || !name) return -1;
    for (int i = 0; i < SGL_MAX_ATTRIBS; i++)
        if ((o->vs_inputs & (1u << i)) && strcmp(o->vs_input_name[i], name) == 0) return i;
    return -1;
}
static const GLubyte *APIENTRY sgl_glGetStringi(GLenum name, GLuint i) { (void)name; (void)i; return NULL; }

/* Location -> (uniform, element). */
static SglUniform *uniform_at(SglObj *o, GLint loc, int *elem) {
    if (!o || loc < 0 || loc >= o->nlocs || !o->loc_u) return NULL;
    SglUniform *u = &o->u[o->loc_u[loc]];
    *elem = loc - u->loc;
    return u;
}
static GLint APIENTRY sgl_glGetUniformLocation(GLuint p, const char *name) {
    SglObj *o = sgl_obj(p);
    if (!o || !o->linked || !name) return -1;
    char base[64];
    int idx = 0;
    const char *br = strchr(name, '[');
    if (br) {
        size_t l = (size_t)(br - name);
        if (l >= sizeof(base)) return -1;
        memcpy(base, name, l); base[l] = 0;
        idx = atoi(br + 1);
    } else {
        snprintf(base, sizeof(base), "%s", name);
    }
    for (int i = 0; i < o->nu; i++)
        if (strcmp(o->u[i].name, base) == 0)
            return idx < o->u[i].count ? o->u[i].loc + idx : -1;
    return -1;
}

static int gltype_is_int(GLenum t) {
    switch (t) {
    case GL_INT: case 0x8B53: case 0x8B54: case 0x8B55:            /* INT, IVEC2..4 */
    case GL_UNSIGNED_INT: case 0x8DC6: case 0x8DC7: case 0x8DC8:   /* UINT, UVEC2..4 */
    case GL_BOOL: case 0x8B57: case 0x8B58: case 0x8B59:
        return 1;
    default: return 0;
    }
}
static int gltype_comps(GLenum t) {
    switch (t) {
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: case GL_BOOL: return 1;
    case 0x8B50: case 0x8B53: case 0x8DC6: case 0x8B57: return 2;
    case 0x8B51: case 0x8B54: case 0x8DC7: case 0x8B58: return 3;
    case 0x8B52: case 0x8B55: case 0x8DC8: case 0x8B59: return 4;
    case 0x8B5C /* MAT4: four std140 columns in a row */: return 16;
    default: return 4;
    }
}

/* Write n components (as ints or floats) to the uniform at loc of the
 * current program, `count` array elements from loc on. */
static void set_uniform(GLint loc, int comps, int count, const float *fv, const GLint *iv) {
    SglObj *o = sgl_obj(g_sgls.program);
    int elem = 0;
    SglUniform *u = uniform_at(o, loc, &elem);
    if (!u) return;
    for (int e = 0; e < count && elem + e < u->count; e++) {
        if (u->sampler) {
            int unit = iv ? iv[e * comps] : (int)fv[e * comps];
            for (int s = 0; s < 2; s++)
                if (u->binding[s] >= 0 && u->binding[s] + elem + e < SGL_MAX_SAMPLERS)
                    o->sampler_unit[s][u->binding[s] + elem + e] = unit;
            continue;
        }
        const int is_int = gltype_is_int(u->gltype);
        const int uc = gltype_comps(u->gltype);
        const int n = comps < uc ? comps : uc;
        for (int s = 0; s < 2; s++) {
            if (u->off[s] < 0 || !o->block[s]) continue;
            uint8_t *dst = o->block[s] + u->off[s] + (size_t)(elem + e) * (size_t)u->stride[s];
            if (dst + (size_t)n * 4 > o->block[s] + o->block_size[s]) continue;
            for (int c = 0; c < n; c++) {
                if (is_int) {
                    int32_t x = iv ? (int32_t)iv[e * comps + c] : (int32_t)fv[e * comps + c];
                    memcpy(dst + 4 * c, &x, 4);
                } else {
                    float x = fv ? fv[e * comps + c] : (float)iv[e * comps + c];
                    memcpy(dst + 4 * c, &x, 4);
                }
            }
            o->block_dirty[s] = 1;
        }
    }
}
static void APIENTRY sgl_glUniform1i(GLint l, GLint a) { GLint v[1] = {a}; set_uniform(l, 1, 1, NULL, v); }
static void APIENTRY sgl_glUniform2i(GLint l, GLint a, GLint b) { GLint v[2] = {a, b}; set_uniform(l, 2, 1, NULL, v); }
static void APIENTRY sgl_glUniform4i(GLint l, GLint a, GLint b, GLint c, GLint d) { GLint v[4] = {a, b, c, d}; set_uniform(l, 4, 1, NULL, v); }
static void APIENTRY sgl_glUniform1f(GLint l, GLfloat a) { float v[1] = {a}; set_uniform(l, 1, 1, v, NULL); }
static void APIENTRY sgl_glUniform2f(GLint l, GLfloat a, GLfloat b) { float v[2] = {a, b}; set_uniform(l, 2, 1, v, NULL); }
static void APIENTRY sgl_glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { float v[3] = {a, b, c}; set_uniform(l, 3, 1, v, NULL); }
static void APIENTRY sgl_glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { float v[4] = {a, b, c, d}; set_uniform(l, 4, 1, v, NULL); }
static void APIENTRY sgl_glUniform4fv(GLint l, GLsizei count, const GLfloat *v) { set_uniform(l, 4, count, v, NULL); }
static void APIENTRY sgl_glUniformMatrix4fv(GLint l, GLsizei count, GLboolean transpose, const GLfloat *v) {
    float m[16];
    for (int e = 0; e < count; e++) {
        const float *src = v + 16 * e;
        if (transpose) for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) m[c * 4 + r] = src[r * 4 + c];
        else memcpy(m, src, sizeof(m));
        set_uniform(l + e, 16, 1, m, NULL);
    }
}

static void APIENTRY sgl_glGetActiveUniform(GLuint p, GLuint index, GLsizei max, GLsizei *len, GLint *size,
                                            GLenum *type, char *name) {
    SglObj *o = sgl_obj(p);
    if (!o || (int)index >= o->nu) { sgl_set_error(GL_INVALID_VALUE); return; }
    SglUniform *u = &o->u[index];
    char buf[80];
    snprintf(buf, sizeof(buf), u->count > 1 ? "%s[0]" : "%s", u->name);
    int n = (int)strlen(buf);
    if (max > 0 && name) {
        if (n > max - 1) n = max - 1;
        memcpy(name, buf, (size_t)n); name[n] = 0;
    }
    if (len) *len = n;
    if (size) *size = u->count;
    if (type) *type = u->gltype;
}
static void get_uniform(GLuint p, GLint loc, float *fv, GLint *iv) {
    SglObj *o = sgl_obj(p);
    int elem = 0;
    SglUniform *u = uniform_at(o, loc, &elem);
    if (!u) return;
    if (u->sampler) {
        for (int s = 0; s < 2; s++)
            if (u->binding[s] >= 0) {
                int unit = o->sampler_unit[s][u->binding[s] + elem];
                if (fv) fv[0] = (float)unit;
                if (iv) iv[0] = unit;
                return;
            }
        return;
    }
    const int is_int = gltype_is_int(u->gltype), n = gltype_comps(u->gltype);
    for (int s = 0; s < 2; s++) {
        if (u->off[s] < 0 || !o->block[s]) continue;
        const uint8_t *src = o->block[s] + u->off[s] + (size_t)elem * (size_t)u->stride[s];
        for (int c = 0; c < n; c++) {
            if (is_int) {
                int32_t x; memcpy(&x, src + 4 * c, 4);
                if (fv) fv[c] = (float)x;
                if (iv) iv[c] = x;
            } else {
                float x; memcpy(&x, src + 4 * c, 4);
                if (fv) fv[c] = x;
                if (iv) iv[c] = (GLint)x;
            }
        }
        return;
    }
}
static void APIENTRY sgl_glGetUniformfv(GLuint p, GLint loc, GLfloat *v) { get_uniform(p, loc, v, NULL); }
static void APIENTRY sgl_glGetUniformiv(GLuint p, GLint loc, GLint *v) { get_uniform(p, loc, NULL, v); }

/* ---- fixed-function state ----------------------------------------------- */
static int *cap_slot(GLenum cap) {
    switch (cap) {
    case GL_BLEND: return &g_sgls.blend;
    case GL_DEPTH_TEST: return &g_sgls.depth_test;
    case GL_STENCIL_TEST: return &g_sgls.stencil_test;
    case GL_SCISSOR_TEST: return &g_sgls.scissor_test;
    default: return NULL;   /* CULL_FACE, DITHER ...: always off here */
    }
}
static void APIENTRY sgl_glEnable(GLenum cap) { int *s = cap_slot(cap); if (s) *s = 1; }
static void APIENTRY sgl_glDisable(GLenum cap) { int *s = cap_slot(cap); if (s) *s = 0; }
static GLboolean APIENTRY sgl_glIsEnabled(GLenum cap) { int *s = cap_slot(cap); return s && *s ? GL_TRUE : GL_FALSE; }
static void APIENTRY sgl_glBlendFunc(GLenum s, GLenum d) {
    PSET(g_sgls.bsrc_rgb, s); PSET(g_sgls.bsrc_a, s); PSET(g_sgls.bdst_rgb, d); PSET(g_sgls.bdst_a, d);
}
static void APIENTRY sgl_glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    PSET(g_sgls.bsrc_rgb, sr); PSET(g_sgls.bdst_rgb, dr); PSET(g_sgls.bsrc_a, sa); PSET(g_sgls.bdst_a, da);
}
static void APIENTRY sgl_glBlendEquationSeparate(GLenum rgb, GLenum a) { PSET(g_sgls.beq_rgb, rgb); PSET(g_sgls.beq_a, a); }
static void APIENTRY sgl_glBlendEquation(GLenum m) { PSET(g_sgls.beq_rgb, m); PSET(g_sgls.beq_a, m); }
static void APIENTRY sgl_glBindSampler(GLuint unit, GLuint sampler) { (void)unit; (void)sampler; }   /* 0 only */
static void APIENTRY sgl_glPolygonMode(GLenum face, GLenum mode) { (void)face; (void)mode; }       /* fill only */
static void APIENTRY sgl_glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    g_sgls.blend_color[0] = r; g_sgls.blend_color[1] = g; g_sgls.blend_color[2] = b; g_sgls.blend_color[3] = a;
}
static void APIENTRY sgl_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    PSET(g_sgls.cmask[0], (uint8_t)(r != 0)); PSET(g_sgls.cmask[1], (uint8_t)(g != 0));
    PSET(g_sgls.cmask[2], (uint8_t)(b != 0)); PSET(g_sgls.cmask[3], (uint8_t)(a != 0));
}
static void APIENTRY sgl_glDepthMask(GLboolean m) { PSET(g_sgls.depth_mask, m != 0); }
static void APIENTRY sgl_glDepthFunc(GLenum f) { PSET(g_sgls.depth_func, f); }
static void APIENTRY sgl_glStencilMask(GLuint m) { PSET(g_sgls.st_wmask, m); }
static void APIENTRY sgl_glStencilFunc(GLenum f, GLint ref, GLuint mask) {
    PSET(g_sgls.st_func, f); g_sgls.st_ref = ref; PSET(g_sgls.st_vmask, mask);   /* ref is dynamic */
}
static void APIENTRY sgl_glStencilOp(GLenum sf, GLenum zf, GLenum zp) {
    PSET(g_sgls.st_fail, sf); PSET(g_sgls.st_zfail, zf); PSET(g_sgls.st_zpass, zp);
}
static void APIENTRY sgl_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    g_sgls.clear_color[0] = r; g_sgls.clear_color[1] = g; g_sgls.clear_color[2] = b; g_sgls.clear_color[3] = a;
}
static void APIENTRY sgl_glClearDepth(GLdouble d) { g_sgls.clear_depth = d; }
static void APIENTRY sgl_glClearStencil(GLint s) { g_sgls.clear_stencil = s; }
static void APIENTRY sgl_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    g_sgls.vp[0] = x; g_sgls.vp[1] = y; g_sgls.vp[2] = w; g_sgls.vp[3] = h;
}
static void APIENTRY sgl_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    g_sgls.sc[0] = x; g_sgls.sc[1] = y; g_sgls.sc[2] = w; g_sgls.sc[3] = h;
}
static void APIENTRY sgl_glLineWidth(GLfloat w) { g_sgls.line_width = w; }
static void APIENTRY sgl_glPixelStorei(GLenum pname, GLint v) {
    switch (pname) {
    case GL_PACK_ALIGNMENT: g_sgls.pack_align = v; break;
    case GL_PACK_ROW_LENGTH: g_sgls.pack_row_len = v; break;
    case GL_PACK_SKIP_PIXELS: g_sgls.pack_skip_px = v; break;
    case GL_PACK_SKIP_ROWS: g_sgls.pack_skip_rows = v; break;
    case GL_UNPACK_ALIGNMENT: g_sgls.unpack_align = v; break;
    case GL_UNPACK_ROW_LENGTH: g_sgls.unpack_row_len = v; break;
    case GL_UNPACK_SKIP_PIXELS: g_sgls.unpack_skip_px = v; break;
    case GL_UNPACK_SKIP_ROWS: g_sgls.unpack_skip_rows = v; break;
    default: break;
    }
}

static void APIENTRY sgl_glGetIntegerv(GLenum pname, GLint *v) {
    if (!v) return;
    switch (pname) {
    case GL_VIEWPORT: memcpy(v, g_sgls.vp, sizeof(g_sgls.vp)); break;
    case GL_SCISSOR_BOX: memcpy(v, g_sgls.sc, sizeof(g_sgls.sc)); break;
    case GL_MAX_TEXTURE_SIZE: *v = g_sgl.caps.max_texture_size; break;
    case 0x84E8 /* MAX_RENDERBUFFER_SIZE */: *v = g_sgl.caps.max_texture_size; break;
    case 0x0D3A /* MAX_VIEWPORT_DIMS */: v[0] = v[1] = g_sgl.caps.max_texture_size; break;
    case GL_PACK_ALIGNMENT: *v = g_sgls.pack_align; break;
    case GL_PACK_ROW_LENGTH: *v = g_sgls.pack_row_len; break;
    case GL_PACK_SKIP_PIXELS: *v = g_sgls.pack_skip_px; break;
    case GL_PACK_SKIP_ROWS: *v = g_sgls.pack_skip_rows; break;
    case GL_UNPACK_ALIGNMENT: *v = g_sgls.unpack_align; break;
    case GL_UNPACK_ROW_LENGTH: *v = g_sgls.unpack_row_len; break;
    case GL_UNPACK_SKIP_PIXELS: *v = g_sgls.unpack_skip_px; break;
    case GL_UNPACK_SKIP_ROWS: *v = g_sgls.unpack_skip_rows; break;
    case GL_TEXTURE_BINDING_2D: *v = (GLint)g_sgls.unit_tex[g_sgls.active_unit]; break;
    case 0x84E0 /* ACTIVE_TEXTURE */: *v = 0x84C0 + g_sgls.active_unit; break;
    case 0x8B8D /* CURRENT_PROGRAM */: *v = (GLint)g_sgls.program; break;
    case 0x85B5 /* VERTEX_ARRAY_BINDING */: *v = (GLint)g_sgls.vao; break;
    case 0x8894 /* ARRAY_BUFFER_BINDING */: *v = (GLint)g_sgls.array_buffer; break;
    case 0x88ED /* PIXEL_PACK_BUFFER_BINDING */: *v = (GLint)g_sgls.pack_buffer; break;
    case 0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */: *v = (GLint)g_sgls.unpack_buffer; break;
    case 0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */: *v = (GLint)g_sgls.draw_fbo; break;
    case 0x8CAA /* READ_FRAMEBUFFER_BINDING */: *v = (GLint)g_sgls.read_fbo; break;
    case 0x8CA7 /* RENDERBUFFER_BINDING */: *v = (GLint)g_sgls.renderbuffer; break;
    case GL_READ_BUFFER: {
        SglFbo *f = fbo_get(g_sgls.read_fbo);
        *v = f ? (GLint)f->read_buffer : GL_BACK;
        break;
    }
    case GL_DEPTH_FUNC: *v = (GLint)g_sgls.depth_func; break;
    case 0x8009 /* BLEND_EQUATION_RGB */: *v = (GLint)g_sgls.beq_rgb; break;
    case 0x883D /* BLEND_EQUATION_ALPHA */: *v = (GLint)g_sgls.beq_a; break;
    case 0x80C8 /* BLEND_DST_RGB */: *v = (GLint)g_sgls.bdst_rgb; break;
    case 0x80C9 /* BLEND_SRC_RGB */: *v = (GLint)g_sgls.bsrc_rgb; break;
    case 0x80CA /* BLEND_DST_ALPHA */: *v = (GLint)g_sgls.bdst_a; break;
    case 0x80CB /* BLEND_SRC_ALPHA */: *v = (GLint)g_sgls.bsrc_a; break;
    case GL_STENCIL_WRITEMASK: *v = (GLint)g_sgls.st_wmask; break;
    case GL_STENCIL_FUNC: *v = (GLint)g_sgls.st_func; break;
    case GL_STENCIL_REF: *v = g_sgls.st_ref; break;
    case GL_STENCIL_VALUE_MASK: *v = (GLint)g_sgls.st_vmask; break;
    case GL_STENCIL_FAIL: *v = (GLint)g_sgls.st_fail; break;
    case GL_STENCIL_PASS_DEPTH_FAIL: *v = (GLint)g_sgls.st_zfail; break;
    case GL_STENCIL_PASS_DEPTH_PASS: *v = (GLint)g_sgls.st_zpass; break;
    case GL_STENCIL_CLEAR_VALUE: *v = g_sgls.clear_stencil; break;
    case 0x8895 /* ELEMENT_ARRAY_BUFFER_BINDING */: *v = (GLint)cur_vao()->element; break;
    case 0x9126 /* CONTEXT_PROFILE_MASK */: *v = 1; break;   /* core */
    case 0x0B40 /* POLYGON_MODE */: v[0] = v[1] = 0x1B02 /* FILL */; break;
    case 0x821B /* MAJOR_VERSION */: *v = 3; break;
    case 0x821C /* MINOR_VERSION */: *v = 3; break;
    default: *v = 0; break;
    }
}
static void APIENTRY sgl_glGetBooleanv(GLenum pname, GLboolean *v) {
    if (!v) return;
    if (pname == GL_COLOR_WRITEMASK) for (int i = 0; i < 4; i++) v[i] = g_sgls.cmask[i];
    else if (pname == GL_DEPTH_WRITEMASK) v[0] = (GLboolean)g_sgls.depth_mask;
    else v[0] = GL_FALSE;
}
static void APIENTRY sgl_glGetFloatv(GLenum pname, GLfloat *v) {
    if (!v) return;
    if (pname == GL_COLOR_CLEAR_VALUE) memcpy(v, g_sgls.clear_color, sizeof(g_sgls.clear_color));
    else if (pname == GL_LINE_WIDTH) v[0] = g_sgls.line_width;
    else v[0] = 0.0f;
}
static const GLubyte *APIENTRY sgl_glGetString(GLenum name) {
    static char ver[96];
    if (name == GL_VERSION) {
        snprintf(ver, sizeof(ver), "3.3 (%s)", sgl_impl_name());
        return (const GLubyte *)ver;
    }
    if (name == GL_RENDERER) return (const GLubyte *)(g_sgl.name[0] ? g_sgl.name : "sgl");
    if (name == GL_VENDOR) return (const GLubyte *)"psxrecomp sgl";
    if (name == 0x8B8C /* SHADING_LANGUAGE_VERSION */) return (const GLubyte *)"3.30";
    return (const GLubyte *)"";
}
static GLenum APIENTRY sgl_glGetError(void) { GLenum e = g_sgls.error; g_sgls.error = GL_NO_ERROR; return e; }
static void APIENTRY sgl_glFinish(void) { sgl_flush(1, 0); }
static void APIENTRY sgl_glFlush(void) { sgl_flush(0, 0); }

/* ---- sync objects ------------------------------------------------------- */
static GLsync APIENTRY sgl_glFenceSync(GLenum cond, GLbitfield flags) {
    (void)cond; (void)flags;
    GLuint n = tab_new(&s_sync);
    SglSync *s = (SglSync *)tab_get(&s_sync, n);
    if (s) s->stream = g_sgl_stream.id;
    return (GLsync)(uintptr_t)n;
}
/* One queue: everything later in the stream already runs after the fence. */
static void APIENTRY sgl_glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    (void)sync; (void)flags; (void)timeout;
}
static void APIENTRY sgl_glDeleteSync(GLsync sync) {
    SglSync *s = (SglSync *)tab_get(&s_sync, (GLuint)(uintptr_t)sync);
    if (s) s->used = 0;
}

/* ---- drawing ------------------------------------------------------------ */
extern SDL_GPUTexture *sgl_gpu_dummy_texture(int uint_kind);

/* GL_TRIANGLES / GL_LINES of the current program into the draw framebuffer. */
static void APIENTRY sgl_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (count <= 0) return;
    SglObj *p = sgl_obj(g_sgls.program);
    if (!p || !p->linked) return;
    SglTarget t;
    if (!resolve_target(&t)) return;
    SglVao *vao = cur_vao();
    /* The last pipelines by the state that went into them: the renderer
     * alternates a few states per batch (colour pass, mark passes), so a
     * short memory saves building and hashing the full key each draw. */
    const SglState *gs = &g_sgls;
    const uint64_t sig[6] = {
        (uint64_t)gs->bsrc_rgb | (uint64_t)gs->bdst_rgb << 16 | (uint64_t)gs->bsrc_a << 32 |
            (uint64_t)gs->bdst_a << 48,
        (uint64_t)gs->beq_rgb | (uint64_t)gs->beq_a << 16 | (uint64_t)gs->depth_func << 32 |
            (uint64_t)((gs->blend ? 1 : 0) | (gs->depth_test ? 2 : 0) | (gs->depth_mask ? 4 : 0) |
                       (gs->stencil_test ? 8 : 0) | gs->cmask[0] << 4 | gs->cmask[1] << 5 |
                       gs->cmask[2] << 6 | gs->cmask[3] << 7) << 48,
        (uint64_t)gs->st_func | (uint64_t)gs->st_fail << 16 | (uint64_t)gs->st_zfail << 32 |
            (uint64_t)gs->st_zpass << 48,
        (uint64_t)(gs->st_vmask & 0xFF) | (uint64_t)(gs->st_wmask & 0xFF) << 8 | (uint64_t)mode << 16 |
            (uint64_t)gs->vao << 32,
        (uint64_t)p->id | (uint64_t)gs->pipe_epoch << 32,
        (uint64_t)(t.color ? t.color_fmt + 1 : 0) | (uint64_t)(t.ds ? t.ds_fmt + 1 : 0) << 16 |
            (uint64_t)t.ds_has_stencil << 32,
    };
    enum { PC_N = 16 };
    static struct { uint64_t sig[6]; SDL_GPUGraphicsPipeline *pipe; GLuint bufs[4]; uint32_t bases[4]; int nbuf; } pc[PC_N];
    static int pc_next;
    GLuint bufs[4];
    uint32_t bases[4];
    int nbuf = 0;
    SDL_GPUGraphicsPipeline *pipe = NULL;
    for (int i = 0; i < PC_N; i++) {
        if (pc[i].pipe && memcmp(pc[i].sig, sig, sizeof(sig)) == 0) {
            pipe = pc[i].pipe;
            nbuf = pc[i].nbuf;
            memcpy(bufs, pc[i].bufs, sizeof(bufs));
            memcpy(bases, pc[i].bases, sizeof(bases));
            break;
        }
    }
    if (!pipe) {
        pipe = sgl_pipeline(p, vao, mode, &t, bufs, bases, &nbuf);
        if (!pipe) return;
        int i = pc_next;
        pc_next = (pc_next + 1) % PC_N;
        memcpy(pc[i].sig, sig, sizeof(sig));
        pc[i].pipe = pipe; pc[i].nbuf = nbuf;
        memcpy(pc[i].bufs, bufs, sizeof(bufs));
        memcpy(pc[i].bases, bases, sizeof(bases));
    }

    /* The textures the program samples: the shader's slots in order. */
    SDL_GPUTextureSamplerBinding tb[2][SGL_MAX_SAMPLERS];
    for (int s = 0; s < 2; s++) {
        for (int b = 0; b < p->nsamplers[s]; b++) {
            int unit = p->sampler_unit[s][b];
            SglImage *im = unit >= 0 && unit < SGL_UNITS ? sgl_image(g_sgls.unit_tex[unit], 0) : NULL;
            const int want_uint = p->sampler_uint[s][b];
            if (im && im->tex && ((im->kind == SGL_KIND_UINT) == want_uint)) {
                if (im->tex == t.color || (t.ds && im->tex == t.ds)) {
                    /* Sampling the surface being drawn: GL leaves it to the
                     * application; SDL_GPU cannot. Counted (TCP sgl). */
                    sgl_stats_note(SGL_ST_FEEDBACK);
                }
                GLenum minf = im->min_filter, magf = im->mag_filter;
                if (im->kind == SGL_KIND_UINT) minf = magf = GL_NEAREST;
                tb[s][b].texture = im->tex;
                tb[s][b].sampler = sgl_sampler(minf, magf, im->wrap_s, im->wrap_t);
            } else {
                tb[s][b].texture = sgl_gpu_dummy_texture(want_uint);
                tb[s][b].sampler = sgl_sampler(GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
            }
        }
    }

    SglOp *op = sgl_push_op(SGL_OP_DRAW);
    if (!op) return;
    op->tgt = t;
    op->u.draw.pipe = pipe;
    memcpy(op->u.draw.vp, g_sgls.vp, sizeof(op->u.draw.vp));
    if (g_sgls.scissor_test) {
        int x = g_sgls.sc[0], y = g_sgls.sc[1], w = g_sgls.sc[2], h = g_sgls.sc[3];
        clip_rect(&x, &y, &w, &h, t.w, t.h);
        op->u.draw.sc[0] = x; op->u.draw.sc[1] = y; op->u.draw.sc[2] = w; op->u.draw.sc[3] = h;
    } else {
        op->u.draw.sc[0] = 0; op->u.draw.sc[1] = 0; op->u.draw.sc[2] = t.w; op->u.draw.sc[3] = t.h;
    }
    if (op->u.draw.sc[2] <= 0 || op->u.draw.sc[3] <= 0) { g_sgl_stream.nops--; return; }
    op->u.draw.st_ref = (uint8_t)g_sgls.st_ref;
    memcpy(op->u.draw.bc, g_sgls.blend_color, sizeof(op->u.draw.bc));
    if (g_sgls.bsrc_rgb == 0x8003 || g_sgls.bsrc_rgb == 0x8004 ||
        g_sgls.bdst_rgb == 0x8003 || g_sgls.bdst_rgb == 0x8004) {
        /* GL_CONSTANT_ALPHA: the constant colour's channels all = its alpha. */
        op->u.draw.bc[0] = op->u.draw.bc[1] = op->u.draw.bc[2] = g_sgls.blend_color[3];
    }
    op->u.draw.first = first;
    op->u.draw.count = count;
    /* Vertex data: each buffer once per stream in the vertex arena. */
    op->u.draw.nvb = (uint8_t)nbuf;
    for (int i = 0; i < nbuf; i++) {
        if (bufs[i] == 0) { op->u.draw.vb_off[i] = UINT32_MAX; continue; }  /* zero slot */
        SglBuffer *b = buf_get(bufs[i]);
        if (!b || !b->data) { g_sgl_stream.nops--; return; }
        if (b->arena_stream != g_sgl_stream.id || b->arena_version != b->version) {
            b->arena_off = sgl_arena_put(&g_sgl_stream.vtx, b->data, b->size, 16);
            b->arena_stream = g_sgl_stream.id;
            b->arena_version = b->version;
        }
        op->u.draw.vb_off[i] = b->arena_off + bases[i];
    }
    /* Uniform blocks: copied when changed (or first used in this stream). */
    for (int s = 0; s < 2; s++) {
        op->u.draw.u_size[s] = 0;
        if (p->block_size[s] <= 0) continue;
        if (p->block_dirty[s] || p->block_stream[s] != g_sgl_stream.id) {
            p->block_off[s] = sgl_arena_put(&g_sgl_stream.uni, p->block[s], (size_t)p->block_size[s], 16);
            p->block_stream[s] = g_sgl_stream.id;
            p->block_dirty[s] = 0;
        }
        op->u.draw.u_off[s] = p->block_off[s];
        op->u.draw.u_size[s] = (uint32_t)p->block_size[s];
    }
    for (int s = 0; s < 2; s++) {
        SglStream *st = &g_sgl_stream;
        op->u.draw.nsamp[s] = (uint8_t)p->nsamplers[s];
        op->u.draw.samp_off[s] = (uint32_t)st->nbind;
        if (st->nbind + p->nsamplers[s] > st->cap_bind) {
            int nc = st->cap_bind ? st->cap_bind * 2 : 1024;
            while (nc < st->nbind + p->nsamplers[s]) nc *= 2;
            void *np = realloc(st->bind, sizeof(*st->bind) * (size_t)nc);
            if (!np) { st->nops--; return; }
            st->bind = (SDL_GPUTextureSamplerBinding *)np; st->cap_bind = nc;
        }
        memcpy(st->bind + st->nbind, tb[s], sizeof(tb[s][0]) * (size_t)p->nsamplers[s]);
        st->nbind += p->nsamplers[s];
    }
    sgl_stats_note(SGL_ST_DRAW);
}

/* Indexed draws (Dear ImGui's backend): SDL_GPU draws indexed, but the
 * buffers live in memory here, so the vertices are gathered in index order
 * into scratch buffers with the same layout (each record where the index
 * would have it) and drawn as arrays: no new pipeline, the same pixels. */
static GLuint s_gather[SGL_MAX_ATTRIBS];
static size_t attrib_bytes(const SglAttrib *a) {
    const int n = a->size ? a->size : 4;
    switch (a->type) {
    case GL_UNSIGNED_BYTE: case 0x1400 /* BYTE */: return (size_t)n;
    case 0x1402 /* SHORT */: case 0x1403 /* UNSIGNED_SHORT */: return (size_t)n * 2;
    default: return (size_t)n * 4;
    }
}
static void APIENTRY sgl_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
                                                  GLint basevertex) {
    if (count <= 0) return;
    SglVao *vao = cur_vao();
    SglBuffer *eb = buf_get(vao->element);
    const size_t isz = type == 0x1405 /* UNSIGNED_INT */ ? 4 : type == 0x1403 /* UNSIGNED_SHORT */ ? 2 : 1;
    const size_t ioff = (size_t)(uintptr_t)indices;
    if (!eb || !eb->data || ioff + (size_t)count * isz > eb->size) { sgl_set_error(GL_INVALID_OPERATION); return; }
    const uint8_t *ip = eb->data + ioff;
    GLuint src[SGL_MAX_ATTRIBS], saved[SGL_MAX_ATTRIBS];
    size_t stride[SGL_MAX_ATTRIBS], lo[SGL_MAX_ATTRIBS], hi[SGL_MAX_ATTRIBS];
    int nsrc = 0, grp[SGL_MAX_ATTRIBS];
    for (int i = 0; i < SGL_MAX_ATTRIBS; i++) {
        SglAttrib *a = &vao->a[i];
        saved[i] = a->buffer;
        grp[i] = -1;
        if (!a->enabled || !a->buffer) continue;
        const size_t bytes = attrib_bytes(a);
        const size_t st = a->stride ? (size_t)a->stride : bytes;
        int g = 0;
        while (g < nsrc && !(src[g] == a->buffer && stride[g] == st)) g++;
        if (g == nsrc) {
            src[g] = a->buffer; stride[g] = st; lo[g] = a->offset; hi[g] = a->offset + bytes;
            nsrc++;
        }
        if (a->offset < lo[g]) lo[g] = a->offset;
        if (a->offset + bytes > hi[g]) hi[g] = a->offset + bytes;
        grp[i] = g;
    }
    for (int g = 0; g < nsrc; g++) {
        SglBuffer *sb = buf_get(src[g]);
        if (!s_gather[g]) s_gather[g] = tab_new(&s_buf);
        SglBuffer *tb = buf_get(s_gather[g]);
        if (!sb || !tb) return;
        const size_t rec = hi[g] - lo[g];
        const size_t need = lo[g] + (size_t)count * stride[g] + rec;
        if (need > tb->size || !tb->data) {
            uint8_t *np = (uint8_t *)realloc(tb->data, need);
            if (!np) { sgl_set_error(GL_OUT_OF_MEMORY); return; }
            tb->data = np;
        }
        tb->size = need;
        for (GLsizei k = 0; k < count; k++) {
            const int64_t ix = (int64_t)(isz == 4 ? ((const uint32_t *)ip)[k]
                                         : isz == 2 ? ((const uint16_t *)ip)[k] : ip[k]) + basevertex;
            uint8_t *d = tb->data + lo[g] + (size_t)k * stride[g];
            const size_t so = lo[g] + (size_t)ix * stride[g];
            if (ix >= 0 && so + rec <= sb->size) memcpy(d, sb->data + so, rec);
            else memset(d, 0, rec);
        }
        tb->version++;
    }
    for (int i = 0; i < SGL_MAX_ATTRIBS; i++)
        if (grp[i] >= 0) vao->a[i].buffer = s_gather[grp[i]];
    sgl_glDrawArrays(mode, 0, count);
    for (int i = 0; i < SGL_MAX_ATTRIBS; i++) vao->a[i].buffer = saved[i];
}
static void APIENTRY sgl_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    sgl_glDrawElementsBaseVertex(mode, count, type, indices, 0);
}

/* ---- timer queries (GPU busy time, sgl_gpu.c) ------------------------------ */
static SglQuery *s_query_open;
static void APIENTRY sgl_glGenQueries(GLsizei n, GLuint *out) {
    for (int i = 0; i < n; i++) out[i] = tab_new(&s_query);
}
static void APIENTRY sgl_glDeleteQueries(GLsizei n, const GLuint *names) {
    for (int i = 0; i < n; i++) {
        SglQuery *q = (SglQuery *)tab_get(&s_query, names[i]);
        if (q) { if (q == s_query_open) s_query_open = NULL; q->used = 0; }
    }
}
/* The clock is read after what was recorded so far has gone to the GPU. */
static uint64_t query_mark(void) {
    sgl_gpu_timing_enable();
    sgl_flush(0, 0);
    return sgl_gpu_last_submit();
}
static void APIENTRY sgl_glBeginQuery(GLenum target, GLuint name) {
    SglQuery *q = (SglQuery *)tab_get(&s_query, name);
    if (!q) { sgl_set_error(GL_INVALID_OPERATION); return; }
    q->target = target;
    q->begin = query_mark();
    q->end = 0;
    q->placed = 0;
    s_query_open = q;
}
static void APIENTRY sgl_glEndQuery(GLenum target) {
    (void)target;
    SglQuery *q = s_query_open;
    s_query_open = NULL;
    if (!q) { sgl_set_error(GL_INVALID_OPERATION); return; }
    q->end = query_mark();
    q->placed = 1;
}
static void APIENTRY sgl_glQueryCounter(GLuint name, GLenum target) {
    SglQuery *q = (SglQuery *)tab_get(&s_query, name);
    if (!q) { sgl_set_error(GL_INVALID_OPERATION); return; }
    q->target = target;
    q->begin = 0;
    q->end = query_mark();
    q->placed = 1;
}
static int query_result(SglQuery *q, uint64_t *v, int wait) {
    if (!q || !q->placed) return 0;
    uint64_t a = 0, b = 0;
    if (!sgl_gpu_clock_at(q->end, &b, wait)) return 0;
    if (q->target == 0x8E28 /* TIMESTAMP */) { *v = b; return 1; }
    if (!sgl_gpu_clock_at(q->begin, &a, wait)) return 0;
    *v = b > a ? b - a : 0;
    return 1;
}
static void APIENTRY sgl_glGetQueryObjectui64v(GLuint name, GLenum pname, GLuint64 *v) {
    SglQuery *q = (SglQuery *)tab_get(&s_query, name);
    uint64_t r = 0;
    if (pname == 0x8867 /* RESULT_AVAILABLE */) *v = query_result(q, &r, 0) ? 1 : 0;
    else *v = query_result(q, &r, 1) ? r : 0;
}
static void APIENTRY sgl_glGetQueryObjectiv(GLuint name, GLenum pname, GLint *v) {
    GLuint64 r = 0;
    sgl_glGetQueryObjectui64v(name, pname, &r);
    *v = (GLint)r;
}

/* An internal program's draw: a full-target triangle, scissored to rect. */
static void internal_draw(int which, const SglTarget *t, int cmask, int dwrite, int smask, uint8_t sref,
                          const int rect[4], const void *ublock, int usize,
                          const SDL_GPUTextureSamplerBinding *samp, int nsamp) {
    SDL_GPUGraphicsPipeline *pipe = sgl_internal_pipeline(which, t, cmask, dwrite, smask);
    if (!pipe) return;
    SglOp *op = sgl_push_op(SGL_OP_DRAW);
    if (!op) return;
    op->tgt = *t;
    op->u.draw.pipe = pipe;
    op->u.draw.vp[0] = 0; op->u.draw.vp[1] = 0; op->u.draw.vp[2] = t->w; op->u.draw.vp[3] = t->h;
    memcpy(op->u.draw.sc, rect, sizeof(op->u.draw.sc));
    op->u.draw.st_ref = sref;
    op->u.draw.first = 0; op->u.draw.count = 3;
    op->u.draw.nvb = 0;
    op->u.draw.u_size[0] = 0;
    op->u.draw.u_off[1] = sgl_arena_put(&g_sgl_stream.uni, ublock, (size_t)usize, 16);
    op->u.draw.u_size[1] = (uint32_t)usize;
    op->u.draw.nsamp[0] = 0;
    op->u.draw.nsamp[1] = (uint8_t)nsamp;
    SglStream *st = &g_sgl_stream;
    op->u.draw.samp_off[1] = (uint32_t)st->nbind;
    if (nsamp) {
        if (st->nbind + nsamp > st->cap_bind) {
            int nc = st->cap_bind ? st->cap_bind * 2 : 1024;
            void *np = realloc(st->bind, sizeof(*st->bind) * (size_t)nc);
            if (!np) { st->nops--; return; }
            st->bind = (SDL_GPUTextureSamplerBinding *)np; st->cap_bind = nc;
        }
        memcpy(st->bind + st->nbind, samp, sizeof(*samp) * (size_t)nsamp);
        st->nbind += nsamp;
    }
    sgl_stats_note(SGL_ST_DRAW);
}

static void APIENTRY sgl_glClear(GLbitfield mask) {
    SglTarget t;
    if (!resolve_target(&t)) return;
    const int cm = (g_sgls.cmask[0] ? 1 : 0) | (g_sgls.cmask[1] ? 2 : 0) |
                   (g_sgls.cmask[2] ? 4 : 0) | (g_sgls.cmask[3] ? 8 : 0);
    const int c = (mask & GL_COLOR_BUFFER_BIT) && t.color && cm;
    const int d = (mask & GL_DEPTH_BUFFER_BIT) && t.ds && g_sgls.depth_mask;
    const int s = (mask & GL_STENCIL_BUFFER_BIT) && t.ds && t.ds_has_stencil && (g_sgls.st_wmask & 0xFF);
    if (!c && !d && !s) return;
    int rect[4] = {0, 0, t.w, t.h};
    if (g_sgls.scissor_test) {
        rect[0] = g_sgls.sc[0]; rect[1] = g_sgls.sc[1]; rect[2] = g_sgls.sc[2]; rect[3] = g_sgls.sc[3];
        clip_rect(&rect[0], &rect[1], &rect[2], &rect[3], t.w, t.h);
        if (rect[2] <= 0 || rect[3] <= 0) return;
    }
    const int whole = rect[0] == 0 && rect[1] == 0 && rect[2] == t.w && rect[3] == t.h &&
                      (!c || cm == 15) && (!s || (g_sgls.st_wmask & 0xFF) == 0xFF);
    if (whole) {
        SglOp *op = sgl_push_op(SGL_OP_CLEAR);
        if (!op) return;
        op->tgt = t;
        op->u.clear.color = c; op->u.clear.depth = d; op->u.clear.stencil = s;
        memcpy(op->u.clear.rgba, g_sgls.clear_color, sizeof(op->u.clear.rgba));
        op->u.clear.d = (float)g_sgls.clear_depth;
        op->u.clear.s = (uint8_t)g_sgls.clear_stencil;
        return;
    }
    /* Part of the target: a triangle that writes the clear values there. */
    struct { float rgba[4]; float depth[4]; } ub;
    memcpy(ub.rgba, g_sgls.clear_color, sizeof(ub.rgba));
    ub.depth[0] = (float)g_sgls.clear_depth; ub.depth[1] = ub.depth[2] = ub.depth[3] = 0;
    internal_draw(SGL_IP_CLEAR, &t, c ? cm : 0, d, s ? (int)(g_sgls.st_wmask & 0xFF) : 0,
                  (uint8_t)g_sgls.clear_stencil, rect, &ub, (int)sizeof(ub), NULL, 0);
}

/* ---- glBlitFramebuffer -------------------------------------------------- */
static void APIENTRY sgl_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
                                           GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
    const int linear = filter == GL_LINEAR;
    if (mask & GL_COLOR_BUFFER_BIT) {
        SglImage *src = fbo_color_image(g_sgls.read_fbo, 1);
        SglImage *dst = fbo_color_image(g_sgls.draw_fbo, 0);
        if (src && dst) {
            const int sw = sx1 - sx0, sh = sy1 - sy0, dw = dx1 - dx0, dh = dy1 - dy0;
            const int flip_x = (sw < 0) != (dw < 0), flip_y = (sh < 0) != (dh < 0);
            int ax = sw < 0 ? sx1 : sx0, ay = sh < 0 ? sy1 : sy0, aw = abs(sw), ah = abs(sh);
            int bx = dw < 0 ? dx1 : dx0, by = dh < 0 ? dy1 : dy0, bw = abs(dw), bh = abs(dh);
            int scissored = 0;
            if (g_sgls.scissor_test) {
                int x = g_sgls.sc[0], y = g_sgls.sc[1], w = g_sgls.sc[2], h = g_sgls.sc[3];
                scissored = !(x <= bx && y <= by && x + w >= bx + bw && y + h >= by + bh);
            }
            const int inside = ax >= 0 && ay >= 0 && ax + aw <= src->w && ay + ah <= src->h &&
                               bx >= 0 && by >= 0 && bx + bw <= dst->w && by + bh <= dst->h;
            if (!scissored && inside && aw == bw && ah == bh && !flip_x && !flip_y && src->fmt == dst->fmt) {
                copy_color(src, ax, ay, dst, bx, by, aw, ah);
            } else if (!scissored && inside && src != dst && src->kind == SGL_KIND_COLOR &&
                       dst->kind == SGL_KIND_COLOR) {
                SglOp *op = sgl_push_op(SGL_OP_BLIT);
                if (op) {
                    op->u.blit.src = src->tex; op->u.blit.dst = dst->tex;
                    op->u.blit.sx = ax; op->u.blit.sy = ay; op->u.blit.sw = aw; op->u.blit.sh = ah;
                    op->u.blit.dx = bx; op->u.blit.dy = by; op->u.blit.dw = bw; op->u.blit.dh = bh;
                    op->u.blit.flip = (flip_x ? SDL_FLIP_HORIZONTAL : 0) | (flip_y ? SDL_FLIP_VERTICAL : 0);
                    op->u.blit.linear = linear;
                    op->u.blit.src_w = src->w; op->u.blit.src_h = src->h;
                    op->u.blit.dst_w = dst->w; op->u.blit.dst_h = dst->h;
                    sgl_stats_note(SGL_ST_BLIT);
                }
            } else {
                /* Scissored, partly outside, or in place: a draw that samples
                 * the source like GL's blit (pixel centres mapped linearly). */
                SglImage *from = src;
                SglImage tmp; memset(&tmp, 0, sizeof(tmp));
                if (src == dst) {
                    tmp.used = 1;
                    if (!sgl_gpu_alloc_image(&tmp, src->ifmt, src->w, src->h)) goto color_done;
                    push_copy(src->tex, tmp.tex, 0, 0, 0, 0, src->w, src->h);
                    from = &tmp;
                }
                SglTarget t; memset(&t, 0, sizeof(t));
                t.color = dst->tex; t.color_fmt = dst->fmt; t.w = dst->w; t.h = dst->h;
                int rect[4] = {bx, by, bw, bh};
                if (g_sgls.scissor_test) {
                    int x0 = g_sgls.sc[0] > bx ? g_sgls.sc[0] : bx, y0 = g_sgls.sc[1] > by ? g_sgls.sc[1] : by;
                    int x1 = g_sgls.sc[0] + g_sgls.sc[2], y1 = g_sgls.sc[1] + g_sgls.sc[3];
                    if (x1 > bx + bw) x1 = bx + bw;
                    if (y1 > by + bh) y1 = by + bh;
                    rect[0] = x0; rect[1] = y0; rect[2] = x1 - x0; rect[3] = y1 - y0;
                }
                clip_rect(&rect[0], &rect[1], &rect[2], &rect[3], t.w, t.h);
                if (rect[2] > 0 && rect[3] > 0) {
                    struct { float src[4]; float dst[4]; float size[4]; } ub = {
                        {(float)sx0, (float)sy0, (float)sx1, (float)sy1},
                        {(float)dx0, (float)dy0, (float)dx1, (float)dy1},
                        {(float)from->w, (float)from->h, (float)linear, 0}};
                    SDL_GPUTextureSamplerBinding sb;
                    sb.texture = from->tex;
                    sb.sampler = sgl_sampler(linear ? GL_LINEAR : GL_NEAREST, linear ? GL_LINEAR : GL_NEAREST,
                                             GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
                    internal_draw(from->kind == SGL_KIND_UINT ? SGL_IP_COLOR_BLIT_U : SGL_IP_COLOR_BLIT,
                                  &t, 15, 0, 0, 0, rect, &ub, (int)sizeof(ub), &sb, 1);
                }
                if (tmp.tex) sgl_release_texture(tmp.tex);
            }
        }
    }
color_done:
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        SglImage *src = fbo_ds_image(g_sgls.read_fbo);
        SglImage *dst = fbo_ds_image(g_sgls.draw_fbo);
        if (!src || !dst) return;
        const int same = (sx1 - sx0) == (dx1 - dx0) && (sy1 - sy0) == (dy1 - dy0) &&
                         sx1 > sx0 && sy1 > sy0 && src->fmt == dst->fmt && src != dst;
        const int want_d = (mask & GL_DEPTH_BUFFER_BIT) != 0;
        const int want_s = (mask & GL_STENCIL_BUFFER_BIT) != 0 && src->kind == SGL_KIND_DEPTH_STENCIL;
        const int has_s = src->kind == SGL_KIND_DEPTH_STENCIL;
        if (same && want_d && (want_s || !has_s)) {
            /* Depth (+ stencil) 1:1: a copy of both aspects. */
            int ax = sx0, ay = sy0, w = sx1 - sx0, h = sy1 - sy0, bx = dx0, by = dy0;
            clip_rect(&ax, &ay, &w, &h, src->w, src->h);
            push_copy(src->tex, dst->tex, ax, ay, bx + (ax - sx0), by + (ay - sy0), w, h);
            return;
        }
        if (want_s && !want_d && same) {
            /* Stencil only, 1:1: copy both aspects, then put the
             * destination's depth back (sampled from a copy of it). */
            int ax = sx0, ay = sy0, w = sx1 - sx0, h = sy1 - sy0;
            clip_rect(&ax, &ay, &w, &h, src->w, src->h);
            int bx = dx0 + (ax - sx0), by = dy0 + (ay - sy0);
            SglImage keep; memset(&keep, 0, sizeof(keep)); keep.used = 1;
            if (!sgl_gpu_alloc_image(&keep, dst->ifmt, w, h)) return;
            push_copy(dst->tex, keep.tex, bx, by, 0, 0, w, h);
            push_copy(src->tex, dst->tex, ax, ay, bx, by, w, h);
            {
                SglTarget t; memset(&t, 0, sizeof(t));
                t.ds = dst->tex; t.ds_fmt = dst->fmt; t.ds_has_stencil = 1; t.w = dst->w; t.h = dst->h;
                int rect[4] = {bx, by, w, h};
                clip_rect(&rect[0], &rect[1], &rect[2], &rect[3], t.w, t.h);
                struct { float src[4]; float dst[4]; float size[4]; } ub = {
                    {0, 0, (float)w, (float)h}, {(float)bx, (float)by, (float)(bx + w), (float)(by + h)},
                    {(float)w, (float)h, 0, 0}};
                SDL_GPUTextureSamplerBinding sb = {keep.tex,
                    sgl_sampler(GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE)};
                internal_draw(SGL_IP_DEPTH_BLIT, &t, 0, 1, 0, 0, rect, &ub, (int)sizeof(ub), &sb, 1);
            }
            sgl_release_texture(keep.tex);
            return;
        }
        if (want_d) {
            /* Depth, scaled and/or flipped: a draw writing gl_FragDepth. */
            SglTarget t; memset(&t, 0, sizeof(t));
            t.ds = dst->tex; t.ds_fmt = dst->fmt; t.ds_has_stencil = dst->kind == SGL_KIND_DEPTH_STENCIL;
            t.w = dst->w; t.h = dst->h;
            int bx = dx0 < dx1 ? dx0 : dx1, by = dy0 < dy1 ? dy0 : dy1;
            int rect[4] = {bx, by, abs(dx1 - dx0), abs(dy1 - dy0)};
            clip_rect(&rect[0], &rect[1], &rect[2], &rect[3], t.w, t.h);
            if (rect[2] <= 0 || rect[3] <= 0) return;
            struct { float src[4]; float dst[4]; float size[4]; } ub = {
                {(float)sx0, (float)sy0, (float)sx1, (float)sy1},
                {(float)dx0, (float)dy0, (float)dx1, (float)dy1},
                {(float)src->w, (float)src->h, 0, 0}};
            SglImage *from = src;
            SglImage tmp; memset(&tmp, 0, sizeof(tmp));
            if (src == dst) {
                tmp.used = 1;
                if (!sgl_gpu_alloc_image(&tmp, src->ifmt, src->w, src->h)) return;
                push_copy(src->tex, tmp.tex, 0, 0, 0, 0, src->w, src->h);
                from = &tmp;
            }
            SDL_GPUTextureSamplerBinding sb = {from->tex,
                sgl_sampler(GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE)};
            internal_draw(SGL_IP_DEPTH_BLIT, &t, 0, 1, 0, 0, rect, &ub, (int)sizeof(ub), &sb, 1);
            if (tmp.tex) sgl_release_texture(tmp.tex);
        }
    }
}

/* ---- the name -> function table ----------------------------------------- */
typedef struct { const char *name; void *fn; } SglProc;
#define P(f) { #f, (void *)sgl_##f }
static const SglProc s_procs[] = {
    P(glDisable), P(glEnable), P(glIsEnabled), P(glBindTexture), P(glTexParameteri),
    P(glGetTexParameteriv), P(glGetTexLevelParameteriv), P(glGetIntegerv), P(glGetBooleanv),
    P(glGetFloatv), P(glGetString), P(glGetError), P(glViewport), P(glScissor), P(glDrawArrays),
    P(glPixelStorei), P(glGenTextures), P(glDeleteTextures), P(glTexImage2D), P(glTexSubImage2D),
    P(glCopyTexImage2D), P(glCopyTexSubImage2D), P(glGetTexImage), P(glReadPixels), P(glReadBuffer),
    P(glDrawBuffer), P(glClear), P(glClearColor), P(glClearDepth), P(glClearStencil), P(glColorMask),
    P(glDepthMask), P(glDepthFunc), P(glStencilMask), P(glStencilFunc), P(glStencilOp), P(glBlendFunc),
    P(glLineWidth), P(glFinish), P(glFlush),
    P(glCreateShader), P(glShaderSource), P(glCompileShader), P(glGetShaderiv), P(glGetShaderInfoLog),
    P(glDeleteShader), P(glDeleteProgram), P(glCreateProgram), P(glAttachShader), P(glLinkProgram),
    P(glGetProgramiv), P(glGetProgramInfoLog), P(glUseProgram), P(glGetUniformLocation),
    P(glUniform1i), P(glUniform1f), P(glUniform2i), P(glUniform4i), P(glUniform2f), P(glUniform4f),
    P(glUniform3f), P(glUniform4fv), P(glBlendColor), P(glBlendFuncSeparate), P(glBlendEquationSeparate),
    P(glGenVertexArrays), P(glBindVertexArray), P(glActiveTexture), P(glGenBuffers), P(glDeleteBuffers),
    P(glDeleteVertexArrays), P(glBindBuffer), P(glBufferData), P(glVertexAttribPointer),
    P(glEnableVertexAttribArray), P(glBindFragDataLocationIndexed), P(glGenFramebuffers),
    P(glBufferSubData), P(glDisableVertexAttribArray), P(glGetVertexAttribiv), P(glGetVertexAttribPointerv),
    P(glDrawElements), P(glDrawElementsBaseVertex), P(glBlendEquation), P(glBindSampler), P(glPolygonMode),
    P(glIsProgram), P(glDetachShader), P(glGetAttribLocation), P(glGetStringi), P(glUniformMatrix4fv),
    P(glGenQueries), P(glDeleteQueries), P(glBeginQuery), P(glEndQuery), P(glQueryCounter),
    P(glGetQueryObjectui64v), P(glGetQueryObjectiv),
    P(glDeleteFramebuffers), P(glBindFramebuffer), P(glFramebufferTexture2D), P(glCheckFramebufferStatus),
    P(glBlitFramebuffer), P(glGenRenderbuffers), P(glDeleteRenderbuffers), P(glBindRenderbuffer),
    P(glRenderbufferStorage), P(glFramebufferRenderbuffer), P(glGetActiveUniform), P(glGetUniformfv),
    P(glGetUniformiv), P(glMapBuffer), P(glUnmapBuffer), P(glFenceSync), P(glWaitSync), P(glDeleteSync),
    P(glGetFramebufferAttachmentParameteriv),
};
#undef P

void *sgl_get_proc(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(s_procs) / sizeof(s_procs[0]); i++)
        if (strcmp(s_procs[i].name, name) == 0) return s_procs[i].fn;
    return NULL;   /* the timer queries, and anything the renderer does not use */
}

void sgl_follow_window(void) { back_follow_window(); }

/* Default GL state, at context creation. */
void sgl_state_reset(void) {
    memset(&g_sgls, 0, sizeof(g_sgls));
    g_sgls.bsrc_rgb = g_sgls.bsrc_a = GL_ONE;
    g_sgls.bdst_rgb = g_sgls.bdst_a = GL_ZERO;
    g_sgls.beq_rgb = g_sgls.beq_a = 0x8006; /* FUNC_ADD */
    g_sgls.depth_func = GL_LESS;
    g_sgls.depth_mask = 1;
    g_sgls.st_func = GL_ALWAYS; g_sgls.st_vmask = 0xFFFFFFFFu; g_sgls.st_wmask = 0xFFFFFFFFu;
    g_sgls.st_fail = g_sgls.st_zfail = g_sgls.st_zpass = GL_KEEP;
    g_sgls.cmask[0] = g_sgls.cmask[1] = g_sgls.cmask[2] = g_sgls.cmask[3] = 1;
    g_sgls.clear_depth = 1.0;
    g_sgls.line_width = 1.0f;
    g_sgls.pack_align = 4; g_sgls.unpack_align = 4;
    back_follow_window();
    if (g_sgl.back.tex) {
        g_sgls.vp[2] = g_sgls.sc[2] = g_sgl.back.w;
        g_sgls.vp[3] = g_sgls.sc[3] = g_sgl.back.h;
    }
}
