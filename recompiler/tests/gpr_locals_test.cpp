// [recompiler] gpr_locals (docs/GPR_LOCALS.md): the pass over hand-written
// bodies in the emitters' shapes.
#include "gpr_locals.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool has(const std::string& s, const std::string& k) { return s.find(k) != std::string::npos; }

int main() {
    // A call the pass does not know keeps the body exactly.
    {
        GprLocalsStats st;
        const std::string b =
            "{\n"
            "block_80010000:\n"
            "    cpu->gpr[2] = mystery_helper(cpu);\n"
            "    cpu->pc = cpu->gpr[31]; return;\n"
            "    ;  /* label compatibility */\n"
            "}\n";
        CHECK(gpr_localize_body(b, 8, 0x80010000u, st) == b);
        CHECK(st.kept["call:mystery_helper"] == 1);
    }
    // Taking the register file's address keeps the body.
    {
        GprLocalsStats st;
        const std::string b = "{\n    probe(&cpu->gpr[0]);\n}\n";
        CHECK(gpr_localize_body(b, 8, 0, st) == b);
    }
    // Writes reach the store before the return; the continuation block after a
    // CPS call starts with nothing to store; the entry loads every register.
    {
        GprLocalsStats st;
        const std::string b =
            "{\n"
            "    if (cpu->pc != 0u) {\n"
            "        uint32_t _cont = cpu->pc; cpu->pc = 0;\n"
            "        switch (_cont) {\n"
            "            case 0x80010010u: goto block_80010010;\n"
            "            default: break;\n"
            "        }\n"
            "    }\n"
            "block_80010000:\n"
            "    cpu->gpr[4] = cpu->gpr[5] + 1;  /* addiu */\n"
            "    cpu->gpr[31] = 0x80010010u;  /* jal link */\n"
            "    psx_check_interrupts_at(cpu, 0x80020000u);\n"
            "    cpu->pc = 0x80020000u; return;  /* CPS jal */\n"
            "block_80010010:\n"
            "    psx_icache_fetch(cpu, 0x80010010u);\n"
            "    cpu->gpr[2] = 0;\n"
            "    cpu->pc = cpu->gpr[31]; return;  /* CPS: jr $ra */\n"
            "    ;  /* label compatibility */\n"
            "}\n";
        const std::string r = gpr_localize_body(b, 8, 0x80010000u, st);
        CHECK(st.localized == 1);
        CHECK(has(r, "PSX_B2_DECL(2) PSX_B2_DECL(4) PSX_B2_DECL(5) PSX_B2_DECL(31)"));
        CHECK(has(r, "PSX_R(4) = PSX_R(5) + 1;"));
        CHECK(has(r, "PSX_B2_EDGE(0x80020000u, PSX_B2_ST(4) PSX_B2_ST(31) , "
                     "PSX_B2_LD(4) PSX_B2_LD(31) );"));   // only what is used again
        CHECK(has(r, "cpu->pc = 0x80020000u; { PSX_B2_ST(4) PSX_B2_ST(31) return; }"));
        CHECK(has(r, "PSX_B2_ICACHE(0x80010010u, );"));        // entered clean
        CHECK(has(r, "cpu->pc = PSX_R(31); { PSX_B2_ST(2) return; }"));
        CHECK(!has(r, "cpu->gpr["));
    }
    // A loop: the back edge carries the loop's writes to the loop head.
    {
        GprLocalsStats st;
        const std::string b =
            "{\n"
            "block_80010000:\n"
            "    cpu->gpr[8] = 0;\n"
            "block_80010004:\n"
            "    psx_icache_fetch(cpu, 0x80010004u);\n"
            "    cpu->gpr[8] = cpu->gpr[8] + 1;\n"
            "    if (cpu->gpr[8] != cpu->gpr[9]) {\n"
            "        goto block_80010004;  /* taken */\n"
            "    }\n"
            "    goto block_80010010;  /* not taken */\n"
            "block_80010010:\n"
            "    cpu->pc = cpu->gpr[31]; return;\n"
            "    ;  /* label compatibility */\n"
            "}\n";
        const std::string r = gpr_localize_body(b, 8, 0x80010000u, st);
        CHECK(st.localized == 1);
        CHECK(has(r, "PSX_B2_ICACHE(0x80010004u, PSX_B2_ST(8) );"));
        CHECK(has(r, "cpu->pc = PSX_R(31); { PSX_B2_ST(8) return; }"));
    }
    // A hook that can write registers reloads them after.
    {
        GprLocalsStats st;
        const std::string b =
            "{\n"
            "block_80010000:\n"
            "    cpu->gpr[3] = 7;\n"
            "psx_mod_instruction(cpu, 0x80010004u, 0x00000000u);\n"
            "    cpu->pc = cpu->gpr[31]; return;\n"
            "    ;  /* label compatibility */\n"
            "}\n";
        const std::string r = gpr_localize_body(b, 8, 0x80010000u, st);
        CHECK(has(r, "{ PSX_B2_ST(3) psx_mod_instruction(cpu, 0x80010004u, 0x00000000u); "
                     "PSX_B2_LD(3) PSX_B2_LD(31) }"));
        CHECK(has(r, "cpu->pc = PSX_R(31); { return; }"));     // nothing dirty after the reload
    }
    // A return inside an if-block ends only that path: a register used after
    // the block is still reloaded after an earlier call.
    {
        GprLocalsStats st;
        const std::string b =
            "{\n"
            "block_80010000:\n"
            "    psx_mod_instruction(cpu, 0x80010000u, 0x00000000u);\n"
            "    if (cpu->gpr[4] == 0) {\n"
            "        cpu->pc = 0x80020000u; return;\n"
            "    }\n"
            "    cpu->gpr[2] = cpu->gpr[5];\n"
            "    cpu->pc = cpu->gpr[31]; return;\n"
            "    ;  /* label compatibility */\n"
            "}\n";
        const std::string r = gpr_localize_body(b, 8, 0x80010000u, st);
        CHECK(has(r, "psx_mod_instruction(cpu, 0x80010000u, 0x00000000u); "
                     "PSX_B2_LD(4) PSX_B2_LD(5) PSX_B2_LD(31) }"));
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("gpr_locals_test: ok\n");
    return 0;
}
