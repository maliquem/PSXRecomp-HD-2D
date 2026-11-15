/* Synthetic-sector integration tests for the actual CD controller. */
#include "../src/cdrom.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint64_t s_frame_count, psx_cycle_count;
static int test_dma_active;
static int test_iso_closes;
static int test_xa_audio; /* serve real-time XA audio sectors (mono, 37.8 kHz, 4-bit) */
static int pushed_frames, pushed_nonzero;
int dma_cdrom_transfer_active(void) { return test_dma_active; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= bit; }
int iso_read_raw_sector(void* h, uint32_t lba, uint8_t* out, int size) {
    (void)h; memset(out, 0, size); out[15]=2; out[16]=out[20]=9; out[17]=out[21]=7; out[18]=out[22]=8;
    if (test_xa_audio) { out[18]=out[22]=0x64; memset(out+24, 0x11, 2324); return 1; }
    memset(out+24, (unsigned char)lba, 2048); return 1;
}
int iso_read_sector(void* h,uint32_t lba,uint8_t* out,int size) {(void)h;memset(out,(unsigned char)lba,size);return 1;}

static void setup(void) {
    test_dma_active = 0;
    psx_cycle_count = 1000000;
    cdrom_init("synthetic");
    mode_reg = 0x80;
    reading = 1;
    read_min = 0; read_sec = 3; read_sect = 25; /* LBA 100 */
}
static void sector(void) {
    psx_cycle_count += 225792;
    read_delay = 0;
    process_read_stream(0);
}
static void ack(void) {
    cdrom_write(0x1f801800, 1);
    cdrom_write(0x1f801803, 7);
    cdrom_write(0x1f801800, 0);
}
static void request(void) {
    cdrom_write(0x1f801800, 0);
    cdrom_write(0x1f801803, 0);
    cdrom_write(0x1f801803, 0x80);
}
static void drain(unsigned lba, int words) {
    for (int n = 0; n < words; n++) {
        CHECK(cdrom_dma_ready());
        CHECK(cdrom_dma_read() == lba * 0x01010101u);
    }
}
static void delayed_notification(void) {
    setup(); sector(); sector(); /* 100 announced, 101 pending */
    CHECK(pending_dataready && last_sector_lba == 101);
    ack(); request();
    psx_cycle_count += CDROM_PEND_PRESENT_DELAY;
    cdrom_advance(0); /* 101 becomes visible between BFRD and DMA */
    CHECK(irq_flag == CDIRQ_DATA_READY && !pending_dataready);
    drain(100, 512);
    CHECK(!cdrom_dma_ready());
    ack(); request();
    drain(101, 512);
    CHECK(s_int1_lost == 0);
}
static void dma_continuation(void) {
    setup(); sector(); request(); drain(100, 512);
    test_dma_active = 1;
    sector(); drain(101, 512);
    sector(); drain(102, 512);
    CHECK(!pending_dataready);
    CHECK(s_int1_lost == 0);
    test_dma_active = 0;
}
static void dma_arrival_mid_drain(void) {
    setup(); sector(); request();
    test_dma_active = 1;
    drain(100, 17);
    sector(); /* Decoder advances while the old FIFO still has unread bytes. */
    drain(100, 495);
    drain(101, 512);
    CHECK(!cdrom_dma_ready());
    CHECK(!pending_dataready && s_int1_lost == 0);
    test_dma_active = 0;
}
static void request_rearms_retained_sector(void) {
    setup(); sector(); request(); drain(100, 512); ack();
    sector(); /* BFRD remains asserted and preloads 101. */
    request(); /* BIOS clears BFRD before requesting that same sector. */
    drain(101, 512);
    CHECK(!cdrom_dma_ready());
}
static void snapshot_pending_deadline(void) {
    setup(); sector(); request(); sector(); ack();
    psx_cycle_count += 123;
    const uint32_t size = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(size);
    CHECK(wire);
    cdrom_snapshot_write(wire);
    setup();
    psx_cycle_count = 0; /* Restore at a different cycle origin. */
    CHECK(cdrom_snapshot_read(wire, size));
    CHECK(pending_present_due == CDROM_PEND_PRESENT_DELAY - 123);
    psx_cycle_count = pending_present_due - 1;
    cdrom_advance(0);
    CHECK(irq_flag == 0 && pending_dataready);
    psx_cycle_count++;
    cdrom_advance(0);
    CHECK(irq_flag == CDIRQ_DATA_READY && !pending_dataready);
    drain(100, 512);
    request(); drain(101, 512);
    free(wire);
}
/* Saved parameter/response FIFO cursors out of range are refused in the
 * validate pass (current and legacy layouts), leaving the controller as it was. */
static void snapshot_rejects_fifo_cursors(void) {
    static const int32_t bad[][3] = { {-1, 0, 0}, {17, 0, 0}, {0, -1, 0}, {0, 3, 2}, {0, 0, 17} };
    setup(); sector();
    const uint32_t size = cdrom_snapshot_bytes(), legacy = cdrom_snapshot_legacy_bytes();
    uint8_t *good = malloc(size), *ref = malloc(size), *wire = malloc(size), *again = malloc(size);
    CHECK(good && ref && wire && again);
    cdrom_snapshot_write(good);
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        CHECK(cdrom_snapshot_read(good, size));
        cdrom_snapshot_write(ref);
        param_count = bad[k][0]; response_read = bad[k][1]; response_count = bad[k][2];
        cdrom_snapshot_write(wire);
        CHECK(cdrom_snapshot_read(good, size));
        CHECK(!cdrom_snapshot_validate(wire, size));
        CHECK(!cdrom_snapshot_validate(wire, legacy));
        CHECK(!cdrom_snapshot_read(wire, size));
        cdrom_snapshot_write(again);
        CHECK(!memcmp(again, ref, size));
    }
    free(good); free(ref); free(wire); free(again);
}
static void snapshot_between_request_and_dma(void) {
    setup(); sector(); request(); drain(100, 11); sector();
    const uint32_t size = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(size), *after = malloc(size);
    CHECK(wire && after);
    cdrom_snapshot_write(wire);
    CHECK(cdrom_snapshot_validate(wire, size));
    setup();
    CHECK(cdrom_snapshot_read(wire, size));
    ack();
    psx_cycle_count += CDROM_PEND_PRESENT_DELAY;
    cdrom_advance(0);
    drain(100, 501);
    ack(); request(); drain(101, 512);

    /* Malformed ring indices must be rejected before controller mutation. */
    cdrom_snapshot_write(after);
    const uint32_t legacy = cdrom_snapshot_legacy_bytes();
    wire[legacy + 4] = 0xff;
    CHECK(!cdrom_snapshot_validate(wire, size));
    CHECK(!cdrom_snapshot_read(wire, size));
    cdrom_snapshot_write(wire);
    CHECK(!memcmp(wire, after, size));

    CHECK(!cdrom_snapshot_validate(NULL, size));
    CHECK(!cdrom_snapshot_validate(after, size - 1));
    memcpy(wire, after, size);
    memset(wire + legacy + 16, 0x7f, 4); /* Impossible pending delay. */
    CHECK(!cdrom_snapshot_read(wire, size));
    cdrom_snapshot_write(wire);
    CHECK(!memcmp(wire, after, size));

    /* Old states retain their original wire prefix and readable payload. */
    setup(); sector(); request(); drain(100, 7);
    PstW writer;
    pst_w_init(&writer, wire, size);
    CHECK(cdrom_snap_emit(&writer, 0));
    CHECK(writer.written == legacy);
    setup();
    CHECK(cdrom_snapshot_validate(wire, legacy));
    CHECK(cdrom_snapshot_read(wire, legacy));
    drain(100, 505);
    free(wire); free(after);
}
static void invalid_setloc_preserves_target(void) {
    const uint8_t invalid[][3] = {
        {0xC0,0,0}, {0x0A,0,0}, {0,0x60,0}, {0,0x0A,0},
        {0,0,0x75}, {0,0,0x0A}, {0xFF,0xFF,0xFF}
    };
    for (unsigned n=0;n<sizeof invalid/sizeof invalid[0];++n) {
        setup();
        param_count=3;
        param_fifo[0]=0; param_fifo[1]=3; param_fifo[2]=0x25;
        exec_command(0x02);
        CHECK(irq_flag==CDIRQ_ACK && s_setloc_lba==100 && setloc_pending);
        ack();
        const int pending=setloc_pending, seek_far=setloc_seek_far;
        memcpy(param_fifo,invalid[n],3); param_count=3;
        exec_command(0x02);
        CHECK(irq_flag==CDIRQ_ERROR && response_count==2 && response_fifo[1]==0x10);
        CHECK((response_fifo[0]&1u)!=0);
        CHECK(s_setloc_lba==100 && seek_min==0 && seek_sec==3 && seek_sect==25);
        CHECK(setloc_pending==pending && setloc_seek_far==seek_far);
    }
}
static void pause_read_resumes_position(void) {
    const uint8_t reads[] = {0x06, 0x1B};
    for (unsigned n = 0; n < sizeof reads; ++n) {
        setup();
        seek_min = 0; seek_sec = 2; seek_sect = 0; /* stale target, LBA 0 */
        sector(); ack(); /* delivered 100; drive will read 101 next */
        exec_command(0x09); ack();
        CHECK(!reading && msf_to_lba(read_min, read_sec, read_sect) == 101);
        exec_command(reads[n]);
        CHECK(reading && read_cmd == reads[n] && !setloc_pending);
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == 101);
        ack(); sector(); request(); drain(101, 512);
        ack();
        /* Another pause cannot regress to the original target either. */
        exec_command(0x09); ack(); exec_command(reads[n]);
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == 102);
        ack();
        /* An explicit new Setloc still takes precedence over the drive. */
        exec_command(0x09); ack();
        param_fifo[0] = 0; param_fifo[1] = 4; param_fifo[2] = 0; param_count = 3;
        exec_command(0x02); ack(); exec_command(reads[n]);
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == 150);
        CHECK(!setloc_pending);
        ack(); sector(); request(); drain(150, 512);
        /* A completed explicit seek retains its own destination too. */
        ack(); exec_command(0x09); ack();
        param_fifo[0] = 0; param_fifo[1] = 5; param_fifo[2] = 0; param_count = 3;
        exec_command(0x02); ack(); exec_command(0x15); ack();
        psx_cycle_count += 100000000; process_pending(0); ack();
        exec_command(reads[n]);
        CHECK(msf_to_lba(read_min, read_sec, read_sect) == 225);
    }
}
static void snapshot_setloc_and_paused_head(void) {
    setup(); sector(); ack(); exec_command(0x09); ack();
    param_fifo[0]=0;param_fifo[1]=4;param_fifo[2]=0;param_count=3;
    exec_command(0x02);ack();
    const uint32_t size=cdrom_snapshot_bytes();
    uint8_t* wire=malloc(size);CHECK(wire);
    cdrom_snapshot_write(wire);
    setloc_pending=0;read_sec=9;
    CHECK(cdrom_snapshot_read(wire,size) && setloc_pending);
    exec_command(0x1B);ack();
    CHECK(msf_to_lba(read_min,read_sec,read_sect)==150);
    sector();ack();exec_command(0x09);ack();
    cdrom_snapshot_write(wire);
    CHECK(!setloc_pending);
    setloc_pending=1;read_sec=9;
    CHECK(cdrom_snapshot_read(wire,size) && !setloc_pending);
    exec_command(0x1B);ack();
    CHECK(msf_to_lba(read_min,read_sec,read_sect)==151);
    wire[cdrom_snapshot_level_bytes(3)-1u]=2;CHECK(!cdrom_snapshot_validate(wire,size));
    /* CDF2 states remain readable and retain the paused drive position. */
    exec_command(0x09);ack();
    PstW writer;pst_w_init(&writer,wire,size);
    CHECK(cdrom_snap_emit(&writer,1) && writer.written==cdrom_snapshot_level_bytes(1));
    read_sec=9;
    CHECK(cdrom_snapshot_read(wire,cdrom_snapshot_level_bytes(1)));
    exec_command(0x06);
    CHECK(msf_to_lba(read_min,read_sec,read_sect)==151);
    /* CDF3 states, the ones saved before the header flag, stay readable too. */
    exec_command(0x09);ack();
    pst_w_init(&writer,wire,size);
    CHECK(cdrom_snap_emit(&writer,2) && writer.written==cdrom_snapshot_level_bytes(2));
    CHECK(cdrom_snapshot_validate(wire,cdrom_snapshot_level_bytes(2)));
    read_sec=9;
    CHECK(cdrom_snapshot_read(wire,cdrom_snapshot_level_bytes(2)) && !last_sector_header_stale);
    exec_command(0x06);
    CHECK(msf_to_lba(read_min,read_sec,read_sect)==151);
    free(wire);
}
/* CDS-6: a disc change whose image will not open keeps the disc that is in
 * the drive: no lid cycle, the old handle stays open and selected. */
static void failed_disc_change_keeps_current_disc(void) {
    static const char *const roster[2] = { "disc1.cue", "missing.cue" };
    CdromLidState lid_before;
    uint8_t stat_before;
    void *handle_before;
    int closes_before;
    setup();
    cdrom_disc_roster_set(roster, 2, 1);
    handle_before = iso_handle;
    closes_before = test_iso_closes;
    lid_before = s_lid;
    stat_before = stat_reg;
    CHECK(cdrom_disc_select(2) == 0);
    CHECK(iso_handle == handle_before && iso_handle != NULL);
    CHECK(test_iso_closes == closes_before);
    CHECK(cdrom_disc_selected() == 1);
    CHECK(memcmp(&s_lid, &lid_before, sizeof lid_before) == 0);
    CHECK(stat_reg == stat_before && !s_lid_irq_pending);
}

/* EST-6: the CD->SPU volume matrix round-trips (CDF5); a CDF4 state, which
 * has no matrix, loads and keeps the live matrix exactly as before CDF5. */
static uint8_t *emit_level(int level, uint32_t *len) {
    PstW w;
    *len = cdrom_snapshot_level_bytes(level);
    uint8_t *p = malloc(*len);
    CHECK(p);
    pst_w_init(&w, p, *len);
    CHECK(cdrom_snap_emit(&w, level));
    return p;
}
static void snapshot_volume_matrix(void) {
    static const uint8_t pend[2][2] = { { 0x11, 0x22 }, { 0x33, 0x44 } };
    static const uint8_t dec[2][2]  = { { 0x7e, 0x01 }, { 0x02, 0xff } };
    setup();
    memcpy(cd_pending_vol, pend, 4); memcpy(cd_decode_vol, dec, 4);
    const uint32_t size = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(size);
    CHECK(wire);
    cdrom_snapshot_write(wire);
    uint32_t old_len;
    uint8_t *old = emit_level(3, &old_len);
    CHECK(old_len + 4u + 8u == cdrom_snapshot_level_bytes(4));
    memset(cd_pending_vol, 0x80, 4); memset(cd_decode_vol, 0x80, 4);
    CHECK(cdrom_snapshot_read(wire, size));
    CHECK(!memcmp(cd_pending_vol, pend, 4) && !memcmp(cd_decode_vol, dec, 4));
    memset(cd_pending_vol, 0x55, 4); memset(cd_decode_vol, 0x66, 4);
    CHECK(cdrom_snapshot_validate(old, old_len));
    CHECK(cdrom_snapshot_read(old, old_len));
    for (int i = 0; i < 4; i++)
        CHECK(((uint8_t *)cd_pending_vol)[i] == 0x55 && ((uint8_t *)cd_decode_vol)[i] == 0x66);
    wire[old_len] ^= 1; /* CDF5 marker */
    CHECK(!cdrom_snapshot_validate(wire, size));
    CHECK(!cdrom_snapshot_validate(wire, size - 1u));
    free(wire); free(old);
}

/* EST-8: the state records the roster disc and a load under another disc of
 * the same game is refused; older states and single-disc runs still load. */
static void snapshot_disc_identity(void) {
    static const char *const paths[2] = { "disc1.cue", "disc2.cue" };
    setup();
    cdrom_disc_roster_set(paths, 2, 1);
    const uint32_t size = cdrom_snapshot_bytes();
    CHECK(size == cdrom_snapshot_level_bytes(4) + 5u);
    uint8_t *wire = malloc(size);
    CHECK(wire);
    cdrom_snapshot_write(wire);
    CHECK(wire[size - 1u] == 1);
    CHECK(cdrom_snapshot_validate(wire, size) && cdrom_snapshot_read(wire, size));
    CHECK(cdrom_disc_selected() == 1); /* checked, never restored */
    s_disc_selected = 2; /* the player swapped discs after saving */
    CHECK(!cdrom_snapshot_disc_matches(wire, size));
    CHECK(!cdrom_snapshot_validate(wire, size) && !cdrom_snapshot_read(wire, size));
    uint32_t old_len;
    uint8_t *old = emit_level(4, &old_len); /* CDF5: no disc byte */
    CHECK(cdrom_snapshot_disc_matches(old, old_len) && cdrom_snapshot_read(old, old_len));
    wire[size - 1u] = 0; /* taken with no roster disc mounted */
    CHECK(cdrom_snapshot_validate(wire, size));
    wire[size - 1u] = CDROM_MAX_DISCS + 1; /* out of range */
    CHECK(!cdrom_snapshot_validate(wire, size));
    wire[size - 5u] ^= 1; /* CDF6 marker */
    wire[size - 1u] = 2;
    CHECK(!cdrom_snapshot_validate(wire, size));
    wire[size - 5u] ^= 1;
    CHECK(cdrom_snapshot_validate(wire, size));
    cdrom_disc_roster_set(paths, 1, 1); /* single disc: never refused */
    wire[size - 1u] = 1; s_disc_selected = 0;
    CHECK(cdrom_snapshot_validate(wire, size));
    cdrom_disc_roster_set(NULL, 0, 0);
    free(wire); free(old);
}

/* CDS-11: Mute only silences XA output. The real-time audio sector is still
 * decoded (ADPCM history advances as when unmuted) and never becomes data/INT1. */
static void xa_sector(int muted, int32_t hist[2]) {
    setup();
    memset(cd_decode_vol, 0, 4); cd_decode_vol[0][0] = cd_decode_vol[1][1] = 0x80;
    mode_reg = 0x80 | 0x40; /* double speed, XA-ADPCM on, no filter */
    exec_command(muted ? 0x0B : 0x0C); ack();
    test_xa_audio = 1; pushed_frames = pushed_nonzero = 0;
    sector();
    test_xa_audio = 0;
    CHECK(pushed_frames > 0);
    CHECK(irq_flag != CDIRQ_DATA_READY && !pending_dataready && !cdrom_dma_ready());
    hist[0] = xa_hist_l[0]; hist[1] = xa_hist_l[1];
}
static void xa_mute_keeps_decoding(void) {
    int32_t on[2], off[2];
    xa_sector(0, on);
    CHECK(pushed_nonzero > 0);
    xa_sector(1, off);
    CHECK(pushed_nonzero == 0);
    CHECK(on[0] == off[0] && on[1] == off[1]);
}

int main(void) {
    xa_mute_keeps_decoding();
    failed_disc_change_keeps_current_disc();
    pause_read_resumes_position();
    snapshot_setloc_and_paused_head();
    invalid_setloc_preserves_target();
    delayed_notification();
    dma_continuation();
    dma_arrival_mid_drain();
    request_rearms_retained_sector();
    snapshot_between_request_and_dma();
    snapshot_pending_deadline();
    snapshot_rejects_fifo_cursors();
    snapshot_volume_matrix();
    snapshot_disc_identity();
    puts("PASS: CD FIFO latching, pending IRQ, DMA continuation, and state compatibility");
    return 0;
}

void spu_cd_audio_reset(void) {}
void spu_cd_audio_push(const int16_t* stereo,int frames) {
    pushed_frames += frames;
    for (int i = 0; i < frames * 2; i++) pushed_nonzero += stereo[i] != 0;
}
/* A roster entry named "missing" stands for an image that will not open. */
void* iso_open(const char* p) {return (p && strstr(p,"missing")) ? NULL : (void*)1;}
void iso_close(void* h) {(void)h;++test_iso_closes;}
int iso_has_subq_replacements(void* h) {(void)h;return 0;}
int iso_read_subq(void* h,uint32_t lba,uint8_t* b,int n,int* v) {(void)h;(void)lba;(void)b;(void)n;*v=0;return 0;}
int iso_read_cdda_sector(void* h,uint32_t lba,uint8_t* b,int n) {(void)h;(void)lba;memset(b,0,n);return 0;}
uint32_t iso_cdda_track_start_lba(void* h,int track) {(void)h;(void)track;return 0;}
uint32_t iso_cdda_track_pregap_lba(void* h,int track) {(void)h;(void)track;return 0;}
int iso_cdda_track_count(void* h) {(void)h;return 1;}
uint32_t iso_cdda_sector_count(void* h) {(void)h;return 1000;}
int iso_cdda_track_is_audio(void* h,int track) {(void)h;(void)track;return 0;}
uint32_t iso_sector_count(void* h) {(void)h;return 1000;}
int iso_track_count(void* h) {(void)h;return 1;}
void event_ring_record_aux(uint16_t k,uint8_t d,uint32_t a) {(void)k;(void)d;(void)a;}
void event_ring_record(uint16_t k,uint8_t d) {(void)k;(void)d;}
void savestate_set_disc_scope(int n) {(void)n;}
int psx_netplay_cd_bisect_active(void) {return 0;}
uint32_t psx_netplay_sim_tick(void) {return 0;}
uint32_t interrupts_get_cycles_since_vblank(void) {return 0;}
int psx_netplay_is_resimulating(void) {return 0;}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b) {(void)k;(void)a;(void)b;}
