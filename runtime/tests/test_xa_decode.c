/* CDS-23: XA-ADPCM 4-bit decode (mono and stereo) against the format definition (psx-spx
 * "CDROM XA Audio ADPCM"). Hand-assembled sectors; expected values are hand-derived constants
 * plus a small independent reference decoder. Mismatches are reported as KNOWN and never fail.
 * The decoders are static in cdrom.c, so this reuses the include/stub setup of
 * test_cdrom_sector_fifo.c (same link set as cdrom_sector_fifo_test). */
#define main fifo_baseline_main
#include "test_cdrom_sector_fifo.c"
#undef main

static int known_count, checked;
static void expect(const char *name, int got, int want) {
    checked++;
    if (got != want) { known_count++; printf("KNOWN %s: got %d want %d\n", name, got, want); }
}

#define SECTOR_BYTES (XA_SOUND_GROUPS * 128)
static uint8_t sec[SECTOR_BYTES];
static int16_t outb[XA_NATIVE_FRAMES * 2];

/* Unit u (0..7) of group g: header SP(u) lives at 0..3/4..7 (u<4) or 8..11/12..15 (u>=4), duplicated. */
static void set_hdr(int g, int u, uint8_t h) {
    uint8_t *p = sec + g * 128 + (u < 4 ? 0 : 8) + (u & 3);
    p[0] = h; p[4] = h;
}
/* Word i (0..27) of unit u: byte j holds unit 2j in the low nibble and 2j+1 in the high one. */
static void set_nib(int g, int u, int i, int v) {
    uint8_t *p = sec + g * 128 + 16 + (u >> 1) + i * 4;
    *p = (u & 1) ? (uint8_t)((*p & 0x0F) | ((v & 15) << 4)) : (uint8_t)((*p & 0xF0) | (v & 15));
}
static void clear(void) { memset(sec, 0, sizeof sec); xa_reset_decode(); }
/* Mono frame index of word i of unit u in group g. */
static int mono_at(int g, int u, int i) { return g * 224 + u * 28 + i; }

/* Independent reference: floor(v/64) without relying on shifts of negatives. */
static int fdiv64(int v) { return v >= 0 ? v / 64 : -((-v + 63) / 64); }
static int sat(int v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
static void ref_unit(uint8_t hdr, const int *nib, int *p1, int *p2, int *o) {
    static const int f0[4] = { 0, 60, 115, 98 }, f1[4] = { 0, 0, -52, -55 };
    int range = hdr & 15, f = (hdr >> 4) & 3;
    for (int i = 0; i < 28; i++) {
        int n = nib[i] >= 8 ? nib[i] - 16 : nib[i];
        int s = sat(n * (1 << (12 - range)) + fdiv64(f0[f] * *p1 + f1[f] * *p2 + 32));
        *p2 = *p1; *p1 = s; o[i] = s;
    }
}

static void zero_block(void) {
    clear();
    expect("zero.mono.frames", xa_decode_sector_4bit_mono(sec, outb), XA_NATIVE_FRAMES);
    int nz = 0; for (int i = 0; i < XA_NATIVE_FRAMES * 2; i++) nz |= outb[i];
    expect("zero.mono.all_zero", nz, 0);
    clear();
    expect("zero.stereo.frames", xa_decode_sector_4bit_stereo(sec, outb), XA_NATIVE_FRAMES / 2);
    nz = 0; for (int i = 0; i < XA_NATIVE_FRAMES * 2; i++) nz |= outb[i];
    expect("zero.stereo.all_zero", nz, 0);
}

static void filter0_shift(void) {
    clear();
    set_hdr(0, 0, 0x0C);                 /* range 12 -> shift 0, filter 0 */
    set_nib(0, 0, 0, 5); set_nib(0, 0, 1, 0xD);
    set_hdr(0, 1, 0x00);                 /* range 0 -> shift 12 */
    set_nib(0, 1, 0, 1); set_nib(0, 1, 1, 0xF); set_nib(0, 1, 2, 7); set_nib(0, 1, 3, 8);
    xa_decode_sector_4bit_mono(sec, outb);
    expect("f0.shift0.pos", outb[mono_at(0, 0, 0) * 2], 5);
    expect("f0.shift0.neg", outb[mono_at(0, 0, 1) * 2], -3);
    expect("f0.shift0.rest", outb[mono_at(0, 0, 2) * 2], 0);
    expect("f0.shift12.1", outb[mono_at(0, 1, 0) * 2], 4096);
    expect("f0.shift12.-1", outb[mono_at(0, 1, 1) * 2], -4096);
    expect("f0.shift12.7", outb[mono_at(0, 1, 2) * 2], 28672);
    expect("f0.shift12.-8", outb[mono_at(0, 1, 3) * 2], -32768);
    int dup = 0; for (int i = 0; i < XA_NATIVE_FRAMES; i++) dup |= outb[i * 2] != outb[i * 2 + 1];
    expect("mono.duplicated_to_both_channels", dup, 0);
}

static void filter_history(void) {
    /* Filter 1 (K0=60, K1=0), shift 12: hand-derived chain 4096, 3840, 3600, 3375. */
    clear();
    set_hdr(0, 0, 0x10); set_nib(0, 0, 0, 1);
    xa_decode_sector_4bit_mono(sec, outb);
    expect("f1.chain.0", outb[mono_at(0, 0, 0) * 2], 4096);
    expect("f1.chain.1", outb[mono_at(0, 0, 1) * 2], 3840);
    expect("f1.chain.2", outb[mono_at(0, 0, 2) * 2], 3600);
    expect("f1.chain.3", outb[mono_at(0, 0, 3) * 2], 3375);

    /* History carried between units: unit 0 ends ...,1,2; unit 1 uses filter 2 (115,-52):
     * (230-52+32)>>6=3, (345-104+32)>>6=4, (460-156+32)>>6=5. */
    clear();
    set_hdr(0, 0, 0x0C); set_nib(0, 0, 26, 1); set_nib(0, 0, 27, 2);
    set_hdr(0, 1, 0x2C);
    xa_decode_sector_4bit_mono(sec, outb);
    expect("f2.carry.0", outb[mono_at(0, 1, 0) * 2], 3);
    expect("f2.carry.1", outb[mono_at(0, 1, 1) * 2], 4);
    expect("f2.carry.2", outb[mono_at(0, 1, 2) * 2], 5);

    /* Same history, filter 3 (98,-55): (196-55+32)>>6=2, (196-110+32)>>6=1, (98-110+32)>>6=0. */
    clear();
    set_hdr(0, 0, 0x0C); set_nib(0, 0, 26, 1); set_nib(0, 0, 27, 2);
    set_hdr(0, 1, 0x3C);
    xa_decode_sector_4bit_mono(sec, outb);
    expect("f3.carry.0", outb[mono_at(0, 1, 0) * 2], 2);
    expect("f3.carry.1", outb[mono_at(0, 1, 1) * 2], 1);
    expect("f3.carry.2", outb[mono_at(0, 1, 2) * 2], 0);

    /* Carry across sound groups: group 0 unit 7 ends with 2, group 1 unit 0 filter 1: (120+32)>>6=2. */
    clear();
    set_hdr(0, 7, 0x0C); set_nib(0, 7, 27, 2);
    set_hdr(1, 0, 0x1C);
    xa_decode_sector_4bit_mono(sec, outb);
    expect("carry.group", outb[mono_at(1, 0, 0) * 2], 2);

    /* Carry across sectors: history stays in xa_hist_* until the stream is reset. */
    clear();
    set_hdr(XA_SOUND_GROUPS - 1, 7, 0x0C); set_nib(XA_SOUND_GROUPS - 1, 7, 27, 2);
    xa_decode_sector_4bit_mono(sec, outb);
    memset(sec, 0, sizeof sec); set_hdr(0, 0, 0x1C);
    xa_decode_sector_4bit_mono(sec, outb);
    expect("carry.sector", outb[0], 2);
}

static void saturation(void) {
    /* Filter 1, shift 12, nibble 7 twice: 28672, then 28672+26880 -> clamp 32767; history keeps the
     * clamped value, so the next zero-nibble sample is (60*32767+32)>>6 = 30719. */
    clear();
    set_hdr(0, 0, 0x10); set_nib(0, 0, 0, 7); set_nib(0, 0, 1, 7);
    xa_decode_sector_4bit_mono(sec, outb);
    expect("sat.pos.0", outb[mono_at(0, 0, 0) * 2], 28672);
    expect("sat.pos.1", outb[mono_at(0, 0, 1) * 2], 32767);
    expect("sat.pos.2", outb[mono_at(0, 0, 2) * 2], 30719);
    /* Negative: -32768, then -32768-30720 -> clamp -32768, then (60*-32768+32)>>6 = -30720. */
    clear();
    set_hdr(0, 0, 0x10); set_nib(0, 0, 0, 8); set_nib(0, 0, 1, 8);
    xa_decode_sector_4bit_mono(sec, outb);
    expect("sat.neg.0", outb[mono_at(0, 0, 0) * 2], -32768);
    expect("sat.neg.1", outb[mono_at(0, 0, 1) * 2], -32768);
    expect("sat.neg.2", outb[mono_at(0, 0, 2) * 2], -30720);
}

static void stereo_interleave(void) {
    /* Block b: left = unit 2b, right = unit 2b+1; pair index = b*28 + word. */
    clear();
    set_hdr(0, 0, 0x0C); set_hdr(0, 1, 0x00);   /* block 0: L shift 0, R shift 12 */
    set_nib(0, 0, 0, 3); set_nib(0, 1, 0, 1);
    set_nib(0, 0, 1, 0xE); set_nib(0, 1, 1, 0xF);
    set_hdr(0, 2, 0x0C); set_hdr(0, 3, 0x0C);   /* block 1 */
    set_nib(0, 2, 0, 9); set_nib(0, 3, 0, 0xA);
    set_hdr(0, 4, 0x1C); set_hdr(0, 5, 0x0C);   /* block 2: L filter 1, R filter 0 */
    set_nib(0, 4, 0, 7); set_nib(0, 5, 0, 7);
    expect("st.frames", xa_decode_sector_4bit_stereo(sec, outb), XA_NATIVE_FRAMES / 2);
    expect("st.b0w0.L", outb[0], 3);      expect("st.b0w0.R", outb[1], 4096);
    expect("st.b0w1.L", outb[2], -2);     expect("st.b0w1.R", outb[3], -4096);
    expect("st.b1w0.L", outb[28 * 2], -7); expect("st.b1w0.R", outb[28 * 2 + 1], -6);
    expect("st.b2w0.L", outb[56 * 2], 7); expect("st.b2w0.R", outb[56 * 2 + 1], 7);
    expect("st.b2w1.L", outb[57 * 2], 7); expect("st.b2w1.R", outb[57 * 2 + 1], 0);
}

/* Pseudo-random full sectors (range 0..12, filters 0..3, all nibbles) against the reference. */
static uint32_t lcg_state = 12345u;
static uint32_t lcg(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state >> 8; }
static void random_vs_reference(int stereo) {
    clear();
    int nib[XA_SOUND_GROUPS][8][28], hdr[XA_SOUND_GROUPS][8];
    for (int g = 0; g < XA_SOUND_GROUPS; g++)
        for (int u = 0; u < 8; u++) {
            hdr[g][u] = (int)((lcg() % 4) << 4 | (lcg() % 13));
            set_hdr(g, u, (uint8_t)hdr[g][u]);
            for (int i = 0; i < 28; i++) { nib[g][u][i] = (int)(lcg() & 15); set_nib(g, u, i, nib[g][u][i]); }
        }
    int got = stereo ? xa_decode_sector_4bit_stereo(sec, outb) : xa_decode_sector_4bit_mono(sec, outb);
    expect(stereo ? "rand.stereo.frames" : "rand.mono.frames", got, stereo ? XA_NATIVE_FRAMES / 2 : XA_NATIVE_FRAMES);
    int ph[2][2] = { { 0, 0 }, { 0, 0 } }, bad = 0, o[28];
    for (int g = 0; g < XA_SOUND_GROUPS; g++)
        for (int u = 0; u < 8; u++) {
            int ch = stereo ? (u & 1) : 0;
            ref_unit((uint8_t)hdr[g][u], nib[g][u], &ph[ch][0], &ph[ch][1], o);
            for (int i = 0; i < 28; i++) {
                int fr = stereo ? g * 112 + (u >> 1) * 28 + i : mono_at(g, u, i);
                if (outb[fr * 2 + ch] != o[i]) bad++;
                if (!stereo && outb[fr * 2 + 1] != o[i]) bad++;
            }
        }
    expect(stereo ? "rand.stereo.mismatches" : "rand.mono.mismatches", bad, 0);
}

int main(void) {
    zero_block();
    filter0_shift();
    filter_history();
    saturation();
    stereo_interleave();
    random_vs_reference(0);
    random_vs_reference(1);
    /* UNKNOWN (not asserted): range 13..15 (runtime clamps shift to 0) and header bits 6..7 (runtime masks
     * them off); sample rate / 8-bit depth are decided in maybe_deliver_xa_audio, not here. */
    printf("xa_decode_test: %d checks, KNOWN=%d\n", checked, known_count);
    return 0;
}
