/* settings_save_escape_test: save_user_settings escapes quotes and backslashes
 * (the file still parses), leaves no .tmp behind, and the game.toml upsert keeps
 * CRLF and refuses to write over an unreadable path. */
#include "config_loader.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace PSXRecompV4;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main() {
    const auto root = fs::temp_directory_path() /
        ("settings-save-escape-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);

    const auto st = root / "settings.toml";
    UserSettings s;
    s.has_netplay_player_name = true;
    s.netplay_player_name = "a\"b\\c";
    s.has_language = true;
    s.language = "pt";
    check(save_user_settings(st, s), "save ok");
    check(!fs::exists(root / "settings.toml.tmp"), "no tmp left");
    const UserSettings r = load_user_settings(st);
    check(!r.parse_error, "settings parse");
    check(r.netplay_player_name == "a\"b\\c", "name round trip");

    // [video] fullscreen: exclusive (2) was removed and an old bool meant
    // borderless; both read as borderless (1), out of range as windowed.
    const char* const legacy[][2] = {{"2", "1"}, {"true", "1"}, {"1", "1"}, {"0", "0"}, {"7", "0"}};
    for (const auto& l : legacy) {
        { std::ofstream f(st, std::ios::binary); f << "[video]\nfullscreen = " << l[0] << "\n"; }
        const UserSettings v = load_user_settings(st);
        check(v.has_fullscreen && v.fullscreen == std::atoi(l[1]), "legacy fullscreen value");
    }

    const auto gt = root / "game.toml";
    { std::ofstream f(gt, std::ios::binary); f << "[game]\r\nx = 1\r\n"; }
    check(upsert_game_toml_controller_bool(gt, "k", true), "upsert ok");
    std::ostringstream ss;
    { std::ifstream in(gt, std::ios::binary); ss << in.rdbuf(); }   // closed before remove_all
    const std::string t = ss.str();
    check(t.find("\r\n[controller]\r\nk = true\r\n") != std::string::npos, "crlf kept");
    check(t.find("\n") != std::string::npos && t.find("\r\r") == std::string::npos, "no double CR");
    check(!upsert_game_toml_controller_bool(root, "k", true), "directory refused");

    fs::remove_all(root);
    return failures ? 1 : 0;
}
