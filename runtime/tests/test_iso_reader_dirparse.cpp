// ISO9660 directory parsing must refuse sizes the image cannot back: a name
// longer than its record, and a directory size that wraps or is absurd.
// Well-formed images read exactly as before.

#include "iso_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static const uint32_t kSectors = 20;
static const uint32_t kRootLba = 19;  // last sector: an overrun runs off the image buffer

static void put733(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) { p[i] = (uint8_t)(v >> (8 * i)); p[7 - i] = (uint8_t)(v >> (8 * i)); }
}

static void add_record(std::vector<uint8_t>& dir, uint8_t record_len, uint32_t lba, uint32_t size,
                       uint8_t flags, const std::string& name, uint8_t name_len) {
    std::vector<uint8_t> r(record_len, 0);
    r[0] = record_len;
    put733(&r[2], lba);
    put733(&r[10], size);
    r[25] = flags;
    r[32] = name_len;
    for (size_t i = 0; i < name.size() && 33 + i < r.size(); ++i) r[33 + i] = (uint8_t)name[i];
    dir.insert(dir.end(), r.begin(), r.end());
}

static std::string make_image(const fs::path& path, const std::vector<uint8_t>& dir, uint32_t root_size) {
    std::vector<uint8_t> img(kSectors * 2048, 0);
    uint8_t* pvd = &img[16 * 2048];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    put733(pvd + 156 + 2, kRootLba);
    put733(pvd + 156 + 10, root_size);
    std::memcpy(&img[kRootLba * 2048], dir.data(), dir.size());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(img.data()), (std::streamsize)img.size());
    return path.string();
}

static std::vector<PS1::ISOFileEntry> list_root(const std::string& image) {
    PS1::ISOReader r;
    if (!r.Open(image)) { CHECK(false); return {}; }
    return r.ListFiles("");
}

int main() {
    const fs::path d = fs::temp_directory_path() / "psxrecomp-iso-dirparse-test";
    fs::remove_all(d);
    fs::create_directories(d);

    std::vector<uint8_t> good;
    add_record(good, 34, kRootLba, 2048, 2, std::string(1, '\0'), 1);
    add_record(good, 34, kRootLba, 2048, 2, std::string(1, '\1'), 1);
    add_record(good, 40, 30, 123, 0, "A.BIN;1", 7);
    add_record(good, 36, 31, 2048, 2, "SUB", 3);

    // Well-formed: unchanged behavior.
    {
        auto e = list_root(make_image(d / "good.iso", good, 2048));
        CHECK(e.size() == 2);
        if (e.size() == 2) {
            CHECK(e[0].name == "A.BIN" && e[0].lba == 30 && e[0].size == 123 && !e[0].is_directory);
            CHECK(e[1].name == "SUB" && e[1].lba == 31 && e[1].is_directory);
        }
    }

    // Name length runs past its record (and past the end of the image): dropped.
    {
        std::vector<uint8_t> bad;
        add_record(bad, 40, 30, 123, 0, "A.BIN;1", 7);
        bad.resize(2048 - 40, 0);  // zero padding; last record sits at the very end of the sector
        add_record(bad, 40, 32, 5, 0, "", 255);
        auto e = list_root(make_image(d / "longname.iso", bad, 2048));
        CHECK(e.size() == 1 && e[0].name == "A.BIN");
    }

    // Directory size that wraps (dir_size + 2047) or is absurd: refused, no huge allocation.
    CHECK(list_root(make_image(d / "wrap.iso", good, 0xFFFFFFFFu)).empty());
    CHECK(list_root(make_image(d / "huge.iso", good, 0x7FFFFFFFu)).empty());

    fs::remove_all(d);
    if (g_failures) {
        std::printf("iso_reader_dirparse_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("iso_reader_dirparse_test: PASS\n");
    return 0;
}
