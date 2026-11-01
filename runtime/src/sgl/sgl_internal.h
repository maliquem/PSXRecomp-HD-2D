/* sgl_internal.h — objects, GL state and the recorded stream of the sgl layer.
 * Shared by sgl_gl.c (the GL entry points: state + recording), sgl_gpu.c
 * (device, encoding to SDL_GPU, pipelines, present) and sgl_shader.cpp
 * (GLSL -> SPIR-V, reflection). */
#ifndef PSX_SGL_INTERNAL_H
#define PSX_SGL_INTERNAL_H

#include "sgl.h"

#define SDL_FUNCTION_POINTER_IS_VOID_POINTER
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_opengl.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GL enums the GL 1.1 headers may lack. */
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                0x8D40
#define GL_READ_FRAMEBUFFER           0x8CA8
#define GL_DRAW_FRAMEBUFFER           0x8CA9
#define GL_RENDERBUFFER               0x8D41
#define GL_COLOR_ATTACHMENT0          0x8CE0
#define GL_DEPTH_ATTACHMENT           0x8D00
#define GL_STENCIL_ATTACHMENT         0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT   0x821A
#define GL_FRAMEBUFFER_COMPLETE       0x8CD5
#endif

/* ---- texture formats ---------------------------------------------------- */
enum {
    SGL_KIND_COLOR = 0,      /* unorm / float: sampled as float */
    SGL_KIND_UINT,           /* R16UI: usampler2D */
    SGL_KIND_DEPTH,          /* depth only */
    SGL_KIND_DEPTH_STENCIL
};

/* A texture or a renderbuffer (GL keeps two name spaces; both are images). */
typedef struct SglImage {
    int used;
    SDL_GPUTexture *tex;
    SDL_GPUTextureFormat fmt;
    GLenum ifmt;               /* normalized GL internal format */
    int kind;
    int w, h;
    int bpp;                   /* bytes per texel as the GPU stores it */
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    uint32_t gen;              /* bumps on (re)allocation */
} SglImage;

typedef struct SglFbo {
    int used;
    GLuint color;   int color_rb;  /* attachment 0 (name; renderbuffer?) */
    GLuint ds;      int ds_rb;     /* depth-stencil or depth attachment */
    GLenum draw_buffer;            /* GL_COLOR_ATTACHMENT0 or GL_NONE */
    GLenum read_buffer;
} SglFbo;

/* A pending readback into a pixel-pack buffer (converted on glMapBuffer). */
typedef struct SglPackJob {
    int active;
    uint64_t stream;           /* stream that downloads it */
    SDL_GPUTextureFormat src_fmt;
    int w, h;
    GLenum format, type;
    int row_len, align, skip_px, skip_rows;
    size_t dst_off;
} SglPackJob;

typedef struct SglBuffer {
    int used;
    uint8_t *data;
    size_t size;
    uint32_t version;
    uint64_t arena_stream;     /* stream whose vertex arena holds a copy */
    uint32_t arena_version;
    uint32_t arena_off;
    /* pixel pack */
    SDL_GPUTransferBuffer *xfer;
    size_t xfer_size;
    SglPackJob job;
} SglBuffer;

typedef struct SglAttrib {
    int enabled;
    GLuint buffer;
    int size;
    GLenum type;
    int normalized;
    int stride;
    size_t offset;
} SglAttrib;

#define SGL_MAX_ATTRIBS 16
typedef struct SglVao {
    int used;
    SglAttrib a[SGL_MAX_ATTRIBS];
    GLuint element;            /* ELEMENT_ARRAY_BUFFER (glDrawElements) */
} SglVao;

/* One active uniform of a program (from the SPIR-V of each stage). */
typedef struct SglUniform {
    char name[64];
    int loc;                   /* first location (arrays: loc .. loc+count-1) */
    int count;                 /* array length (1 = not an array) */
    GLenum gltype;             /* GL_FLOAT_VEC4, GL_INT, GL_SAMPLER_2D ... */
    int sampler;               /* a sampler: binding[] are slots */
    int off[2];                /* byte offset in the stage's block, -1 absent */
    int stride[2];             /* array stride per stage */
    int binding[2];            /* sampler slot per stage, -1 absent */
} SglUniform;

#define SGL_MAX_SAMPLERS 16
enum { SGL_VS = 0, SGL_FS = 1 };

typedef struct SglObj {        /* shaders and programs share one name space */
    int used;
    int is_program;
    /* shader */
    GLenum shader_type;
    char *source;
    /* program */
    GLuint attached[4];
    int nattached;
    struct { char name[48]; GLuint color, index; } frag_bind[4];
    int nfrag_bind;
    int linked;
    char *log;
    uint32_t id;               /* unique per link, for the pipeline cache */
    SDL_GPUShader *gs[2];
    SglUniform *u;
    int nu;
    int nlocs;                 /* locations handed out */
    uint16_t *loc_u;           /* location -> uniform index (nlocs entries) */
    uint8_t *block[2];
    int block_size[2];
    int block_dirty[2];
    uint64_t block_stream[2];  /* stream whose uniform arena holds a copy */
    uint32_t block_off[2];
    int nsamplers[2];
    int sampler_unit[2][SGL_MAX_SAMPLERS];
    uint8_t sampler_uint[2][SGL_MAX_SAMPLERS];   /* usampler2D slot */
    uint32_t vs_inputs;        /* bit per vertex input location */
    uint8_t vs_input_comps[SGL_MAX_ATTRIBS];
    char vs_input_name[SGL_MAX_ATTRIBS][32];   /* glGetAttribLocation */
} SglObj;

typedef struct SglSync { int used; uint64_t stream; SDL_GPUFence *fence; } SglSync;

/* ---- the GL state ------------------------------------------------------- */
#define SGL_UNITS 32
typedef struct SglState {
    GLuint draw_fbo, read_fbo, renderbuffer;
    GLuint array_buffer, pack_buffer, unpack_buffer;
    GLuint vao, program;
    int active_unit;
    GLuint unit_tex[SGL_UNITS];
    int blend;
    GLenum bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, beq_rgb, beq_a;
    float blend_color[4];
    int depth_test, depth_mask;
    GLenum depth_func;
    int stencil_test;
    GLenum st_func; GLint st_ref; GLuint st_vmask, st_wmask;
    GLenum st_fail, st_zfail, st_zpass;
    int scissor_test;
    int sc[4], vp[4];
    uint8_t cmask[4];
    float clear_color[4];
    double clear_depth;
    int clear_stencil;
    float line_width;
    int pack_align, pack_row_len, pack_skip_px, pack_skip_rows;
    int unpack_align, unpack_row_len, unpack_skip_px, unpack_skip_rows;
    GLenum error;
    uint32_t pipe_epoch;       /* bumps when a VAO's vertex layout changes */
} SglState;

/* ---- the recorded stream ------------------------------------------------ */
enum {
    SGL_OP_DRAW = 1,
    SGL_OP_CLEAR,      /* whole attachments: a pass that starts with LOAD_OP_CLEAR */
    SGL_OP_UPLOAD,     /* upload arena -> texture region */
    SGL_OP_COPY,       /* texture -> texture, 1:1 */
    SGL_OP_BLIT,       /* SDL_BlitGPUTexture */
    SGL_OP_DOWNLOAD    /* texture region -> transfer buffer */
};

typedef struct SglTarget {
    SDL_GPUTexture *color;
    SDL_GPUTextureFormat color_fmt;
    SDL_GPUTexture *ds;
    SDL_GPUTextureFormat ds_fmt;
    int ds_has_stencil;
    int w, h;
} SglTarget;

typedef struct SglOp {
    uint8_t type;
    SglTarget tgt;
    union {
        struct {
            SDL_GPUGraphicsPipeline *pipe;
            int vp[4], sc[4];
            uint8_t st_ref;
            float bc[4];
            uint8_t nvb;
            uint32_t vb_off[4];
            int first, count;
            uint32_t u_off[2], u_size[2];   /* uniform arena; size 0 = none */
            uint32_t samp_off[2];           /* index into the binding arena */
            uint8_t nsamp[2];
        } draw;
        struct {
            int color, depth, stencil;      /* which attachments */
            float rgba[4];
            float d;
            uint8_t s;
        } clear;
        struct {
            SDL_GPUTexture *tex;
            int x, y, w, h;
            uint32_t off;                   /* upload arena */
        } up;
        struct {
            SDL_GPUTexture *src, *dst;
            int sx, sy, dx, dy, w, h;
        } copy;
        struct {
            SDL_GPUTexture *src, *dst;
            int sx, sy, sw, sh, dx, dy, dw, dh;
            int flip;                       /* SDL_FlipMode */
            int linear;
            int src_w, src_h;               /* source texture size */
            int dst_w, dst_h;
        } blit;
        struct {
            SDL_GPUTexture *src;
            int x, y, w, h;
            SDL_GPUTransferBuffer *xfer;
            uint32_t off;
        } down;
    } u;
} SglOp;

typedef struct SglBytes { uint8_t *p; size_t n, cap; } SglBytes;

typedef struct SglStream {
    uint64_t id;               /* bumps per flush */
    SglOp *ops; int nops, cap_ops;
    SglBytes vtx;              /* vertex arena */
    SglBytes upl;              /* upload arena */
    SglBytes uni;              /* uniform arena */
    SDL_GPUTextureSamplerBinding *bind; int nbind, cap_bind;
    SDL_GPUTexture **release; int nrelease, cap_release;
    SDL_GPUTransferBuffer **release_xfer; int nrelease_xfer, cap_release_xfer;
} SglStream;

/* ---- the context -------------------------------------------------------- */
typedef struct SglContext {
    SDL_GPUDevice *dev;
    SDL_Window *win;
    int claimed;
    SglImage back;             /* the default framebuffer's colour */
    SglImage back_ds;          /* ... and its depth-stencil (on demand) */
    int swap_interval;
    int hdr;                   /* swapchain HDR_EXTENDED_LINEAR */
    SDL_GPUTextureFormat swap_fmt;
    SDL_GPUTextureFormat ds_fmt;      /* depth-stencil format in use */
    SDL_GPUTextureFormat d_fmt;       /* depth-only format in use */
    SDL_GPUBuffer *vbuf; uint32_t vbuf_size;
    SDL_GPUTransferBuffer *vxfer; uint32_t vxfer_size;
    SDL_GPUTransferBuffer *uxfer; uint32_t uxfer_size;
    SDL_GPUTransferBuffer *dxfer; uint32_t dxfer_size;   /* sync readbacks */
    SDL_GPUBuffer *zero_vbuf;  /* 16 zero bytes: unfed vertex inputs */
    char name[64];
    SglCaps caps;
} SglContext;

extern SglContext g_sgl;
extern SglState   g_sgls;
extern SglStream  g_sgl_stream;

/* sgl_gl.c */
SglImage *sgl_image(GLuint name, int rb);
SglObj   *sgl_obj(GLuint name);
void      sgl_set_error(GLenum e);

/* sgl_gpu.c */
int  sgl_gpu_init(SDL_Window *win);
void sgl_gpu_shutdown(void);
/* GPU time (timer queries on sgl): the GPU's busy clock at a submitted
 * stream. Enabled by the first query; every submit is then fenced and timed
 * by a small thread. sgl_gpu_clock_at: 1 and *ns when known (wait: until). */
void     sgl_gpu_timing_enable(void);
uint64_t sgl_gpu_last_submit(void);
int      sgl_gpu_clock_at(uint64_t id, uint64_t *ns, int wait);
int  sgl_gpu_alloc_image(SglImage *im, GLenum ifmt, int w, int h);
void sgl_release_texture(SDL_GPUTexture *t);
/* Encode and submit the stream; wait for the GPU when wait != 0. present !=
 * 0 also copies the default framebuffer to the window. Returns 1 on success. */
int  sgl_flush(int wait, int present);
SglOp *sgl_push_op(int type);
uint32_t sgl_arena_put(SglBytes *a, const void *src, size_t n, size_t align);
void *sgl_arena_reserve(SglBytes *a, size_t n, size_t align, uint32_t *off);
SDL_GPUSampler *sgl_sampler(GLenum min_f, GLenum mag_f, GLenum ws, GLenum wt);
/* The pipeline for the current GL state, program and target. */
SDL_GPUGraphicsPipeline *sgl_pipeline(SglObj *prog, const SglVao *vao,
                                      GLenum mode, const SglTarget *t,
                                      GLuint *buffers_out, uint32_t *bases_out,
                                      int *nbuf_out);
void sgl_follow_window(void);
/* Internal programs (clears / blits of depth / scissored colour blits). */
enum { SGL_IP_CLEAR = 0, SGL_IP_DEPTH_BLIT, SGL_IP_COLOR_BLIT, SGL_IP_COLOR_BLIT_U, SGL_IP_COUNT };
SglObj *sgl_internal_program(int which);
SDL_GPUGraphicsPipeline *sgl_internal_pipeline(int which, const SglTarget *t,
                                               int color_mask, int depth_write,
                                               int stencil_mask);
void sgl_stats_note(int what);
enum { SGL_ST_DRAW, SGL_ST_PASS, SGL_ST_UPLOAD, SGL_ST_COPY, SGL_ST_BLIT,
       SGL_ST_DOWNLOAD, SGL_ST_SUBMIT, SGL_ST_WAIT, SGL_ST_PIPELINE,
       SGL_ST_FEEDBACK, SGL_ST_PIPE_BIND, SGL_ST_SAMP_BIND, SGL_ST_UNI_PUSH, SGL_ST_PRESENT,
       SGL_ST_COUNT };

/* sgl_shader.cpp */
/* Compile + link vs/fs GLSL into prog (SPIR-V, reflection, SDL shaders).
 * Returns 1 on success; on failure prog->log holds the reason. */
int  sgl_shader_link(SglObj *prog, const char *vs_src, const char *fs_src);
/* Internal programs: GLSL 450 written for Vulkan. */
int  sgl_shader_link_internal(SglObj *prog, const char *vs_src, const char *fs_src);
void sgl_shader_shutdown(void);

/* pixel helpers (sgl_gl.c) */
uint16_t sgl_f2h(float f);
float    sgl_h2f(uint16_t h);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SGL_INTERNAL_H */
