/* sgl_gpu.c — the SDL_GPU side of the sgl layer: device and window, images,
 * samplers, pipelines, and the encoding of the recorded stream.
 *
 * A flush turns the stream (sgl_internal.h) into one command buffer: the
 * vertex arena and the upload arena go up once, then the ops run in order —
 * consecutive draws/clears on the same target share one render pass, copies,
 * uploads and downloads share copy passes between them. A swap appends the
 * copy of the default framebuffer to the window (flipped: GL's row 0 is the
 * bottom one). */
#include "sgl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SglContext g_sgl;
static char s_driver[32];
static int s_hdr_req = -1;
void sgl_set_hdr(int on) { s_hdr_req = on ? 1 : 0; }
static uint64_t s_stats[SGL_ST_COUNT];

void sgl_state_reset(void);

void sgl_stats_note(int what) { if (what >= 0 && what < SGL_ST_COUNT) s_stats[what]++; }

const SglCaps *sgl_caps(void) { return &g_sgl.caps; }

const char *sgl_impl_name(void) {
    static char name[64];
    if (!g_sgl.dev) return "sdl_gpu";
    snprintf(name, sizeof(name), "sdl_gpu (%s)", SDL_GetGPUDeviceDriver(g_sgl.dev));
    return name;
}

void sgl_set_driver(const char *name) {
    snprintf(s_driver, sizeof(s_driver), "%s", name ? name : "");
}
static const char *driver_wanted(void) {
    const char *e = getenv("PSX_SGL_DRIVER");
    if (e && e[0]) return e;
    if (s_driver[0] && strcmp(s_driver, "auto") != 0) return s_driver;
#if defined(_WIN32) || defined(__linux__)
    return "vulkan";        /* SPIR-V straight from glslang */
#else
    return NULL;
#endif
}
uint64_t sgl_window_flags(void) {
    const char *d = driver_wanted();
    if (d && strcmp(d, "vulkan") == 0) return SDL_WINDOW_VULKAN;
    if (d && strcmp(d, "metal") == 0) return SDL_WINDOW_METAL;
#if defined(__APPLE__)
    return SDL_WINDOW_METAL;
#else
    return 0;
#endif
}

/* ---- arenas and ops ------------------------------------------------------ */
void *sgl_arena_reserve(SglBytes *a, size_t n, size_t align, uint32_t *off) {
    size_t at = (a->n + align - 1) / align * align;
    if (at + n > a->cap) {
        size_t nc = a->cap ? a->cap : (size_t)1 << 20;
        while (nc < at + n) nc *= 2;
        uint8_t *p = (uint8_t *)realloc(a->p, nc);
        if (!p) return NULL;
        a->p = p; a->cap = nc;
    }
    a->n = at + n;
    if (off) *off = (uint32_t)at;
    return a->p + at;
}
uint32_t sgl_arena_put(SglBytes *a, const void *src, size_t n, size_t align) {
    uint32_t off = 0;
    void *dst = sgl_arena_reserve(a, n ? n : 1, align, &off);
    if (dst && n) memcpy(dst, src, n);
    return off;
}
SglOp *sgl_push_op(int type) {
    SglStream *s = &g_sgl_stream;
    if (s->nops >= s->cap_ops) {
        int nc = s->cap_ops ? s->cap_ops * 2 : 4096;
        SglOp *p = (SglOp *)realloc(s->ops, sizeof(SglOp) * (size_t)nc);
        if (!p) return NULL;
        s->ops = p; s->cap_ops = nc;
    }
    SglOp *op = &s->ops[s->nops++];
    memset(op, 0, sizeof(*op));
    op->type = (uint8_t)type;
    return op;
}
void sgl_release_texture(SDL_GPUTexture *t) {
    if (!t) return;
    SglStream *s = &g_sgl_stream;
    if (s->nrelease >= s->cap_release) {
        int nc = s->cap_release ? s->cap_release * 2 : 64;
        SDL_GPUTexture **p = (SDL_GPUTexture **)realloc(s->release, sizeof(*p) * (size_t)nc);
        if (!p) { SDL_ReleaseGPUTexture(g_sgl.dev, t); return; }
        s->release = p; s->cap_release = nc;
    }
    s->release[s->nrelease++] = t;
}

/* ---- images ------------------------------------------------------------- */
typedef struct { GLenum gl; SDL_GPUTextureFormat fmt; int kind, bpp; } SglFmt;
static int fmt_for(GLenum ifmt, SglFmt *out) {
    out->gl = ifmt;
    switch (ifmt) {
    case GL_RGBA8: out->fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; out->kind = SGL_KIND_COLOR; out->bpp = 4; return 1;
    case 0x881A: out->fmt = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; out->kind = SGL_KIND_COLOR; out->bpp = 8; return 1;
    case 0x8814: out->fmt = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT; out->kind = SGL_KIND_COLOR; out->bpp = 16; return 1;
    case 0x8229: out->fmt = SDL_GPU_TEXTUREFORMAT_R8_UNORM; out->kind = SGL_KIND_COLOR; out->bpp = 1; return 1;
    case 0x8230: out->fmt = SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT; out->kind = SGL_KIND_COLOR; out->bpp = 8; return 1;
    case 0x822E: out->fmt = SDL_GPU_TEXTUREFORMAT_R32_FLOAT; out->kind = SGL_KIND_COLOR; out->bpp = 4; return 1;
    case 0x822F: out->fmt = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT; out->kind = SGL_KIND_COLOR; out->bpp = 4; return 1;
    case 0x822D: out->fmt = SDL_GPU_TEXTUREFORMAT_R16_FLOAT; out->kind = SGL_KIND_COLOR; out->bpp = 2; return 1;
    case 0x8059: out->fmt = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM; out->kind = SGL_KIND_COLOR; out->bpp = 4; return 1;
    case 0x8234: out->fmt = SDL_GPU_TEXTUREFORMAT_R16_UINT; out->kind = SGL_KIND_UINT; out->bpp = 2; return 1;
    case 0x88F0: out->fmt = g_sgl.ds_fmt; out->kind = SGL_KIND_DEPTH_STENCIL; out->bpp = 4; return 1;
    case 0x81A6: out->fmt = g_sgl.d_fmt; out->kind = SGL_KIND_DEPTH; out->bpp = 4; return 1;
    case 0x8CAC: out->fmt = SDL_GPU_TEXTUREFORMAT_D32_FLOAT; out->kind = SGL_KIND_DEPTH; out->bpp = 4; return 1;
    default: return 0;
    }
}

int sgl_gpu_alloc_image(SglImage *im, GLenum ifmt, int w, int h) {
    SglFmt f;
    if (!fmt_for(ifmt, &f)) {
        fprintf(stdout, "psxrecomp: sgl: texture format 0x%04X not offered, using RGBA8\n", (unsigned)ifmt);
        fmt_for(GL_RGBA8, &f);
        ifmt = GL_RGBA8;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    SDL_GPUTextureCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = f.fmt;
    if (f.kind == SGL_KIND_DEPTH || f.kind == SGL_KIND_DEPTH_STENCIL)
        ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    else
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (!SDL_GPUTextureSupportsFormat(g_sgl.dev, ci.format, ci.type, ci.usage)) {
        ci.usage &= ~SDL_GPU_TEXTUREUSAGE_SAMPLER;
        if (!SDL_GPUTextureSupportsFormat(g_sgl.dev, ci.format, ci.type, ci.usage)) {
            fprintf(stdout, "psxrecomp: sgl: format %d unsupported for 0x%04X\n", (int)ci.format, (unsigned)ifmt);
            return 0;
        }
    }
    ci.width = (Uint32)w;
    ci.height = (Uint32)h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *t = SDL_CreateGPUTexture(g_sgl.dev, &ci);
    if (!t) {
        fprintf(stdout, "psxrecomp: sgl: texture %dx%d failed: %s\n", w, h, SDL_GetError());
        return 0;
    }
    if (im->tex) sgl_release_texture(im->tex);
    im->tex = t;
    im->fmt = f.fmt;
    im->ifmt = ifmt;
    im->kind = f.kind;
    im->bpp = f.bpp;
    im->w = w; im->h = h;
    im->gen++;
    return 1;
}

static int fmt_bpp(SDL_GPUTextureFormat fmt) {
    switch (fmt) {
    case SDL_GPU_TEXTUREFORMAT_R16_UINT: case SDL_GPU_TEXTUREFORMAT_R16_FLOAT: return 2;
    case SDL_GPU_TEXTUREFORMAT_R8_UNORM: return 1;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT: case SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT: return 8;
    case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 4;
    }
}
static int fmt_is_uint(SDL_GPUTextureFormat fmt) {
    return fmt == SDL_GPU_TEXTUREFORMAT_R16_UINT || fmt == SDL_GPU_TEXTUREFORMAT_R8_UINT ||
           fmt == SDL_GPU_TEXTUREFORMAT_R32_UINT || fmt == SDL_GPU_TEXTUREFORMAT_R16G16_UINT;
}

/* 1x1 textures for slots the program samples but GL leaves unbound
 * (GL returns 0,0,0,1 there). */
static SDL_GPUTexture *s_dummy[2];
SDL_GPUTexture *sgl_gpu_dummy_texture(int uint_kind) { return s_dummy[uint_kind ? 1 : 0]; }

/* ---- samplers ------------------------------------------------------------ */
typedef struct { GLenum minf, magf, ws, wt; SDL_GPUSampler *s; } SglSamplerEntry;
static SglSamplerEntry s_samplers[64];
static int s_nsamplers;
static SDL_GPUSamplerAddressMode wrap_of(GLenum w) {
    if (w == GL_REPEAT) return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    if (w == 0x8370 /* MIRRORED_REPEAT */) return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
}
static SDL_GPUFilter filter_of(GLenum f) {
    return (f == GL_LINEAR || f == GL_LINEAR_MIPMAP_LINEAR || f == GL_LINEAR_MIPMAP_NEAREST)
           ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
}
SDL_GPUSampler *sgl_sampler(GLenum minf, GLenum magf, GLenum ws, GLenum wt) {
    /* No mipmaps here: a mipmap minifying filter is its base level's. */
    if (minf == GL_NEAREST_MIPMAP_NEAREST || minf == GL_NEAREST_MIPMAP_LINEAR) minf = GL_NEAREST;
    if (minf == GL_LINEAR_MIPMAP_NEAREST || minf == GL_LINEAR_MIPMAP_LINEAR) minf = GL_LINEAR;
    for (int i = 0; i < s_nsamplers; i++) {
        SglSamplerEntry *e = &s_samplers[i];
        if (e->minf == minf && e->magf == magf && e->ws == ws && e->wt == wt) return e->s;
    }
    SDL_GPUSamplerCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.min_filter = filter_of(minf);
    ci.mag_filter = filter_of(magf);
    ci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    ci.address_mode_u = wrap_of(ws);
    ci.address_mode_v = wrap_of(wt);
    ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ci.min_lod = 0.0f;
    ci.max_lod = 0.0f;
    SDL_GPUSampler *s = SDL_CreateGPUSampler(g_sgl.dev, &ci);
    if (s && s_nsamplers < (int)(sizeof(s_samplers) / sizeof(s_samplers[0]))) {
        SglSamplerEntry *e = &s_samplers[s_nsamplers++];
        e->minf = minf; e->magf = magf; e->ws = ws; e->wt = wt; e->s = s;
    }
    return s;
}

/* ---- pipelines ----------------------------------------------------------- */
typedef struct SglPipeKey {
    uint32_t prog;
    uint8_t topo, has_color, has_ds, ds_stencil;
    uint32_t color_fmt, ds_fmt;
    uint8_t blend, cmask, depth_test, depth_write, stencil_test;
    uint8_t bsr, bdr, bsa, bda, ber, bea;
    uint8_t dfunc, sfunc, sfail, szfail, szpass, svmask, swmask;
    uint8_t nslots;
    uint16_t pitch[4];
    uint8_t rate[4];
    uint8_t nattr;
    struct { uint8_t loc, fmt, slot, pad; uint16_t off; } attr[SGL_MAX_ATTRIBS];
} SglPipeKey;

typedef struct { SglPipeKey key; SDL_GPUGraphicsPipeline *pipe; int used; } SglPipeEntry;
static SglPipeEntry *s_pipes;
static int s_pipe_cap, s_pipe_n;
static SDL_GPUGraphicsPipeline **s_pipe_release;
static int s_pipe_nrelease, s_pipe_cap_release;

static uint32_t key_hash(const SglPipeKey *k) {
    const uint8_t *p = (const uint8_t *)k;
    uint64_t h = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= sizeof(*k); i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < sizeof(*k); i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return (uint32_t)(h ^ (h >> 29));
}

static uint8_t blendf(GLenum f) {
    switch (f) {
    case GL_ZERO: return SDL_GPU_BLENDFACTOR_ZERO;
    case GL_ONE: return SDL_GPU_BLENDFACTOR_ONE;
    case GL_SRC_COLOR: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case GL_DST_COLOR: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case 0x8001: case 0x8003: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;      /* (const alpha: constants = a) */
    case 0x8002: case 0x8004: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    case GL_SRC_ALPHA_SATURATE: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case 0x8589: /* SRC1_ALPHA: not offered (sgl_caps); the renderer avoids it */
        sgl_stats_note(SGL_ST_FEEDBACK);
        return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    default: return SDL_GPU_BLENDFACTOR_ONE;
    }
}
static uint8_t blendop(GLenum e) {
    switch (e) {
    case 0x800A: return SDL_GPU_BLENDOP_SUBTRACT;
    case 0x800B: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case 0x8007: return SDL_GPU_BLENDOP_MIN;
    case 0x8008: return SDL_GPU_BLENDOP_MAX;
    default: return SDL_GPU_BLENDOP_ADD;
    }
}
static uint8_t cmpop(GLenum f) {
    switch (f) {
    case GL_NEVER: return SDL_GPU_COMPAREOP_NEVER;
    case GL_LESS: return SDL_GPU_COMPAREOP_LESS;
    case GL_EQUAL: return SDL_GPU_COMPAREOP_EQUAL;
    case GL_LEQUAL: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case GL_GREATER: return SDL_GPU_COMPAREOP_GREATER;
    case GL_NOTEQUAL: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case GL_GEQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default: return SDL_GPU_COMPAREOP_ALWAYS;
    }
}
static uint8_t stencilop(GLenum o) {
    switch (o) {
    case GL_ZERO: return SDL_GPU_STENCILOP_ZERO;
    case GL_REPLACE: return SDL_GPU_STENCILOP_REPLACE;
    case GL_INCR: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
    case GL_DECR: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
    case GL_INVERT: return SDL_GPU_STENCILOP_INVERT;
    case 0x8507: return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
    case 0x8508: return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
    default: return SDL_GPU_STENCILOP_KEEP;
    }
}
static uint8_t vfmt(const SglAttrib *a) {
    if (a->type == GL_FLOAT) {
        switch (a->size) {
        case 1: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
        case 2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        case 3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        default: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        }
    }
    if (a->type == GL_UNSIGNED_BYTE && a->normalized)
        return a->size <= 2 ? SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM : SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    if (a->type == GL_SHORT && a->normalized)
        return a->size <= 2 ? SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM : SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM;
    return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;   /* the renderer only uses GL_FLOAT */
}
static int vfmt_size(const SglAttrib *a) {
    if (a->type == GL_FLOAT) return 4 * a->size;
    if (a->type == GL_UNSIGNED_BYTE) return a->size;
    return 2 * a->size;
}

static SDL_GPUGraphicsPipeline *create_pipeline(const SglPipeKey *k, SDL_GPUShader *vs, SDL_GPUShader *fs) {
    SDL_GPUGraphicsPipelineCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.vertex_shader = vs;
    ci.fragment_shader = fs;
    SDL_GPUVertexBufferDescription vbd[4];
    SDL_GPUVertexAttribute va[SGL_MAX_ATTRIBS];
    memset(vbd, 0, sizeof(vbd));
    for (int i = 0; i < k->nslots; i++) {
        vbd[i].slot = (Uint32)i;
        vbd[i].pitch = k->pitch[i];
        vbd[i].input_rate = k->rate[i] ? SDL_GPU_VERTEXINPUTRATE_INSTANCE : SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vbd[i].instance_step_rate = 0;
    }
    for (int i = 0; i < k->nattr; i++) {
        va[i].location = k->attr[i].loc;
        va[i].buffer_slot = k->attr[i].slot;
        va[i].format = (SDL_GPUVertexElementFormat)k->attr[i].fmt;
        va[i].offset = k->attr[i].off;
    }
    ci.vertex_input_state.vertex_buffer_descriptions = vbd;
    ci.vertex_input_state.num_vertex_buffers = k->nslots;
    ci.vertex_input_state.vertex_attributes = va;
    ci.vertex_input_state.num_vertex_attributes = k->nattr;
    ci.primitive_type = k->topo == 1 ? SDL_GPU_PRIMITIVETYPE_LINELIST : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    /* GL's counter-clockwise front in y-up window space = clockwise in the
     * y-down space the same memory rows have here. */
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    ci.rasterizer_state.enable_depth_clip = true;
    ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    if (k->has_ds) {
        SDL_GPUDepthStencilState *d = &ci.depth_stencil_state;
        d->enable_depth_test = k->depth_test != 0;
        d->enable_depth_write = k->depth_write != 0;
        d->compare_op = (SDL_GPUCompareOp)k->dfunc;
        d->enable_stencil_test = k->stencil_test != 0;
        SDL_GPUStencilOpState so;
        so.fail_op = (SDL_GPUStencilOp)k->sfail;
        so.depth_fail_op = (SDL_GPUStencilOp)k->szfail;
        so.pass_op = (SDL_GPUStencilOp)k->szpass;
        so.compare_op = (SDL_GPUCompareOp)k->sfunc;
        d->front_stencil_state = so;
        d->back_stencil_state = so;
        d->compare_mask = k->svmask;
        d->write_mask = k->swmask;
    }
    SDL_GPUColorTargetDescription ct;
    memset(&ct, 0, sizeof(ct));
    if (k->has_color) {
        ct.format = (SDL_GPUTextureFormat)k->color_fmt;
        ct.blend_state.enable_blend = k->blend != 0;
        ct.blend_state.src_color_blendfactor = (SDL_GPUBlendFactor)k->bsr;
        ct.blend_state.dst_color_blendfactor = (SDL_GPUBlendFactor)k->bdr;
        ct.blend_state.color_blend_op = (SDL_GPUBlendOp)k->ber;
        ct.blend_state.src_alpha_blendfactor = (SDL_GPUBlendFactor)k->bsa;
        ct.blend_state.dst_alpha_blendfactor = (SDL_GPUBlendFactor)k->bda;
        ct.blend_state.alpha_blend_op = (SDL_GPUBlendOp)k->bea;
        ct.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)k->cmask;
        ct.blend_state.enable_color_write_mask = true;
        ci.target_info.color_target_descriptions = &ct;
        ci.target_info.num_color_targets = 1;
    }
    ci.target_info.has_depth_stencil_target = k->has_ds != 0;
    ci.target_info.depth_stencil_format = (SDL_GPUTextureFormat)k->ds_fmt;
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(g_sgl.dev, &ci);
    sgl_stats_note(SGL_ST_PIPELINE);
    if (!p) fprintf(stdout, "psxrecomp: sgl: pipeline failed: %s\n", SDL_GetError());
    return p;
}

static SDL_GPUGraphicsPipeline *pipe_lookup(const SglPipeKey *k, SDL_GPUShader *vs, SDL_GPUShader *fs) {
    if (s_pipe_n * 2 >= s_pipe_cap) {
        int nc = s_pipe_cap ? s_pipe_cap * 2 : 1024;
        SglPipeEntry *n = (SglPipeEntry *)calloc((size_t)nc, sizeof(SglPipeEntry));
        if (!n) return NULL;
        for (int i = 0; i < s_pipe_cap; i++) {
            if (!s_pipes[i].used) continue;
            uint32_t h = key_hash(&s_pipes[i].key) & (uint32_t)(nc - 1);
            while (n[h].used) h = (h + 1) & (uint32_t)(nc - 1);
            n[h] = s_pipes[i];
        }
        free(s_pipes);
        s_pipes = n; s_pipe_cap = nc;
    }
    uint32_t h = key_hash(k) & (uint32_t)(s_pipe_cap - 1);
    while (s_pipes[h].used) {
        if (memcmp(&s_pipes[h].key, k, sizeof(*k)) == 0) return s_pipes[h].pipe;
        h = (h + 1) & (uint32_t)(s_pipe_cap - 1);
    }
    SDL_GPUGraphicsPipeline *p = create_pipeline(k, vs, fs);
    if (!p) return NULL;
    s_pipes[h].used = 1;
    s_pipes[h].key = *k;
    s_pipes[h].pipe = p;
    s_pipe_n++;
    return p;
}

/* A program goes away: its pipelines too, once the stream that may still
 * draw with them is submitted. */
void sgl_gpu_forget_program(SglObj *prog) {
    if (!prog->id) return;
    for (int i = 0; i < s_pipe_cap; i++) {
        if (!s_pipes[i].used || s_pipes[i].key.prog != prog->id) continue;
        if (s_pipe_nrelease >= s_pipe_cap_release) {
            int nc = s_pipe_cap_release ? s_pipe_cap_release * 2 : 64;
            void *p = realloc(s_pipe_release, sizeof(*s_pipe_release) * (size_t)nc);
            if (!p) continue;
            s_pipe_release = (SDL_GPUGraphicsPipeline **)p; s_pipe_cap_release = nc;
        }
        s_pipe_release[s_pipe_nrelease++] = s_pipes[i].pipe;
        s_pipes[i].used = 0;
        s_pipe_n--;
    }
    /* Rebuild the probe chains the holes broke. */
    SglPipeEntry *old = s_pipes;
    int cap = s_pipe_cap;
    s_pipes = (SglPipeEntry *)calloc((size_t)cap, sizeof(SglPipeEntry));
    if (!s_pipes) { s_pipes = old; return; }
    for (int i = 0; i < cap; i++) {
        if (!old[i].used) continue;
        uint32_t h = key_hash(&old[i].key) & (uint32_t)(cap - 1);
        while (s_pipes[h].used) h = (h + 1) & (uint32_t)(cap - 1);
        s_pipes[h] = old[i];
    }
    free(old);
    for (int s = 0; s < 2; s++) {
        if (prog->gs[s]) SDL_ReleaseGPUShader(g_sgl.dev, prog->gs[s]);
        prog->gs[s] = NULL;
    }
}

static void key_target(SglPipeKey *k, const SglTarget *t) {
    k->has_color = t->color != NULL;
    k->color_fmt = t->color ? (uint32_t)t->color_fmt : 0;
    k->has_ds = t->ds != NULL;
    k->ds_fmt = t->ds ? (uint32_t)t->ds_fmt : 0;
    k->ds_stencil = (uint8_t)t->ds_has_stencil;
}

SDL_GPUGraphicsPipeline *sgl_pipeline(SglObj *prog, const SglVao *vao, GLenum mode,
                                      const SglTarget *t, GLuint *bufs, uint32_t *bases, int *nbuf) {
    SglPipeKey k;
    memset(&k, 0, sizeof(k));
    k.prog = prog->id;
    k.topo = mode == GL_LINES ? 1 : 0;
    key_target(&k, t);
    const SglState *s = &g_sgls;
    if (k.has_color) {
        k.blend = s->blend && !fmt_is_uint(t->color_fmt);
        k.cmask = (uint8_t)((s->cmask[0] ? 1 : 0) | (s->cmask[1] ? 2 : 0) | (s->cmask[2] ? 4 : 0) | (s->cmask[3] ? 8 : 0));
        if (k.blend) {
            k.bsr = blendf(s->bsrc_rgb); k.bdr = blendf(s->bdst_rgb);
            k.bsa = blendf(s->bsrc_a);   k.bda = blendf(s->bdst_a);
            k.ber = blendop(s->beq_rgb); k.bea = blendop(s->beq_a);
        }
    }
    if (k.has_ds) {
        k.depth_test = (uint8_t)s->depth_test;
        k.depth_write = (uint8_t)(s->depth_test && s->depth_mask);
        k.dfunc = s->depth_test ? cmpop(s->depth_func) : SDL_GPU_COMPAREOP_ALWAYS;
        k.stencil_test = (uint8_t)(s->stencil_test && t->ds_has_stencil);
        if (k.stencil_test) {
            k.sfunc = cmpop(s->st_func);
            k.sfail = stencilop(s->st_fail); k.szfail = stencilop(s->st_zfail); k.szpass = stencilop(s->st_zpass);
            k.svmask = (uint8_t)(s->st_vmask & 0xFF);
            k.swmask = (uint8_t)(s->st_wmask & 0xFF);
        }
    }
    /* Vertex inputs the program reads: from the VAO, else a constant 0,0,0,1. */
    int nslots = 0;
    uint32_t slot_base[4] = {0};
    for (int loc = 0; loc < SGL_MAX_ATTRIBS; loc++) {
        if (!(prog->vs_inputs & (1u << loc))) continue;
        const SglAttrib *a = &vao->a[loc];
        GLuint b = 0;
        int pitch = 16, rate = 1;
        uint32_t base = 0, off = 0;
        uint8_t f = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        if (a->enabled && a->buffer) {
            b = a->buffer;
            pitch = a->stride ? a->stride : vfmt_size(a);
            rate = 0;
            off = (uint32_t)a->offset;
            if (off >= 2048) { base = off; off = 0; }
            f = vfmt(a);
        }
        int slot = -1;
        for (int i = 0; i < nslots; i++)
            if (bufs[i] == b && k.pitch[i] == pitch && k.rate[i] == rate && slot_base[i] == base) { slot = i; break; }
        if (slot < 0) {
            if (nslots >= 4) return NULL;
            slot = nslots++;
            bufs[slot] = b; k.pitch[slot] = (uint16_t)pitch; k.rate[slot] = (uint8_t)rate; slot_base[slot] = base;
        }
        k.attr[k.nattr].loc = (uint8_t)loc;
        k.attr[k.nattr].fmt = f;
        k.attr[k.nattr].slot = (uint8_t)slot;
        k.attr[k.nattr].off = (uint16_t)off;
        k.nattr++;
    }
    k.nslots = (uint8_t)nslots;
    *nbuf = nslots;
    /* A slot binds at its buffer's arena copy + base (the caller adds the
     * arena part). */
    memcpy(bases, slot_base, sizeof(slot_base));
    return pipe_lookup(&k, prog->gs[0], prog->gs[1]);
}

/* ---- internal programs --------------------------------------------------- */
static const char *IP_VS =
    "#version 450\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *IP_FS[SGL_IP_COUNT] = {
    /* clear */
    "#version 450\n"
    "layout(set = 3, binding = 0) uniform U { vec4 rgba; vec4 depth; };\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = rgba; gl_FragDepth = depth.x; }\n",
    /* depth blit */
    "#version 450\n"
    "layout(set = 2, binding = 0) uniform sampler2D u_src;\n"
    "layout(set = 3, binding = 0) uniform U { vec4 src; vec4 dst; vec4 size; };\n"
    "void main() {\n"
    "    vec2 t = (gl_FragCoord.xy - dst.xy) / (dst.zw - dst.xy);\n"
    "    vec2 s = src.xy + t * (src.zw - src.xy);\n"
    "    ivec2 i = clamp(ivec2(floor(s)), ivec2(0), ivec2(size.xy) - 1);\n"
    "    gl_FragDepth = texelFetch(u_src, i, 0).r;\n"
    "}\n",
    /* colour blit */
    "#version 450\n"
    "layout(set = 2, binding = 0) uniform sampler2D u_src;\n"
    "layout(set = 3, binding = 0) uniform U { vec4 src; vec4 dst; vec4 size; };\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    vec2 t = (gl_FragCoord.xy - dst.xy) / (dst.zw - dst.xy);\n"
    "    vec2 s = src.xy + t * (src.zw - src.xy);\n"
    "    if (size.z > 0.5) o = texture(u_src, s / size.xy);\n"
    "    else o = texelFetch(u_src, clamp(ivec2(floor(s)), ivec2(0), ivec2(size.xy) - 1), 0);\n"
    "}\n",
    /* unsigned colour blit */
    "#version 450\n"
    "layout(set = 2, binding = 0) uniform usampler2D u_src;\n"
    "layout(set = 3, binding = 0) uniform U { vec4 src; vec4 dst; vec4 size; };\n"
    "layout(location = 0) out uvec4 o;\n"
    "void main() {\n"
    "    vec2 t = (gl_FragCoord.xy - dst.xy) / (dst.zw - dst.xy);\n"
    "    vec2 s = src.xy + t * (src.zw - src.xy);\n"
    "    o = texelFetch(u_src, clamp(ivec2(floor(s)), ivec2(0), ivec2(size.xy) - 1), 0);\n"
    "}\n",
    /* HDR10 present: the scRGB default framebuffer (linear BT.709, 1.0 = 80
     * nits, GL rows) -> BT.2020, ST 2084 PQ, the swapchain's rows */
    "#version 450\n"
    "layout(set = 2, binding = 0) uniform sampler2D u_src;\n"
    "layout(set = 3, binding = 0) uniform U { vec4 size; };\n"   /* source w, h; target w, h */
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "    vec2 t = gl_FragCoord.xy / size.zw;\n"
    "    ivec2 i = clamp(ivec2(floor(vec2(t.x, 1.0 - t.y) * size.xy)), ivec2(0), ivec2(size.xy) - 1);\n"
    "    vec3 c = max(texelFetch(u_src, i, 0).rgb, vec3(0.0));\n"
    "    c = mat3(0.627404, 0.069097, 0.016391, 0.329283, 0.919540, 0.088013,\n"
    "             0.043313, 0.011362, 0.895595) * c;\n"
    "    vec3 y = pow(clamp(c * (80.0 / 10000.0), 0.0, 1.0), vec3(0.1593017578125));\n"
    "    o = vec4(pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375)), 1.0);\n"
    "}\n",
};
static const char *IP_FS_CLEAR_U =
    "#version 450\n"
    "layout(set = 3, binding = 0) uniform U { vec4 rgba; vec4 depth; };\n"
    "layout(location = 0) out uvec4 o;\n"
    "void main() { o = uvec4(rgba); gl_FragDepth = depth.x; }\n";

static SglObj s_ip[SGL_IP_COUNT + 1];
static int s_ip_tried[SGL_IP_COUNT + 1];
SglObj *sgl_internal_program(int which) {
    if (which < 0 || which > SGL_IP_COUNT) return NULL;
    if (!s_ip_tried[which]) {
        s_ip_tried[which] = 1;
        SglObj *o = &s_ip[which];
        memset(o, 0, sizeof(*o));
        o->used = 1; o->is_program = 1;
        o->id = 0x80000000u | (uint32_t)which;
        const char *fs = which == SGL_IP_COUNT ? IP_FS_CLEAR_U : IP_FS[which];
        o->linked = sgl_shader_link_internal(o, IP_VS, fs);
        if (!o->linked) fprintf(stdout, "psxrecomp: sgl: internal program %d: %s\n", which, o->log ? o->log : "?");
    }
    return s_ip[which].linked ? &s_ip[which] : NULL;
}

SDL_GPUGraphicsPipeline *sgl_internal_pipeline(int which, const SglTarget *t, int cmask, int dwrite,
                                               int smask) {
    int prog_which = which;
    if (which == SGL_IP_CLEAR && t->color && fmt_is_uint(t->color_fmt)) prog_which = SGL_IP_COUNT;
    SglObj *p = sgl_internal_program(prog_which);
    if (!p) return NULL;
    SglPipeKey k;
    memset(&k, 0, sizeof(k));
    k.prog = p->id;
    key_target(&k, t);
    k.cmask = (uint8_t)cmask;
    if (k.has_ds) {
        k.depth_test = (uint8_t)(dwrite != 0);
        k.depth_write = (uint8_t)(dwrite != 0);
        k.dfunc = SDL_GPU_COMPAREOP_ALWAYS;
        k.stencil_test = (uint8_t)(smask && t->ds_has_stencil);
        if (k.stencil_test) {
            k.sfunc = SDL_GPU_COMPAREOP_ALWAYS;
            k.sfail = SDL_GPU_STENCILOP_KEEP;
            k.szfail = SDL_GPU_STENCILOP_REPLACE;
            k.szpass = SDL_GPU_STENCILOP_REPLACE;
            k.svmask = 0xFF;
            k.swmask = (uint8_t)smask;
        }
    }
    return pipe_lookup(&k, p->gs[0], p->gs[1]);
}

/* ---- device -------------------------------------------------------------- */
static SDL_GPUTransferBuffer *make_xfer(SDL_GPUTransferBufferUsage usage, uint32_t size) {
    SDL_GPUTransferBufferCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.usage = usage;
    ci.size = size;
    return SDL_CreateGPUTransferBuffer(g_sgl.dev, &ci);
}
SDL_GPUTransferBuffer *sgl_gpu_download_buffer(size_t size) {
    return make_xfer(SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, (uint32_t)size);
}

static SDL_GPUPresentMode present_mode_for(int interval) {
    if (interval == 0) {
        if (SDL_WindowSupportsGPUPresentMode(g_sgl.dev, g_sgl.win, SDL_GPU_PRESENTMODE_IMMEDIATE))
            return SDL_GPU_PRESENTMODE_IMMEDIATE;
        if (SDL_WindowSupportsGPUPresentMode(g_sgl.dev, g_sgl.win, SDL_GPU_PRESENTMODE_MAILBOX))
            return SDL_GPU_PRESENTMODE_MAILBOX;
    }
    return SDL_GPU_PRESENTMODE_VSYNC;
}
/* The window shown in HDR now. Windows says so per window (SDL); elsewhere
 * a compositor that offers an HDR swapchain is taken at its word. */
static int display_hdr(void) {
#ifdef _WIN32
    return SDL_GetBooleanProperty(SDL_GetWindowProperties(g_sgl.win), SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
#else
    return 1;
#endif
}
/* The output HDR asks for: scRGB, else HDR10 (PSX_HDR_FORMAT=hdr10 first). */
static int hdr_output_wanted(void) {
    if (!g_sgl.hdr || !display_hdr()) return 0;
    static int pref = -1;
    if (pref < 0) {
        const char *e = getenv("PSX_HDR_FORMAT");
        pref = e && (strcmp(e, "hdr10") == 0 || strcmp(e, "pq") == 0) ? 2 : 1;
    }
    const int lin = SDL_WindowSupportsGPUSwapchainComposition(g_sgl.dev, g_sgl.win,
                                                              SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);
    const int pq = SDL_WindowSupportsGPUSwapchainComposition(g_sgl.dev, g_sgl.win,
                                                             SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);
    if (pref == 2 && pq) return 2;
    return lin ? 1 : pq ? 2 : 0;
}
static int apply_swapchain(void) {
    if (!g_sgl.dev || !g_sgl.win || !g_sgl.claimed) return 0;
    const int out = hdr_output_wanted();
    const SDL_GPUSwapchainComposition comp = out == 1 ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR
                                           : out == 2 ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084
                                                      : SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    if (!SDL_SetGPUSwapchainParameters(g_sgl.dev, g_sgl.win, comp, present_mode_for(g_sgl.swap_interval)))
        return 0;
    if (out != g_sgl.hdr_out && g_sgl.hdr)
        fprintf(stdout, "psxrecomp: sgl: HDR output %s\n", out == 1 ? "scRGB" : out == 2 ? "HDR10 (PQ)" : "off (SDR)");
    g_sgl.hdr_out = out;
    g_sgl.swap_fmt = SDL_GetGPUSwapchainTextureFormat(g_sgl.dev, g_sgl.win);
    return 1;
}
int sgl_hdr_output(void) { return g_sgl.hdr_out; }
/* After each swap: HDR follows the display (Windows' HDR switch, the window
 * moved to another screen) without reopening the game. */
static void hdr_follow(void) {
    if (!g_sgl.hdr || !g_sgl.dev) return;
    static int every;
    if (++every < 15) return;   /* four times a second at 60 */
    every = 0;
    if (hdr_output_wanted() != g_sgl.hdr_out) apply_swapchain();
}

static int upload_now(SDL_GPUTexture *tex, int w, int h, const void *px, size_t bytes) {
    SDL_GPUTransferBuffer *x = make_xfer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (uint32_t)bytes);
    if (!x) return 0;
    void *m = SDL_MapGPUTransferBuffer(g_sgl.dev, x, false);
    if (m) { memcpy(m, px, bytes); SDL_UnmapGPUTransferBuffer(g_sgl.dev, x); }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_sgl.dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    if (tex) {
        SDL_GPUTextureTransferInfo src = { x, 0, (Uint32)w, (Uint32)h };
        SDL_GPUTextureRegion dst;
        memset(&dst, 0, sizeof(dst));
        dst.texture = tex; dst.w = (Uint32)w; dst.h = (Uint32)h; dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    } else {
        SDL_GPUTransferBufferLocation src = { x, 0 };
        SDL_GPUBufferRegion dst = { g_sgl.zero_vbuf, 0, (Uint32)bytes };
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(g_sgl.dev, x);
    return 1;
}

int sgl_gpu_init(SDL_Window *win) {
    memset(&g_sgl, 0, sizeof(g_sgl));
    const char *drv = driver_wanted();
    const int debug = getenv("PSX_SGL_DEBUG") && getenv("PSX_SGL_DEBUG")[0] == '1';
    g_sgl.dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug, drv);
    if (!g_sgl.dev) {
        fprintf(stdout, "psxrecomp: sgl: no SDL_GPU device (%s): %s\n", drv ? drv : "default", SDL_GetError());
        return 0;
    }
    g_sgl.win = win;
    if (!SDL_ClaimWindowForGPUDevice(g_sgl.dev, win)) {
        fprintf(stdout, "psxrecomp: sgl: window not claimed: %s\n", SDL_GetError());
        SDL_DestroyGPUDevice(g_sgl.dev);
        memset(&g_sgl, 0, sizeof(g_sgl));
        return 0;
    }
    g_sgl.claimed = 1;
    g_sgl.swap_interval = 1;
    if (s_hdr_req >= 0) {
        g_sgl.hdr = s_hdr_req;
    } else {
        const char *h = getenv("PSX_HDR");
        g_sgl.hdr = h && h[0] == '1';
    }
    apply_swapchain();
    const SDL_GPUTextureUsageFlags dsu = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    g_sgl.ds_fmt = SDL_GPUTextureSupportsFormat(g_sgl.dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
                                                SDL_GPU_TEXTURETYPE_2D, dsu)
                   ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    g_sgl.d_fmt = SDL_GPUTextureSupportsFormat(g_sgl.dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM,
                                               SDL_GPU_TEXTURETYPE_2D, dsu)
                  ? SDL_GPU_TEXTUREFORMAT_D24_UNORM : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    g_sgl.caps.dual_source_blend = 0;
    g_sgl.caps.timer_queries = 1;   /* by fences: GPU busy time (sgl_gpu_timing_enable) */
    g_sgl.caps.wide_lines = 0;
    g_sgl.caps.max_texture_size = 16384;
    {
        SDL_PropertiesID pr = SDL_GetGPUDeviceProperties(g_sgl.dev);
        const char *n = pr ? SDL_GetStringProperty(pr, SDL_PROP_GPU_DEVICE_NAME_STRING, NULL) : NULL;
        snprintf(g_sgl.name, sizeof(g_sgl.name), "%s", n ? n : SDL_GetGPUDeviceDriver(g_sgl.dev));
    }
    /* Constant inputs and unbound textures. */
    {
        SDL_GPUBufferCreateInfo bi;
        memset(&bi, 0, sizeof(bi));
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = 16;
        g_sgl.zero_vbuf = SDL_CreateGPUBuffer(g_sgl.dev, &bi);
        const float one[4] = {0, 0, 0, 1};
        upload_now(NULL, 0, 0, one, sizeof(one));
        SglImage d0, d1;
        memset(&d0, 0, sizeof(d0)); memset(&d1, 0, sizeof(d1));
        sgl_gpu_alloc_image(&d0, GL_RGBA8, 1, 1);
        sgl_gpu_alloc_image(&d1, 0x8234, 1, 1);
        const uint8_t black[4] = {0, 0, 0, 255};
        const uint16_t zero = 0;
        upload_now(d0.tex, 1, 1, black, 4);
        upload_now(d1.tex, 1, 1, &zero, 2);
        s_dummy[0] = d0.tex;
        s_dummy[1] = d1.tex;
    }
    fprintf(stdout, "psxrecomp: sgl: SDL_GPU %s on %s (depth-stencil %s)\n",
            SDL_GetGPUDeviceDriver(g_sgl.dev), g_sgl.name,
            g_sgl.ds_fmt == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
    fflush(stdout);
    return 1;
}

static void timing_shutdown(void);
void sgl_gpu_shutdown(void) {
    if (!g_sgl.dev) return;
    sgl_flush(1, 0);
    SDL_WaitForGPUIdle(g_sgl.dev);
    timing_shutdown();
    for (int i = 0; i < s_pipe_cap; i++)
        if (s_pipes[i].used) SDL_ReleaseGPUGraphicsPipeline(g_sgl.dev, s_pipes[i].pipe);
    free(s_pipes); s_pipes = NULL; s_pipe_cap = s_pipe_n = 0;
    for (int i = 0; i < s_nsamplers; i++) SDL_ReleaseGPUSampler(g_sgl.dev, s_samplers[i].s);
    s_nsamplers = 0;
    if (g_sgl.back.tex) SDL_ReleaseGPUTexture(g_sgl.dev, g_sgl.back.tex);
    if (g_sgl.back_ds.tex) SDL_ReleaseGPUTexture(g_sgl.dev, g_sgl.back_ds.tex);
    for (int i = 0; i < 2; i++) if (s_dummy[i]) SDL_ReleaseGPUTexture(g_sgl.dev, s_dummy[i]);
    if (g_sgl.vbuf) SDL_ReleaseGPUBuffer(g_sgl.dev, g_sgl.vbuf);
    if (g_sgl.zero_vbuf) SDL_ReleaseGPUBuffer(g_sgl.dev, g_sgl.zero_vbuf);
    if (g_sgl.vxfer) SDL_ReleaseGPUTransferBuffer(g_sgl.dev, g_sgl.vxfer);
    if (g_sgl.uxfer) SDL_ReleaseGPUTransferBuffer(g_sgl.dev, g_sgl.uxfer);
    if (g_sgl.dxfer) SDL_ReleaseGPUTransferBuffer(g_sgl.dev, g_sgl.dxfer);
    sgl_shader_shutdown();
    if (g_sgl.claimed) SDL_ReleaseWindowFromGPUDevice(g_sgl.dev, g_sgl.win);
    SDL_DestroyGPUDevice(g_sgl.dev);
    memset(&g_sgl, 0, sizeof(g_sgl));
    memset(s_dummy, 0, sizeof(s_dummy));
    memset(s_ip_tried, 0, sizeof(s_ip_tried));
}

/* ---- GPU time without timer queries --------------------------------------
 * SDL_GPU has no timestamp queries. Once the renderer asks for GPU time
 * (glBeginQuery / glQueryCounter on sgl), every submit carries a fence that a
 * small thread waits on, in submit order, stamping when it signals. The GPU's
 * busy time runs a clock: a command buffer counts from its submit or the end
 * of the one before it, whichever is later, to its own end (idle gaps, CPU
 * recording, do not count). A query reads that clock at the stream it was
 * placed after, so GL_TIME_ELAPSED is the busy time of the streams between. */
typedef struct { SDL_GPUFence *f; uint64_t id, submit_ns; } SglTimed;
#define SGL_TQ 256
#define SGL_TCLOCK 4096
static SglTimed s_tq[SGL_TQ];
static unsigned s_tq_head, s_tq_tail;
static struct { uint64_t id, ns; } s_tclock[SGL_TCLOCK];
static uint64_t s_tq_done;                /* last stream whose end is stamped */
static uint64_t s_last_submit;            /* last stream submitted */
static int s_timing, s_tq_quit;
static SDL_Mutex *s_tq_mu;
static SDL_Condition *s_tq_cv, *s_tq_done_cv;
static SDL_Thread *s_tq_thread;

static int SDLCALL timed_thread(void *arg) {
    (void)arg;
    uint64_t clock = 0, last_end = 0;
    for (;;) {
        SDL_LockMutex(s_tq_mu);
        while (s_tq_head == s_tq_tail && !s_tq_quit) SDL_WaitCondition(s_tq_cv, s_tq_mu);
        if (s_tq_head == s_tq_tail) { SDL_UnlockMutex(s_tq_mu); break; }
        const SglTimed b = s_tq[s_tq_tail % SGL_TQ];
        SDL_UnlockMutex(s_tq_mu);
        SDL_WaitForGPUFences(g_sgl.dev, true, (SDL_GPUFence *const *)&b.f, 1);
        const uint64_t t = SDL_GetTicksNS();
        const uint64_t start = b.submit_ns > last_end ? b.submit_ns : last_end;
        if (t > start) clock += t - start;
        last_end = t;
        SDL_ReleaseGPUFence(g_sgl.dev, b.f);
        SDL_LockMutex(s_tq_mu);
        s_tclock[b.id % SGL_TCLOCK].id = b.id;
        s_tclock[b.id % SGL_TCLOCK].ns = clock;
        s_tq_done = b.id;
        s_tq_tail++;
        SDL_BroadcastCondition(s_tq_done_cv);
        SDL_UnlockMutex(s_tq_mu);
    }
    return 0;
}
void sgl_gpu_timing_enable(void) {
    if (s_timing || !g_sgl.dev) return;
    s_tq_mu = SDL_CreateMutex();
    s_tq_cv = SDL_CreateCondition();
    s_tq_done_cv = SDL_CreateCondition();
    s_tq_quit = 0;
    s_tq_head = s_tq_tail = 0;
    s_tq_done = s_last_submit;
    if (s_tq_mu && s_tq_cv && s_tq_done_cv)
        s_tq_thread = SDL_CreateThread(timed_thread, "sgl_gpu_time", NULL);
    s_timing = s_tq_thread != NULL;
}
static void timing_shutdown(void) {
    if (!s_timing) return;
    SDL_LockMutex(s_tq_mu);
    s_tq_quit = 1;
    SDL_SignalCondition(s_tq_cv);
    SDL_UnlockMutex(s_tq_mu);
    SDL_WaitThread(s_tq_thread, NULL);
    SDL_DestroyCondition(s_tq_cv);
    SDL_DestroyCondition(s_tq_done_cv);
    SDL_DestroyMutex(s_tq_mu);
    s_tq_thread = NULL; s_tq_cv = s_tq_done_cv = NULL; s_tq_mu = NULL;
    s_timing = 0;
}
static void timed_push(SDL_GPUFence *f, uint64_t id, uint64_t submit_ns) {
    SDL_LockMutex(s_tq_mu);
    while (s_tq_head - s_tq_tail >= SGL_TQ) SDL_WaitCondition(s_tq_done_cv, s_tq_mu);
    s_tq[s_tq_head % SGL_TQ] = (SglTimed){f, id, submit_ns};
    s_tq_head++;
    SDL_SignalCondition(s_tq_cv);
    SDL_UnlockMutex(s_tq_mu);
}
static void timed_wait(uint64_t id) {
    SDL_LockMutex(s_tq_mu);
    while (s_tq_done < id) SDL_WaitCondition(s_tq_done_cv, s_tq_mu);
    SDL_UnlockMutex(s_tq_mu);
}
uint64_t sgl_gpu_last_submit(void) { return s_last_submit; }
int sgl_gpu_clock_at(uint64_t id, uint64_t *ns, int wait) {
    if (!s_timing) { *ns = 0; return 1; }
    if (id == 0) { *ns = 0; return 1; }
    if (wait && id <= s_last_submit) timed_wait(id);
    SDL_LockMutex(s_tq_mu);
    int ok = s_tq_done >= id && s_tclock[id % SGL_TCLOCK].id == id;
    if (ok) *ns = s_tclock[id % SGL_TCLOCK].ns;
    else if (s_tq_done >= id) { *ns = 0; ok = 1; }   /* before timing, or too old */
    SDL_UnlockMutex(s_tq_mu);
    return ok;
}

/* ---- fences of submitted streams ---------------------------------------- */
typedef struct { uint64_t id; SDL_GPUFence *f; } SglFenceSlot;
static SglFenceSlot s_fences[16];
static void fence_keep(uint64_t id, SDL_GPUFence *f) {
    SglFenceSlot *slot = &s_fences[id % 16];
    if (slot->f) SDL_ReleaseGPUFence(g_sgl.dev, slot->f);
    slot->id = id;
    slot->f = f;
}
int sgl_gpu_wait_stream(uint64_t id) {
    if (id >= g_sgl_stream.id) return sgl_flush(1, 0);
    if (s_timing) { timed_wait(id); return 1; }
    SglFenceSlot *slot = &s_fences[id % 16];
    if (slot->f && slot->id == id) {
        sgl_stats_note(SGL_ST_WAIT);
        return SDL_WaitForGPUFences(g_sgl.dev, true, &slot->f, 1) ? 1 : 0;
    }
    return SDL_WaitForGPUIdle(g_sgl.dev) ? 1 : 0;
}

/* ---- encoding ----------------------------------------------------------- */
static int same_target(const SglTarget *a, const SglTarget *b) {
    return a->color == b->color && a->ds == b->ds;
}
static SDL_GPURenderPass *begin_pass(SDL_GPUCommandBuffer *cmd, const SglTarget *t, const SglOp *clear) {
    SDL_GPUColorTargetInfo ci;
    SDL_GPUDepthStencilTargetInfo di;
    memset(&ci, 0, sizeof(ci));
    memset(&di, 0, sizeof(di));
    if (t->color) {
        ci.texture = t->color;
        ci.load_op = SDL_GPU_LOADOP_LOAD;
        ci.store_op = SDL_GPU_STOREOP_STORE;
        if (clear && clear->u.clear.color) {
            ci.load_op = SDL_GPU_LOADOP_CLEAR;
            ci.clear_color.r = clear->u.clear.rgba[0];
            ci.clear_color.g = clear->u.clear.rgba[1];
            ci.clear_color.b = clear->u.clear.rgba[2];
            ci.clear_color.a = clear->u.clear.rgba[3];
        }
    }
    if (t->ds) {
        di.texture = t->ds;
        di.load_op = SDL_GPU_LOADOP_LOAD;
        di.store_op = SDL_GPU_STOREOP_STORE;
        di.stencil_load_op = t->ds_has_stencil ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
        di.stencil_store_op = t->ds_has_stencil ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
        if (clear && clear->u.clear.depth) { di.load_op = SDL_GPU_LOADOP_CLEAR; di.clear_depth = clear->u.clear.d; }
        if (clear && clear->u.clear.stencil && t->ds_has_stencil) {
            di.stencil_load_op = SDL_GPU_LOADOP_CLEAR; di.clear_stencil = clear->u.clear.s;
        }
    }
    sgl_stats_note(SGL_ST_PASS);
    return SDL_BeginGPURenderPass(cmd, t->color ? &ci : NULL, t->color ? 1 : 0, t->ds ? &di : NULL);
}

static int ensure_buffer(SDL_GPUBuffer **b, uint32_t *size, uint32_t need) {
    if (*b && *size >= need) return 1;
    uint32_t n = *size ? *size : (1u << 20);
    while (n < need) n *= 2;
    if (*b) SDL_ReleaseGPUBuffer(g_sgl.dev, *b);
    SDL_GPUBufferCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    ci.size = n;
    *b = SDL_CreateGPUBuffer(g_sgl.dev, &ci);
    *size = *b ? n : 0;
    return *b != NULL;
}
static int ensure_xfer(SDL_GPUTransferBuffer **x, uint32_t *size, uint32_t need, SDL_GPUTransferBufferUsage u) {
    if (*x && *size >= need) return 1;
    uint32_t n = *size ? *size : (1u << 20);
    while (n < need) n *= 2;
    if (*x) SDL_ReleaseGPUTransferBuffer(g_sgl.dev, *x);
    *x = make_xfer(u, n);
    *size = *x ? n : 0;
    return *x != NULL;
}

static void stream_reset(void) {
    SglStream *s = &g_sgl_stream;
    for (int i = 0; i < s->nrelease; i++) SDL_ReleaseGPUTexture(g_sgl.dev, s->release[i]);
    for (int i = 0; i < s->nrelease_xfer; i++) SDL_ReleaseGPUTransferBuffer(g_sgl.dev, s->release_xfer[i]);
    for (int i = 0; i < s_pipe_nrelease; i++) SDL_ReleaseGPUGraphicsPipeline(g_sgl.dev, s_pipe_release[i]);
    s_pipe_nrelease = 0;
    s->nrelease = s->nrelease_xfer = 0;
    s->nops = 0;
    s->vtx.n = s->upl.n = s->uni.n = 0;
    s->nbind = 0;
    s->id++;
}

static int stream_has(int type) {
    for (int i = 0; i < g_sgl_stream.nops; i++) if (g_sgl_stream.ops[i].type == type) return 1;
    return 0;
}

int sgl_flush(int wait, int present) {
    SglStream *s = &g_sgl_stream;
    if (!g_sgl.dev) return 0;
    if (s->nops == 0 && !present && !wait) { stream_reset(); return 1; }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_sgl.dev);
    if (!cmd) { stream_reset(); return 0; }

    /* The arenas go up first. */
    if (s->vtx.n || s->upl.n) {
        if (s->vtx.n) {
            ensure_buffer(&g_sgl.vbuf, &g_sgl.vbuf_size, (uint32_t)s->vtx.n);
            ensure_xfer(&g_sgl.vxfer, &g_sgl.vxfer_size, (uint32_t)s->vtx.n, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
            void *m = g_sgl.vxfer ? SDL_MapGPUTransferBuffer(g_sgl.dev, g_sgl.vxfer, true) : NULL;
            if (m) { memcpy(m, s->vtx.p, s->vtx.n); SDL_UnmapGPUTransferBuffer(g_sgl.dev, g_sgl.vxfer); }
        }
        if (s->upl.n) {
            ensure_xfer(&g_sgl.uxfer, &g_sgl.uxfer_size, (uint32_t)s->upl.n, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
            void *m = g_sgl.uxfer ? SDL_MapGPUTransferBuffer(g_sgl.dev, g_sgl.uxfer, true) : NULL;
            if (m) { memcpy(m, s->upl.p, s->upl.n); SDL_UnmapGPUTransferBuffer(g_sgl.dev, g_sgl.uxfer); }
        }
        if (s->vtx.n && g_sgl.vbuf && g_sgl.vxfer) {
            SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation src = { g_sgl.vxfer, 0 };
            SDL_GPUBufferRegion dst = { g_sgl.vbuf, 0, (Uint32)s->vtx.n };
            SDL_UploadToGPUBuffer(cp, &src, &dst, true);
            SDL_EndGPUCopyPass(cp);
        }
    }

    SDL_GPURenderPass *rp = NULL;
    SDL_GPUCopyPass *cp = NULL;
    SglTarget cur;
    memset(&cur, 0, sizeof(cur));
    SDL_GPUGraphicsPipeline *bound_pipe = NULL;
    uint32_t pushed[2] = {UINT32_MAX, UINT32_MAX}, pushed_size[2] = {0, 0};
    int vp[4] = {-1, -1, -1, -1}, sc[4] = {-1, -1, -1, -1};
    int ref = -1;
    float bc[4] = {-1, -1, -1, -1};
    uint32_t vb_bound[4] = {UINT32_MAX - 1, UINT32_MAX - 1, UINT32_MAX - 1, UINT32_MAX - 1};
    int nvb_bound = -1;
    /* Textures bound per stage: SDL_GPU's Vulkan driver writes a new
     * descriptor set at the next draw after every bind, so same = skip. */
    SDL_GPUTextureSamplerBinding samp_bound[2][SGL_MAX_SAMPLERS];
    int nsamp_bound[2] = {-1, -1};

#define END_RP() do { if (rp) { SDL_EndGPURenderPass(rp); rp = NULL; } } while (0)
#define END_CP() do { if (cp) { SDL_EndGPUCopyPass(cp); cp = NULL; } } while (0)
#define RESET_BOUND() do { bound_pipe = NULL; pushed[0] = pushed[1] = UINT32_MAX; \
    vp[0] = vp[1] = vp[2] = vp[3] = -1; sc[0] = sc[1] = sc[2] = sc[3] = -1; ref = -1; \
    bc[0] = bc[1] = bc[2] = bc[3] = -1; nvb_bound = -1; nsamp_bound[0] = nsamp_bound[1] = -1; } while (0)

    for (int i = 0; i < s->nops; i++) {
        SglOp *op = &s->ops[i];
        switch (op->type) {
        case SGL_OP_CLEAR:
            END_CP();
            END_RP();
            rp = begin_pass(cmd, &op->tgt, op);
            cur = op->tgt;
            RESET_BOUND();
            break;
        case SGL_OP_DRAW: {
            END_CP();
            if (!rp || !same_target(&cur, &op->tgt)) {
                END_RP();
                rp = begin_pass(cmd, &op->tgt, NULL);
                cur = op->tgt;
                RESET_BOUND();
            }
            if (!rp) break;
            if (op->u.draw.pipe != bound_pipe) {
                SDL_BindGPUGraphicsPipeline(rp, op->u.draw.pipe);
                bound_pipe = op->u.draw.pipe;
                sgl_stats_note(SGL_ST_PIPE_BIND);
            }
            if (memcmp(vp, op->u.draw.vp, sizeof(vp)) != 0) {
                SDL_GPUViewport v = { (float)op->u.draw.vp[0], (float)op->u.draw.vp[1],
                                      (float)op->u.draw.vp[2], (float)op->u.draw.vp[3], 0.0f, 1.0f };
                SDL_SetGPUViewport(rp, &v);
                memcpy(vp, op->u.draw.vp, sizeof(vp));
            }
            if (memcmp(sc, op->u.draw.sc, sizeof(sc)) != 0) {
                SDL_Rect r = { op->u.draw.sc[0], op->u.draw.sc[1], op->u.draw.sc[2], op->u.draw.sc[3] };
                SDL_SetGPUScissor(rp, &r);
                memcpy(sc, op->u.draw.sc, sizeof(sc));
            }
            if (ref != op->u.draw.st_ref) { SDL_SetGPUStencilReference(rp, op->u.draw.st_ref); ref = op->u.draw.st_ref; }
            if (memcmp(bc, op->u.draw.bc, sizeof(bc)) != 0) {
                SDL_FColor c = { op->u.draw.bc[0], op->u.draw.bc[1], op->u.draw.bc[2], op->u.draw.bc[3] };
                SDL_SetGPUBlendConstants(rp, c);
                memcpy(bc, op->u.draw.bc, sizeof(bc));
            }
            if (op->u.draw.nvb) {
                int same = nvb_bound == op->u.draw.nvb &&
                           memcmp(vb_bound, op->u.draw.vb_off, sizeof(uint32_t) * op->u.draw.nvb) == 0;
                if (!same) {
                    SDL_GPUBufferBinding vb[4];
                    for (int b = 0; b < op->u.draw.nvb; b++) {
                        if (op->u.draw.vb_off[b] == UINT32_MAX) { vb[b].buffer = g_sgl.zero_vbuf; vb[b].offset = 0; }
                        else { vb[b].buffer = g_sgl.vbuf; vb[b].offset = op->u.draw.vb_off[b]; }
                    }
                    SDL_BindGPUVertexBuffers(rp, 0, vb, op->u.draw.nvb);
                    memcpy(vb_bound, op->u.draw.vb_off, sizeof(uint32_t) * op->u.draw.nvb);
                    nvb_bound = op->u.draw.nvb;
                }
            }
            for (int st = 0; st < 2; st++) {
                const int n = op->u.draw.nsamp[st];
                const SDL_GPUTextureSamplerBinding *b = s->bind + op->u.draw.samp_off[st];
                if (!n || (nsamp_bound[st] == n && memcmp(samp_bound[st], b, sizeof(*b) * (size_t)n) == 0))
                    continue;
                if (st == 0) SDL_BindGPUVertexSamplers(rp, 0, b, (Uint32)n);
                else SDL_BindGPUFragmentSamplers(rp, 0, b, (Uint32)n);
                memcpy(samp_bound[st], b, sizeof(*b) * (size_t)n);
                nsamp_bound[st] = n;
                sgl_stats_note(SGL_ST_SAMP_BIND);
            }
            /* Uniforms: pushed when the block differs from the last one
             * pushed (a new arena copy of the same values is not pushed). */
            for (int st = 0; st < 2; st++) {
                const uint32_t off = op->u.draw.u_off[st], size = op->u.draw.u_size[st];
                if (!size || pushed[st] == off) continue;
                if (pushed[st] != UINT32_MAX && pushed_size[st] == size &&
                    memcmp(s->uni.p + pushed[st], s->uni.p + off, size) == 0) {
                    pushed[st] = off;
                    continue;
                }
                if (st == 0) SDL_PushGPUVertexUniformData(cmd, 0, s->uni.p + off, size);
                else SDL_PushGPUFragmentUniformData(cmd, 0, s->uni.p + off, size);
                pushed[st] = off;
                pushed_size[st] = size;
                sgl_stats_note(SGL_ST_UNI_PUSH);
            }
            SDL_DrawGPUPrimitives(rp, (Uint32)op->u.draw.count, 1, (Uint32)op->u.draw.first, 0);
            break;
        }
        case SGL_OP_UPLOAD: {
            END_RP();
            if (!cp) cp = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo src = { g_sgl.uxfer, op->u.up.off, (Uint32)op->u.up.w, (Uint32)op->u.up.h };
            SDL_GPUTextureRegion dst;
            memset(&dst, 0, sizeof(dst));
            dst.texture = op->u.up.tex;
            dst.x = (Uint32)op->u.up.x; dst.y = (Uint32)op->u.up.y;
            dst.w = (Uint32)op->u.up.w; dst.h = (Uint32)op->u.up.h; dst.d = 1;
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
            break;
        }
        case SGL_OP_COPY: {
            END_RP();
            if (!cp) cp = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureLocation a, b;
            memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
            a.texture = op->u.copy.src; a.x = (Uint32)op->u.copy.sx; a.y = (Uint32)op->u.copy.sy;
            b.texture = op->u.copy.dst; b.x = (Uint32)op->u.copy.dx; b.y = (Uint32)op->u.copy.dy;
            SDL_CopyGPUTextureToTexture(cp, &a, &b, (Uint32)op->u.copy.w, (Uint32)op->u.copy.h, 1, false);
            break;
        }
        case SGL_OP_DOWNLOAD: {
            END_RP();
            if (!cp) cp = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src;
            memset(&src, 0, sizeof(src));
            src.texture = op->u.down.src;
            src.x = (Uint32)op->u.down.x; src.y = (Uint32)op->u.down.y;
            src.w = (Uint32)op->u.down.w; src.h = (Uint32)op->u.down.h; src.d = 1;
            SDL_GPUTextureTransferInfo dst = { op->u.down.xfer, op->u.down.off, (Uint32)op->u.down.w, (Uint32)op->u.down.h };
            SDL_DownloadFromGPUTexture(cp, &src, &dst);
            break;
        }
        case SGL_OP_BLIT: {
            END_RP();
            END_CP();
            SDL_GPUBlitInfo bi;
            memset(&bi, 0, sizeof(bi));
            bi.source.texture = op->u.blit.src;
            bi.source.x = (Uint32)op->u.blit.sx; bi.source.y = (Uint32)op->u.blit.sy;
            bi.source.w = (Uint32)op->u.blit.sw; bi.source.h = (Uint32)op->u.blit.sh;
            bi.destination.texture = op->u.blit.dst;
            bi.destination.x = (Uint32)op->u.blit.dx; bi.destination.y = (Uint32)op->u.blit.dy;
            bi.destination.w = (Uint32)op->u.blit.dw; bi.destination.h = (Uint32)op->u.blit.dh;
            bi.load_op = SDL_GPU_LOADOP_LOAD;
            bi.flip_mode = (SDL_FlipMode)op->u.blit.flip;
            bi.filter = op->u.blit.linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
            bi.cycle = false;
            SDL_BlitGPUTexture(cmd, &bi);
            break;
        }
        default: break;
        }
    }
    END_RP();
    END_CP();
#undef END_RP
#undef END_CP
#undef RESET_BOUND

    if (present && g_sgl.back.tex) {
        SDL_GPUTexture *sw = NULL;
        Uint32 sw_w = 0, sw_h = 0;
        SglTarget pt;
        memset(&pt, 0, sizeof(pt));
        SDL_GPUGraphicsPipeline *pq = NULL;
        if (g_sgl.hdr_out == 2) {
            pt.color = g_sgl.back.tex;     /* only its format goes into the pipeline */
            pt.color_fmt = g_sgl.swap_fmt;
            pq = sgl_internal_pipeline(SGL_IP_PQ, &pt, 0xF, 0, 0);
        }
        if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g_sgl.win, &sw, &sw_w, &sw_h) && sw && pq) {
            /* HDR10: encoded on the way to the swapchain */
            SDL_GPUColorTargetInfo ci;
            memset(&ci, 0, sizeof(ci));
            ci.texture = sw;
            ci.load_op = SDL_GPU_LOADOP_DONT_CARE;
            ci.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *prp = SDL_BeginGPURenderPass(cmd, &ci, 1, NULL);
            SDL_BindGPUGraphicsPipeline(prp, pq);
            const SDL_GPUViewport vp = {0, 0, (float)sw_w, (float)sw_h, 0, 1};
            SDL_SetGPUViewport(prp, &vp);
            SDL_GPUTextureSamplerBinding tb = {g_sgl.back.tex, sgl_sampler(GL_NEAREST, GL_NEAREST, 0x812F, 0x812F)};
            SDL_BindGPUFragmentSamplers(prp, 0, &tb, 1);
            const float u[4] = {(float)g_sgl.back.w, (float)g_sgl.back.h, (float)sw_w, (float)sw_h};
            SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof(u));
            SDL_DrawGPUPrimitives(prp, 3, 1, 0, 0);
            SDL_EndGPURenderPass(prp);
            sgl_stats_note(SGL_ST_PRESENT);
        } else if (sw) {
            SDL_GPUBlitInfo bi;
            memset(&bi, 0, sizeof(bi));
            bi.source.texture = g_sgl.back.tex;
            bi.source.w = (Uint32)g_sgl.back.w; bi.source.h = (Uint32)g_sgl.back.h;
            bi.destination.texture = sw;
            bi.destination.w = sw_w; bi.destination.h = sw_h;
            bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
            bi.flip_mode = SDL_FLIP_VERTICAL;     /* GL's row 0 is the bottom one */
            bi.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cmd, &bi);
            sgl_stats_note(SGL_ST_PRESENT);
        }
    }

    const int need_fence = wait || stream_has(SGL_OP_DOWNLOAD);
    const uint64_t id = s->id;
    int ok;
    sgl_stats_note(SGL_ST_SUBMIT);
    if (s_timing) {
        /* timed: the thread owns the fence (and waits on it) */
        const uint64_t sub = SDL_GetTicksNS();
        SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        ok = f != NULL;
        if (f) {
            s_last_submit = id;
            timed_push(f, id, sub);
            if (need_fence && wait) { timed_wait(id); sgl_stats_note(SGL_ST_WAIT); }
        }
    } else if (need_fence) {
        SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        ok = f != NULL;
        if (f) {
            s_last_submit = id;
            if (wait) { SDL_WaitForGPUFences(g_sgl.dev, true, &f, 1); sgl_stats_note(SGL_ST_WAIT); }
            fence_keep(id, f);
        }
    } else {
        ok = SDL_SubmitGPUCommandBuffer(cmd);
        if (ok) s_last_submit = id;
    }
    stream_reset();
    return ok;
}

/* Blocking read of a texture region in its GPU layout. */
static uint8_t *s_read_buf;
static size_t s_read_cap;
int sgl_gpu_read_now(SDL_GPUTexture *tex, SDL_GPUTextureFormat fmt, int x, int y, int w, int h,
                     const uint8_t **out) {
    const size_t need = (size_t)w * h * (size_t)fmt_bpp(fmt);
    if (!ensure_xfer(&g_sgl.dxfer, &g_sgl.dxfer_size, (uint32_t)need, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD))
        return 0;
    SglOp *op = sgl_push_op(SGL_OP_DOWNLOAD);
    if (!op) return 0;
    op->u.down.src = tex; op->u.down.x = x; op->u.down.y = y; op->u.down.w = w; op->u.down.h = h;
    op->u.down.xfer = g_sgl.dxfer; op->u.down.off = 0;
    sgl_stats_note(SGL_ST_DOWNLOAD);
    if (!sgl_flush(1, 0)) return 0;
    if (need > s_read_cap) {
        uint8_t *p = (uint8_t *)realloc(s_read_buf, need);
        if (!p) return 0;
        s_read_buf = p; s_read_cap = need;
    }
    const void *m = SDL_MapGPUTransferBuffer(g_sgl.dev, g_sgl.dxfer, false);
    if (!m) return 0;
    memcpy(s_read_buf, m, need);
    SDL_UnmapGPUTransferBuffer(g_sgl.dev, g_sgl.dxfer);
    *out = s_read_buf;
    return 1;
}

/* ---- the context API ---------------------------------------------------- */
void *sgl_create_context(SDL_Window *win) {
    if (g_sgl.dev) return NULL;       /* one context: no shared second one */
    if (!sgl_gpu_init(win)) return NULL;
    sgl_state_reset();
    return &g_sgl;
}
bool sgl_make_current(SDL_Window *win, void *ctx) {
    (void)win;
    return ctx == NULL || ctx == &g_sgl;
}
void sgl_delete_context(void *ctx) {
    if (ctx == &g_sgl) sgl_gpu_shutdown();
}
bool sgl_swap_window(SDL_Window *win) {
    (void)win;
    int ok = sgl_flush(0, 1);
    sgl_follow_window();
    hdr_follow();
    return ok != 0;
}
bool sgl_set_swap_interval(int interval) {
    g_sgl.swap_interval = interval < 0 ? 1 : interval;
    return apply_swapchain() != 0;
}
bool sgl_get_swap_interval(int *interval) {
    if (interval) *interval = g_sgl.swap_interval;
    return g_sgl.dev != NULL;
}

int sgl_stats_json(char *out, int cap) {
    static const char *names[SGL_ST_COUNT] = {"draws", "passes", "uploads", "copies", "blits",
                                              "downloads", "submits", "waits", "pipelines", "warnings",
                                              "pipeline_binds", "sampler_binds", "uniform_pushes",
                                              "presents"};
    int n = snprintf(out, (size_t)cap, "{\"impl\":\"%s\",\"device\":\"%s\",\"pipelines_cached\":%d",
                     sgl_impl_name(), g_sgl.name, s_pipe_n);
    for (int i = 0; i < SGL_ST_COUNT && n < cap; i++)
        n += snprintf(out + n, (size_t)(cap - n), ",\"%s\":%llu", names[i], (unsigned long long)s_stats[i]);
    if (n < cap) n += snprintf(out + n, (size_t)(cap - n), "}");
    return n;
}
