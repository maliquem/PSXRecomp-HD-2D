/* test_sw_faithful_authority.c -- the netplay CPU-authoritative rasterizer
 * draws the hardware image whatever presentation enhancements are armed.
 *
 * Under netplay the software rasterizer (dual-raster SW@1x, or the software
 * GPU) owns the guest-visible VRAM: GP0 C0 readback and savestates come from
 * it. PGXP's perspective/precise overrides depend on host-only shadows (a
 * rollback load drops them on one peer only) and the texture filter is each
 * player's Display setting, so neither may reach those pixels: V8:2 reads its
 * own frame back pixel by pixel. sw_set_faithful_authority(1) must make an
 * enhanced draw byte-identical to the plain one, and turning it off must
 * restore the player's filter. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/gpu_sw_renderer.c"

int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { return 0; }
int g_psx_vram_dirty_tracking;
void gpu_vram_dirty_mark_row_impl(uint32_t y) { (void)y; }
void gpu_vram_dirty_mark_rect(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void gpu_vram_dirty_mark_all(void) {}

static uint16_t vram[1024 * 512];
static uint16_t plain[256 * 200], out[256 * 200];

/* 15-bit direct texture page at x=512: every texel opaque and distinct. */
#define TEXPAGE ((uint16_t)((2u << 7) | 8u))

static void reset_vram(void) {
    memset(vram, 0, sizeof vram);
    for (int v = 0; v < 256; v++)
        for (int u = 0; u < 256; u++)
            vram[v * 1024 + 512 + u] =
                (uint16_t)(((u * 7) & 31) | (((v * 5) & 31) << 5) |
                           ((((u ^ v) * 3) & 31) << 10) | 0x8000u);
}

static void draw(int perspective, int precise, int filter, uint16_t *dst) {
    reset_vram();
    sw_set_texture_filter(filter);
    sw_set_color_modulation(16, 16, 16, 1);
    if (precise)
        sw_set_precise_triangle(1, 10 << 16 | 0x8000, 260 << 16 | 0x4000,
                                230 << 16 | 0x8000, 280 << 16,
                                40 << 16, 455 << 16 | 0xC000);
    if (perspective) sw_set_perspective_triangle(1, 1.0f, 0.2f, 0.45f);
    sw_draw_textured_triangle(10, 260, 0, 0,
                              230, 280, 250, 10,
                              40, 455, 20, 240,
                              0, 0, TEXPAGE);
    for (int y = 0; y < 200; y++)
        memcpy(dst + y * 256, vram + (260 + y) * 1024, 256 * sizeof(uint16_t));
}

/* GPU-O2: the y loop starts and ends at the draw area. A big triangle mostly
 * outside it must draw, inside, exactly what the full draw does there. Edges by
 * hand: the long edge (0,0)->(0,400) stays at x=0; the short one (400,0)->(0,400)
 * steps exactly -1 column per line, so row y covers x in [0, 400-y). */
enum { CL_X1 = 100, CL_Y1 = 200, CL_X2 = 300, CL_Y2 = 210 };
static uint16_t clip_ref[CL_Y2 - CL_Y1 + 1][512];

static void big_tri(int kind) {
    switch (kind) {
    case 0: sw_draw_flat_triangle(0, 0, 400, 0, 0, 400, 0x1234); break;
    case 1: sw_draw_gouraud_triangle(0, 0, 0x001F, 400, 0, 0x03E0, 0, 400, 0x7C00); break;
    case 2: sw_draw_textured_triangle(0, 0, 0, 0, 400, 0, 255, 0, 0, 400, 0, 255,
                                      0, 0, TEXPAGE); break;
    default: sw_draw_shaded_textured_triangle(0, 0, 0, 0, 0x0000FF,
                                              400, 0, 255, 0, 0x00FF00,
                                              0, 400, 0, 255, 0xFF0000,
                                              0, 0, TEXPAGE, 0); break;
    }
}

static void check_clipped_big_triangles(void) {
    sw_set_texture_filter(0);
    for (int kind = 0; kind < 4; kind++) {
        reset_vram();
        sw_set_draw_area(0, 0, 1023, 511);
        big_tri(kind);
        for (int y = CL_Y1; y <= CL_Y2; y++)
            memcpy(clip_ref[y - CL_Y1], vram + y * 1024, sizeof clip_ref[0]);

        reset_vram();
        sw_set_draw_area(CL_X1, CL_Y1, CL_X2, CL_Y2);
        big_tri(kind);
        for (int y = 0; y < 512; y++)
            for (int x = 0; x < 512; x++) {
                int in = y >= CL_Y1 && y <= CL_Y2 && x >= CL_X1 && x <= CL_X2;
                assert(vram[y * 1024 + x] == (in ? clip_ref[y - CL_Y1][x] : 0));
            }
        if (kind == 0)
            for (int y = CL_Y1; y <= CL_Y2; y++) {
                assert(vram[y * 1024 + CL_X1 - 1] == 0);
                assert(vram[y * 1024 + CL_X1] == 0x1234);
                assert(vram[y * 1024 + 399 - y] == 0x1234);
                assert(vram[y * 1024 + 400 - y] == 0);
            }
    }
    sw_set_draw_area(0, 0, 1023, 511);
}

int main(void) {
    sw_renderer_init(vram);
    assert(!sw_faithful_authority());

    draw(0, 0, 0, plain);
    int lit = 0;
    for (int i = 0; i < 256 * 200; i++) lit += plain[i] != 0;
    assert(lit > 10000);

    /* Offline: each enhancement changes the native pixels. */
    draw(1, 0, 0, out);
    assert(memcmp(out, plain, sizeof plain) != 0);
    draw(0, 0, 1, out);
    assert(memcmp(out, plain, sizeof plain) != 0);

    /* Faithful authority: the same requests draw the hardware image. */
    sw_set_texture_filter(1);
    sw_set_faithful_authority(1);
    assert(sw_faithful_authority() && sw_texture_filter() == 0);
    draw(1, 1, 1, out);
    assert(memcmp(out, plain, sizeof plain) == 0);
    /* A filter change while armed is remembered, not applied. */
    sw_set_texture_filter(1);
    assert(sw_texture_filter() == 0);

    /* Off again: the player's filter is back and enhancements apply. */
    sw_set_faithful_authority(0);
    assert(sw_texture_filter() == 1);
    draw(1, 0, 1, out);
    assert(memcmp(out, plain, sizeof plain) != 0);

    check_clipped_big_triangles();

    printf("sw_faithful_authority_test: all checks passed\n");
    return 0;
}
