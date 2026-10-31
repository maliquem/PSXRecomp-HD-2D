/* test_sgl_basic.c — the sgl layer alone (no renderer): uploads, readbacks,
 * clears, draws with uniforms, blits. Run by run_sgl_parity.py --fixture
 * test_sgl_basic.c --impls sgl (and opengl: the same checks on the driver's
 * GL, through the same gl_api table, so a difference is the layer's). */
#include "psx_sdl.h"
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl_api.h"

typedef GLuint (APIENTRY *F_CreateShader)(GLenum);
#define DECL(ret, name, args) typedef ret (APIENTRY *T_##name) args; static T_##name q_##name;
DECL(GLuint, glCreateShader, (GLenum))
DECL(void, glShaderSource, (GLuint, GLsizei, const char *const *, const GLint *))
DECL(void, glCompileShader, (GLuint))
DECL(GLuint, glCreateProgram, (void))
DECL(void, glAttachShader, (GLuint, GLuint))
DECL(void, glLinkProgram, (GLuint))
DECL(void, glGetProgramiv, (GLuint, GLenum, GLint *))
DECL(void, glUseProgram, (GLuint))
DECL(GLint, glGetUniformLocation, (GLuint, const char *))
DECL(void, glUniform1i, (GLint, GLint))
DECL(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
DECL(void, glGenVertexArrays, (GLsizei, GLuint *))
DECL(void, glBindVertexArray, (GLuint))
DECL(void, glGenBuffers, (GLsizei, GLuint *))
DECL(void, glBindBuffer, (GLenum, GLuint))
DECL(void, glBufferData, (GLenum, ptrdiff_t, const void *, GLenum))
DECL(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
DECL(void, glEnableVertexAttribArray, (GLuint))
DECL(void, glGenFramebuffers, (GLsizei, GLuint *))
DECL(void, glBindFramebuffer, (GLenum, GLuint))
DECL(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
DECL(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
DECL(void, glActiveTexture, (GLenum))

static int checks, failures;
static void check(int ok, const char *what) {
    checks++;
    if (!ok) { failures++; fprintf(stderr, "FAIL %s\n", what); }
}

static GLuint program(const char *vs, const char *fs) {
    GLuint v = q_glCreateShader(0x8B31), f = q_glCreateShader(0x8B30), p = q_glCreateProgram();
    q_glShaderSource(v, 1, &vs, NULL); q_glCompileShader(v);
    q_glShaderSource(f, 1, &fs, NULL); q_glCompileShader(f);
    q_glAttachShader(p, v); q_glAttachShader(p, f); q_glLinkProgram(p);
    GLint ok = 0; q_glGetProgramiv(p, 0x8B82, &ok);
    return ok ? p : 0;
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 2;
    const Uint32 wf = gl_api_impl() == 1 ? (Uint32)sgl_window_flags() : SDL_WINDOW_OPENGL;
    if (gl_api_impl() != 1) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    }
    SDL_Window *win = SDL_CreateWindow("sgl basic", 0, 0, 64, 64, wf | SDL_WINDOW_HIDDEN);
    if (!win) return 2;
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx || SDL_GL_MakeCurrent(win, ctx) != 0) return 2;
#define LOADQ(n) q_##n = (T_##n)SDL_GL_GetProcAddress(#n);
    LOADQ(glCreateShader) LOADQ(glShaderSource) LOADQ(glCompileShader) LOADQ(glCreateProgram)
    LOADQ(glAttachShader) LOADQ(glLinkProgram) LOADQ(glGetProgramiv) LOADQ(glUseProgram)
    LOADQ(glGetUniformLocation) LOADQ(glUniform1i) LOADQ(glUniform4f) LOADQ(glGenVertexArrays)
    LOADQ(glBindVertexArray) LOADQ(glGenBuffers) LOADQ(glBindBuffer) LOADQ(glBufferData)
    LOADQ(glVertexAttribPointer) LOADQ(glEnableVertexAttribArray) LOADQ(glGenFramebuffers)
    LOADQ(glBindFramebuffer) LOADQ(glFramebufferTexture2D) LOADQ(glBlitFramebuffer) LOADQ(glActiveTexture)
    printf("driver=%s\n", (const char *)glGetString(GL_VERSION));

    /* 1. R16UI: whole upload, then a sub-rect from a row-length-1024 array. */
    static uint16_t vram[1024 * 512], back[64 * 64];
    for (int i = 0; i < 1024 * 512; i++) vram[i] = (uint16_t)(i * 17);
    GLuint raw; glGenTextures(1, &raw); glBindTexture(GL_TEXTURE_2D, raw);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x8234, 1024, 512, 0, 0x8D94, GL_UNSIGNED_SHORT, vram);
    for (int x = 0; x < 16; x++) vram[512 + x] = (uint16_t)(0x400 + x + 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 1024);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 512, 0, 16, 1, 0x8D94, GL_UNSIGNED_SHORT, vram + 512);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    GLuint fb; q_glGenFramebuffers(1, &fb); q_glBindFramebuffer(0x8D40, fb);
    q_glFramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, raw, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(500, 0, 40, 2, 0x8D94, GL_UNSIGNED_SHORT, back);
    int bad = 0;
    for (int y = 0; y < 2; y++) for (int x = 0; x < 40; x++)
        if (back[y * 40 + x] != vram[y * 1024 + 500 + x]) {
            if (bad < 4) fprintf(stderr, "  r16ui (%d,%d) got %04x want %04x\n", 500 + x, y, back[y * 40 + x], vram[y * 1024 + 500 + x]);
            bad++;
        }
    check(bad == 0, "R16UI sub-rect upload with row length");

    /* 2. RGBA8 target: clear all, scissored clear, read back. */
    GLuint col; glGenTextures(1, &col); glBindTexture(GL_TEXTURE_2D, col);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    GLuint fb2; q_glGenFramebuffers(1, &fb2); q_glBindFramebuffer(0x8D40, fb2);
    q_glFramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, col, 0);
    glViewport(0, 0, 64, 64);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST); glScissor(8, 4, 10, 6);
    glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    static uint8_t px[64 * 64 * 4];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check(px[(5 * 64 + 9) * 4 + 1] == 255 && px[(5 * 64 + 9) * 4 + 0] == 0, "scissored clear inside");
    check(px[(20 * 64 + 30) * 4 + 0] == 255 && px[(20 * 64 + 30) * 4 + 1] == 0, "scissored clear outside");
    check(px[(3 * 64 + 9) * 4 + 0] == 255, "scissored clear: row below the box untouched (y up)");

    /* 3. A draw with a uniform colour and a vertex buffer. */
    const char *vs = "#version 330\nlayout(location=0) in vec2 a_pos;\nuniform vec4 u_off;\n"
                     "void main(){ gl_Position = vec4(a_pos + u_off.xy, 0.0, 1.0); }\n";
    const char *fs = "#version 330\nuniform vec4 u_col;\nout vec4 frag;\nvoid main(){ frag = u_col; }\n";
    GLuint p = program(vs, fs);
    check(p != 0, "program links");
    GLuint vao, vbo; q_glGenVertexArrays(1, &vao); q_glBindVertexArray(vao);
    q_glGenBuffers(1, &vbo); q_glBindBuffer(0x8892, vbo);
    /* lower-left quarter of the target in NDC */
    float tri[12] = {-1, -1, 0, -1, -1, 0, 0, -1, -1, 0, 0, 0};
    q_glBufferData(0x8892, sizeof tri, tri, 0x88E0);
    q_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, 0);
    q_glEnableVertexAttribArray(0);
    q_glUseProgram(p);
    q_glUniform4f(q_glGetUniformLocation(p, "u_col"), 0, 0, 1, 1);
    q_glUniform4f(q_glGetUniformLocation(p, "u_off"), 0, 0, 0, 0);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(stderr, "  locs col %d off %d; px(2,2) %d %d %d %d; px(40,40) %d %d %d; px(2,40) %d %d %d\n",
            q_glGetUniformLocation(p, "u_col"), q_glGetUniformLocation(p, "u_off"),
            px[(2 * 64 + 2) * 4], px[(2 * 64 + 2) * 4 + 1], px[(2 * 64 + 2) * 4 + 2], px[(2 * 64 + 2) * 4 + 3],
            px[(40 * 64 + 40) * 4], px[(40 * 64 + 40) * 4 + 1], px[(40 * 64 + 40) * 4 + 2],
            px[(40 * 64 + 2) * 4], px[(40 * 64 + 2) * 4 + 1], px[(40 * 64 + 2) * 4 + 2]);
    check(px[(2 * 64 + 2) * 4 + 2] == 255 && px[(2 * 64 + 2) * 4 + 0] == 0, "quad at the bottom left (GL y up)");
    check(px[(40 * 64 + 40) * 4 + 0] == 255, "rest untouched");
    /* second draw, uniform changed between the two */
    q_glUniform4f(q_glGetUniformLocation(p, "u_col"), 1, 1, 1, 1);
    q_glUniform4f(q_glGetUniformLocation(p, "u_off"), 1, 1, 0, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check(px[(40 * 64 + 40) * 4 + 0] == 255 && px[(40 * 64 + 40) * 4 + 2] == 255, "second draw, new uniforms");
    check(px[(2 * 64 + 2) * 4 + 2] == 255 && px[(2 * 64 + 2) * 4 + 0] == 0, "first draw kept its uniforms");

    /* 4. Texture sampling with usampler2D + texelFetch (the renderer's way). */
    const char *vs2 = "#version 330\nlayout(location=0) in vec2 a_pos;\n"
                      "void main(){ gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    const char *fs2 = "#version 330\nuniform usampler2D u_raw;\nuniform ivec2 u_at;\nout vec4 frag;\n"
                      "void main(){ uint v = texelFetch(u_raw, u_at + ivec2(gl_FragCoord.xy), 0).r;\n"
                      " frag = vec4(float(v & 31u)/31.0, float((v>>5)&31u)/31.0, float((v>>10)&31u)/31.0, 1.0); }\n";
    GLuint p2 = program(vs2, fs2);
    check(p2 != 0, "usampler program links");
    q_glUseProgram(p2);
    q_glActiveTexture(0x84C0 + 3);
    glBindTexture(GL_TEXTURE_2D, raw);
    q_glUniform1i(q_glGetUniformLocation(p2, "u_raw"), 3);
    GLint at = q_glGetUniformLocation(p2, "u_at");
    typedef void (APIENTRY *T_u2i)(GLint, GLint, GLint);
    T_u2i u2i = (T_u2i)SDL_GL_GetProcAddress("glUniform2i");
    u2i(at, 512, 0);
    float full[12] = {-1, -1, 1, -1, -1, 1, 1, -1, -1, 1, 1, 1};
    q_glBindBuffer(0x8892, vbo);
    q_glBufferData(0x8892, sizeof full, full, 0x88E0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glReadPixels(0, 0, 16, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    bad = 0;
    for (int x = 0; x < 16; x++) {
        uint16_t v = vram[512 + x];
        int r = (int)((v & 31) * 255 + 15) / 31;
        if (abs(px[x * 4] - r) > 1) {
            if (bad < 4) fprintf(stderr, "  fetch x %d got r %d want %d\n", x, px[x * 4], r);
            bad++;
        }
    }
    check(bad == 0, "usampler texelFetch reads the uploaded texels");

    /* 4b. Varyings declared in a different order in each stage (GL matches
     * them by name), one flat. */
    const char *vs3 = "#version 330\nlayout(location=0) in vec2 a_pos;\n"
                      "out float v_b;\nflat out vec2 v_rg;\nout float v_one;\n"
                      "void main(){ gl_Position = vec4(a_pos, 0.0, 1.0); v_b = 0.25; v_rg = vec2(1.0, 0.5); v_one = 1.0; }\n";
    const char *fs3 = "#version 330\nin float v_one;\nflat in vec2 v_rg;\nin float v_b;\nout vec4 frag;\n"
                      "void main(){ frag = vec4(v_rg, v_b, v_one); }\n";
    GLuint p3 = program(vs3, fs3);
    check(p3 != 0, "varyings program links");
    q_glUseProgram(p3);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check(px[0] == 255 && abs(px[1] - 128) <= 1 && abs(px[2] - 64) <= 1 && px[3] == 255,
          "varyings matched by name");
    if (!(px[0] == 255 && abs(px[1] - 128) <= 1)) fprintf(stderr, "  varyings got %d %d %d %d\n", px[0], px[1], px[2], px[3]);

    /* 5. Blit with a vertical flip. */
    GLuint col2; glGenTextures(1, &col2); glBindTexture(GL_TEXTURE_2D, col2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    GLuint fb3; q_glGenFramebuffers(1, &fb3); q_glBindFramebuffer(0x8D40, fb3);
    q_glFramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, col2, 0);
    q_glBindFramebuffer(0x8CA8, fb2);
    q_glBindFramebuffer(0x8CA9, fb3);
    q_glBlitFramebuffer(0, 0, 64, 64, 0, 64, 64, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    q_glBindFramebuffer(0x8D40, fb3);
    static uint8_t px2[64 * 64 * 4];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px2);
    q_glBindFramebuffer(0x8D40, fb2);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
    bad = 0;
    for (int y = 0; y < 64; y++) if (memcmp(px2 + y * 256, px + (63 - y) * 256, 256)) bad++;
    check(bad == 0, "flipped blit");

    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
