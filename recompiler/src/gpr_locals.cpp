// [recompiler] gpr_locals (docs/GPR_LOCALS.md).
//
// Runs over one generated function body as text, the way block_timing_rewrite
// and ram_fast_rewrite run over instructions: the emitters' output has a small
// fixed vocabulary, and anything outside it leaves the body as it was.
//
//   1. cpu->gpr[N] becomes PSX_R(N); every register the body names is loaded
//      into its local at the top (PSX_B2_DECL), before the CPS entry switch.
//   2. The body is cut at its block labels. Each block's writes and gotos give
//      a forward union, to a fixpoint, of the registers that may differ from
//      cpu->gpr when the block starts (a block is also entered by fallthrough
//      from the one above it). A block whose set is larger than flush_max
//      stores it on entry and starts clean.
//   3. Every call is classified (classify below). Calls into code that can
//      read guest registers get the dirty set stored first (OUT); calls into
//      code that can write them reload every local after (IN). The inline
//      helpers with a cold out-of-line path become the PSX_B2_* macros of
//      runtime/include/psx_gpr_locals.h, which do OUT/IN in that path only.
//   4. Every `return;` stores the dirty set first.
//
// Off (PSX_GPR_LOCALS not defined) the macros expand to the original text.
#include "gpr_locals.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using RegSet = uint32_t;  // bit n: guest register n

enum class Kind {
    Unknown,
    Ignore,       // never looks at cpu->gpr, never runs device service or longjmps
    OutOnly,      // can read guest registers / run device service: OUT before
    Icache, Charge, Edge, Slice,
    LoadW, LoadH, LoadB, Lwc2, StoreW, StoreH, StoreB,
    CallStmt,     // `name(args);` that can write guest registers: OUT, IN
    CallIfReturn, // `if (name(args)) return;` hooks: OUT, IN
};

Kind classify(const std::string& n) {
    static const char* const ignore[] = {
        "cosim_instr", "cosim_block", "psx_cyc_bb_defer_flush", "psx_cyc_bb_defer_begin",
        "psx_store_cycle_barrier", "psx_mult_latency_s", "psx_mult_latency_u",
        "gte_read_data", "gte_read_ctrl", "gte_write_data", "gte_write_ctrl",
        "psx_ws_cull_sltiu", "psx_ws_cull_sltiu_y", "psx_game_text_native_ok",
    };
    static const char* const out_only[] = {
        "psx_muldiv_stall", "psx_muldiv_set", "psx_gte_stall", "psx_gte_read",
        "gte_precision_store_word", "psx_lwl", "psx_lwr", "psx_swl", "psx_swr",
        "debug_server_cyc_observe", "debug_server_log_call_entry", "psx_ws_sprite_tag",
    };
    static const char* const call_stmt[] = {
        "psx_mod_instruction", "gte_execute", "psx_syscall", "psx_break",
        "psx_unknown_dispatch", "psx_unaligned_access",
    };
    static const char* const call_if_return[] = {
        "psx_mod_function_entry", "psx_vsync_query_hle_enter", "psx_datashard_enter",
    };
    if (n.rfind("PGXP_", 0) == 0) return Kind::Ignore;  // pgxp.cpp has no cpu-> access
    for (const char* k : ignore) if (n == k) return Kind::Ignore;
    for (const char* k : out_only) if (n == k) return Kind::OutOnly;
    for (const char* k : call_stmt) if (n == k) return Kind::CallStmt;
    for (const char* k : call_if_return) if (n == k) return Kind::CallIfReturn;
    if (n.rfind("func_", 0) == 0 || n.rfind("psx_alias_body_", 0) == 0) return Kind::CallStmt;
    if (n == "psx_icache_fetch") return Kind::Icache;
    if (n == "psx_cyc_charge") return Kind::Charge;
    if (n == "psx_check_interrupts_at") return Kind::Edge;
    if (n == "psx_slice_block") return Kind::Slice;
    if (n == "psx_blk_load_word") return Kind::LoadW;
    if (n == "psx_blk_load_half") return Kind::LoadH;
    if (n == "psx_blk_load_byte") return Kind::LoadB;
    if (n == "psx_blk_lwc2_read") return Kind::Lwc2;
    if (n == "psx_store_word") return Kind::StoreW;
    if (n == "psx_store_half") return Kind::StoreH;
    if (n == "psx_store_byte") return Kind::StoreB;
    return Kind::Unknown;
}

bool is_ident(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// The line with every /* comment */ blanked out (same length), for scanning.
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

// The ')' matching the '(' at `open` in the masked text, or npos.
size_t match_paren(const std::string& m, size_t open) {
    int depth = 0;
    for (size_t i = open; i < m.size(); i++) {
        if (m[i] == '(') depth++;
        else if (m[i] == ')' && --depth == 0) return i;
    }
    return std::string::npos;
}

// The top-level arguments between `open` and `close` (taken from `s`, ranges
// found in the masked text `m`), trimmed.
std::vector<std::string> split_args(const std::string& s, const std::string& m,
                                    size_t open, size_t close) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = open + 1;
    auto push = [&](size_t end) {
        std::string a = s.substr(start, end - start);
        size_t b = a.find_first_not_of(" \t"), e = a.find_last_not_of(" \t");
        out.push_back(b == std::string::npos ? std::string() : a.substr(b, e - b + 1));
    };
    for (size_t i = open + 1; i < close; i++) {
        if (m[i] == '(') depth++;
        else if (m[i] == ')') depth--;
        else if (m[i] == ',' && depth == 0) { push(i); start = i + 1; }
    }
    push(close);
    return out;
}

struct Call {
    size_t name_at, open, close;  // positions in the line
    std::string name;
    Kind kind;
};

struct LineScan {
    RegSet writes = 0;
    RegSet reads = 0;
    std::vector<Call> calls;
    std::vector<size_t> returns;           // positions of `return;`
    std::vector<std::string> gotos;        // block label names
    std::string label;                     // a block label line: its name
    bool preprocessor = false;
    std::string bail;                      // non-empty: the body cannot be kept exact
};

const std::set<std::string>& keywords() {
    static const std::set<std::string> k = {
        "if", "while", "switch", "for", "return", "sizeof", "case", "do", "else",
        "PSX_R", "__attribute__", "cleanup", "defined",
    };
    return k;
}

LineScan scan_line(const std::string& line, const std::string& m) {
    LineScan r;
    size_t first = m.find_first_not_of(" \t");
    if (first == std::string::npos) return r;
    if (m[first] == '#') { r.preprocessor = true; return r; }
    // Labels: only the emitters' block_XXXXXXXX: (or block_XXXXXXXX:;) alone.
    {
        size_t last = m.find_last_not_of(" \t\r\n");
        std::string t = m.substr(first, last - first + 1);
        if (!t.empty() && (t.back() == ':' || (t.size() > 1 && t.substr(t.size() - 2) == ":;"))) {
            std::string name = t.substr(0, t.find(':'));
            bool ident = !name.empty();
            for (char c : name) ident = ident && is_ident(c);
            if (ident && name != "default") {
                if (name.rfind("block_", 0) == 0 && name.size() == 14) { r.label = name; return r; }
                r.bail = "label:" + name;
                return r;
            }
        }
    }
    for (size_t i = 0; i < m.size();) {
        if (!(std::isalpha((unsigned char)m[i]) || m[i] == '_') || (i > 0 && is_ident(m[i - 1]))) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < m.size() && is_ident(m[j])) j++;
        const std::string word = m.substr(i, j - i);
        size_t k = j;
        while (k < m.size() && m[k] == ' ') k++;
        if (word == "PSX_R" && k < m.size() && m[k] == '(') {
            size_t close = match_paren(m, k);
            if (close == std::string::npos) { r.bail = "paren"; return r; }
            const int reg = std::atoi(m.substr(k + 1, close - k - 1).c_str());
            size_t a = close + 1;
            while (a < m.size() && m[a] == ' ') a++;
            if (a + 1 < m.size() && ((m[a] == '+' && m[a + 1] == '+') || (m[a] == '-' && m[a + 1] == '-'))) {
                r.bail = "incdec";
                return r;
            }
            size_t b = a;
            if (b < m.size() && std::strchr("+-*/%&|^", m[b])) b++;
            else if (b + 1 < m.size() && (m.compare(b, 2, "<<") == 0 || m.compare(b, 2, ">>") == 0)) b += 2;
            const bool assigns = b < m.size() && m[b] == '=' && !(b + 1 < m.size() && m[b + 1] == '=');
            if (reg >= 0 && reg < 32) {
                if (assigns) r.writes |= 1u << reg;
                if (!assigns || b != a) r.reads |= 1u << reg;   // compound assignment reads too
            }
            i = close + 1;
            continue;
        }
        if (word == "return") {
            size_t a = k;
            if (a < m.size() && m[a] == ';') r.returns.push_back(i);
            else { r.bail = "return-value"; return r; }
            i = j;
            continue;
        }
        if (word == "goto") {
            size_t a = k, b = a;
            while (b < m.size() && is_ident(m[b])) b++;
            std::string target = m.substr(a, b - a);
            if (target.rfind("block_", 0) != 0) { r.bail = "goto:" + target; return r; }
            r.gotos.push_back(target);
            i = b;
            continue;
        }
        if (k < m.size() && m[k] == '(' && !keywords().count(word)) {
            size_t close = match_paren(m, k);
            if (close == std::string::npos) { r.bail = "paren"; return r; }
            Kind kind = classify(word);
            if (kind == Kind::Unknown) { r.bail = "call:" + word; return r; }
            r.calls.push_back({i, k, close, word, kind});
            i = k + 1;   // calls inside the arguments are scanned too
            continue;
        }
        i = j;
    }
    return r;
}

std::string set_text(RegSet s, const char* macro) {
    std::string out;
    for (int n = 0; n < 32; n++)
        if (s & (1u << n)) { out += macro; out += "(" + std::to_string(n) + ") "; }
    return out;
}

struct Edit {
    size_t from, to;
    std::string text;
};

}  // namespace

std::string gpr_localize_body(const std::string& body, int flush_max,
                              uint32_t func_addr, GprLocalsStats& stats) {
    (void)func_addr;
    stats.functions++;
    auto keep = [&](const std::string& why) -> std::string {
        stats.kept[why]++;
        return body;
    };

    // 1. cpu->gpr[N] -> PSX_R(N); anything else naming cpu->gpr keeps the body.
    std::string s;
    s.reserve(body.size() + body.size() / 8);
    RegSet referenced = 0;
    {
        static const std::string key = "cpu->gpr";
        size_t at = 0;
        for (size_t p = body.find(key); p != std::string::npos; p = body.find(key, at)) {
            s.append(body, at, p - at);
            size_t q = p + key.size();
            if (q >= body.size() || body[q] != '[') return keep("gpr-shape");
            size_t e = q + 1;
            while (e < body.size() && std::isdigit((unsigned char)body[e])) e++;
            if (e == q + 1 || e >= body.size() || body[e] != ']') return keep("gpr-index");
            const int reg = std::atoi(body.substr(q + 1, e - q - 1).c_str());
            if (reg < 0 || reg > 31) return keep("gpr-index");
            referenced |= 1u << reg;
            s += "PSX_R(" + std::to_string(reg) + ")";
            at = e + 1;
        }
        s.append(body, at, std::string::npos);
    }
    if (!referenced) return keep("no-registers");

    // Lines (each keeps its '\n').
    std::vector<std::string> lines;
    for (size_t at = 0; at < s.size();) {
        size_t nl = s.find('\n', at);
        size_t end = nl == std::string::npos ? s.size() : nl + 1;
        lines.push_back(s.substr(at, end - at));
        at = end;
    }
    if (lines.size() < 3 || lines.front().compare(0, 1, "{") != 0) return keep("shape");

    std::vector<std::string> masked(lines.size());
    std::vector<LineScan> scans(lines.size());
    {
        bool in_comment = false;
        for (size_t i = 0; i < lines.size(); i++) {
            masked[i] = mask_comments(lines[i], in_comment);
            scans[i] = scan_line(lines[i], masked[i]);
            if (!scans[i].bail.empty()) return keep(scans[i].bail);
        }
    }

    // 2. Segments: [0] the prologue up to the first label, then one per label.
    struct Seg {
        size_t first_line;
        std::string label;
        RegSet writes = 0, in = 0, out = 0;
        bool flush = false;
        bool falls = false;     // reached by fallthrough from this block's end
        std::vector<int> succ;
    };
    std::vector<Seg> segs;
    segs.push_back({0, "", 0, 0, 0, false, false, {}});
    std::vector<int> line_seg(lines.size(), 0);
    for (size_t i = 0; i < lines.size(); i++) {
        if (!scans[i].label.empty()) segs.push_back({i, scans[i].label, 0, 0, 0, false, false, {}});
        line_seg[i] = (int)segs.size() - 1;
        segs.back().writes |= scans[i].writes;
    }
    {
        std::map<std::string, int> by_label;
        for (size_t k = 1; k < segs.size(); k++) {
            if (by_label.count(segs[k].label)) return keep("duplicate-label");
            by_label[segs[k].label] = (int)k;
        }
        for (size_t i = 0; i < lines.size(); i++) {
            for (const auto& g : scans[i].gotos) {
                auto it = by_label.find(g);
                if (it == by_label.end()) return keep("goto-unknown");
                segs[line_seg[i]].succ.push_back(it->second);
            }
        }
        // Fallthrough into the next label, unless the block's last statement is
        // an unconditional return or goto (a CPS call's continuation block is
        // entered only through the entry switch, with nothing dirty).
        for (size_t k = 0; k + 1 < segs.size(); k++) {
            bool ends = false;
            for (size_t i = segs[k + 1].first_line; i-- > segs[k].first_line;) {
                if (scans[i].preprocessor) continue;
                const std::string& mi = masked[i];
                size_t b = mi.find_first_not_of(" \t\r\n"), e = mi.find_last_not_of(" \t\r\n");
                if (b == std::string::npos) continue;   // blank or comment only
                const std::string t = mi.substr(b, e - b + 1);
                const bool cond = t.rfind("if", 0) == 0 || t.rfind("else", 0) == 0 || t[0] == '}';
                ends = !cond && ((t.size() >= 7 && t.compare(t.size() - 7, 7, "return;") == 0) ||
                                 t.rfind("goto block_", 0) == 0);
                break;
            }
            if (!ends) { segs[k].succ.push_back((int)k + 1); segs[k].falls = true; }
        }
    }
    // Forward union to a fixpoint.
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t k = 0; k < segs.size(); k++) {
            Seg& g = segs[k];
            const bool flush = flush_max >= 0 && __builtin_popcount(g.in) > flush_max;
            const RegSet out = (flush ? 0u : g.in) | g.writes;
            g.flush = flush;
            if (out != g.out) { g.out = out; changed = true; }
            for (int t : g.succ) {
                const RegSet in = segs[t].in | g.out;
                if (in != segs[t].in) { segs[t].in = in; changed = true; }
            }
        }
    }

    RegSet written_any = 0;
    for (const auto& g : segs) written_any |= g.writes;

    // 3. The set each line's stores carry, as the rewrite below computes it.
    auto line_resets = [&](size_t i) {
        for (const Call& c : scans[i].calls)
            if (c.kind == Kind::CallStmt || c.kind == Kind::CallIfReturn) return true;
        return false;
    };
    auto line_syncs = [&](size_t i) {
        if (!scans[i].returns.empty()) return true;
        for (const Call& c : scans[i].calls) if (c.kind != Kind::Ignore) return true;
        return false;
    };
    std::vector<RegSet> cur_at(lines.size(), 0);
    {
        RegSet cur = 0;
        for (size_t i = 1; i < lines.size(); i++) {
            const LineScan& sc = scans[i];
            if (!sc.label.empty()) {
                const Seg& g = segs[line_seg[i]];
                cur = g.flush ? 0u : g.in;
                continue;
            }
            if (sc.preprocessor) continue;
            cur |= sc.writes;
            cur_at[i] = cur;
            if (line_resets(i) && !sc.writes) cur = 0;
        }
    }

    // Backward liveness of the locals: a line uses what it reads and what its
    // stores carry (a store of a local is a use of it). After a call that can
    // write guest registers, IN reloads only the locals that may be used again
    // before they are written: one that is dead can never be stored back, so
    // the callee's value stays in cpu->gpr.
    std::vector<RegSet> live_in(lines.size(), 0);
    const bool conditional_text = s.find("#else") != std::string::npos || s.find("#elif") != std::string::npos;
    if (!conditional_text) {
        std::map<std::string, int> by_label;
        for (size_t k = 1; k < segs.size(); k++) by_label[segs[k].label] = (int)k;
        auto uses = [&](size_t i) -> RegSet {
            const LineScan& sc = scans[i];
            if (!sc.label.empty()) {
                const Seg& g = segs[line_seg[i]];
                return g.flush ? g.in : 0u;
            }
            RegSet u = sc.reads;
            if (line_syncs(i)) u |= cur_at[i];
            if (i + 2 == lines.size()) u |= written_any;   // falling off the end stores these
            return u;
        };
        // Brace depth at the start of each line (1: the function's top level).
        // A top-level `...; return;` or `goto block_X;` ends every path through
        // it, so nothing after it is live there; inside an if-block it does not
        // (the not-taken path goes on past the closing brace).
        std::vector<int> depth(lines.size(), 0);
        {
            int d = 0;
            for (size_t i = 0; i < lines.size(); i++) {
                depth[i] = d;
                for (char ch : masked[i]) d += ch == '{' ? 1 : ch == '}' ? -1 : 0;
            }
        }
        auto cuts = [&](size_t i) {
            if (depth[i] != 1 || scans[i].preprocessor) return false;
            const std::string& mi = masked[i];
            size_t b = mi.find_first_not_of(" \t\r\n"), e = mi.find_last_not_of(" \t\r\n");
            if (b == std::string::npos) return false;
            const std::string t = mi.substr(b, e - b + 1);
            int bal = 0;
            for (char ch : t) bal += ch == '{' ? 1 : ch == '}' ? -1 : 0;
            if (bal != 0 || t.rfind("if", 0) == 0 || t.rfind("else", 0) == 0 || t.rfind("case", 0) == 0 ||
                t.rfind("default", 0) == 0 || t[0] == '}')
                return false;
            return (t.size() >= 7 && t.compare(t.size() - 7, 7, "return;") == 0) || t.rfind("goto block_", 0) == 0;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t k = segs.size(); k-- > 0;) {
                const size_t first = segs[k].first_line;
                const size_t end = k + 1 < segs.size() ? segs[k + 1].first_line : lines.size();
                RegSet live = segs[k].falls ? live_in[segs[k + 1].first_line] : 0u;
                for (size_t i = end; i-- > first;) {
                    if (cuts(i)) live = 0;
                    for (const auto& g : scans[i].gotos) live |= live_in[segs[by_label[g]].first_line];
                    live = scans[i].preprocessor ? live : (uses(i) | (live & ~scans[i].writes));
                    if (live != live_in[i]) { live_in[i] = live; changed = true; }
                }
            }
        }
    }
    auto in_text = [&](size_t i) {
        // $zero is never reloaded unless the body writes it (no callee makes it nonzero).
        const RegSet zero = (written_any & 1u) ? 0u : 1u;
        return set_text((conditional_text ? referenced : (live_in[i] | cur_at[i]) & referenced) & ~zero,
                        "PSX_B2_LD");
    };

    // 4. Rewrite.
    std::string out_text;
    out_text.reserve(s.size() + s.size() / 2);
    RegSet cur = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        const LineScan& sc = scans[i];
        std::string line = lines[i];
        if (i == 0) {
            out_text += line;
            out_text += "    " + set_text(referenced, "PSX_B2_DECL") + "\n";
            continue;
        }
        if (!sc.label.empty()) {
            const Seg& g = segs[line_seg[i]];
            out_text += line;
            if (g.flush && g.in) {
                out_text += "    " + set_text(g.in, "PSX_B2_ST") + " /* gpr_locals: entry store */\n";
                stats.entry_flushes++;
            }
            cur = g.flush ? 0u : g.in;
            continue;
        }
        if (sc.preprocessor) { out_text += line; continue; }
        cur |= sc.writes;
        const std::string out_st = set_text(cur, "PSX_B2_ST");
        const std::string out_sx = set_text(cur, "PSX_B2_SX");
        const std::string& m = masked[i];
        std::vector<Edit> edits;
        bool resets = false;
        for (const Call& c : sc.calls) {
            const std::vector<std::string> a = split_args(line, m, c.open, c.close);
            const std::string head = line.substr(c.name_at, c.open - c.name_at);
            const std::string call = line.substr(c.name_at, c.close + 1 - c.name_at);
            switch (c.kind) {
            case Kind::Ignore: break;
            case Kind::OutOnly:
                if (cur) edits.push_back({c.name_at, c.close + 1, "(" + out_sx + call + ")"});
                break;
            case Kind::Icache:
                if (a.size() != 2) return keep("args:" + c.name);
                edits.push_back({c.name_at, c.close + 1, "PSX_B2_ICACHE(" + a[1] + ", " + out_st + ")"});
                break;
            case Kind::Charge:
                if (a.size() != 1) return keep("args:" + c.name);
                edits.push_back({c.name_at, c.close + 1, "PSX_B2_CHARGE(" + a[0] + ", " + out_st + ")"});
                break;
            case Kind::Edge:
                if (a.size() != 2) return keep("args:" + c.name);
                edits.push_back({c.name_at, c.close + 1,
                                 "PSX_B2_EDGE(" + a[1] + ", " + out_st + ", " + in_text(i) + ")"});
                break;
            case Kind::Slice: {
                if (a.size() != 4) return keep("args:" + c.name);
                // if (psx_slice_block(cpu, A, N, S)) return;
                size_t ifp = m.rfind("if", c.name_at);
                std::string between = ifp == std::string::npos ? "x" : m.substr(ifp + 2, c.name_at - ifp - 2);
                size_t rp = m.find("return", c.close);
                size_t semi = rp == std::string::npos ? std::string::npos : m.find(';', rp);
                std::string tail = rp == std::string::npos ? "x" : m.substr(c.close + 1, rp - c.close - 1);
                if (between.find_first_not_of(" (") != std::string::npos ||
                    tail.find_first_not_of(" )") != std::string::npos || semi == std::string::npos)
                    return keep("slice-shape");
                edits.push_back({ifp, semi + 1,
                                 "PSX_B2_SLICE(" + a[1] + ", " + a[2] + ", " + a[3] + ", " +
                                 out_st + ", " + in_text(i) + ");"});
                break;
            }
            case Kind::LoadW: case Kind::LoadH: case Kind::LoadB: {
                if (a.size() != 5) return keep("args:" + c.name);
                const char* mac = c.kind == Kind::LoadW ? "PSX_B2_LOADW(" :
                                  c.kind == Kind::LoadH ? "PSX_B2_LOADH(" : "PSX_B2_LOADB(";
                edits.push_back({c.name_at, c.close + 1, std::string(mac) + a[0] + ", " + a[2] + ", " +
                                 a[3] + ", " + a[4] + ", " + out_st + ")"});
                break;
            }
            case Kind::Lwc2:
                if (a.size() != 3) return keep("args:" + c.name);
                edits.push_back({c.name_at, c.close + 1,
                                 "PSX_B2_LWC2(" + a[0] + ", " + a[2] + ", " + out_st + ")"});
                break;
            case Kind::StoreW: case Kind::StoreH: case Kind::StoreB: {
                if (a.size() != 3) return keep("args:" + c.name);
                const char* mac = c.kind == Kind::StoreW ? "PSX_B2_STOREW(" :
                                  c.kind == Kind::StoreH ? "PSX_B2_STOREH(" : "PSX_B2_STOREB(";
                edits.push_back({c.name_at, c.close + 1, std::string(mac) + a[1] + ", " + a[2] + ", " +
                                 out_st + ")"});
                break;
            }
            case Kind::CallStmt: {
                size_t semi = c.close + 1;
                while (semi < m.size() && m[semi] == ' ') semi++;
                size_t before = m.find_last_not_of(" \t", c.name_at == 0 ? 0 : c.name_at - 1);
                const bool starts = c.name_at == 0 || before == std::string::npos ||
                                    (before < c.name_at && (m[before] == ';' || m[before] == '{' || m[before] == '}'));
                if (!starts || semi >= m.size() || m[semi] != ';') return keep("call-shape:" + c.name);
                edits.push_back({c.name_at, semi + 1, "{ " + out_st + call + "; " + in_text(i) + "}"});
                resets = true;
                break;
            }
            case Kind::CallIfReturn: {
                size_t ifp = m.rfind("if", c.name_at);
                std::string between = ifp == std::string::npos ? "x" : m.substr(ifp + 2, c.name_at - ifp - 2);
                size_t rp = m.find("return", c.close);
                size_t semi = rp == std::string::npos ? std::string::npos : m.find(';', rp);
                std::string tail = rp == std::string::npos ? "x" : m.substr(c.close + 1, rp - c.close - 1);
                if (between.find_first_not_of(" (") != std::string::npos ||
                    tail.find_first_not_of(" )") != std::string::npos || semi == std::string::npos)
                    return keep("hook-shape:" + c.name);
                edits.push_back({ifp, semi + 1, "{ " + out_st + line.substr(ifp, semi + 1 - ifp) + " " + in_text(i) + "}"});
                resets = true;
                break;
            }
            case Kind::Unknown: return keep("call:" + c.name);
            }
        }
        for (size_t rp : sc.returns) {
            bool inside = false;
            for (const Edit& e : edits) inside = inside || (rp >= e.from && rp < e.to);
            if (inside) continue;
            size_t semi = m.find(';', rp);
            edits.push_back({rp, semi + 1, "{ " + out_st + "return; }"});
        }
        // Nested calls (an Ignore inside an OutOnly's arguments) never both edit;
        // two edits that overlap mean a shape the pass does not know.
        std::sort(edits.begin(), edits.end(), [](const Edit& x, const Edit& y) { return x.from < y.from; });
        for (size_t k = 1; k < edits.size(); k++)
            if (edits[k].from < edits[k - 1].to) return keep("overlap");
        for (size_t k = edits.size(); k-- > 0;)
            line.replace(edits[k].from, edits[k].to - edits[k].from, edits[k].text);
        if (i + 2 == lines.size() && masked[i].find_first_not_of(" ;\t\r\n") == std::string::npos) {
            // The `;` after the last label: falling off the end stores what may differ.
            out_text += "    { " + set_text(written_any, "PSX_B2_ST") + "}\n";
        }
        out_text += line;
        // After IN every local equals cpu->gpr again (unless the line also
        // writes a register: then keep the set, it may be after the call).
        if (resets && !sc.writes) cur = 0;
    }
    stats.localized++;
    return out_text;
}
