/* No shadows, lights, OT tracking, overdraw, MDEC, overlays, telemetry, perf
 * meter or presentation pacer in these fixtures: the renderer's hooks for them are inert. */
int area_lights_get(uint32_t a,AreaLightSet *o){(void)a;(void)o;return 0;}
const unsigned char *area_lights_masks(int *side,int *count){*side=0;*count=0;return 0;}
int gpu_prim_nolight_now(void){return 0;}
int gpu_shadow_area_off(void){return 0;}
int gpu_ot_front_now(void){return 0;}
uint32_t gpu_shadow_current_area(void){return 0;}
int gpu_shadow_moon_area(float *az,float *el){(void)az;(void)el;return 0;}
int gpu_shadow_off_active(void){return 0;}
int gpu_shadow_sun_area(float *az,float *el){(void)az;(void)el;return 0;}
int psx_overdraw_x(void){return 0;}
int psx_overdraw_y(void){return 0;}
uint32_t mdec_mean_qscale_x16(void){return 0;}
int mdec_recently_active(uint32_t f){(void)f;return 0;}
int bug_report_ui_draw(int w,int h){(void)w;(void)h;return 0;}
int psx_savestate_menu_overlay_blend(void){return 0;}
int g_perf_meter_on=0;
void perf_meter_note_present(void){}
void perf_meter_pop_(int s){(void)s;}
void perf_meter_push_(int s){(void)s;}
void host_sampler_register_render_thread(void){}
void psx_video_telemetry_compose(int r,int s,int sw,int sh,int dw,int dh,int vw,int vh){(void)r;(void)s;(void)sw;(void)sh;(void)dw;(void)dh;(void)vw;(void)vh;}
void psx_video_telemetry_present(int r){(void)r;}
int present_phase_active(void){return 0;}
int present_phase_hold(uint64_t r,void *w){(void)r;(void)w;return 0;}
void present_phase_note_done(uint64_t s,uint64_t d,void *w){(void)s;(void)d;(void)w;}
uint64_t psx_present_ref_ticks(void){return 0;}
int psx_presentation_fast_forward(void){return 0;}
uint64_t psx_host_thread_cpu_ns(void){return 0;}
