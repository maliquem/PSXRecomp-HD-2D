/* pgxp_session.cpp -- the per-session PGXP arming main.cpp runs
 * (docs/ENHANCEMENTS.md G1.11/G1.12). See pgxp_session.h for the sequence.
 * Kept out of main.cpp so runtime/tests/test_pgxp_session.cpp runs exactly
 * this code against the real mod runtime and builtin plugin. */
#include "pgxp_session.h"
#include "pgxp.h"
#include "pgxp_hooks.h"

#include <cstdlib>
#include <cstring>

extern "C" void gte_geometry_correction_set(int enabled);
extern "C" void gpu_texture_correction_set(int enabled);

extern "C" void psx_pgxp_session_reset(void) {
    pgxp_mod_request(0, 0, 0);
}

/* [video] pgxp_alu: the ALU hook classes that reach the engine (pgxp_hooks.h).
 * Every class by default: the generated code calls the engine exactly as
 * before the classes existed. */
extern "C" uint32_t g_pgxp_alu_mask = PGXP_ALU_MASK_FULL;
static uint32_t s_pgxp_alu_configured = PGXP_ALU_MASK_FULL;

/* [video] pgxp_mem: the load/store hooks left out (pgxp_hooks.h). None by
 * default; none while CPU mode is on. */
extern "C" uint32_t g_pgxp_mem_skip = 0;
static uint32_t s_pgxp_mem_configured = 0;

extern "C" void psx_pgxp_alu_cpu_mode(int cpu_mode) {
    g_pgxp_alu_mask = cpu_mode ? PGXP_ALU_MASK_FULL : s_pgxp_alu_configured;
    g_pgxp_mem_skip = cpu_mode ? 0u : s_pgxp_mem_configured;
}

/* The overlay DLLs' load/store hooks (overlay_loader.c hook table): the same
 * mask as the generated code's PGXP_LOAD / PGXP_STORE. */
extern "C" void psx_pgxp_load_masked(struct CPUState* cpu, uint32_t instr, uint32_t addr, uint32_t value) {
    if (PGXP_MEM_ON(instr)) psx_pgxp_load(cpu, instr, addr, value);
}
extern "C" void psx_pgxp_store_masked(struct CPUState* cpu, uint32_t instr, uint32_t addr, uint32_t value) {
    if (PGXP_MEM_ON(instr)) psx_pgxp_store(cpu, instr, addr, value);
}

/* The PGXP_ALU_K class pgxp_hook_emitter.cpp (alu3_kind, append_pgxp_hooks)
 * tags an ALU hook's instruction word with. */
static int pgxp_alu_class(uint32_t instr) {
    const uint32_t op = instr >> 26, rs = (instr >> 21) & 31u, rt = (instr >> 16) & 31u;
    if (op == 0x00) {
        const uint32_t f = instr & 0x3Fu;
        if (f <= 0x07) return PGXP_ALU_K_SHIFT;
        if (f >= 0x10 && f <= 0x13) return PGXP_ALU_K_HILO;
        if (f == 0x20 || f == 0x21) return (rs == 0 || rt == 0) ? PGXP_ALU_K_MOVE : PGXP_ALU_K_ADD;
        if (f == 0x22 || f == 0x23) return rt == 0 ? PGXP_ALU_K_MOVE : PGXP_ALU_K_ADD;
        if (f == 0x25) return (rs == 0 || rt == 0) ? PGXP_ALU_K_MOVE : PGXP_ALU_K_LOGIC;
        if (f == 0x2A || f == 0x2B) return PGXP_ALU_K_SLT;
        return PGXP_ALU_K_LOGIC;                 /* AND/XOR/NOR */
    }
    if (op == 0x08 || op == 0x09) return (instr & 0xFFFFu) ? PGXP_ALU_K_ADD : PGXP_ALU_K_MOVE;
    if (op == 0x0A || op == 0x0B) return PGXP_ALU_K_SLT;
    if (op == 0x0D) return PGXP_ALU_K_ORI;
    if (op == 0x0F) return PGXP_ALU_K_LUI;
    if (op == 0x10) return PGXP_ALU_K_COP0;
    return PGXP_ALU_K_LOGIC;                     /* ANDI/XORI */
}

/* The ALU hook of the interpreters and the overlay DLLs: the same class mask
 * as the generated code's PGXP_ALU_K, so a function moving between
 * interpreted, DLL and compiled code keeps the same shadows. */
extern "C" void psx_pgxp_alu_masked(struct CPUState* cpu, uint32_t instr, uint32_t result,
                                    uint32_t s1, uint32_t s2) {
    if (g_pgxp_alu_mask & (1u << pgxp_alu_class(instr))) psx_pgxp_alu(cpu, instr, result, s1, s2);
}

extern "C" void pgxp_set_mem_live(int enabled);   /* pgxp.cpp, stage C2.3 */

static uint32_t pgxp_mem_skip_from(const char* e, int mem_words) {
    if (e && *e) {
        if (std::strcmp(e, "all") == 0) return 0u;
        if (std::strcmp(e, "words") == 0) return PGXP_MEM_SKIP_WORDS;
        if (std::strcmp(e, "live") == 0) return PGXP_MEM_SKIP_WORDS;   /* + light mode */
        return (uint32_t)std::strtoul(e, nullptr, 0);
    }
    return mem_words ? PGXP_MEM_SKIP_WORDS : 0u;
}

static uint32_t pgxp_alu_mask_from(const char* e, int alu_light) {
    if (e && *e) {
        if (std::strcmp(e, "full") == 0) return PGXP_ALU_MASK_FULL;
        if (std::strcmp(e, "moves") == 0) return PGXP_ALU_MASK_MOVES;
        return (uint32_t)std::strtoul(e, nullptr, 0);
    }
    return alu_light ? PGXP_ALU_MASK_MOVES : PGXP_ALU_MASK_FULL;
}

extern "C" PSXPgxpSessionArm psx_pgxp_session_arm(
        const PSXPgxpSessionConfig* config, PSXPgxpSessionInputs* used) {
    PSXPgxpSessionInputs in{};
    in.env_geometry =
        psx_pgxp_session_env_flag(std::getenv("PSX_GEOMETRY_CORRECTION"));
    in.env_texture =
        psx_pgxp_session_env_flag(std::getenv("PSX_PERSPECTIVE_TEXTURING"));
    in.env_cpu_mode =
        psx_pgxp_session_env_flag(std::getenv("PSX_PGXP_CPU_MODE"));
    in.env_culling =
        psx_pgxp_session_env_flag(std::getenv("PSX_PGXP_CULLING"));
    in.video_geometry = config->video_geometry ? 1 : 0;
    in.video_texture = config->video_texture ? 1 : 0;
    in.video_cpu_mode = config->video_cpu_mode ? 1 : 0;
    in.mod_only = config->mod_only ? 1 : 0;
    in.netplay = config->netplay ? 1 : 0;
    /* Take, not peek: the request belongs to this session alone. */
    in.mod_enabled = pgxp_mod_request_take(&in.mod_cpu_mode, &in.mod_culling);

    const PSXPgxpSessionArm arm = psx_pgxp_session_resolve(&in);
    /* The hook flavor tracks the full-word dataflow. A coordinate cache can
     * inject unrelated world fractions into HUD vertices, and a tolerance
     * clamp discards valid shadows, mixing precise/native triangle corners.
     * Base builds lack that coverage and retain their validated defaults.
     * Neither default enables the optional PGXP enhancement itself. */
#if defined(PSX_PGXP) && PSX_PGXP
    const float default_tolerance = -1.0f;
    const int default_position_fallback = 0;
#else
    const float default_tolerance = 0.5f;
    const int default_position_fallback = 1;
#endif
    pgxp_set_tolerance(config->tolerance_set ? config->tolerance : default_tolerance);
    pgxp_set_position_fallback(config->position_fallback_set ?
                                config->position_fallback : default_position_fallback);
    pgxp_set_preserve_projection(config->preserve_projection);
    pgxp_set_culling(arm.culling);
    gte_geometry_correction_set(arm.geometry);
    gpu_texture_correction_set(arm.texture);
    pgxp_set_cpu_mode(arm.cpu_mode);
    s_pgxp_alu_configured = pgxp_alu_mask_from(std::getenv("PSX_PGXP_ALU"), config->alu_light);
    s_pgxp_mem_configured = pgxp_mem_skip_from(std::getenv("PSX_PGXP_MEM"), config->mem_words);
    {
        const char* m = std::getenv("PSX_PGXP_MEM");
        pgxp_set_mem_live((m && *m) ? std::strcmp(m, "live") == 0 : config->mem_live);
    }
    psx_pgxp_alu_cpu_mode(arm.cpu_mode);
    if (used) *used = in;
    return arm;
}
