/* parse_hex_strict_test: parse_hex keeps accepting 0x/0X/bare hex and rejects trailing junk,
 * signs, empty digits and values above 32 bits. Includes the .cpp to reach the static helper. */
#include "../src/config_loader.cpp"
#include <cstdio>

using PSXRecompV4::parse_hex;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
static bool rejects(const char* s) {
    try { parse_hex(s, "t"); } catch (const std::runtime_error&) { return true; }
    return false;
}

int main() {
    check(parse_hex("0x80010000", "t") == 0x80010000u, "0x lower");
    check(parse_hex("0X8001aBcD", "t") == 0x8001ABCDu, "0X mixed case");
    check(parse_hex("8001abcd", "t") == 0x8001ABCDu, "bare");
    check(parse_hex("0xFFFFFFFF", "t") == 0xFFFFFFFFu, "max");
    check(parse_hex("0", "t") == 0u, "zero");
    check(parse_hex(" 0x10 ", "t") == 0x10u, "blanks");
    check(rejects("0x8001zz"), "trailing junk");
    check(rejects("-1"), "negative");
    check(rejects("+1"), "plus");
    check(rejects("0x180010000"), "above 32 bits");
    check(rejects(""), "empty");
    check(rejects("0x"), "prefix only");
    return failures;
}
