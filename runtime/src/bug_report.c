/* bug_report.c - player bug reports (docs/BUG_REPORT.md, bug_report.h).
 *
 * The BugReport hotkey takes the moment while the game keeps running for a
 * few frames: the next presented picture, the PSX picture without effects, a
 * save state (SAVESTATE_BUG_SLOT) and the presented-image ring, frozen so its
 * last seconds become the clip. Then a form opens over the frozen game (a
 * title page): type, severity, frequency, a typed description. Saving writes
 * one folder per report; discarding deletes it.
 *
 * Folder: <reports>/<YYYY-MM-DD_HHMMSS>_<type>/
 *   report.json      what the player chose and wrote, what was taken, the
 *                    runtime's state and the title's own members ("game")
 *   photo.png        the presented picture (effects, window size)
 *   photo_ps1.png    the PSX picture without the effects (native size)
 *   state.pst        the save state of the moment
 *   clip.gif         the last seconds as shown (small, every presented frame)
 *   clip_sheet.png   twelve of them in a grid
 *   config/          the settings files
 *   photo_marked.png the picture with the player's red rectangles (when any)
 *   <id>.zip         what a release build sends (docs/BUG_REPORT.md) */

#include "bug_report.h"

#include "bug_report_pack.h"
#include "execution_profile.h"
#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "host_osd.h"
#include "mod_plugins.h"
#include "png_write.h"
#include "present_image_ring.h"
#include "psx_savestate_menu.h"
#include "psx_sdl.h"
#include "savestate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

#ifndef PSX_GAME_VERSION
#define PSX_GAME_VERSION "dev"
#endif

extern int present_shot_request(const char *path);
extern int present_shot_seq(void);
extern int present_shot_ok(void);
extern int psx_netplay_active(void);
#ifndef PSX_NO_DEBUG_TOOLS
extern uint64_t s_frame_count;
extern int debug_server_write_wide_picture(const char *path);
#endif

#define BR_CAPTURE_MS   3000   /* the moment's captures, at most */
#define BR_CLIP_VBLANKS 180    /* the clip: the last three seconds */
#define BR_CLIP_MAX     360    /* presented frames in it (Smooth motion: more than one a VBlank) */
#define BR_TEXT_MAX     BUG_REPORT_TEXT_MAX
#define BR_TYPES_MAX    32
#define BR_FILES_MAX    16
#define BR_WORDS_MAX    64
#define BR_JPEG_WIDTH   1920   /* the sent photo, at most */
#define BR_JPEG_QUALITY 88
#define BR_URL_MAX      7000   /* a longer address fails on some browsers */

enum { BR_IDLE = 0, BR_CAPTURING, BR_FORM };

static const char *const k_base_types[] = {
    "Graphics", "Sound", "Crash / freeze", "Slow / stutter", "Controls", "Menu / text", "Other",
};
/* Critical: the game cannot go on; Annoying: it gets in the way; Minor: a detail. */
static const char *const k_severities[] = { "Critical", "Annoying", "Minor" };
static const char *const k_frequencies[] = { "Always", "Sometimes", "Once" };

/* The form's words for a player whose system is in Portuguese. A title's own
 * (psx_mod_bug_report_translate) come first. */
static const char *const k_words_pt[][2] = {
    { "Report a bug", "Relatar um bug" },
    { "Type", "Tipo" },
    { "Severity", "Gravidade" },
    { "Frequency", "Frequ\xC3\xAAncia" },
    { "Description", "Descri\xC3\xA7\xC3\xA3o" },
    { "Type here what happened.", "Escreva aqui o que aconteceu." },
    { "Photo", "Foto" },
    { "State", "Estado" },
    { "Clip", "Clipe" },
    { "Save report", "Salvar relato" },
    { "Save and send", "Salvar e enviar" },
    { "Mark on picture", "Marcar na foto" },
    { "Done", "Pronto" },
    { "Undo", "Desfazer" },
    { "Clear", "Limpar" },
    { "Drag a red rectangle over the problem. Mark as many as you need.",
      "Arraste um ret\xC3\xA2ngulo vermelho sobre o problema. Marque quantos precisar." },
    { "Keys or controller: move, Enter or Cross marks each corner, Backspace or Square undoes, Esc or Circle goes back.",
      "Teclado ou controle: mova, Enter ou X marca cada canto, Backspace ou Quadrado desfaz, Esc ou C\xC3\xADrculo volta." },
    { "Sending opens GitHub. Your text and the photos become public. The save state and your settings go encrypted.",
      "Enviar abre o GitHub. Seu texto e as fotos ficam p\xC3\xBA" "blicos. O save state e as suas configura\xC3\xA7\xC3\xB5" "es v\xC3\xA3o criptografados." },
    { "Discard", "Descartar" },
    { "Discard? Press again", "Descartar? Aperte de novo" },
    { "Ctrl+Enter saves \xC2\xB7 Esc discards", "Ctrl+Enter salva \xC2\xB7 Esc descarta" },
    { "Bug report saved", "Relato de bug salvo" },
    { "Bug report saved, not packed", "Relato salvo, mas o zip falhou" },
    { "Bug report discarded", "Relato de bug descartado" },
    { "Graphics", "Gr\xC3\xA1" "ficos" },
    { "Sound", "Som" },
    { "Crash / freeze", "Travou / congelou" },
    { "Slow / stutter", "Lento / engasgos" },
    { "Controls", "Controles" },
    { "Menu / text", "Menu / texto" },
    { "Other", "Outro" },
    { "Critical", "Cr\xC3\xADtico: o jogo n\xC3\xA3o continua" },
    { "Annoying", "Inc\xC3\xB4modo: atrapalha" },
    { "Minor", "Pequeno: um detalhe" },
    { "Always", "Sempre" },
    { "Sometimes", "\xC3\x80s vezes" },
    { "Once", "Uma vez" },
};

/* The title's additions (cleared at session activation). */
static char *s_title_types[BR_TYPES_MAX];
static int s_title_type_n;
static char *s_files[BR_FILES_MAX];
static int s_file_n;
static PSXModBugReportRenderer s_render;
static PSXModBugReportInfo s_info;
static char s_issue_url[256];
static uint8_t s_send_key[32];
static int s_send_has_key;
static char *s_word_en[BR_WORDS_MAX], *s_word_shown[BR_WORDS_MAX];
static int s_word_n;

/* The report being made. */
static int s_stage = BR_IDLE;
static int s_page_open;
static char s_root[512], s_folder[700], s_name[128], s_created[32];
static uint64_t s_t0;
static int s_shot_seq0, s_photo, s_photo_ps1;
static uint32_t s_save_gen0;
static int s_state;
static int s_ring_ours;
static uint64_t s_frame;
static uint32_t *s_thumb;
static int s_thumb_w, s_thumb_h, s_clip_frames;
static const char *s_types[BR_TYPES_MAX + 8];
static int s_type_n;
static int s_row, s_type, s_sev = 1, s_freq = 1, s_confirm;
static char s_text[BR_TEXT_MAX + 1];
static int s_caret;
static int s_ui;   /* the form is up in Dear ImGui (bug_report_ui.cpp), not on the canvas */
static BugReportMark s_marks[BUG_REPORT_MARKS_MAX];
static int s_mark_n, s_marking_seen;

/* ---- files ---------------------------------------------------------------- */

static void br_mkdir(const char *p) {
#ifdef _WIN32
    (void)CreateDirectoryA(p, NULL);
#else
    (void)mkdir(p, 0777);
#endif
}
static int br_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}
static int br_copy(const char *from, const char *to) {
#ifdef _WIN32
    return CopyFileA(from, to, FALSE) != 0;
#else
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    char buf[65536];
    size_t n;
    int ok = a && b;
    while (ok && (n = fread(buf, 1, sizeof buf, a)) > 0) ok = fwrite(buf, 1, n, b) == n;
    if (a) fclose(a);
    if (b && fclose(b) != 0) ok = 0;
    return ok;
#endif
}
static int br_rename(const char *from, const char *to) {
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_COPY_ALLOWED) != 0;
#else
    return rename(from, to) == 0;
#endif
}
static void br_rmdir(const char *p) {
#ifdef _WIN32
    (void)RemoveDirectoryA(p);
#else
    (void)rmdir(p);
#endif
}
static void br_path(char *out, size_t cap, const char *file) {
    snprintf(out, cap, "%s/%s", s_folder, file);
}
static uint64_t br_now_ms(void) { return (uint64_t)SDL_GetTicks(); }

/* PSX_BUG_REPORT_DIR, else bug_reports/ beside the exe. */
static void br_root_path(char *out, size_t cap) {
    const char *e = getenv("PSX_BUG_REPORT_DIR");
    if (e && *e) {
        snprintf(out, cap, "%s", e);
        return;
    }
    const char *base = SDL_GetBasePath();
    snprintf(out, cap, "%sbug_reports", base ? base : "");
}
static void br_root(void) {
    br_root_path(s_root, sizeof s_root);
    br_mkdir(s_root);
}

/* JSON string body (no quotes). */
static void json_str(FILE *f, const char *s) {
    for (; s && *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') continue;
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
}

/* ---- PNG, compressed (png_write.h stores its pixels uncompressed: ~5 MB a
 * window-sized picture, ~20 MB a high internal resolution) --------------- */

static int png_write_z(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {
    const size_t stride = (size_t)w * 3, raw_len = (size_t)h * (stride + 1);
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    uLongf z_len = compressBound((uLong)raw_len);
    uint8_t *z = raw ? (uint8_t *)malloc(z_len) : NULL;
    int ok = 0;
    if (raw && z) {
        /* filter Sub per row: neighbours alike in a game picture */
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = raw + y * (stride + 1);
            const uint8_t *src = rgb + y * stride;
            row[0] = 1;
            for (size_t i = 0; i < stride; i++) row[1 + i] = (uint8_t)(src[i] - (i >= 3 ? src[i - 3] : 0));
        }
        FILE *f = compress2(z, &z_len, raw, (uLong)raw_len, 6) == Z_OK ? fopen(path, "wb") : NULL;
        if (f) {
            static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
            uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                                 (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                                 8, 2, 0, 0, 0 };
            fwrite(sig, 1, 8, f);
            png_chunk(f, "IHDR", ihdr, sizeof ihdr);
            png_chunk(f, "IDAT", z, z_len);
            png_chunk(f, "IEND", NULL, 0);
            ok = !ferror(f);
            if (fclose(f) != 0) ok = 0;
        }
    }
    free(z);
    free(raw);
    return ok;
}

/* A PNG the framework wrote (8-bit RGB, filter None) written again compressed.
 * keep: the pixels stay with the caller (free them); *w, *h their size. */
static int png_recompress(const char *path, uint8_t **keep, uint32_t *keep_w, uint32_t *keep_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = size > 33 ? (uint8_t *)malloc((size_t)size) : NULL;
    const int got = file && fread(file, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!got) { free(file); return 0; }
    uint32_t w = 0, h = 0;
    uint8_t *idat = (uint8_t *)malloc((size_t)size), *raw = NULL, *rgb = NULL;
    size_t idat_n = 0;
    int ok = 0;
    for (long at = 8; idat && at + 12 <= size;) {
        const uint32_t len = (uint32_t)file[at] << 24 | (uint32_t)file[at + 1] << 16 | (uint32_t)file[at + 2] << 8 | file[at + 3];
        const uint8_t *type = file + at + 4, *data = file + at + 8;
        if (at + 12 + (long)len > size) break;
        if (!memcmp(type, "IHDR", 4) && len == 13) {
            w = (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
            h = (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 | data[7];
            if (data[8] != 8 || data[9] != 2 || data[12] != 0) break;
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(idat + idat_n, data, len);
            idat_n += len;
        }
        at += 12 + (long)len;
    }
    const size_t stride = (size_t)w * 3;
    uLongf raw_len = (uLongf)(h * (stride + 1));
    if (w && h && idat_n && (raw = (uint8_t *)malloc(raw_len)) && (rgb = (uint8_t *)malloc(h * stride)) &&
        uncompress(raw, &raw_len, idat, (uLong)idat_n) == Z_OK && raw_len == h * (stride + 1)) {
        ok = 1;
        for (uint32_t y = 0; y < h && ok; y++) {
            if (raw[y * (stride + 1)] != 0) ok = 0;   /* only the framework's own (filter None) */
            memcpy(rgb + y * stride, raw + y * (stride + 1) + 1, stride);
        }
        if (ok) {
            char tmp[900];
            snprintf(tmp, sizeof tmp, "%s.tmp", path);
            ok = png_write_z(tmp, rgb, w, h) && (remove(path) == 0) && rename(tmp, path) == 0;
            if (!ok) remove(tmp);
        }
    }
    if (ok && keep) {
        *keep = rgb;
        *keep_w = w;
        *keep_h = h;
        rgb = NULL;
    }
    free(rgb);
    free(raw);
    free(idat);
    free(file);
    return ok;
}

/* ---- the moment's pictures ------------------------------------------------ */

/* The PSX picture as the game drew it, before every effect: the native-wide
 * surface when one is presented (debug-tools builds), else the display area. */
static int write_ps1_picture(const char *path) {
#ifndef PSX_NO_DEBUG_TOOLS
    if (debug_server_write_wide_picture(path)) return 1;
#endif
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (di.disabled || di.width == 0 || di.height == 0) return 0;
    if (!di.depth24) gl_renderer_sync_cpu();
    uint32_t w = di.width > 640 ? 640 : di.width, h = di.height > 512 ? 512 : di.height;
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) return 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            gpu_display_pixel_rgb(&di, x, y, &p[0], &p[1], &p[2]);
        }
    const int ok = png_write_z(path, rgb, w, h);
    free(rgb);
    return ok;
}

/* The newest presented frame of the (frozen) ring, for the form. */
static void take_thumb(void) {
    uint32_t lo = 0, hi = 0, frame = 0;
    uint8_t *rgb = NULL;
    int w = 0, h = 0;
    free(s_thumb);
    s_thumb = NULL;
    s_thumb_w = s_thumb_h = 0;
    present_image_ring_sequence_span(&lo, &hi);
    if (!hi || !present_image_ring_get_sequence_rgb(hi, &rgb, &w, &h, &frame)) return;
    s_thumb = (uint32_t *)malloc((size_t)w * h * sizeof *s_thumb);
    if (s_thumb) {
        for (int i = 0; i < w * h; i++)
            s_thumb[i] = 0xFF000000u | (uint32_t)rgb[3 * i] << 16 | (uint32_t)rgb[3 * i + 1] << 8 | rgb[3 * i + 2];
        s_thumb_w = w;
        s_thumb_h = h;
    }
    free(rgb);
}

/* ---- the clip (GIF: own encoder, fixed 6x7x6 palette) ---------------------- */

typedef struct { FILE *f; uint32_t acc; int nbits; uint8_t blk[255]; int blen; } GifBits;

static void gb_byte(GifBits *b, int v) {
    b->blk[b->blen++] = (uint8_t)v;
    if (b->blen == 255) {
        fputc(255, b->f);
        fwrite(b->blk, 1, 255, b->f);
        b->blen = 0;
    }
}
static void gb_code(GifBits *b, int code, int size) {
    b->acc |= (uint32_t)code << b->nbits;
    b->nbits += size;
    while (b->nbits >= 8) {
        gb_byte(b, (int)(b->acc & 0xFF));
        b->acc >>= 8;
        b->nbits -= 8;
    }
}
static void gb_end(GifBits *b) {
    if (b->nbits > 0) gb_byte(b, (int)(b->acc & 0xFF));
    if (b->blen) {
        fputc(b->blen, b->f);
        fwrite(b->blk, 1, (size_t)b->blen, b->f);
    }
    fputc(0, b->f);
}

#define GIF_HASH 5003
/* LZW with 8-bit pixels. Code sizes follow the decoder: it adds the entry the
 * encoder added for code j only when it reads code j+1, so both grow the size
 * after the same entry; the last code gets the decoder's pending growth. */
static void gif_lzw(FILE *f, const uint8_t *idx, int n) {
    static int hkey[GIF_HASH];
    static uint16_t hval[GIF_HASH];
    GifBits b;
    memset(&b, 0, sizeof b);
    b.f = f;
    int size = 9, next = 258;
    fputc(8, f);
    for (int i = 0; i < GIF_HASH; i++) hkey[i] = -1;
    gb_code(&b, 256, size);
    int prefix = idx[0];
    for (int i = 1; i < n; i++) {
        const int c = idx[i], key = (prefix << 8) | c;
        int h = (int)(((uint32_t)key * 2654435761u) % GIF_HASH), found = -1;
        while (hkey[h] != -1) {
            if (hkey[h] == key) { found = hval[h]; break; }
            if (++h == GIF_HASH) h = 0;
        }
        if (found >= 0) { prefix = found; continue; }
        gb_code(&b, prefix, size);
        const int e = next++;
        hkey[h] = key;
        hval[h] = (uint16_t)e;
        if (e >= (1 << size)) size++;
        if (e == 4095) {
            gb_code(&b, 256, size);
            for (int k = 0; k < GIF_HASH; k++) hkey[k] = -1;
            size = 9;
            next = 258;
        }
        prefix = c;
    }
    gb_code(&b, prefix, size);
    if (next >= (1 << size) && size < 12) size++;
    gb_code(&b, 257, size);
    gb_end(&b);
}

/* The palette cell of a pixel, with a 4x4 ordered dither (the cube's steps
 * are coarse: dark scenes would band). */
static uint8_t pal_index(const uint8_t *p, int x, int y) {
    static const int k_bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    const int t = k_bayer[y & 3][x & 3] * 2 - 15;   /* -15..15, 1/32 of a step each */
    int r = (p[0] * 5 * 32 + t * 255 + 255 * 16) / (255 * 32);
    int g = (p[1] * 6 * 32 + t * 255 + 255 * 16) / (255 * 32);
    int b = (p[2] * 5 * 32 + t * 255 + 255 * 16) / (255 * 32);
    r = r < 0 ? 0 : r > 5 ? 5 : r;
    g = g < 0 ? 0 : g > 6 ? 6 : g;
    b = b < 0 ? 0 : b > 5 ? 5 : b;
    return (uint8_t)(r * 42 + g * 6 + b);
}

static void put16(FILE *f, int v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }

static int write_gif(const char *path, uint8_t **frames, const int *delay_cs, int n, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite("GIF89a", 1, 6, f);
    put16(f, w);
    put16(f, h);
    fputc(0xF7, f);
    fputc(0, f);
    fputc(0, f);
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        if (i < 252) { r = i / 42 * 255 / 5; g = i / 6 % 7 * 255 / 6; b = i % 6 * 255 / 5; }
        else r = g = b = (i - 252) * 64 + 32;
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fwrite("\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00", 1, 19, f);
    uint8_t *idx = (uint8_t *)malloc((size_t)w * h);
    if (!idx) { fclose(f); return 0; }
    for (int k = 0; k < n; k++) {
        fwrite("\x21\xF9\x04\x00", 1, 4, f);
        put16(f, delay_cs[k]);
        fputc(0, f);
        fputc(0, f);
        fputc(0x2C, f);
        put16(f, 0); put16(f, 0); put16(f, w); put16(f, h);
        fputc(0, f);
        for (int i = 0; i < w * h; i++) idx[i] = pal_index(frames[k] + 3 * i, i % w, i / w);
        gif_lzw(f, idx, w * h);
    }
    free(idx);
    fputc(0x3B, f);
    const int ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

/* Twelve frames of the clip, four to a row. */
static int write_sheet(const char *path, uint8_t **frames, int n, int w, int h) {
    const int cols = 4, pick = n < 12 ? n : 12, rows = (pick + cols - 1) / cols, gap = 2;
    const int sw = cols * w + (cols + 1) * gap, sh = rows * h + (rows + 1) * gap;
    uint8_t *rgb = (uint8_t *)malloc((size_t)sw * sh * 3);
    if (!rgb || !pick) { free(rgb); return 0; }
    memset(rgb, 40, (size_t)sw * sh * 3);
    for (int k = 0; k < pick; k++) {
        const int src = pick == 1 ? 0 : k * (n - 1) / (pick - 1);
        const int ox = gap + (k % cols) * (w + gap), oy = gap + (k / cols) * (h + gap);
        for (int y = 0; y < h; y++)
            memcpy(rgb + ((size_t)(oy + y) * sw + ox) * 3, frames[src] + (size_t)y * w * 3, (size_t)w * 3);
    }
    const int ok = png_write_z(path, rgb, (uint32_t)sw, (uint32_t)sh);
    free(rgb);
    return ok;
}

/* The frozen ring's last BR_CLIP_VBLANKS VBlanks, oldest first. */
static int write_clip(int *span_vblanks) {
    uint32_t lo = 0, hi = 0;
    uint8_t *frames[BR_CLIP_MAX];
    uint32_t fno[BR_CLIP_MAX];
    int n = 0, w0 = 0, h0 = 0;
    *span_vblanks = 0;
    present_image_ring_sequence_span(&lo, &hi);
    if (!hi) return 0;
    uint32_t newest = 0;
    for (uint32_t seq = hi; seq >= lo && seq > 0 && n < BR_CLIP_MAX; seq--) {
        uint8_t *rgb = NULL;
        int w = 0, h = 0;
        uint32_t frame = 0;
        if (!present_image_ring_get_sequence_rgb(seq, &rgb, &w, &h, &frame)) break;
        if (!n) { newest = frame; w0 = w; h0 = h; }
        if (w != w0 || h != h0 || newest - frame > BR_CLIP_VBLANKS) { free(rgb); break; }
        frames[n] = rgb;
        fno[n] = frame;
        n++;
    }
    if (!n) return 0;
    /* oldest first */
    for (int i = 0; i < n / 2; i++) {
        uint8_t *t = frames[i]; frames[i] = frames[n - 1 - i]; frames[n - 1 - i] = t;
        uint32_t u = fno[i]; fno[i] = fno[n - 1 - i]; fno[n - 1 - i] = u;
    }
    int delay[BR_CLIP_MAX];
    for (int i = 0; i < n; i++) {
        const uint32_t d = i + 1 < n ? fno[i + 1] - fno[i] : 2;
        delay[i] = (int)((d * 100 + 30) / 60);
        if (delay[i] < 2) delay[i] = 2;   /* players slow anything shorter */
    }
    char path[800];
    br_path(path, sizeof path, "clip.gif");
    int ok = write_gif(path, frames, delay, n, w0, h0);
    br_path(path, sizeof path, "clip_sheet.png");
    (void)write_sheet(path, frames, n, w0, h0);
    for (int i = 0; i < n; i++) free(frames[i]);
    *span_vblanks = (int)(fno[n - 1] - fno[0]);
    return ok ? n : 0;
}

static void ring_release(void) {
    if (s_ring_ours) present_image_ring_set_frozen(0);
    s_ring_ours = 0;
}

/* ---- the moment ---------------------------------------------------------- */

static void build_types(void) {
    s_type_n = 0;
    for (int i = 0; i < s_title_type_n; i++) s_types[s_type_n++] = s_title_types[i];
    for (size_t i = 0; i < sizeof k_base_types / sizeof *k_base_types; i++)
        s_types[s_type_n++] = k_base_types[i];
}

static void bug_slot_forget(void) {
    char p[600], q[640];
    if (!savestate_slot_path(SAVESTATE_BUG_SLOT, p, sizeof p)) return;
    remove(p);
    size_t n = strlen(p);
    if (n > 4 && !strcmp(p + n - 4, ".pst")) {
        snprintf(q, sizeof q, "%.*s.thumb", (int)(n - 4), p); remove(q);
        snprintf(q, sizeof q, "%.*s.hdres", (int)(n - 4), p); remove(q);
    }
}

int bug_report_begin(void) {
    if (s_stage != BR_IDLE) return 0;
    if (psx_netplay_active()) {
        host_osd_push("Bug reports are off in netplay", 2000);
        return 0;
    }
    br_root();
    time_t t = time(NULL);
    struct tm lt = *localtime(&t);
    strftime(s_name, sizeof s_name, "%Y-%m-%d_%H%M%S", &lt);
    strftime(s_created, sizeof s_created, "%Y-%m-%dT%H:%M:%S", &lt);
    snprintf(s_folder, sizeof s_folder, "%s/%s", s_root, s_name);
    br_mkdir(s_folder);
    char path[800];
    br_path(path, sizeof path, "report.tmp");
    FILE *probe = fopen(path, "wb");
    if (!probe) {
        char msg[600];
        snprintf(msg, sizeof msg, "Bug report: cannot write to %s", s_root);
        host_osd_push(msg, 3000);
        return 0;
    }
    fclose(probe);
    remove(path);
#ifndef PSX_NO_DEBUG_TOOLS
    s_frame = s_frame_count;
#endif
    /* the clip: what was shown up to now */
    gl_renderer_render_thread_sync("bug_report");
    s_ring_ours = !present_image_ring_frozen();
    if (s_ring_ours) present_image_ring_set_frozen(1);
    br_path(path, sizeof path, "photo_ps1.png");
    s_photo_ps1 = write_ps1_picture(path);
    br_path(path, sizeof path, "photo.png");
    s_shot_seq0 = present_shot_seq();
    s_photo = present_shot_request(path) ? -1 : 0;
    bug_slot_forget();
    savestate_status_get(&s_save_gen0, NULL, NULL, NULL);
    s_state = savestate_request_save(SAVESTATE_BUG_SLOT) ? -1 : 0;
    build_types();
    if (s_type >= s_type_n) s_type = 0;
    s_row = PSX_MOD_BUG_ROW_TYPE;
    s_confirm = 0;
    s_text[0] = 0;
    s_caret = 0;
    s_clip_frames = 0;
    s_mark_n = s_marking_seen = 0;
    s_t0 = br_now_ms();
    s_stage = BR_CAPTURING;
    s_page_open = 0;
    return 1;
}

/* The title gave a page to send to, in a release build. A developer's own
 * builds keep their reports at home (PSX_BUG_REPORT_SEND=1 tests the path). */
static int send_available(void) {
    if (!s_issue_url[0]) return 0;
#ifdef PSX_NO_DEBUG_TOOLS
    return 1;
#else
    const char *e = getenv("PSX_BUG_REPORT_SEND");
    return e && *e == '1';
#endif
}

/* The form in Dear ImGui when the build and the window have it: the names
 * in the title's words, the description empty. */
static void ui_open(void) {
    static const char *types[BR_TYPES_MAX + 8], *sev[3], *freq[3];
    for (int i = 0; i < s_type_n; i++) types[i] = bug_report_tr(s_types[i]);
    for (int i = 0; i < 3; i++) {
        sev[i] = bug_report_tr(k_severities[i]);
        freq[i] = bug_report_tr(k_frequencies[i]);
    }
    BugReportUiForm f;
    memset(&f, 0, sizeof f);
    f.types = types; f.type_count = s_type_n; f.type = s_type;
    f.severities = sev; f.severity_count = 3; f.severity = s_sev;
    f.frequencies = freq; f.frequency_count = 3; f.frequency = s_freq;
    f.thumb = s_thumb; f.thumb_w = s_thumb_w; f.thumb_h = s_thumb_h;
    f.photo = s_photo > 0;
    f.state = s_state;
    f.clip_frames = s_clip_frames;
    f.can_send = send_available();
    s_ui = bug_report_ui_open(&f);
}
/* What the player chose and wrote, from the ImGui form. */
static void ui_take(void) {
    if (!s_ui) return;
    bug_report_ui_values(&s_type, &s_sev, &s_freq, s_text, (int)sizeof s_text);
    s_caret = (int)strlen(s_text);
    s_mark_n = bug_report_ui_marks(s_marks, BUG_REPORT_MARKS_MAX);
}
static void ui_end(void) {
    if (!s_ui) return;
    bug_report_ui_close();
    s_ui = 0;
}

int bug_report_tick(void) {
    if (s_stage == BR_CAPTURING) {
        const int late = br_now_ms() - s_t0 > BR_CAPTURE_MS;
        if (s_photo < 0 && present_shot_seq() != s_shot_seq0) s_photo = present_shot_ok();
        if (s_state < 0) {
            uint32_t gen = 0;
            int pending = 0, ok = 0, slot = -1;
            savestate_status_get(&gen, &pending, &ok, &slot);
            if (gen != s_save_gen0 && !pending) s_state = slot == SAVESTATE_BUG_SLOT && ok;
        }
        if (!late && (s_photo < 0 || s_state < 0)) return 0;
        if (s_photo < 0) s_photo = 0;
        gl_renderer_render_thread_sync("bug_report");
        take_thumb();
        {
            uint32_t lo = 0, hi = 0;
            present_image_ring_sequence_span(&lo, &hi);
            s_clip_frames = hi ? (int)(hi - lo + 1 < BR_CLIP_MAX ? hi - lo + 1 : BR_CLIP_MAX) : 0;
        }
        s_stage = BR_FORM;
    }
    if (s_stage != BR_FORM || s_page_open) return 0;
    if (!psx_mod_open_title_page(bug_report_page_render, bug_report_page_input)) return 0;
    s_page_open = 1;
    ui_open();
    return 1;
}

int bug_report_page_active(void) { return s_stage == BR_FORM && s_page_open; }
int bug_report_busy(void) { return s_stage != BR_IDLE; }

/* ---- writing ------------------------------------------------------------- */

static void slug(const char *s, char *out, size_t cap) {
    size_t n = 0;
    int dash = 0;
    for (; *s && n + 1 < cap; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && n) out[n++] = '-';
            out[n++] = c;
            dash = 0;
        } else {
            dash = 1;
        }
    }
    out[n] = 0;
}

static void exe_info(char *path, size_t cap, char *modified, size_t mcap) {
    path[0] = modified[0] = 0;
#ifdef _WIN32
    GetModuleFileNameA(NULL, path, (DWORD)cap);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &a)) {
        FILETIME lft;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&a.ftLastWriteTime, &lft);
        FileTimeToSystemTime(&lft, &st);
        snprintf(modified, mcap, "%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    }
#else
    (void)cap; (void)mcap;
#endif
}

static int copy_config(char names[][96], int max) {
    static const char *const k_base[] = { "settings.toml", "config.ini", "input.ini", "keybinds.ini", "game.toml" };
    char dir[800], to[900];
    int n = 0;
    br_path(dir, sizeof dir, "config");
    br_mkdir(dir);
    for (size_t i = 0; i < sizeof k_base / sizeof *k_base + (size_t)s_file_n && n < max; i++) {
        const char *src = i < sizeof k_base / sizeof *k_base ? k_base[i] : s_files[i - sizeof k_base / sizeof *k_base];
        if (!br_exists(src)) continue;
        const char *base = strrchr(src, '/');
        const char *b2 = strrchr(src, '\\');
        if (b2 > base) base = b2;
        base = base ? base + 1 : src;
        snprintf(to, sizeof to, "%s/%s", dir, base);
        if (br_copy(src, to)) snprintf(names[n++], 96, "%s", base);
    }
    if (!n) br_rmdir(dir);
    return n;
}

/* ---- the player's marks --------------------------------------------------- */

static void rgb_fill(uint8_t *rgb, uint32_t w, uint32_t h, int x0, int y0, int x1, int y1,
                     uint8_t r, uint8_t g, uint8_t b) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)w) x1 = (int)w;
    if (y1 > (int)h) y1 = (int)h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint8_t *p = rgb + ((size_t)y * w + (size_t)x) * 3;
            p[0] = r; p[1] = g; p[2] = b;
        }
}
static void rgb_frame(uint8_t *rgb, uint32_t w, uint32_t h, int x0, int y0, int x1, int y1, int t,
                      uint8_t r, uint8_t g, uint8_t b) {
    rgb_fill(rgb, w, h, x0, y0, x1, y0 + t, r, g, b);
    rgb_fill(rgb, w, h, x0, y1 - t, x1, y1, r, g, b);
    rgb_fill(rgb, w, h, x0, y0, x0 + t, y1, r, g, b);
    rgb_fill(rgb, w, h, x1 - t, y0, x1, y1, r, g, b);
}
/* Red rectangles, with a dark edge so they show on a red scene too. */
static void marks_draw(uint8_t *rgb, uint32_t w, uint32_t h) {
    const int t = w / 480 > 3 ? (int)(w / 480) : 3, e = t / 3 > 1 ? t / 3 : 1;
    for (int i = 0; i < s_mark_n; i++) {
        const int x0 = (int)(s_marks[i].x0 * (float)w), y0 = (int)(s_marks[i].y0 * (float)h);
        const int x1 = (int)(s_marks[i].x1 * (float)w), y1 = (int)(s_marks[i].y1 * (float)h);
        rgb_frame(rgb, w, h, x0 - e, y0 - e, x1 + e, y1 + e, t + 2 * e, 0, 0, 0);
        rgb_frame(rgb, w, h, x0, y0, x1, y1, t, 255, 32, 32);
    }
}

/* ---- report.json ---------------------------------------------------------- */

typedef struct BrMade {
    int state, clip, span, marked, ncfg, sent;
    char cfg[BR_FILES_MAX + 8][96];
} BrMade;

/* pub: the copy that goes out. No path of the player's machine in it. */
static void write_json(FILE *f, const BrMade *m, int pub) {
    char exe[600], exe_mod[32];
    exe_info(exe, sizeof exe, exe_mod, sizeof exe_mod);
    const char *exe_name = exe;
    if (pub) {
        for (const char *p = exe; *p; p++)
            if (*p == '/' || *p == '\\') exe_name = p + 1;
    }
    fprintf(f, "{\n  \"format\": 1,\n  \"id\": \"");
    json_str(f, s_name);
    fprintf(f, "\",\n  \"created\": \"%s\",\n  \"status\": \"new\",\n  \"auto\": false,\n", s_created);
    fprintf(f, "  \"type\": \""); json_str(f, s_types[s_type]);
    fprintf(f, "\",\n  \"severity\": \""); json_str(f, k_severities[s_sev]);
    fprintf(f, "\",\n  \"frequency\": \""); json_str(f, k_frequencies[s_freq]);
    fprintf(f, "\",\n  \"description\": \""); json_str(f, s_text);
    fprintf(f, "\",\n  \"files\": {\n");
    if (pub) {
        fprintf(f, "    \"photo\": %s,\n", s_photo > 0 ? "\"photo.jpg\"" : "null");
        fprintf(f, "    \"photo_ps1\": %s,\n", s_photo_ps1 ? "\"photo_ps1.png\"" : "null");
        fprintf(f, "    \"clip_sheet\": %s,\n", m->clip ? "\"clip_sheet.png\"" : "null");
        fprintf(f, "    \"private\": %s\n", m->sent ? "\"private.bin\"" : "null");
    } else {
        fprintf(f, "    \"photo\": %s,\n", s_photo > 0 ? "\"photo.png\"" : "null");
        fprintf(f, "    \"photo_marked\": %s,\n", m->marked ? "\"photo_marked.png\"" : "null");
        fprintf(f, "    \"photo_ps1\": %s,\n", s_photo_ps1 ? "\"photo_ps1.png\"" : "null");
        fprintf(f, "    \"state\": %s,\n", m->state ? "\"state.pst\"" : "null");
        fprintf(f, "    \"clip\": %s,\n", m->clip ? "\"clip.gif\"" : "null");
        fprintf(f, "    \"clip_sheet\": %s,\n", m->clip ? "\"clip_sheet.png\"" : "null");
        fprintf(f, "    \"config\": [");
        for (int i = 0; i < m->ncfg; i++) {
            fprintf(f, "%s\"config/", i ? ", " : "");
            json_str(f, m->cfg[i]);
            fputc('"', f);
        }
        fprintf(f, "]\n");
    }
    fprintf(f, "  },\n  \"marks\": [");
    for (int i = 0; i < s_mark_n; i++)
        fprintf(f, "%s{\"x\": %.4f, \"y\": %.4f, \"w\": %.4f, \"h\": %.4f}", i ? ", " : "", s_marks[i].x0,
                s_marks[i].y0, s_marks[i].x1 - s_marks[i].x0, s_marks[i].y1 - s_marks[i].y0);
    fprintf(f, "],\n");
    fprintf(f, "  \"clip\": {\"frames\": %d, \"vblanks\": %d},\n", m->clip, m->span);
    fprintf(f, "  \"build\": {\"version\": \"%s\", \"execution\": \"%s\"},\n", PSX_GAME_VERSION, PSX_EXECUTION_ID);
    GlPacing pace;
    memset(&pace, 0, sizeof pace);
    (void)gl_renderer_present_pacing(5.0, &pace);
    fprintf(f, "  \"runtime\": {\n    \"exe\": \"");
    json_str(f, exe_name);
    fprintf(f, "\",\n    \"exe_modified\": \"%s\",\n    \"frame\": %llu,\n", exe_mod, (unsigned long long)s_frame);
    fprintf(f, "    \"present_fps\": %.1f, \"generated\": %d, \"frame_ms\": %.2f, \"p99_ms\": %.2f, "
               "\"max_ms\": %.2f, \"low1_fps\": %.1f\n  }",
            pace.fps, pace.generated, pace.avg_ms, pace.p99_ms, pace.max_ms, pace.low1_fps);
    if (s_info) {
        static char game[16384];
        game[0] = 0;
        if (s_info(game, (int)sizeof game) > 0 && game[0])
            fprintf(f, ",\n  \"game\": {%s}", game);
    }
    fprintf(f, ",\n  \"history\": [{\"at\": \"%s\", \"status\": \"new\", \"by\": \"player\"}]\n}\n", s_created);
}

/* ---- what a release build sends ------------------------------------------- */

typedef struct BrBuf { uint8_t *p; size_t n, cap; int failed; } BrBuf;

static void buf_put(void *ctx, void *data, int size) {
    BrBuf *b = (BrBuf *)ctx;
    if (b->failed || size <= 0) return;
    if (b->n + (size_t)size > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1u << 20;
        while (cap < b->n + (size_t)size) cap *= 2;
        uint8_t *q = (uint8_t *)realloc(b->p, cap);
        if (!q) { b->failed = 1; return; }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, (size_t)size);
    b->n += (size_t)size;
}

/* The photo, at most BR_JPEG_WIDTH wide (2.3 MB as a PNG, about 0.4 here). */
static int photo_jpeg(const uint8_t *rgb, uint32_t w, uint32_t h, BrBuf *out) {
    const uint32_t div = (w + BR_JPEG_WIDTH - 1) / BR_JPEG_WIDTH;
    const uint32_t sw = w / div, sh = h / div;
    uint8_t *small = (uint8_t *)malloc((size_t)sw * sh * 3);
    if (!small) return 0;
    for (uint32_t y = 0; y < sh; y++)
        for (uint32_t x = 0; x < sw; x++)
            for (int c = 0; c < 3; c++) {
                uint32_t sum = 0;
                for (uint32_t j = 0; j < div; j++)
                    for (uint32_t i = 0; i < div; i++)
                        sum += rgb[((size_t)(y * div + j) * w + (x * div + i)) * 3 + (size_t)c];
                small[((size_t)y * sw + x) * 3 + (size_t)c] = (uint8_t)(sum / (div * div));
            }
    const int ok = stbi_write_jpg_to_func(buf_put, out, (int)sw, (int)sh, 3, small, BR_JPEG_QUALITY);
    free(small);
    return ok && !out->failed;
}

static void url_byte(char *out, size_t *n, unsigned char c) {
    static const char hex[] = "0123456789ABCDEF";
    const int plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_' || c == '.' || c == '~';
    if (plain) {
        out[(*n)++] = (char)c;
    } else {
        out[(*n)++] = '%';
        out[(*n)++] = hex[c >> 4];
        out[(*n)++] = hex[c & 15];
    }
}
static void url_text(char *out, size_t *n, const char *s) {
    for (; *s; s++) url_byte(out, n, (unsigned char)*s);
}

/* The page of a new issue, filled in. The description stops short rather than
 * make the address too long: all of it is in the ZIP. */
static void issue_url(char *out, size_t cap) {
    char title[140], head[420], tail[300], first[80];
    size_t n = 0, fl = 0;
    (void)cap;
    for (; s_text[fl] && s_text[fl] != '\n' && fl + 1 < sizeof first - 4; fl++) first[fl] = s_text[fl];
    while (fl > 0 && ((unsigned char)s_text[fl] & 0xC0) == 0x80) fl--;   /* a whole character */
    first[fl] = 0;
    snprintf(title, sizeof title, "[%s] %s", s_types[s_type], fl ? first : "Bug report");
    snprintf(head, sizeof head, "**Type:** %s | **Severity:** %s | **Frequency:** %s\n**Version:** %s (%.12s)\n"
             "**Report:** %s\n\n", s_types[s_type], k_severities[s_sev], k_frequencies[s_freq], PSX_GAME_VERSION,
             PSX_EXECUTION_ID, s_name);
    snprintf(tail, sizeof tail, "\n\n---\n**Before you press Submit:** drag the file `%s.zip` into this box. "
             "The folder with it has just opened.\n", s_name);
    n = (size_t)snprintf(out, cap, "%s?title=", s_issue_url);
    url_text(out, &n, title);
    memcpy(out + n, "&body=", 6);
    n += 6;
    url_text(out, &n, head);
    size_t tail_n = 0;
    {
        static char enc[1024];
        url_text(enc, &tail_n, tail);
        const size_t room = BR_URL_MAX - tail_n;
        for (const char *p = s_text; *p;) {
            int len = 1;
            const unsigned char c = (unsigned char)*p;
            if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
            if (n + (size_t)len * 3 >= room) break;
            for (int i = 0; i < len && p[i]; i++) url_byte(out, &n, (unsigned char)p[i]);
            p += len;
        }
        memcpy(out + n, enc, tail_n);
        n += tail_n;
    }
    out[n] = 0;
}

static void reveal_file(const char *path) {
#ifdef _WIN32
    wchar_t wide[1024], args[1100];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1024) <= 0) return;
    for (wchar_t *c = wide; *c; c++) if (*c == L'/') *c = L'\\';
    _snwprintf(args, 1100, L"/select,\"%ls\"", wide);
    args[1099] = 0;
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
#else
    char url[1024];
    snprintf(url, sizeof url, "file://%s", s_folder);
    (void)path;
    SDL_OpenURL(url);
#endif
}

/* <id>.zip in the folder: what anyone may read, and the save state and the
 * settings in a box only the title's developer opens. Then the issue's page
 * and the folder, for the player to drag the file. A title without a key sends
 * no state and no settings. */
static int send_report(BrMade *m, const uint8_t *rgb, uint32_t w, uint32_t h) {
    char path[800], name[160];
    BrZip inner, zip;
    uint8_t *box = NULL;
    size_t box_n = 0;
    if (s_send_has_key) {
        br_zip_init(&inner);
        br_path(path, sizeof path, "state.pst");
        if (m->state) (void)br_zip_add_file(&inner, "state.pst", path);
        for (int i = 0; i < m->ncfg; i++) {
            char member[120];
            snprintf(member, sizeof member, "config/%s", m->cfg[i]);
            br_path(path, sizeof path, member);
            (void)br_zip_add_file(&inner, member, path);
        }
        if (br_zip_finish(&inner)) {
            box = (uint8_t *)malloc(inner.len + BR_SEAL_OVERHEAD);
            if (box && br_seal(s_send_key, inner.buf, inner.len, box)) box_n = inner.len + BR_SEAL_OVERHEAD;
        }
        br_zip_free(&inner);
    }
    m->sent = box_n != 0;

    br_zip_init(&zip);
    br_path(path, sizeof path, "report_public.tmp");
    FILE *f = fopen(path, "wb");
    if (f) {
        write_json(f, m, 1);
        fclose(f);
        (void)br_zip_add_file(&zip, "report.json", path);
        remove(path);
    }
    if (rgb) {
        BrBuf jpg = { 0 };
        if (photo_jpeg(rgb, w, h, &jpg)) br_zip_add(&zip, "photo.jpg", jpg.p, jpg.n);
        free(jpg.p);
    }
    br_path(path, sizeof path, "photo_ps1.png");
    if (s_photo_ps1) (void)br_zip_add_file(&zip, "photo_ps1.png", path);
    br_path(path, sizeof path, "clip_sheet.png");
    if (m->clip) (void)br_zip_add_file(&zip, "clip_sheet.png", path);
    if (box_n) br_zip_add(&zip, "private.bin", box, box_n);
    free(box);
    int ok = br_zip_finish(&zip);
    snprintf(name, sizeof name, "%s.zip", s_name);
    br_path(path, sizeof path, name);
    if (ok) {
        f = fopen(path, "wb");
        ok = f && fwrite(zip.buf, 1, zip.len, f) == zip.len;
        if (f && fclose(f) != 0) ok = 0;
    }
    br_zip_free(&zip);
    if (!ok) return 0;

    static char url[BR_URL_MAX + 1024];
    issue_url(url, sizeof url);
    char url_path[800];
    br_path(url_path, sizeof url_path, "issue_url.txt");
    f = fopen(url_path, "wb");
    if (f) {
        fputs(url, f);
        fclose(f);
    }
    if (!getenv("PSX_BUG_REPORT_NO_OPEN")) {
        SDL_OpenURL(url);
        reveal_file(path);
    }
    return 1;
}

static void write_report(int send) {
    char path[800], from[600], typeslug[64];
    BrMade m;
    memset(&m, 0, sizeof m);
    /* the folder's name says the type */
    slug(s_types[s_type], typeslug, sizeof typeslug);
    {
        char renamed[700];
        snprintf(renamed, sizeof renamed, "%s/%s_%s", s_root, s_name, typeslug);
        if (br_rename(s_folder, renamed)) {
            snprintf(s_folder, sizeof s_folder, "%s", renamed);
            snprintf(s_name + strlen(s_name), sizeof s_name - strlen(s_name), "_%s", typeslug);
        }
    }
    /* the state: a save that ran late may have landed since */
    if (savestate_slot_path(SAVESTATE_BUG_SLOT, from, sizeof from) && br_exists(from)) {
        br_path(path, sizeof path, "state.pst");
        m.state = br_copy(from, path);
    }
    bug_slot_forget();
    /* the pictures the present and the debug server wrote, compressed */
    uint8_t *rgb = NULL;
    uint32_t pw = 0, ph = 0;
    br_path(path, sizeof path, "photo.png");
    if (s_photo > 0) (void)png_recompress(path, &rgb, &pw, &ph);
    br_path(path, sizeof path, "photo_ps1.png");
    if (s_photo_ps1) (void)png_recompress(path, NULL, NULL, NULL);
    if (rgb && s_mark_n) {
        marks_draw(rgb, pw, ph);
        br_path(path, sizeof path, "photo_marked.png");
        m.marked = png_write_z(path, rgb, pw, ph);
    }
    m.clip = write_clip(&m.span);
    ring_release();
    m.ncfg = copy_config(m.cfg, BR_FILES_MAX + 8);
    const int sent = send ? send_report(&m, rgb, pw, ph) : 0;
    free(rgb);

    br_path(path, sizeof path, "report.json");
    FILE *f = fopen(path, "wb");
    if (!f) {
        host_osd_push("Bug report: could not write report.json", 3000);
        return;
    }
    write_json(f, &m, 0);
    fclose(f);
    char msg[300];
    snprintf(msg, sizeof msg, "%s: %s", bug_report_tr(send && !sent ? "Bug report saved, not packed" :
                                                      "Bug report saved"), s_name);
    host_osd_push(msg, 3500);
}

static void discard_report(void) {
    static const char *const k_files[] = { "photo.png", "photo_ps1.png", "report.tmp" };
    char path[800];
    /* a picture still pending would land later: take it now, gone or not */
    for (size_t i = 0; i < sizeof k_files / sizeof *k_files; i++) {
        br_path(path, sizeof path, k_files[i]);
        remove(path);
    }
    br_rmdir(s_folder);
    bug_slot_forget();
    ring_release();
    host_osd_push(bug_report_tr("Bug report discarded"), 1500);
}

/* 0 discard, 1 save, 2 save and send. */
static void finish(int how) {
    ui_take();
    ui_end();
    if (how) write_report(how == 2);
    else discard_report();
    free(s_thumb);
    s_thumb = NULL;
    s_stage = BR_IDLE;
    s_page_open = 0;
}

void bug_report_reset(void) {
    if (s_stage != BR_IDLE) {
        ui_end();
        discard_report();
        free(s_thumb);
        s_thumb = NULL;
        s_stage = BR_IDLE;
        s_page_open = 0;
    }
    for (int i = 0; i < s_title_type_n; i++) free(s_title_types[i]);
    for (int i = 0; i < s_file_n; i++) free(s_files[i]);
    for (int i = 0; i < s_word_n; i++) { free(s_word_en[i]); free(s_word_shown[i]); }
    s_title_type_n = s_file_n = s_word_n = 0;
    s_render = NULL;
    s_info = NULL;
    s_issue_url[0] = 0;
    s_send_has_key = 0;
    s_type = 0;
}

/* ---- the form ------------------------------------------------------------ */

static int utf8_prev(int at) {
    while (at > 0 && ((unsigned char)s_text[at - 1] & 0xC0) == 0x80) at--;
    return at > 0 ? at - 1 : 0;
}
static int utf8_next(int at) {
    const int n = (int)strlen(s_text);
    if (at >= n) return n;
    at++;
    while (at < n && ((unsigned char)s_text[at] & 0xC0) == 0x80) at++;
    return at;
}

static void text_insert(const char *s) {
    const int n = (int)strlen(s_text), add = (int)strlen(s);
    if (add <= 0 || n + add > BR_TEXT_MAX) return;
    memmove(s_text + s_caret + add, s_text + s_caret, (size_t)(n - s_caret + 1));
    memcpy(s_text + s_caret, s, (size_t)add);
    s_caret += add;
}
static void text_erase(int from, int to) {
    const int n = (int)strlen(s_text);
    if (from < 0 || to > n || from >= to) return;
    memmove(s_text + from, s_text + to, (size_t)(n - to + 1));
    s_caret = from;
}

void bug_report_page_text(const char *utf8) {
    if (!bug_report_page_active() || !utf8) return;
    char clean[256];
    size_t n = 0;
    for (; *utf8 && n + 1 < sizeof clean; utf8++)
        if ((unsigned char)*utf8 >= 0x20 || *utf8 == '\n') clean[n++] = *utf8;
    clean[n] = 0;
    if (!n) return;
    if (s_ui) {
        bug_report_ui_text(clean);
        return;
    }
    if (s_row != PSX_MOD_BUG_ROW_TEXT) {
        s_row = PSX_MOD_BUG_ROW_TEXT;
        s_caret = (int)strlen(s_text);
    }
    s_confirm = 0;
    text_insert(clean);
    psx_mod_title_page_redraw();
}

static void cycle(int *v, int n, int d) { *v = ((*v + d) % n + n) % n; }

/* 0 = the form closes (saved or discarded). */
static int activate_discard(void) {
    if (s_text[0] && !s_confirm) {
        s_confirm = 1;
        return 1;
    }
    finish(0);
    return 0;
}

int bug_report_page_input(uint32_t pressed, uint32_t held, int key, int mods) {
    if (s_stage != BR_FORM) return 0;
    if (s_ui) {
        /* the ImGui form takes its events itself; here the controller, the
         * debug server's keys and the player's save / discard */
        if (key) bug_report_ui_key(key, mods);
        else bug_report_ui_pad(held);
        /* the picture shows bright while the player marks it */
        const int marking = bug_report_ui_marking();
        if (marking != s_marking_seen) {
            s_marking_seen = marking;
            psx_mod_title_page_redraw();
        }
        const int act = bug_report_ui_action();
        if (!act) return 1;
        finish(act == BUG_REPORT_UI_SEND ? 2 : act == BUG_REPORT_UI_SAVE ? 1 : 0);
        return 0;
    }
    const int was_confirm = s_confirm;
    int redraw = 0, up = 0, down = 0, left = 0, right = 0, ok = 0, back = 0, save = 0;
    if (key) {
        const int ctrl = (mods & KMOD_CTRL) != 0, shift = (mods & KMOD_SHIFT) != 0;
        switch (key) {
        case SDLK_UP: up = 1; break;
        case SDLK_DOWN: down = 1; break;
        case SDLK_LEFT: left = 1; break;
        case SDLK_RIGHT: right = 1; break;
        case SDLK_TAB: if (shift) up = 1; else down = 1; break;
        case SDLK_ESCAPE: back = 1; break;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            if (ctrl) save = 1;
            else if (shift && s_row == PSX_MOD_BUG_ROW_TEXT) { text_insert("\n"); redraw = 1; }
            else ok = 1;
            break;
        case SDLK_BACKSPACE:
            if (s_row == PSX_MOD_BUG_ROW_TEXT && s_caret > 0) { text_erase(utf8_prev(s_caret), s_caret); redraw = 1; }
            break;
        case SDLK_DELETE:
            if (s_row == PSX_MOD_BUG_ROW_TEXT) { text_erase(s_caret, utf8_next(s_caret)); redraw = 1; }
            break;
        case SDLK_HOME: if (s_row == PSX_MOD_BUG_ROW_TEXT) { s_caret = 0; redraw = 1; } break;
        case SDLK_END: if (s_row == PSX_MOD_BUG_ROW_TEXT) { s_caret = (int)strlen(s_text); redraw = 1; } break;
        case SDLK_v:
            if (ctrl) {
                char *clip = SDL_GetClipboardText();
                if (clip) {
                    if (s_row != PSX_MOD_BUG_ROW_TEXT) { s_row = PSX_MOD_BUG_ROW_TEXT; s_caret = (int)strlen(s_text); }
                    for (char *c = clip; *c; c++) if (*c == '\r' || *c == '\t') *c = ' ';
                    text_insert(clip);
                    SDL_free(clip);
                    redraw = 1;
                }
            }
            break;
        default: break;
        }
    } else {
        up = (pressed & PSX_MOD_PAGE_UP) != 0;
        down = (pressed & PSX_MOD_PAGE_DOWN) != 0;
        left = (pressed & PSX_MOD_PAGE_LEFT) != 0;
        right = (pressed & PSX_MOD_PAGE_RIGHT) != 0;
        ok = (pressed & PSX_MOD_PAGE_SOUTH) != 0;
        back = (pressed & PSX_MOD_PAGE_EAST) != 0;
        save = (pressed & PSX_MOD_PAGE_START) != 0;
    }
    if (!(up | down | left | right | ok | back | save | redraw)) return 1;
    s_confirm = 0;
    if (save) { finish(1); return 0; }
    if (back) {
        s_confirm = was_confirm;
        if (!activate_discard()) return 0;
        psx_mod_title_page_redraw();
        return 1;
    }
    if (up) s_row = s_row > 0 ? s_row - 1 : PSX_MOD_BUG_ROWS - 1;
    if (down) s_row = s_row + 1 < PSX_MOD_BUG_ROWS ? s_row + 1 : 0;
    if (left || right) {
        const int d = right ? 1 : -1;
        switch (s_row) {
        case PSX_MOD_BUG_ROW_TYPE: cycle(&s_type, s_type_n, d); break;
        case PSX_MOD_BUG_ROW_SEVERITY: cycle(&s_sev, 3, d); break;
        case PSX_MOD_BUG_ROW_FREQUENCY: cycle(&s_freq, 3, d); break;
        case PSX_MOD_BUG_ROW_TEXT: s_caret = right ? utf8_next(s_caret) : (s_caret > 0 ? utf8_prev(s_caret) : 0); break;
        case PSX_MOD_BUG_ROW_SAVE: s_row = PSX_MOD_BUG_ROW_DISCARD; break;
        default: s_row = PSX_MOD_BUG_ROW_SAVE; break;
        }
    }
    if (ok) {
        if (s_row == PSX_MOD_BUG_ROW_SAVE) { finish(1); return 0; }
        if (s_row == PSX_MOD_BUG_ROW_DISCARD) {
            s_confirm = was_confirm;
            if (!activate_discard()) return 0;
        } else {
            s_row++;
        }
    }
    psx_mod_title_page_redraw();
    return 1;
}

/* The framework's own look: its 8x8 font (capitals; accents folded). */
static char fold(unsigned cp) {
    static const char *const k_latin = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp < 0x80) return (char)cp;
    if (cp >= 0xC0 && cp <= 0xFF) return k_latin[cp - 0xC0];
    return '?';
}

/* The description as display characters (one per code point) and where the
 * caret falls among them. */
static int display_text(char *out, int cap, int *caret_at) {
    int n = 0;
    *caret_at = -1;
    for (int i = 0; s_text[i] && n + 1 < cap;) {
        if (i == s_caret) *caret_at = n;
        const unsigned char c = (unsigned char)s_text[i];
        unsigned cp = c;
        int len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        for (int k = 1; k < len && s_text[i + k]; k++) cp = cp << 6 | ((unsigned char)s_text[i + k] & 0x3F);
        out[n++] = fold(cp);
        i += len;
    }
    if (*caret_at < 0) *caret_at = n;
    out[n] = 0;
    return n;
}

static void fill(uint32_t *d, int dw, int dh, int x, int y, int w, int h, uint32_t c) {
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < dh; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < dw; xx++) d[yy * dw + xx] = c;
}

static int default_render(uint32_t *d, int w, int h) {
    const int s = h >= 480 ? 2 : 1, lh = 12 * s, x0 = 10 * s;
    const uint32_t bg = 0xE8141820u, sel = 0xFF2C3A55u, fg = 0xFFE2E5EBu, dim = 0xFF8C94A3u, hi = 0xFFFFD24Du;
    fill(d, w, h, 0, 0, w, h, bg);
    psx_ui_text(d, w, h, x0, 8 * s, "BUG REPORT", hi, s);
    /* the moment */
    int tw = 0, th = 0;
    if (s_thumb && s_thumb_h) {
        th = 64 * s;
        tw = s_thumb_w * th / s_thumb_h;
        const int tx = w - tw - 8 * s, ty = 6 * s;
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++)
                d[(ty + y) * w + tx + x] = s_thumb[(y * s_thumb_h / th) * s_thumb_w + x * s_thumb_w / tw];
    }
    const char *const label[3] = { "TYPE", "SEVERITY", "FREQUENCY" };
    const char *const value[3] = { s_types[s_type], k_severities[s_sev], k_frequencies[s_freq] };
    for (int r = 0; r < 3; r++) {
        const int y = (24 + r * 14) * s;
        if (s_row == r) fill(d, w, h, x0 - 4 * s, y - 3 * s, w - tw - 24 * s, lh, sel);
        psx_ui_text(d, w, h, x0, y, label[r], dim, s);
        char buf[96];
        snprintf(buf, sizeof buf, "< %s >", value[r]);
        psx_ui_text(d, w, h, x0 + 88 * s, y, buf, s_row == r ? hi : fg, s);
    }
    /* the description, wrapped */
    const int ty = 82 * s, cols = (w - 2 * x0) / (8 * s), rows = 9;
    fill(d, w, h, x0 - 4 * s, ty - 4 * s, w - 2 * x0 + 8 * s, (rows * 10 + 6) * s,
         s_row == PSX_MOD_BUG_ROW_TEXT ? sel : 0xFF1E2430u);
    psx_ui_text(d, w, h, x0, ty - 12 * s, s_text[0] || s_row == PSX_MOD_BUG_ROW_TEXT ? "DESCRIPTION" :
                "DESCRIPTION (TYPE ON THE KEYBOARD)", dim, s);
    {
        static char disp[BR_TEXT_MAX + 1];
        int caret = 0;
        const int n = display_text(disp, sizeof disp, &caret);
        /* lines of `cols`, breaking at newlines; the caret's line kept in view */
        int line_start[512], nl = 0, caret_line = 0;
        line_start[nl++] = 0;
        for (int i = 0, col = 0; i < n && nl < 511; i++) {
            if (disp[i] == '\n') { line_start[nl++] = i + 1; col = 0; continue; }
            if (++col >= cols) { line_start[nl++] = i + 1; col = 0; }
        }
        for (int l = 0; l < nl; l++) if (line_start[l] <= caret) caret_line = l;
        const int first = caret_line >= rows ? caret_line - rows + 1 : 0;
        for (int l = first; l < nl && l < first + rows; l++) {
            const int a = line_start[l], b = l + 1 < nl ? line_start[l + 1] : n;
            char line[256];
            int k = 0;
            for (int i = a; i < b && k < 255; i++) if (disp[i] != '\n') line[k++] = disp[i];
            line[k] = 0;
            const int y = ty + (l - first) * 10 * s;
            psx_ui_text(d, w, h, x0, y, line, fg, s);
            if (l == caret_line && s_row == PSX_MOD_BUG_ROW_TEXT)
                fill(d, w, h, x0 + (caret - a) * 8 * s, y + 8 * s, 7 * s, s, hi);
        }
    }
    /* what was taken */
    char got[160];
    snprintf(got, sizeof got, "PHOTO %s  STATE %s  CLIP %d FRAMES", s_photo > 0 ? "OK" : "-",
             s_state > 0 ? "OK" : s_state < 0 ? "..." : "-", s_clip_frames);
    psx_ui_text(d, w, h, x0, 186 * s, got, dim, s);
    /* buttons */
    const int by = 206 * s;
    if (s_row == PSX_MOD_BUG_ROW_SAVE) fill(d, w, h, x0 - 4 * s, by - 3 * s, 120 * s, lh, sel);
    if (s_row == PSX_MOD_BUG_ROW_DISCARD) fill(d, w, h, x0 + 124 * s, by - 3 * s, 140 * s, lh, sel);
    psx_ui_text(d, w, h, x0, by, "SAVE REPORT", s_row == PSX_MOD_BUG_ROW_SAVE ? hi : fg, s);
    psx_ui_text(d, w, h, x0 + 128 * s, by, s_confirm ? "DISCARD? AGAIN" : "DISCARD",
                s_row == PSX_MOD_BUG_ROW_DISCARD ? hi : fg, s);
    psx_ui_text(d, w, h, x0, 224 * s, "CTRL+ENTER SAVE  ESC DISCARD", dim, s);
    return 1;
}

int bug_report_page_render(uint32_t *argb, int w, int h) {
    if (s_stage != BR_FORM) return 0;
    if (s_ui) {   /* the frozen game, dimmed, under the ImGui form */
        const uint32_t veil = bug_report_ui_marking() ? 0u : 0xA8000000u;
        for (int i = 0; i < w * h; i++) argb[i] = veil;
        return 1;
    }
    if (s_render) {
        PSXModBugReportView v;
        memset(&v, 0, sizeof v);
        v.size = (int)sizeof v;
        v.row = s_row;
        v.types = s_types; v.type_count = s_type_n; v.type = s_type;
        v.severities = k_severities; v.severity_count = 3; v.severity = s_sev;
        v.frequencies = k_frequencies; v.frequency_count = 3; v.frequency = s_freq;
        v.text = s_text;
        v.caret = s_caret;
        v.photo = s_photo > 0;
        v.state = s_state;
        v.clip_frames = s_clip_frames;
        v.thumb = s_thumb; v.thumb_w = s_thumb_w; v.thumb_h = s_thumb_h;
        v.confirm_discard = s_confirm;
        v.folder = s_folder;
        if (s_render(&v, argb, w, h)) return 1;
    }
    return default_render(argb, w, h);
}

/* ---- a frozen or crashed game ------------------------------------------- */

void bug_report_note_freeze(const char *dump_path, unsigned wedge_kind) {
    char root[512], folder[700], path[800], name[96], created[32], full[600];
    br_root_path(root, sizeof root);
    br_mkdir(root);
    time_t t = time(NULL);
    struct tm lt = *localtime(&t);
    strftime(name, sizeof name, "%Y-%m-%d_%H%M%S_freeze", &lt);
    strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%S", &lt);
    snprintf(folder, sizeof folder, "%s/%s", root, name);
    br_mkdir(folder);
    snprintf(path, sizeof path, "%s/report.json", folder);
    FILE *f = fopen(path, "wb");
    if (!f) return;
#ifdef _WIN32
    if (!_fullpath(full, dump_path, sizeof full)) snprintf(full, sizeof full, "%s", dump_path);
#else
    if (!realpath(dump_path, full)) snprintf(full, sizeof full, "%s", dump_path);
#endif
    char exe[600], exe_mod[32];
    exe_info(exe, sizeof exe, exe_mod, sizeof exe_mod);
    fprintf(f, "{\n  \"format\": 1,\n  \"id\": \"%s\",\n  \"created\": \"%s\",\n  \"status\": \"new\",\n"
               "  \"auto\": true,\n  \"type\": \"Crash / freeze\",\n  \"severity\": \"Critical\",\n"
               "  \"frequency\": \"Once\",\n  \"description\": \"Written by the framework: the game "
               "stopped responding (wedge kind %u). See the freeze dump.\",\n  \"files\": {\"freeze_dump\": \"",
            name, created, wedge_kind);
    json_str(f, full);
    fprintf(f, "\"},\n  \"runtime\": {\"exe\": \"");
    json_str(f, exe);
    fprintf(f, "\", \"exe_modified\": \"%s\"},\n  \"history\": [{\"at\": \"%s\", \"status\": \"new\", "
               "\"by\": \"framework\"}]\n}\n", exe_mod, created);
    fclose(f);
}

/* ---- the title's additions and the debug server -------------------------- */

void psx_mod_set_bug_report_renderer(PSXModBugReportRenderer render) { s_render = render; }
void psx_mod_set_bug_report_info(PSXModBugReportInfo info) { s_info = info; }
void psx_mod_set_bug_report_send(const char *new_issue_url, const uint8_t public_key[32]) {
    snprintf(s_issue_url, sizeof s_issue_url, "%s", new_issue_url ? new_issue_url : "");
    s_send_has_key = public_key != NULL;
    if (public_key) memcpy(s_send_key, public_key, sizeof s_send_key);
}
void psx_mod_bug_report_add_type(const char *name) {
    if (!name || !*name || s_title_type_n >= BR_TYPES_MAX) return;
    for (int i = 0; i < s_title_type_n; i++) if (!strcmp(s_title_types[i], name)) return;
    s_title_types[s_title_type_n] = (char *)malloc(strlen(name) + 1);
    if (s_title_types[s_title_type_n]) strcpy(s_title_types[s_title_type_n++], name);
}
void psx_mod_bug_report_add_file(const char *path) {
    if (!path || !*path || s_file_n >= BR_FILES_MAX) return;
    for (int i = 0; i < s_file_n; i++) if (!strcmp(s_files[i], path)) return;
    s_files[s_file_n] = (char *)malloc(strlen(path) + 1);
    if (s_files[s_file_n]) strcpy(s_files[s_file_n++], path);
}
void psx_mod_bug_report_translate(const char *english, const char *shown) {
    if (!english || !*english || !shown) return;
    int i = 0;
    while (i < s_word_n && strcmp(s_word_en[i], english)) i++;
    if (i == BR_WORDS_MAX) return;
    char *s = (char *)malloc(strlen(shown) + 1);
    if (!s) return;
    strcpy(s, shown);
    if (i < s_word_n) {
        free(s_word_shown[i]);
        s_word_shown[i] = s;
        return;
    }
    s_word_en[i] = (char *)malloc(strlen(english) + 1);
    if (!s_word_en[i]) { free(s); return; }
    strcpy(s_word_en[i], english);
    s_word_shown[i] = s;
    s_word_n++;
}
const char *bug_report_tr(const char *english) {
    if (!english) return english;
    for (int i = 0; i < s_word_n; i++)
        if (!strcmp(s_word_en[i], english)) return s_word_shown[i];
    if (!strcmp(psx_mod_host_language(), "pt"))
        for (size_t i = 0; i < sizeof k_words_pt / sizeof *k_words_pt; i++)
            if (!strcmp(k_words_pt[i][0], english)) return k_words_pt[i][1];
    return english;
}

/* The system's first language, PSX_HOST_LANGUAGE winning (tests). */
const char *psx_mod_host_language(void) {
    static char lang[8];
    if (lang[0]) return lang;
    const char *e = getenv("PSX_HOST_LANGUAGE");
    if (e && *e) {
        snprintf(lang, sizeof lang, "%s", e);
        return lang;
    }
#if defined(PSX_SDL3)
    int n = 0;
    SDL_Locale **list = SDL_GetPreferredLocales(&n);
    if (list && n > 0 && list[0]->language) snprintf(lang, sizeof lang, "%s", list[0]->language);
    SDL_free(list);
#else
    SDL_Locale *list = SDL_GetPreferredLocales();
    if (list && list[0].language) snprintf(lang, sizeof lang, "%s", list[0].language);
    SDL_free(list);
#endif
    if (!lang[0]) snprintf(lang, sizeof lang, "en");
    return lang;
}
int psx_mod_bug_report_open(void) { return bug_report_begin(); }
int psx_mod_bug_report_active(void) { return bug_report_busy(); }

int bug_report_debug(const char *op, const char *text, int key, int row, int value, char *out, size_t cap) {
    int ok = 1;
    if (s_stage == BR_FORM) ui_take();
    if (!op || !strcmp(op, "status")) {
    } else if (!strcmp(op, "open")) {
        ok = bug_report_begin();
    } else if (!strcmp(op, "text")) {
        ok = bug_report_page_active();
        if (ok) bug_report_page_text(text ? text : "");
    } else if (!strcmp(op, "key")) {
        ok = bug_report_page_active();
        if (ok && !bug_report_page_input(0, 0, key, value)) {
            /* the form closed: main's loop sees the page gone on its next tick */
            extern void psx_bug_report_close_page(void);
            psx_bug_report_close_page();
        }
    } else if (!strcmp(op, "set")) {
        ok = s_stage == BR_FORM;
        if (ok) {
            if (row == PSX_MOD_BUG_ROW_TYPE && value >= 0 && value < s_type_n) s_type = value;
            else if (row == PSX_MOD_BUG_ROW_SEVERITY && value >= 0 && value < 3) s_sev = value;
            else if (row == PSX_MOD_BUG_ROW_FREQUENCY && value >= 0 && value < 3) s_freq = value;
            else if (row >= 0 && row < PSX_MOD_BUG_ROWS && value < 0) s_row = row;
            else ok = 0;
            if (ok && value >= 0) bug_report_ui_set(row, value);
            psx_mod_title_page_redraw();
        }
    } else if (!strcmp(op, "save") || !strcmp(op, "discard") || !strcmp(op, "send")) {
        ok = s_stage == BR_FORM && (strcmp(op, "send") || send_available());
        if (ok) {
            s_confirm = 1;
            finish(!strcmp(op, "send") ? 2 : !strcmp(op, "save"));
            extern void psx_bug_report_close_page(void);
            psx_bug_report_close_page();
        }
    } else {
        ok = 0;
    }
    int n = snprintf(out, cap, "\"done\":%s,\"stage\":%d,\"page\":%d,\"folder\":\"", ok ? "true" : "false",
                     s_stage, s_page_open);
    for (const char *p = s_folder; *p && (size_t)n + 2 < cap; p++) out[n++] = *p == '\\' ? '/' : *p;
    n += snprintf(out + n, cap > (size_t)n ? cap - (size_t)n : 0,
                  "\",\"row\":%d,\"type\":%d,\"types\":%d,\"severity\":%d,\"frequency\":%d,"
                  "\"photo\":%d,\"photo_ps1\":%d,\"state\":%d,\"clip_frames\":%d,\"text_bytes\":%d,"
                  "\"marks\":%d,\"marking\":%d,\"can_send\":%d,\"language\":\"%s\"",
                  s_row, s_type, s_type_n, s_sev, s_freq, s_photo, s_photo_ps1, s_state, s_clip_frames,
                  (int)strlen(s_text), s_mark_n, bug_report_ui_marking(), send_available(),
                  psx_mod_host_language());
    return n;
}
