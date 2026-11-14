/* disc_requests_config_test — game.toml [[game.disc_requests]].
 *
 *   1. absent means no automatic disc change;
 *   2. a rule parses with its defaults (size 1, full mask);
 *   3. a rule naming no disc of [game] discs, a bad size or a value with bits
 *      outside its mask is refused at load, not silently never matched;
 *   4. the rules stay out of the overlay codegen hash. */
#include "config_loader.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main() {
    const auto root = fs::temp_directory_path() /
        ("disc-requests-config-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto config = root / "game.toml";
    auto load = [&](const std::string& extra) {
        {
            std::ofstream f(config);
            f << "[game]\nname=\"Test\"\nid=\"SLUS-TEST\"\nexe=\"stock.exe\"\n"
                 "load_address=\"0x80010000\"\nentry_pc=\"0x80010000\"\ntext_size=\"0x1000\"\n"
                 "stack_base=\"0x801ffff0\"\ndiscs=[\"a.cue\",\"b.cue\"]\n"
                 "[recompiler]\nseeds=\"seeds.txt\"\n"
              << extra;
        }
        return PSXRecompV4::load_game_config(config);
    };
    auto refused = [&](const std::string& extra) {
        try { (void)load(extra); } catch (const std::exception&) { return true; }
        return false;
    };

    const auto none = load("");
    check(none.disc_requests.empty(), "absent: no rules");

    const auto one = load("[[game.disc_requests]]\ndisc=2\naddr=\"0x80012340\"\nvalue=\"0x7\"\n");
    check(one.disc_requests.size() == 1, "one rule");
    if (one.disc_requests.size() == 1) {
        const auto& r = one.disc_requests[0];
        check(r.disc == 2 && r.addr == 0x80012340u && r.size == 1 &&
              r.mask == 0xFFFFFFFFu && r.value == 7u, "rule fields and defaults");
    }
    const auto two = load("[[game.disc_requests]]\ndisc=2\naddr=\"0x80012340\"\nsize=2\n"
                          "mask=\"0xFF00\"\nvalue=\"0x0200\"\n"
                          "[[game.disc_requests]]\ndisc=1\naddr=\"0x80012344\"\nsize=4\nvalue=\"0x1\"\n");
    check(two.disc_requests.size() == 2 && two.disc_requests[0].size == 2 &&
          two.disc_requests[0].mask == 0xFF00u && two.disc_requests[1].disc == 1,
          "two rules, in order");

    check(refused("[[game.disc_requests]]\ndisc=3\naddr=\"0x80012340\"\nvalue=\"0x1\"\n"),
          "disc outside [game] discs refused");
    check(refused("[[game.disc_requests]]\ndisc=0\naddr=\"0x80012340\"\nvalue=\"0x1\"\n"),
          "disc 0 refused");
    check(refused("[[game.disc_requests]]\ndisc=2\naddr=\"0x80012340\"\nsize=3\nvalue=\"0x1\"\n"),
          "size 3 refused");
    check(refused("[[game.disc_requests]]\ndisc=2\naddr=\"0x80012340\"\nmask=\"0xFF\"\nvalue=\"0x100\"\n"),
          "value outside mask refused");

    check(PSXRecompV4::overlay_codegen_config_hash(none) ==
          PSXRecompV4::overlay_codegen_config_hash(two),
          "rules do not change the overlay codegen hash");

    fs::remove(config);
    fs::remove(root);
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("disc_requests_config_test: ok");
    return 0;
}
