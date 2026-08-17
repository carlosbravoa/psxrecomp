/*
 * test_disc_tree.cpp — the disc-tree engine (runtime/src/disc_tree.cpp).
 *
 *  1. Sector protection: EDC vectors + a P/Q parity fingerprint. The
 *     fingerprint bytes were produced by the reference implementation that
 *     reproduces all 142,096 sectors of a retail MODE2/2352 dump byte for
 *     byte (psx-disc-tree verify), so this pins that behaviour.
 *  2. A synthetic tree (manifest + files written by the test) mounted through
 *     PS1::ISOReader: TOC, PVD patching, directory records/path tables (walked
 *     back with the reader's own ISO9660 parser), Form 1 payload + EDC, raw
 *     2336-byte sectors, empty sectors, CD-DA pregap + PCM, sector counts.
 *  3. Modification: a file grown past its extent is relocated after the
 *     original data area, the data track and TOC grow, a new file/directory
 *     appear in the (regenerated) directory + path tables, and the game's
 *     hardcoded LBA table inside a PS-X EXE is rewritten (LBA + size) and
 *     reported through RamPatches().
 */

#include "disc_tree.h"
#include "iso_reader.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void write_bytes(const fs::path& p, const std::vector<uint8_t>& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)b.data(), (std::streamsize)b.size());
    assert(f.good());
}
static void write_text(const fs::path& p, const std::string& s) {
    write_bytes(p, std::vector<uint8_t>(s.begin(), s.end()));
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ─── 1. protection ─────────────────────────────────────────────────────── */

static void test_ecc() {
    /* Mode 2 Form 1, zero payload, subheader 00 00 08 00 (what a mastering
     * tool writes for the licence-area filler): EDC observed on retail discs. */
    uint8_t s[2352];
    std::memset(s, 0, sizeof s);
    s[16] = 0; s[17] = 0; s[18] = 0x08; s[19] = 0;
    std::memcpy(s + 20, s + 16, 4);
    assert(PS1::cd_edc_compute(s + 16, 2056) == 0x9481880Bu);
    /* all-zero subheader + payload -> EDC 0 and ECC 0 (empty sector) */
    std::memset(s, 0, sizeof s);
    assert(PS1::cd_edc_compute(s + 16, 2056) == 0);
    PS1::cd_ecc_generate_mode2_form1(s);
    for (int i = 2076; i < 2352; i++) assert(s[i] == 0);
    /* deterministic payload fingerprint (see file header) */
    uint32_t x = 12345;
    std::memset(s, 0, sizeof s);
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    std::memcpy(s, sync, 12);
    s[12] = 0x00; s[13] = 0x02; s[14] = 0x16; s[15] = 0x02;
    s[16] = 0; s[17] = 0; s[18] = 8; s[19] = 0; std::memcpy(s + 20, s + 16, 4);
    for (int i = 0; i < 2048; i++) { x = x * 1103515245u + 12345u; s[24 + i] = (uint8_t)(x >> 16); }
    const uint32_t edc = PS1::cd_edc_compute(s + 16, 2056);
    assert(edc == 0x77191A6Du);
    wr32(s + 2072, edc);
    PS1::cd_ecc_generate_mode2_form1(s);
    static const uint8_t p0[8] = {0x82, 0x13, 0x2e, 0xc6, 0xaf, 0x31, 0xe8, 0x95};
    static const uint8_t q0[8] = {0x39, 0xb5, 0xff, 0x85, 0x80, 0xde, 0x95, 0xb9};
    static const uint8_t qe[8] = {0x58, 0x08, 0x95, 0xa6, 0x9a, 0x20, 0xdd, 0x4b};
    assert(std::memcmp(s + 2076, p0, 8) == 0);
    assert(std::memcmp(s + 2248, q0, 8) == 0);
    assert(std::memcmp(s + 2344, qe, 8) == 0);
    /* MSF */
    uint8_t m[3];
    PS1::cd_lba_to_msf_bcd(0, m);      assert(m[0] == 0x00 && m[1] == 0x02 && m[2] == 0x00);
    PS1::cd_lba_to_msf_bcd(142095, m); assert(m[0] == 0x31 && m[1] == 0x36 && m[2] == 0x45);
    std::puts("ecc: ok");
}

/* ─── 2. synthetic tree ─────────────────────────────────────────────────── */

struct Fixture {
    fs::path root;
    std::vector<uint8_t> exe;      /* PS-X EXE with a 3-entry LBA table at 0x80010100 */
    std::vector<uint8_t> a_bin;    /* cdrom/DIR/A.BIN, Form 1, 3000 bytes */
    std::vector<uint8_t> b_str;    /* cdrom/DIR/B.STR, raw, 2 sectors of 2336 */
    std::vector<uint8_t> pcm;      /* audio, 3 sectors */
};

static Fixture make_tree(const fs::path& root) {
    Fixture fx;
    fx.root = root;
    fs::remove_all(root);
    fs::create_directories(root / "cdrom" / "DIR");
    fs::create_directories(root / "meta");
    fs::create_directories(root / "audio");

    /* system area: 16 raw sectors, marker bytes */
    std::vector<uint8_t> sysa(16 * 2352, 0x5A);
    write_bytes(root / "meta" / "system_area.bin", sysa);
    /* PVD: minimal; the engine patches the geometry fields */
    std::vector<uint8_t> pvd(2048, 0);
    pvd[0] = 1; std::memcpy(&pvd[1], "CD001", 5); pvd[6] = 1;
    std::memcpy(&pvd[8], "PLAYSTATION                     ", 32);
    std::memcpy(&pvd[40], "TREETEST                        ", 32);
    write_bytes(root / "meta" / "pvd.bin", pvd);

    /* boot EXE: header + 0x1000 bytes, table at RAM 0x80010100 (load 0x80010000):
     * {lba,size,x}: A.BIN@40 3000, B.STR@42 4672 (2 x 2336), TRACK@? */
    fx.exe.assign(0x800 + 0x1000, 0);
    std::memcpy(fx.exe.data(), "PS-X EXE", 8);
    wr32(&fx.exe[0x18], 0x80010000);
    wr32(&fx.exe[0x1C], 0x1000);
    uint8_t* t = &fx.exe[0x800 + 0x100];
    wr32(t + 0, 40); wr32(t + 4, 3000); wr32(t + 8, 1);
    wr32(t + 12, 42); wr32(t + 16, 2 * 2336); wr32(t + 20, 2);
    wr32(t + 24, 100 + 150); wr32(t + 28, 3 * 2048); wr32(t + 32, 3);   /* CD-DA alias */
    write_bytes(root / "cdrom" / "MAIN.EXE", fx.exe);
    write_text(root / "cdrom" / "SYSTEM.CNF", "BOOT = cdrom:\\MAIN.EXE;1\r\nTCB = 4\r\n");

    fx.a_bin.resize(3000);
    for (size_t i = 0; i < fx.a_bin.size(); i++) fx.a_bin[i] = (uint8_t)(i * 7 + 3);
    write_bytes(root / "cdrom" / "DIR" / "A.BIN", fx.a_bin);
    fx.b_str.resize(2 * 2336);
    for (size_t i = 0; i < fx.b_str.size(); i++) fx.b_str[i] = (uint8_t)(i * 13 + 1);
    /* raw sectors carry their own subheader: file 1, channel 1, form 2 video */
    fx.b_str[0] = 1; fx.b_str[1] = 1; fx.b_str[2] = 0x64; fx.b_str[3] = 0x01;
    std::memcpy(&fx.b_str[4], &fx.b_str[0], 4);
    write_bytes(root / "cdrom" / "DIR" / "B.STR", fx.b_str);

    /* audio: 3 sectors of PCM as a canonical WAV */
    fx.pcm.resize(3 * 2352);
    for (size_t i = 0; i < fx.pcm.size(); i++) fx.pcm[i] = (uint8_t)(i * 3 + 9);
    std::vector<uint8_t> wav;
    auto put = [&](const char* s) { wav.insert(wav.end(), s, s + 4); };
    auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) wav.push_back((uint8_t)(v >> (8 * i))); };
    auto p16 = [&](uint16_t v) { wav.push_back((uint8_t)v); wav.push_back((uint8_t)(v >> 8)); };
    put("RIFF"); p32(36 + (uint32_t)fx.pcm.size()); put("WAVE");
    put("fmt "); p32(16); p16(1); p16(2); p32(44100); p32(44100 * 4); p16(4); p16(16);
    put("data"); p32((uint32_t)fx.pcm.size());
    wav.insert(wav.end(), fx.pcm.begin(), fx.pcm.end());
    write_bytes(root / "audio" / "track02.wav", wav);

    /* manifest: data track 100 sectors (postgap 10), root@22, DIR@30,
     * MAIN.EXE@23 (3 sectors), SYSTEM.CNF@26, A.BIN@40 (2 sectors), B.STR@42 (2) */
    write_text(root / "disc.toml",
        "format = \"psxrecomp-disc-tree\"\nformat_version = 1\n"
        "[source]\ncue = \"synthetic.cue\"\nvolume_id = \"TREETEST\"\ndata_track_sectors = 100\n"
        "[layout]\nsystem_area = \"meta/system_area.bin\"\ndescriptors = [\"meta/pvd.bin\"]\n"
        "descriptor_lba = 16\nterminator_lba = 17\npath_table_l = [18, 19]\npath_table_m = [20, 21]\n"
        "postgap_sectors = 10\n"
        "[[track]]\nnumber = 1\ntype = \"data\"\nsectors = 100\n"
        "[[track]]\nnumber = 2\ntype = \"audio\"\nfile = \"audio/track02.wav\"\npregap_sectors = 150\nsectors = 153\n"
        "[[dir]]\npath = \"\"\nlba = 22\nsectors = 1\ndate = \"6101090e2f3b24\"\n"
        "xa = \"000000008d555841000000000000\"\nentries = [\"DIR\", \"MAIN.EXE\", \"SYSTEM.CNF\", \"TRACK.DA\"]\n"
        "[[dir]]\npath = \"DIR\"\nlba = 30\nsectors = 1\ndate = \"6101090e2f3b24\"\n"
        "xa = \"000000008d555841000000000000\"\nentries = [\"A.BIN\", \"B.STR\"]\n"
        "[[file]]\npath = \"MAIN.EXE\"\nlba = 23\nsize = 6144\nform = \"1\"\ndate = \"6101090e151424\"\n"
        "xa = \"000000000d555841000000000000\"\n"
        "[[file]]\npath = \"SYSTEM.CNF\"\nlba = 26\nsize = 35\nform = \"1\"\ndate = \"6101090e151424\"\n"
        "xa = \"000000000d555841000000000000\"\n"
        "[[file]]\npath = \"TRACK.DA\"\nlba = 250\nsize = 6144\nform = \"cdda\"\ntrack = 2\ndate = \"6101090e151424\"\n"
        "xa = \"0000000045555841000000000000\"\n"
        "[[file]]\npath = \"DIR/A.BIN\"\nlba = 40\nsize = 3000\nform = \"1\"\ndate = \"6101090e151424\"\n"
        "xa = \"000000000d555841000000000000\"\n"
        "[[file]]\npath = \"DIR/B.STR\"\nlba = 42\nsize = 4096\nform = \"raw\"\ndate = \"6101090e151424\"\n"
        "xa = \"0000000025555841010000000000\"\n");
    return fx;
}

static PS1::DiscTreeHints hints() {
    PS1::DiscTreeHints h;
    h.exe_name = "MAIN.EXE";
    PS1::DiscTreeLbaTable t;
    t.address = 0x80010100; t.count = 3; t.stride = 12; t.lba_offset = 0; t.size_offset = 4;
    h.lba_tables.push_back(t);
    return h;
}

static void check_form1_sector(const uint8_t* s, uint32_t lba, uint8_t submode) {
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    assert(std::memcmp(s, sync, 12) == 0);
    uint8_t m[3]; PS1::cd_lba_to_msf_bcd(lba, m);
    assert(std::memcmp(s + 12, m, 3) == 0 && s[15] == 2);
    assert(s[16] == 0 && s[17] == 0 && s[18] == submode && s[19] == 0);
    assert(std::memcmp(s + 16, s + 20, 4) == 0);
    assert(rd32(s + 2072) == PS1::cd_edc_compute(s + 16, 2056));
    uint8_t copy[2352]; std::memcpy(copy, s, 2352);
    PS1::cd_ecc_generate_mode2_form1(copy);
    assert(std::memcmp(copy + 2076, s + 2076, 276) == 0);
}

static void test_pristine(const Fixture& fx) {
    PS1::ISOReader::SetDiscTreeHints(hints());
    PS1::ISOReader r;
    assert(r.Open(fx.root.string()));
    for (const std::string& n : PS1::ISOReader::LastDiscTreeMount().notes) std::printf("  note: %s\n", n.c_str());
    assert(r.Tree() && r.Tree()->IsPristineLayout());
    assert(r.GetSectorCount() == 100 + 153);
    assert(r.TrackCount() == 2);
    assert(!r.TrackIsAudio(1) && r.TrackStartLBA(1) == 0);
    assert(r.TrackIsAudio(2) && r.TrackPregapLBA(2) == 100 && r.TrackStartLBA(2) == 250);
    assert(r.GetVolumeID() == "TREETEST");
    assert(PS1::ISOReader::LastDiscTreeMount().mounted);
    assert(PS1::ISOReader::LastDiscTreeMount().ram_patches.empty());

    uint8_t s[2352], u[2048];
    /* system area verbatim */
    assert(r.ReadRawSector(0, s) && s[0] == 0x5A && s[2351] == 0x5A);
    /* PVD: patched geometry, volume id kept, terminator follows */
    assert(r.ReadSector(16, u));
    assert(u[0] == 1 && std::memcmp(&u[1], "CD001", 5) == 0);
    assert(rd32(&u[80]) == 253);                       /* volume space = whole disc */
    assert(rd32(&u[140]) == 18 && rd32(&u[144]) == 19);
    assert(u[156] == 34 && rd32(&u[158]) == 22 && rd32(&u[166]) == 2048);
    assert(r.ReadRawSector(16, s)); check_form1_sector(s, 16, 0x09);
    assert(r.ReadSector(17, u) && u[0] == 0xFF && std::memcmp(&u[1], "CD001", 5) == 0);
    assert(r.ReadRawSector(17, s)); check_form1_sector(s, 17, 0x89);
    /* path table: root + DIR, L little-endian / M big-endian */
    assert(r.ReadSector(18, u));
    assert(u[0] == 1 && u[1] == 0 && rd32(&u[2]) == 22 && u[6] == 1 && u[8] == 0);
    assert(u[10] == 3 && rd32(&u[12]) == 30 && u[16] == 1 && std::memcmp(&u[18], "DIR", 3) == 0);
    assert(r.ReadSector(20, u));
    assert(u[0] == 1 && u[5] == 22 && u[7] == 1);
    /* the reader's own ISO9660 walker sees the synthesized directories */
    PS1::ISOFileEntry e;
    assert(r.FindFile("MAIN.EXE", e) && e.lba == 23 && e.size == 6144);
    assert(r.FindFile("SYSTEM.CNF", e) && e.lba == 26 && e.size == 35);
    assert(r.FindFile("TRACK.DA", e) && e.lba == 250 && e.size == 3 * 2048);
    assert(r.FindFile("DIR/A.BIN", e) && e.lba == 40 && e.size == 3000);
    assert(r.FindFile("DIR/B.STR", e) && e.lba == 42 && e.size == 4096);
    {
        auto root = r.ListFiles("");
        assert(root.size() == 4);
        assert(root[0].name == "DIR" && root[0].is_directory && root[0].lba == 30);
        assert(root[1].name == "MAIN.EXE");
    }
    /* Form 1 payload + protection; last sector carries EOR|EOF */
    std::vector<uint8_t> got(2 * 2048);
    assert(r.ReadSector(40, got.data()) && r.ReadSector(41, got.data() + 2048));
    assert(std::memcmp(got.data(), fx.a_bin.data(), 3000) == 0);
    for (size_t i = 3000; i < 4096; i++) assert(got[i] == 0);
    assert(r.ReadRawSector(40, s)); check_form1_sector(s, 40, 0x08);
    assert(r.ReadRawSector(41, s)); check_form1_sector(s, 41, 0x89);
    /* raw file: subheader + body verbatim behind a synthesized sync/header */
    assert(r.ReadRawSector(42, s));
    assert(s[15] == 2 && std::memcmp(s + 16, fx.b_str.data(), 2336) == 0);
    assert(r.ReadRawSector(43, s) && std::memcmp(s + 16, fx.b_str.data() + 2336, 2336) == 0);
    /* EXE served unpatched (pristine) */
    std::vector<uint8_t> exe(3 * 2048);
    for (int i = 0; i < 3; i++) assert(r.ReadSector(23 + i, exe.data() + i * 2048));
    assert(std::memcmp(exe.data(), fx.exe.data(), fx.exe.size()) == 0);
    /* empty sector inside the data track */
    assert(r.ReadRawSector(60, s));
    assert(s[15] == 2); for (int i = 16; i < 2352; i++) assert(s[i] == 0);
    assert(r.ReadRawSector(99, s));                     /* postgap */
    for (int i = 16; i < 2352; i++) assert(s[i] == 0);
    /* audio: pregap silence, then the PCM */
    assert(r.ReadRawSector(100, s)); for (int i = 0; i < 2352; i++) assert(s[i] == 0);
    assert(r.ReadRawSector(250, s) && std::memcmp(s, fx.pcm.data(), 2352) == 0);
    assert(r.ReadRawSector(252, s) && std::memcmp(s, fx.pcm.data() + 2 * 2352, 2352) == 0);
    assert(!r.ReadRawSector(253, s));
    r.Close();
    std::puts("pristine tree: ok");
}

static void test_modified(Fixture& fx) {
    /* grow A.BIN to 5000 bytes (3 sectors > its 2-sector extent), add a file
     * and a directory, then remount */
    fx.a_bin.resize(5000, 0xAB);
    write_bytes(fx.root / "cdrom" / "DIR" / "A.BIN", fx.a_bin);
    write_text(fx.root / "cdrom" / "NEW.TXT", "new file");
    write_text(fx.root / "cdrom" / "SUB" / "DEEP.TXT", "deep");

    PS1::ISOReader r;
    assert(r.Open(fx.root.string()));
    const PS1::DiscTree* t = r.Tree();
    assert(t && !t->IsPristineLayout());
    /* append region begins after the original data area minus its postgap (90) */
    PS1::ISOFileEntry e;
    assert(r.FindFile("DIR/A.BIN", e) && e.lba == 90 && e.size == 5000);
    assert(r.FindFile("DIR/B.STR", e) && e.lba == 42);            /* untouched */
    assert(r.FindFile("NEW.TXT", e) && e.size == 8 && e.lba > 90);
    assert(r.FindFile("SUB/DEEP.TXT", e) && e.size == 4);
    const uint32_t data_sectors = t->DataTrackSectors();
    assert(data_sectors > 100 && data_sectors == e.lba + 1 + 10);  /* last object + postgap */
    assert(r.TrackPregapLBA(2) == data_sectors && r.TrackStartLBA(2) == data_sectors + 150);
    assert(r.GetSectorCount() == data_sectors + 153);
    /* CD-DA alias follows the moved track */
    assert(r.FindFile("TRACK.DA", e) && e.lba == data_sectors + 150);
    /* directory listing: manifest order first, then additions in ISO order */
    auto root = r.ListFiles("");
    assert(root.size() == 6);
    assert(root[0].name == "DIR" && root[1].name == "MAIN.EXE" && root[2].name == "SYSTEM.CNF" &&
           root[3].name == "TRACK.DA" && root[4].name == "NEW.TXT" && root[5].name == "SUB");
    /* path table now has three directories, SUB after DIR */
    uint8_t u[2048];
    assert(r.ReadSector(18, u));
    assert(std::memcmp(&u[18], "DIR", 3) == 0);
    assert(u[22] == 3 && std::memcmp(&u[30], "SUB", 3) == 0 && u[28] == 1);
    assert(r.ReadSector(16, u) && rd32(&u[80]) == r.GetSectorCount());
    /* the served EXE carries the rewritten table: A.BIN moved + resized,
     * TRACK.DA moved, B.STR untouched */
    std::vector<uint8_t> exe(3 * 2048);
    for (int i = 0; i < 3; i++) assert(r.ReadSector(23 + i, exe.data() + i * 2048));
    const uint8_t* tab = &exe[0x800 + 0x100];
    assert(rd32(tab + 0) == 90 && rd32(tab + 4) == 5000 && rd32(tab + 8) == 1);
    assert(rd32(tab + 12) == 42 && rd32(tab + 16) == 2 * 2336);
    assert(rd32(tab + 24) == data_sectors + 150 && rd32(tab + 28) == 3 * 2048);
    /* and reports the RAM patches for the text-image guard */
    const auto& info = PS1::ISOReader::LastDiscTreeMount();
    assert(info.mounted && !info.pristine_layout);
    bool saw_lba = false, saw_size = false, saw_cdda = false;
    for (const PS1::DiscTreeRamPatch& p : info.ram_patches) {
        if (p.address == 0x80010100 && p.bytes.size() == 4 && rd32(p.bytes.data()) == 90) saw_lba = true;
        if (p.address == 0x80010104 && p.bytes.size() == 4 && rd32(p.bytes.data()) == 5000) saw_size = true;
        if (p.address == 0x80010118 && rd32(p.bytes.data()) == data_sectors + 150) saw_cdda = true;
    }
    assert(saw_lba && saw_size && saw_cdda);
    /* payload of the relocated file, EOF marker on its last sector */
    uint8_t s[2352];
    std::vector<uint8_t> got(3 * 2048);
    for (int i = 0; i < 3; i++) assert(r.ReadSector(90 + i, got.data() + i * 2048));
    assert(std::memcmp(got.data(), fx.a_bin.data(), 5000) == 0);
    assert(r.ReadRawSector(92, s)); check_form1_sector(s, 92, 0x89);
    /* the old extent is empty now */
    assert(r.ReadRawSector(40, s)); for (int i = 16; i < 2352; i++) assert(s[i] == 0);
    r.Close();
    std::puts("modified tree: ok");
}

int main() {
    test_ecc();
    const fs::path root = fs::temp_directory_path() / "psxrecomp-disc-tree-test";
    Fixture fx = make_tree(root);
    /* not a tree without disc.toml */
    assert(!PS1::DiscTree::IsTree(root / "cdrom"));
    assert(PS1::DiscTree::IsTree(root));
    test_pristine(fx);
    test_modified(fx);
    fs::remove_all(root);
    std::puts("disc_tree_test: all ok");
    return 0;
}
