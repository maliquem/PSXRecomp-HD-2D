/* Pins the RAM trampoline patterns psx_unknown_dispatch resolves, through the
 * pure psx_trampoline_decode(). Words are hand-encoded MIPS. Known divergences
 * from hardware are printed as KNOWN and do not fail: fixing them changes what
 * the game executes and belongs to its own round. */
#include "ram_trampoline.h"

#include <stdio.h>
#include <string.h>

enum { T0 = 8, T1 = 9, RA = 31 };

static int failures, known;

static void check(int condition, const char *name) {
    printf("%s  %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) failures++;
}

static void note_known(const char *text) {
    printf("KNOWN %d: %s\n", ++known, text);
}

/* Same lazy loop as psx_unknown_dispatch; *reads = words fetched. */
static int run(uint32_t addr, const uint32_t *mem, const uint32_t gpr[32],
               psx_trampoline_t *t, int *reads) {
    uint32_t w[PSX_TRAMPOLINE_MAX_WORDS];
    int n = 0, need = 3;
    for (;;) {
        for (; n < need; n++) w[n] = mem[n];
        if (psx_trampoline_decode(addr, w, n, gpr, t) != PSX_TRAMP_NEED_WORDS) break;
        need = t->need_words;
    }
    *reads = n;
    return t->kind;
}

typedef struct {
    const char *name;
    uint32_t addr;
    uint32_t mem[PSX_TRAMPOLINE_MAX_WORDS];
    int gpr_reg;
    uint32_t gpr_val;
    int kind;
    uint32_t target;
    int ds_reg;
    uint32_t ds_val;
    int reads;
} tcase_t;

#define TABLE_W 0x3C088000u, 0x25080200u, 0x00094880u, 0x01094021u

static const tcase_t cases[] = {
    {"J", 0x800000A0u, {0x08000170u}, 0, 0, PSX_TRAMP_J, 0x800005C0u, -1, 0, 3},
    {"J max field", 0xA0001000u, {0x0BFFFFFFu}, 0, 0, PSX_TRAMP_J, 0xAFFFFFFCu, -1, 0, 3},
    {"JR t0", 0x80001000u, {0x01000008u}, T0, 0x80012340u, PSX_TRAMP_JR, 0x80012340u, -1, 0, 3},
    {"JR ra", 0x80001000u, {0x03E00008u}, RA, 0xBFC01234u, PSX_TRAMP_JR, 0xBFC01234u, -1, 0, 3},
    {"LI/JR + addiu slot", 0x80001000u, {0x240805C0u, 0x01000008u, 0x2409003Bu}, 0, 0,
     PSX_TRAMP_LI_JR, 0x000005C0u, T1, 0x3Bu, 3},
    {"LI/JR negative imm, nop slot", 0x80001000u, {0x24088000u, 0x01000008u, 0}, 0, 0,
     PSX_TRAMP_LI_JR, 0xFFFF8000u, -1, 0, 3},
    {"LUI/ADDIU/JR", 0x80001000u, {0x3C088001u, 0x25082340u, 0x01000008u}, 0, 0,
     PSX_TRAMP_LUI_JR, 0x80012340u, -1, 0, 3},
    {"LUI/ADDIU carry", 0x80001000u, {0x3C088002u, 0x25088000u, 0x01000008u}, 0, 0,
     PSX_TRAMP_LUI_JR, 0x80018000u, -1, 0, 3},
    {"LUI/ORI/JR", 0x80001000u, {0x3C088002u, 0x35088000u, 0x01000008u}, 0, 0,
     PSX_TRAMP_LUI_JR, 0x80028000u, -1, 0, 3},
    {"table", 0x80000500u, {TABLE_W, 0x8D080000u, 0x01000008u, 0}, T1, 0x3Fu,
     PSX_TRAMP_TABLE, 0x800002FCu, -1, 0, 6},
    {"table with nop", 0x80000500u, {TABLE_W, 0x8D080000u, 0, 0x01000008u}, T1, 0x3Fu,
     PSX_TRAMP_TABLE, 0x800002FCu, -1, 0, 7},
    {"neg LI/JR other reg", 0x80001000u, {0x240805C0u, 0x01200008u}, 0, 0,
     PSX_TRAMP_NONE, 0, -1, 0, 3},
    {"neg ADDIU rt != rN", 0x80001000u, {0x3C088001u, 0x25092340u, 0x01000008u}, 0, 0,
     PSX_TRAMP_NONE, 0, -1, 0, 3},
    {"neg LUI/ADDIU jr other", 0x80001000u, {0x3C088001u, 0x25082340u, 0x01200008u}, 0, 0,
     PSX_TRAMP_NONE, 0, -1, 0, 6},
    {"neg lui/ori/sw prologue", 0x80001000u, {0x3C088001u, 0x35082340u, 0xAFBF0010u}, 0, 0,
     PSX_TRAMP_NONE, 0, -1, 0, 6},
    {"neg table wrong lw", 0x80000500u, {TABLE_W, 0x8D090000u, 0x01000008u, 0}, T1, 0x3Fu,
     PSX_TRAMP_NONE, 0, -1, 0, 6},
    {"slot rewrites jump reg", 0x80001000u, {0x240805C0u, 0x01000008u, 0x24080000u}, 0, 0,
     PSX_TRAMP_LI_JR, 0x000005C0u, T0, 0, 3},
};

static int expect_case(const tcase_t *c, psx_trampoline_t *t) {
    uint32_t gpr[32];
    int reads, ok;
    memset(gpr, 0, sizeof(gpr));
    if (c->gpr_reg) gpr[c->gpr_reg] = c->gpr_val;
    ok = run(c->addr, c->mem, gpr, t, &reads) == c->kind && t->ds_reg == c->ds_reg &&
         reads == c->reads;
    if (c->kind != PSX_TRAMP_NONE) ok = ok && t->target == c->target;
    if (c->ds_reg >= 0) ok = ok && t->ds_val == c->ds_val;
    return ok;
}

int main(void) {
    psx_trampoline_t t;
    uint32_t gpr[32];
    int reads;
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        check(expect_case(&cases[i], &t), cases[i].name);

    /* Known divergences: pinned at today's behaviour, reported, never failing. */
    memset(gpr, 0, sizeof(gpr));
    {
        const uint32_t m[7] = {0x240805C0u, 0x01000008u, 0x24000005u};
        run(0x80001000u, m, gpr, &t, &reads);
        check(t.kind == PSX_TRAMP_LI_JR && t.ds_reg == 0 && t.ds_val == 5u,
              "li/jr slot addiu $zero decoded as today");
        note_known("li/jr delay slot 'addiu $zero,$zero,5' writes gpr[0]=5; hardware leaves $zero at 0");
    }
    {
        const uint32_t m[7] = {0x3C008001u, 0x24002340u, 0x00000008u};
        run(0x80001000u, m, gpr, &t, &reads);
        check(t.kind == PSX_TRAMP_LUI_JR && t.target == 0x80012340u,
              "lui/addiu/jr on $zero decoded as today");
        note_known("lui/addiu/jr through $zero jumps to 0x80012340; hardware jumps to 0");
    }
    {
        const uint32_t m[7] = {0x24000123u, 0x00000008u, 0};
        run(0x80001000u, m, gpr, &t, &reads);
        check(t.kind == PSX_TRAMP_LI_JR && t.target == 0x123u,
              "li/jr on $zero decoded as today");
        note_known("li/jr through $zero jumps to 0x123; hardware jumps to 0");
    }
    {
        const uint32_t m[7] = {0x08000170u, 0x2409003Bu};
        run(0x800000A0u, m, gpr, &t, &reads);
        check(t.kind == PSX_TRAMP_J && t.ds_reg == -1, "j delay slot dropped as today");
        note_known("delay slots of j/jr/lui-jr/table are not executed and the jump register "
                   "(and the table's shifted index) is never written back");
    }
    note_known("trampoline instructions are not charged to the cycle count");
    {
        const uint32_t m[7] = {0x08000000u};
        run(0x1F800000u, m, gpr, &t, &reads);
        check(t.kind == PSX_TRAMP_J && t.target == 0x10000000u,
              "target outside mapped memory accepted as today");
        note_known("resolved target is not checked against a mapped region (0x10000000 accepted)");
    }
    {
        const uint32_t m[7] = {0x01000808u};
        gpr[T0] = 0x80012340u;
        run(0x80001000u, m, gpr, &t, &reads);
        check(t.kind == PSX_TRAMP_JR && t.target == 0x80012340u,
              "non-canonical jr (rd=1) accepted as today");
        note_known("jr is matched on opcode/funct only; rt/rd/sa are ignored (hardware result unknown)");
    }

    printf("%d failure(s), %d known divergence(s)\n", failures, known);
    return failures ? 1 : 0;
}
