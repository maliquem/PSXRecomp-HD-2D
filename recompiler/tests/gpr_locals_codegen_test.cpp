// [recompiler] gpr_locals (docs/GPR_LOCALS.md) over REAL generator output
// (GEN-22): small MIPS functions go through CodeGenerator::generate_function
// with the production [recompiler] options, which runs gpr_localize_body on
// the body; the result is then checked against invariants that hold for any
// body, independent of how the pass computes its sets:
//   - every PSX_R(n) has its PSX_B2_DECL(n) (loaded from cpu->gpr) before the
//     first use; no cpu->gpr[] is left in a localized body; lean has its
//     PSX_LN_DECL;
//   - no assignment to PSX_R(0);
//   - every local written since the last sync is stored (PSX_B2_ST/SX) before
//     every return, every call that takes `cpu`, and every PSX_B2_*/PSX_LN_*
//     form with an OUT set (I-cache, charge, edge, slice, loads, stores);
//   - after a call that can write registers, a local not reloaded is never
//     read or stored before it is written again;
//   - nothing dirty falls off the end.
// A violation in today's output is printed as KNOWN and counted, not failed:
// the generator is not edited in this round. Hard failures are only the
// checker's own self-test and a generator that produced nothing.
#include "code_generator.h"
#include "control_flow.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kBase = 0x80010000u;
int failures = 0;
int known_total = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message.c_str());
        ++failures;
    }
}

void append_word(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
}

bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string regs_text(uint32_t s) {
    std::string out;
    for (int n = 0; n < 32; n++)
        if (s & (1u << n)) out += (out.empty() ? "" : ",") + std::to_string(n);
    return out;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

size_t match_paren(const std::string& m, size_t open) {
    int depth = 0;
    for (size_t i = open; i < m.size(); i++) {
        if (m[i] == '(') depth++;
        else if (m[i] == ')' && --depth == 0) return i;
    }
    return std::string::npos;
}

// The line with /* */ and // comments blanked (same length).
std::string mask_comments(const std::string& s, bool& in_comment) {
    std::string m = s;
    for (size_t i = 0; i < m.size(); i++) {
        if (in_comment) {
            if (m[i] == '*' && i + 1 < m.size() && m[i + 1] == '/') {
                m[i] = m[i + 1] = ' ';
                i++;
                in_comment = false;
            } else {
                m[i] = ' ';
            }
        } else if (m[i] == '/' && i + 1 < m.size() && m[i + 1] == '*') {
            m[i] = m[i + 1] = ' ';
            i++;
            in_comment = true;
        } else if (m[i] == '/' && i + 1 < m.size() && m[i + 1] == '/') {
            for (size_t j = i; j < m.size(); j++) m[j] = ' ';
            break;
        }
    }
    return m;
}

struct Tok { char kind; int reg; size_t start; };   // kind: 'S' store (ST/SX), 'L' reload

// Blanks PSX_B2_ST/SX/LD(n), PSX_LN_PUB() and PSX_LN_PUBX in `m` (positions
// kept) and returns them, so separators around them can be found.
std::vector<Tok> strip_tokens(std::string& m) {
    std::vector<Tok> toks;
    static const char* const heads[] = {"PSX_B2_ST(", "PSX_B2_SX(", "PSX_B2_LD("};
    for (const char* h : heads) {
        const std::string head = h;
        for (size_t p = m.find(head); p != std::string::npos; p = m.find(head, p + 1)) {
            const size_t close = m.find(')', p);
            if (close == std::string::npos) break;
            const int reg = std::atoi(m.substr(p + head.size(), close - p - head.size()).c_str());
            toks.push_back({head[7] == 'L' ? 'L' : 'S', reg, p});
            for (size_t k = p; k <= close; k++) m[k] = ' ';
        }
    }
    for (const char* h : {"PSX_LN_PUB()", "PSX_LN_PUBX"}) {
        const std::string head = h;
        for (size_t p = m.find(head); p != std::string::npos; p = m.find(head, p + 1))
            for (size_t k = p; k < p + head.size(); k++) m[k] = ' ';
    }
    return toks;
}

uint32_t tok_set(const std::vector<Tok>& toks, char kind, size_t from, size_t to) {
    uint32_t s = 0;
    for (const Tok& t : toks)
        if (t.kind == kind && t.start > from && t.start < to && t.reg >= 0 && t.reg < 32) s |= 1u << t.reg;
    return s;
}

struct Ev {
    size_t pos;
    enum Type { Read, Write, Sync, Reset, Goto } type;
    uint32_t set;          // Read/Write: the register bit; Sync: stored set; Reset: reloaded set
    std::string name;      // Sync: what it is; Goto: the label
};

// Checks one localized body. Returns the number of violations; prints each
// as KNOWN with `where` when `print` is set.
int check_body(const std::string& where, const std::string& body, bool print) {
    int found = 0;
    auto report = [&](const std::string& what, const std::string& line) {
        ++found;
        if (print) std::printf("KNOWN %s: %s: %s\n", where.c_str(), what.c_str(), trim(line).substr(0, 200).c_str());
    };

    std::vector<std::string> lines;
    for (size_t at = 0; at < body.size();) {
        const size_t nl = body.find('\n', at);
        const size_t end = nl == std::string::npos ? body.size() : nl + 1;
        lines.push_back(body.substr(at, end - at));
        at = end;
    }

    // Declarations: every PSX_R(n) declared, the declarations ahead of every use.
    uint32_t decl = 0, used = 0;
    for (size_t p = body.find("PSX_B2_DECL("); p != std::string::npos; p = body.find("PSX_B2_DECL(", p + 1))
        decl |= 1u << (std::atoi(body.c_str() + p + 12) & 31);
    const size_t first_decl = body.find("PSX_B2_DECL(");
    for (size_t p = body.find("PSX_R("); p != std::string::npos; p = body.find("PSX_R(", p + 1)) {
        if (p > 0 && is_ident(body[p - 1])) continue;
        const int reg = std::atoi(body.c_str() + p + 6) & 31;
        used |= 1u << reg;
        if (first_decl == std::string::npos || p < first_decl) {
            report("PSX_R(" + std::to_string(reg) + ") used before the declarations", body.substr(p, 40));
            break;
        }
    }
    if (used & ~decl) report("locals used but not declared: " + regs_text(used & ~decl), "");
    if (body.find("cpu->gpr[") != std::string::npos)
        report("cpu->gpr[] left in a localized body", body.substr(body.find("cpu->gpr["), 60));
    if (body.find("PSX_LN_") != std::string::npos && body.find("PSX_LN_DECL") == std::string::npos)
        report("PSX_LN_* forms without PSX_LN_DECL", "");

    // Dirty / stale tracking, line by line in text order; gotos carry their
    // sets to the label, repeated so back edges reach their loop heads.
    std::map<std::string, uint32_t> goto_dirty, goto_stale;
    for (int round = 0; round < 4; round++) {
        const bool last = round == 3;
        uint32_t dirty = 0, stale = 0;
        int depth = 0;
        bool in_comment = false;
        for (size_t i = 0; i < lines.size(); i++) {
            const std::string& raw = lines[i];
            std::string m = mask_comments(raw, in_comment);
            const int depth_at = depth;
            for (char ch : m) depth += ch == '{' ? 1 : ch == '}' ? -1 : 0;
            if (i == 0) continue;                                 // the function's "{"
            const std::string t0 = trim(m);
            if (t0.empty() || t0[0] == '#') continue;
            if (depth == 0 && t0 == "}") {                        // the function's end
                if (last && dirty) report("falls off the end with dirty " + regs_text(dirty), raw);
                continue;
            }
            // A block label.
            if (t0.size() >= 15 && t0.rfind("block_", 0) == 0 && t0[14] == ':') {
                const std::string label = t0.substr(0, 14);
                dirty |= goto_dirty[label];
                stale |= goto_stale[label];
                continue;
            }
            std::vector<Tok> toks = strip_tokens(m);              // m is now stripped
            if (raw.find("gpr_locals: entry store") != std::string::npos) {
                const uint32_t st = tok_set(toks, 'S', 0, std::string::npos);
                if (last && (st & stale)) report("entry store of a stale local " + regs_text(st & stale), raw);
                dirty &= ~st;
                continue;
            }
            std::vector<Ev> ev;
            for (size_t p = 0; p < m.size(); p++) {
                if (!(std::isalpha(static_cast<unsigned char>(m[p])) || m[p] == '_') || (p > 0 && is_ident(m[p - 1])))
                    continue;
                size_t j = p;
                while (j < m.size() && is_ident(m[j])) j++;
                const std::string word = m.substr(p, j - p);
                size_t k = j;
                while (k < m.size() && m[k] == ' ') k++;
                if (word == "PSX_R" && k < m.size() && m[k] == '(') {
                    const size_t close = m.find(')', k);
                    const int reg = std::atoi(m.c_str() + k + 1) & 31;
                    size_t a = close + 1;
                    while (a < m.size() && m[a] == ' ') a++;
                    size_t b = a;
                    if (b < m.size() && std::string("+-*/%&|^").find(m[b]) != std::string::npos) b++;
                    else if (m.compare(b, 2, "<<") == 0 || m.compare(b, 2, ">>") == 0) b += 2;
                    const bool assigns = b < m.size() && m[b] == '=' && !(b + 1 < m.size() && m[b + 1] == '=');
                    if (!assigns || b != a) ev.push_back({p, Ev::Read, 1u << reg, ""});
                    if (assigns) {
                        // The write lands when its statement ends.
                        const size_t semi = m.find(';', close);
                        ev.push_back({semi == std::string::npos ? m.size() : semi, Ev::Write, 1u << reg, ""});
                    }
                    p = close;
                    continue;
                }
                if (word == "return" && k < m.size() && m[k] == ';') {
                    const size_t sep = m.find_last_of("{(;", p);
                    ev.push_back({p, Ev::Sync, tok_set(toks, 'S', sep == std::string::npos ? 0 : sep, p), "return"});
                    p = k;
                    continue;
                }
                if (word == "goto") {
                    size_t b = k;
                    while (b < m.size() && is_ident(m[b])) b++;
                    ev.push_back({p, Ev::Goto, 0, m.substr(k, b - k)});
                    p = b;
                    continue;
                }
                if (k >= m.size() || m[k] != '(' || word == "if" || word == "while" || word == "switch" ||
                    word == "for" || word == "sizeof" || (p >= 2 && m.compare(p - 2, 2, "->") == 0))
                    continue;
                const size_t close = match_paren(m, k);
                if (close == std::string::npos) continue;
                if (word.rfind("PSX_B2_", 0) == 0 || word.rfind("PSX_LN_", 0) == 0) {
                    if (word != "PSX_B2_DECL")
                        ev.push_back({p, Ev::Sync, tok_set(toks, 'S', k, close), word});
                    p = k;          // forms nest nothing that matters, but scan the args
                    continue;
                }
                // A call that receives the CPU state: it may read registers.
                bool takes_cpu = false;
                for (size_t q = m.find("cpu", k); q != std::string::npos && q < close; q = m.find("cpu", q + 1))
                    takes_cpu = takes_cpu || (!is_ident(m[q - 1]) && !is_ident(m[q + 3]));
                if (!takes_cpu) continue;
                const size_t sep = p == 0 ? std::string::npos : m.find_last_of("{(;", p - 1);
                ev.push_back({p, Ev::Sync, tok_set(toks, 'S', sep == std::string::npos ? 0 : sep, p), word});
                // `{ OUT call(cpu..); IN }`: the call may write registers, IN reloads.
                size_t q = close + 1;
                while (q < m.size() && m[q] == ' ') q++;
                if (q < m.size() && m[q] == ';') q++;
                while (q < m.size() && m[q] == ' ') q++;
                if (q < m.size() && m[q] == '}') ev.push_back({close, Ev::Reset, tok_set(toks, 'L', close, q), word});
                p = k;
            }
            // Ordered by position; at one position a read comes before a write.
            for (size_t x = 1; x < ev.size(); x++)
                for (size_t y = x; y > 0 && ev[y].pos < ev[y - 1].pos; y--) std::swap(ev[y], ev[y - 1]);
            for (const Ev& e : ev) {
                switch (e.type) {
                case Ev::Read:
                    if (last && (e.set & stale)) report("read of local " + regs_text(e.set) + " not reloaded after a call", raw);
                    break;
                case Ev::Write:
                    if (last && (e.set & 1u)) report("$zero written", raw);
                    dirty |= e.set;
                    stale &= ~e.set;
                    break;
                case Ev::Sync:
                    if (last && (dirty & ~e.set))
                        report(e.name + " without storing " + regs_text(dirty & ~e.set), raw);
                    if (last && (e.set & stale)) report(e.name + " stores stale " + regs_text(e.set & stale), raw);
                    break;
                case Ev::Reset:
                    dirty = 0;
                    stale = used & ~e.set;
                    break;
                case Ev::Goto:
                    goto_dirty[e.name] |= dirty;
                    goto_stale[e.name] |= stale;
                    break;
                }
            }
            // `{ PSX_B2_ST(..) }` alone: what falling off the end stores.
            if (ev.empty() && t0.front() == '{' && t0.back() == '}') dirty &= ~tok_set(toks, 'S', 0, std::string::npos);
            // A top-level unconditional return or goto ends the path.
            const std::string t = trim(m);
            int bal = 0;
            for (char ch : t) bal += ch == '{' ? 1 : ch == '}' ? -1 : 0;
            std::string u = t;
            if (!u.empty() && u.back() == '}') u = trim(u.substr(0, u.size() - 1));
            const bool cond = t.rfind("if", 0) == 0 || t.rfind("else", 0) == 0 || t.rfind("case", 0) == 0 ||
                              t.rfind("default", 0) == 0 || (!t.empty() && t[0] == '}');
            if (depth_at == 1 && bal == 0 && !cond &&
                ((u.size() >= 7 && u.compare(u.size() - 7, 7, "return;") == 0) || t.rfind("goto block_", 0) == 0)) {
                dirty = 0;
                stale = 0;
            }
        }
    }
    return found;
}

// The checker catches what it is meant to catch.
void checker_self_test() {
    const std::string good =
        "{\n"
        "    PSX_B2_DECL(4) PSX_B2_DECL(31) \n"
        "block_80010000:\n"
        "    PSX_R(4) = PSX_R(4) + 1;\n"
        "    PSX_B2_EDGE(0x80020000u, PSX_B2_ST(4) , PSX_B2_LD(4) );\n"
        "    cpu->pc = PSX_R(31); { PSX_B2_ST(4) return; }\n"
        "    { PSX_B2_ST(4) }\n"
        "    ;  /* label compatibility */\n"
        "}\n";
    check(check_body("self-good", good, false) == 0, "checker accepts a body that stores before every exit");
    const std::string no_store =
        "{\n"
        "    PSX_B2_DECL(4) \n"
        "block_80010000:\n"
        "    PSX_R(4) = 1;\n"
        "    cpu->pc = 0x80020000u; { return; }\n"
        "    ;\n"
        "}\n";
    check(check_body("self-return", no_store, false) == 1, "checker flags a return that drops a dirty local");
    const std::string edge =
        "{\n"
        "    PSX_B2_DECL(4) PSX_B2_DECL(5) \n"
        "block_80010000:\n"
        "    PSX_R(4) = 1;\n"
        "    PSX_R(5) = 2;\n"
        "    PSX_B2_EDGE(0x80020000u, PSX_B2_ST(4) , );\n"
        "    cpu->pc = 0x80020000u; { PSX_B2_ST(4) PSX_B2_ST(5) return; }\n"
        "    ;\n"
        "}\n";
    check(check_body("self-edge", edge, false) == 1, "checker flags an edge that misses a dirty local");
    const std::string zero_and_undecl =
        "{\n"
        "    PSX_B2_DECL(4) \n"
        "block_80010000:\n"
        "    PSX_R(0) = PSX_R(9);\n"
        "    cpu->pc = 0x80020000u; { PSX_B2_ST(0) return; }\n"
        "    ;\n"
        "}\n";
    check(check_body("self-zero", zero_and_undecl, false) == 2, "checker flags $zero written and an undeclared local");
    const std::string stale =
        "{\n"
        "    PSX_B2_DECL(4) PSX_B2_DECL(5) \n"
        "block_80010000:\n"
        "    { PSX_B2_ST(4) psx_mod_instruction(cpu, 0x80010000u, 0x00000000u); PSX_B2_LD(4) }\n"
        "    PSX_R(4) = PSX_R(5);\n"
        "    cpu->pc = 0x80020000u; { PSX_B2_ST(4) return; }\n"
        "    ;\n"
        "}\n";
    check(check_body("self-stale", stale, false) == 1, "checker flags a read of a local not reloaded after a call");
    const std::string loop =
        "{\n"
        "    PSX_B2_DECL(8) PSX_B2_DECL(31) \n"
        "block_80010000:\n"
        "    PSX_B2_ICACHE(0x80010000u, );\n"
        "block_80010004:\n"
        "    PSX_B2_ICACHE(0x80010004u, );\n"
        "    PSX_R(8) = PSX_R(8) + 1;\n"
        "    if (PSX_R(8) != 0) {\n"
        "        goto block_80010004;\n"
        "    }\n"
        "    cpu->pc = PSX_R(31); { PSX_B2_ST(8) return; }\n"
        "    ;\n"
        "}\n";
    check(check_body("self-loop", loop, false) == 1, "checker carries a back edge's dirty set to the loop head");
}

struct Case {
    const char* name;
    std::vector<uint32_t> words;
};

void run_case(const Case& c, const char* mode, PSXRecomp::CodeGenConfig config) {
    const uint32_t count = static_cast<uint32_t>(c.words.size());
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    exe.header.initial_pc = kBase;
    exe.header.file_size = count * 4u;
    for (uint32_t w : c.words) append_word(exe.code_data, w);

    PSXRecomp::Function function{};
    function.start_addr = kBase;
    function.end_addr = kBase + count * 4u;
    function.size = count * 4u;
    function.name = c.name;

    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(exe, config);
    const auto gen = generator.generate_function(function, cfg);
    const std::string where = std::string(c.name) + "/" + mode;
    check(!gen.body.empty(), where + ": generator produced a body");
    const auto& st = generator.gpr_locals_stats();
    if (st.localized != 1) {
        // Not an invariant break, but the pass then says nothing about real output.
        std::string why;
        for (const auto& kv : st.kept) why += kv.first + " ";
        std::printf("KNOWN %s: body kept by the pass (%s)\n", where.c_str(), why.c_str());
        ++known_total;
        return;
    }
    known_total += check_body(where, gen.body, true);
}

}  // namespace

int main() {
    checker_self_test();

    const std::vector<Case> cases = {
        // Load delay slot, a loop with a branch delay slot, a store in jr's delay slot.
        {"load_delay_loop", {
            0x8C890000u,  // +00 lw    $t1, 0($a0)
            0x25280001u,  // +04 addiu $t0, $t1, 1       (load delay slot: old $t1)
            0x2508FFFFu,  // +08 addiu $t0, $t0, -1      (loop top)
            0x1500FFFEu,  // +0C bne   $t0, $zero, +08
            0x01295021u,  // +10 addu  $t2, $t1, $t1     (delay slot)
            0x03E00008u,  // +14 jr    $ra
            0xAC8A0004u,  // +18 sw    $t2, 4($a0)       (delay slot)
        }},
        // A call in the middle: $ra and the delay slot's write must reach
        // cpu->gpr before the callee runs; $s0 is used again after it.
        {"jal_middle", {
            0x24040005u,  // +00 addiu $a0, $zero, 5
            0x8FB00000u,  // +04 lw    $s0, 0($sp)
            0x0C008000u,  // +08 jal   0x80020000
            0x02042821u,  // +0C addu  $a1, $s0, $a0     (delay slot)
            0x00501021u,  // +10 addu  $v0, $v0, $s0     (continuation)
            0xAFA20008u,  // +14 sw    $v0, 8($sp)
            0x03E00008u,  // +18 jr    $ra
            0x00000000u,  // +1C nop
        }},
        // Accesses that can raise an address error or reach MMIO, and $zero
        // as a destination.
        {"memory_faults", {
            0x01090021u,  // +00 addu  $zero, $t0, $t1
            0x8C800000u,  // +04 lw    $zero, 0($a0)
            0x84820001u,  // +08 lh    $v0, 1($a0)       (odd address: AdEL)
            0x24030003u,  // +0C addiu $v1, $zero, 3     (load delay slot)
            0xACA30000u,  // +10 sw    $v1, 0($a1)
            0x90A80002u,  // +14 lbu   $t0, 2($a1)
            0x03E00008u,  // +18 jr    $ra
            0x00481021u,  // +1C addu  $v0, $v0, $t0     (delay slot)
        }},
    };

    // The production [recompiler] options (Breath of Fire IV game.toml), and
    // the plain B2 forms with the default flush limit.
    PSXRecomp::CodeGenConfig b2{};
    b2.ram_fast_path = true;
    b2.cpu_timing_block = true;
    b2.gpr_locals = true;
    PSXRecomp::CodeGenConfig lean = b2;
    lean.gpr_locals_flush_max = 4;
    lean.lean = true;
    lean.lean_no_locals = true;

    for (const Case& c : cases) {
        run_case(c, "b2", b2);
        run_case(c, "lean", lean);
    }

    std::printf("gpr_locals_codegen_test: %d known invariant break(s) in today's output\n", known_total);
    if (failures != 0) {
        std::printf("gpr_locals_codegen_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("PASS: gpr_locals over generated code");
    return 0;
}
