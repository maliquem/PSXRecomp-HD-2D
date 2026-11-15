/* CD audio with SPUCNT.15 clear, using the existing SPU fidelity fixture. */
#define main spu_fidelity_original_main
#include "test_spu_fidelity.c"
#undef main

/* psx-spx: SPU enable does not gate CD audio playback. The CD bus must keep
 * draining (no backlog replayed on re-enable) and stay audible. */
static void test_cd_plays_with_spu_disabled(void) {
    spu_init();
    spu_cd_audio_reset();
    wr(R_MAINVOLL, 0x3FFF);
    wr(R_MAINVOLR, 0x3FFF);
    wr(R_CDVOLL, 0x7FFF);
    wr(R_CDVOLR, 0x7FFF);
    wr(R_SPUCNT, 0x0001); /* CD enable, SPU off */

    push_cd_const(256, 0x2000, 0x2000);
    memset(g_render_buf, 0, sizeof(g_render_buf));
    spu_render_block(g_render_buf, 256);
    CHECK(cd_frame_count == 0);
    CHECK(buf_has_nonzero(g_render_buf, 256 * 2));
    CHECK(cd_underflow_frames == 0);

    /* CD enable off as well: still drained, output silent. */
    wr(R_SPUCNT, 0x0000);
    push_cd_const(128, 0x2000, 0x2000);
    memset(g_render_buf, 0, sizeof(g_render_buf));
    spu_render_block(g_render_buf, 128);
    CHECK(cd_frame_count == 0);
    CHECK(!buf_has_nonzero(g_render_buf, 128 * 2));

    /* No capture writes while disabled (still gated, see spu_render_block). */
    CHECK(ram_region_zero(0x0000u, 0x0800u));
}

int main(void) {
    test_cd_plays_with_spu_disabled();
    printf("spu_cd_disabled: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
