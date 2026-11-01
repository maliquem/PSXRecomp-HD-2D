/* spu_clone.inc - spu.c compiled once more as a host-only SPU (audio_music.c).
 *
 * The including file defines SPUC(x) (the clone's name prefix) first. Its
 * state is spu.c's own statics, separate in this translation unit; every
 * exported name is renamed, and what spu.c tells the rest of the runtime is
 * cut: no guest IRQ, no audio taps, no golden hashes, no HQ shadow. Its guest
 * clock is SPUC(clock)(), defined by the including file. The guest never
 * reaches a clone; only the fast-forward's sound reads it. */
#define spu_init                  SPUC(init)
#define spu_catch_up              SPUC(catch_up)
#define spu_perf_observer         SPUC(perf_observer)
#define spu_output_available      SPUC(output_available)
#define spu_output_pop            SPUC(output_pop)
#define spu_output_drop           SPUC(output_drop)
#define spu_output_flush          SPUC(output_flush)
#define spu_output_overflow_drops SPUC(output_overflow_drops)
#define spu_set_output_gate       SPUC(set_output_gate)
#define spu_debug_info            SPUC(debug_info)
#define spu_get_voice_state       SPUC(get_voice_state)
#define spu_get_global_state      SPUC(get_global_state)
#define spu_ctrl_read             SPUC(ctrl_read)
#define spu_ram_peek              SPUC(ram_peek)
#define spu_keyon_ctx_hook        SPUC(keyon_ctx_hook)
#define spu_io_tap                SPUC(io_tap)
#define spu_event_total           SPUC(event_total)
#define spu_event_get             SPUC(event_get)
#define spu_event_reset           SPUC(event_reset)
#define spu_read                  SPUC(read)
#define spu_write                 SPUC(write)
#define spu_dma_write             SPUC(dma_write)
#define spu_dma_read              SPUC(dma_read)
#define spu_dma_ready             SPUC(dma_ready)
#define spu_cd_audio_push         SPUC(cd_audio_push)
#define spu_cd_audio_reset        SPUC(cd_audio_reset)
#define spu_cd_snapshot_bytes     SPUC(cd_snapshot_bytes)
#define spu_cd_snapshot_read      SPUC(cd_snapshot_read)
#define spu_cd_snapshot_write     SPUC(cd_snapshot_write)
#define spu_get_ram               SPUC(get_ram)
#define spu_register_sample_bank  SPUC(register_sample_bank)
#define spu_bind_next_voice_bank  SPUC(bind_next_voice_bank)
#define spu_private_voice_play    SPUC(private_voice_play)
#define spu_private_voice_stop    SPUC(private_voice_stop)
#define spu_private_voice_volume  SPUC(private_voice_volume)
#define spu_private_voice_active  SPUC(private_voice_active)
#define spu_shadow_tap_buffer     SPUC(shadow_tap_buffer)
#define spu_shadow_tap_count      SPUC(shadow_tap_count)
#define spu_snapshot_bytes        SPUC(snapshot_bytes)
#define spu_snapshot_len_ok       SPUC(snapshot_len_ok)
#define spu_snapshot_write        SPUC(snapshot_write)
#define spu_snapshot_read         SPUC(snapshot_read)
#define spu_snapshot_part_digests SPUC(snapshot_part_digests)
#define spu_get_ram_ptr           SPUC(get_ram_ptr)
#define spu_get_ram_bytes         SPUC(get_ram_bytes)
#define g_spu_blocks              SPUC(g_blocks)
#define g_spu_blocks_by_voice     SPUC(g_blocks_by_voice)
#define g_spu_frames_by_voice     SPUC(g_frames_by_voice)
#define g_spu_cd_restored         SPUC(g_cd_restored)
/* what spu.c tells the runtime: cut (defined below) */
#define psx_get_cycle_count       SPUC(clock)
#define psx_irq_raise             SPUC(irq_raise)
#define audio_trace_event         SPUC(trace_event)
#define audio_trace_pcm           SPUC(trace_pcm)
#define golden_spu_block          SPUC(golden_block)
#define g_golden_on               SPUC(golden_on)
#define spu_shadow_active         SPUC(shadow_active)
#define spu_shadow_process        SPUC(shadow_process)
#define spu_shadow_reset          SPUC(shadow_reset)

#include "spu.c"

void SPUC(irq_raise)(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void SPUC(trace_event)(uint16_t kind, uint32_t a, uint32_t b) { (void)kind; (void)a; (void)b; }
void SPUC(trace_pcm)(int tap, const int16_t *stereo, int frames) { (void)tap; (void)stereo; (void)frames; }
void SPUC(golden_block)(const int16_t *stereo, uint32_t frames) { (void)stereo; (void)frames; }
int  SPUC(golden_on) = 0;
bool SPUC(shadow_active)(void) { return false; }
void SPUC(shadow_reset)(void) {}
void SPUC(shadow_process)(int16_t *canon, int frames) { (void)canon; (void)frames; }
