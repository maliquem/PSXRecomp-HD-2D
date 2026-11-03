// [recompiler] gpr_locals (docs/GPR_LOCALS.md): the pass that moves a generated
// function's guest registers into C locals.
#pragma once

#include <cstdint>
#include <map>
#include <string>

struct GprLocalsStats {
    uint32_t functions = 0;      // bodies seen
    uint32_t localized = 0;      // bodies rewritten
    uint32_t entry_flushes = 0;  // blocks that store their incoming set on entry
    std::map<std::string, uint32_t> kept;  // bodies left as they were, by reason
};

// body: one generated function body, "{\n ... }\n", as generate_function emits
// it. Returns the rewritten body, or `body` unchanged (counted in stats.kept)
// when it holds anything the pass does not know how to keep exact.
// lean ([recompiler] lean, docs/LEAN_CORE.md): write the PSX_LN_* forms, which
// keep the block's cycles in the function's local _psx_lc, and hand them to
// the batch (PSX_LN_PUB) before everything that can see the clock.
std::string gpr_localize_body(const std::string& body, int flush_max,
                              uint32_t func_addr, GprLocalsStats& stats,
                              bool lean = false);
