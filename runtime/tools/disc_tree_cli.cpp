/*
 * psx-disc-tree — command-line front end of the disc-tree engine
 * (runtime/src/disc_tree.cpp, docs/DISC_TREE.md).
 *
 *   psx-disc-tree layout <tree> [--game-toml game.toml]
 *       print the synthesized layout (extents, tracks, notes)
 *   psx-disc-tree verify <tree> <original.cue|.bin|.chd> [--game-toml ...]
 *       compare every sector of the synthesized disc with the source image
 *       (byte-for-byte proof that a tree round-trips); exit 0 when identical
 *   psx-disc-tree build <tree> <out.cue> [--game-toml ...] [--split]
 *       write a bin/cue image of the tree (single .bin, or one per track with
 *       --split, redump style) — for emulators / other players
 *   psx-disc-tree md5 <tree> [--game-toml ...]
 *       md5 of the synthesized data track (compare with the dump's known md5)
 *
 * --game-toml applies the title's [disc_tree] hints (boot EXE name, hardcoded
 * LBA tables) exactly as the runtime does, so a modified tree builds the same
 * image the game will see.
 */

#include "disc_tree.h"
#include "iso_reader.h"
#include "toml.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/* Minimal MD5 (RFC 1321) — the tool has no other digest dependency. */
struct Md5 {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    uint64_t len = 0;
    uint8_t buf[64];
    size_t used = 0;
    static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
        static const int S[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,
                                  5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,
                                  6,10,15,21,6,10,15,21};
        uint32_t M[16];
        for (int i = 0; i < 16; i++)
            M[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) | ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);       g = (7*i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B; B = B + rotl(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            const size_t take = std::min(n, 64 - used);
            std::memcpy(buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (used != 56) update(&z, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (8 * i));
        update(l, 8);
        char out[33];
        const uint32_t v[4] = {a0, b0, c0, d0};
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) std::snprintf(out + i*8 + k*2, 3, "%02x", (v[i] >> (8*k)) & 0xff);
        out[32] = 0;
        return out;
    }
};

bool load_hints(const std::string& game_toml, PS1::DiscTreeHints& h) {
    try {
        const toml::value cfg = toml::parse(game_toml);
        if (cfg.contains("game")) {
            const auto& g = toml::find(cfg, "game");
            if (g.contains("exe")) h.exe_name = fs::path(toml::find<std::string>(g, "exe")).filename().string();
        }
        if (cfg.contains("disc_tree")) {
            const auto& dt = toml::find(cfg, "disc_tree");
            if (dt.contains("lba_table")) {
                for (const toml::value& v : toml::find(dt, "lba_table").as_array()) {
                    PS1::DiscTreeLbaTable t;
                    t.address = (uint32_t)std::stoul(toml::find<std::string>(v, "address"), nullptr, 0);
                    t.count = (uint32_t)toml::find<int64_t>(v, "count");
                    t.stride = (uint32_t)toml::find<int64_t>(v, "stride");
                    if (v.contains("lba_offset")) t.lba_offset = (int32_t)toml::find<int64_t>(v, "lba_offset");
                    if (v.contains("size_offset")) t.size_offset = (int32_t)toml::find<int64_t>(v, "size_offset");
                    if (v.contains("lba_is_msf")) t.lba_is_msf = toml::find<bool>(v, "lba_is_msf");
                    h.lba_tables.push_back(t);
                }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", game_toml.c_str(), e.what());
        return false;
    }
    return true;
}

void msf_str(uint32_t frames, char out[16]) {
    std::snprintf(out, 16, "%02u:%02u:%02u", frames / 4500, (frames / 75) % 60, frames % 75);
}

int usage() {
    std::fprintf(stderr,
        "usage: psx-disc-tree layout <tree> [--game-toml game.toml]\n"
        "       psx-disc-tree verify <tree> <original.cue|.bin|.chd> [--game-toml ...]\n"
        "       psx-disc-tree build  <tree> <out.cue> [--game-toml ...] [--split]\n"
        "       psx-disc-tree md5    <tree> [--game-toml ...]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    std::vector<std::string> pos;
    std::string game_toml;
    bool split = false;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--game-toml" && i + 1 < argc) game_toml = argv[++i];
        else if (a == "--split") split = true;
        else pos.push_back(a);
    }
    if (pos.empty()) return usage();

    PS1::DiscTreeHints hints;
    if (!game_toml.empty() && !load_hints(game_toml, hints)) return 2;

    PS1::DiscTree tree;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!tree.Open(pos[0], &hints, &err)) {
        std::fprintf(stderr, "psx-disc-tree: %s\n", err.c_str());
        return 1;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double open_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (cmd == "layout") {
        std::fputs(tree.DescribeLayout().c_str(), stdout);
        std::printf("  (mounted in %.1f ms)\n", open_ms);
        return 0;
    }

    if (cmd == "md5") {
        Md5 h;
        std::vector<uint8_t> sec(2352);
        for (uint32_t l = 0; l < tree.DataTrackSectors(); l++) {
            if (!tree.ReadRaw(l, sec.data())) { std::fprintf(stderr, "read failed at LBA %u\n", l); return 1; }
            h.update(sec.data(), sec.size());
        }
        std::printf("%s  data track (%u sectors)\n", h.hex().c_str(), tree.DataTrackSectors());
        return 0;
    }

    if (cmd == "verify") {
        if (pos.size() < 2) return usage();
        PS1::ISOReader orig;
        if (!orig.Open(pos[1])) {
            std::fprintf(stderr, "psx-disc-tree: cannot mount %s\n", pos[1].c_str());
            return 1;
        }
        int bad = 0;
        auto fail = [&](const char* fmt, auto... a) { std::printf(fmt, a...); bad++; };
        if (orig.GetSectorCount() != tree.SectorCount())
            fail("sector count differs: tree %u, image %u\n", tree.SectorCount(), orig.GetSectorCount());
        if (orig.TrackCount() != (int)tree.Tracks().size())
            fail("track count differs: tree %zu, image %d\n", tree.Tracks().size(), orig.TrackCount());
        for (const PS1::DiscTreeTrack& t : tree.Tracks()) {
            if (orig.TrackStartLBA(t.number) != t.start_lba || orig.TrackPregapLBA(t.number) != t.pregap_lba ||
                orig.TrackIsAudio(t.number) != t.is_audio)
                fail("track %d differs: tree start %u pregap %u audio %d, image start %u pregap %u audio %d\n",
                     t.number, t.start_lba, t.pregap_lba, (int)t.is_audio, orig.TrackStartLBA(t.number),
                     orig.TrackPregapLBA(t.number), (int)orig.TrackIsAudio(t.number));
        }
        const uint32_t n = std::min(orig.GetSectorCount(), tree.SectorCount());
        std::vector<uint8_t> a(2352), b(2352);
        uint32_t mismatched = 0, first_bad = 0;
        std::string first_desc;
        for (uint32_t l = 0; l < n; l++) {
            if (!tree.ReadRaw(l, a.data())) { fail("tree read failed at LBA %u\n", l); break; }
            if (!orig.ReadRawSector(l, b.data())) { fail("image read failed at LBA %u\n", l); break; }
            if (std::memcmp(a.data(), b.data(), 2352) != 0) {
                if (!mismatched) {
                    first_bad = l;
                    size_t k = 0;
                    while (k < 2352 && a[k] == b[k]) k++;
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "first mismatch at LBA %u byte %zu: tree %02x image %02x", l, k, a[k], b[k]);
                    first_desc = buf;
                }
                mismatched++;
            }
        }
        const auto t2 = std::chrono::steady_clock::now();
        if (mismatched) fail("%u of %u sectors differ; %s\n", mismatched, n, first_desc.c_str());
        std::printf("%s: %u sectors compared in %.1f s (mount %.1f ms)%s\n",
                    bad ? "DIFFERENT" : "IDENTICAL", n,
                    std::chrono::duration<double>(t2 - t1).count(), open_ms,
                    tree.IsPristineLayout() ? "" : " [tree layout is modified]");
        (void)first_bad;
        return bad ? 1 : 0;
    }

    if (cmd == "build") {
        if (pos.size() < 2) return usage();
        fs::path cue = pos[1];
        if (cue.extension() != ".cue") cue += ".cue";
        const std::string stem = cue.stem().string();
        std::ofstream cf(cue);
        if (!cf) { std::fprintf(stderr, "cannot write %s\n", cue.string().c_str()); return 1; }
        std::vector<uint8_t> sec(2352);
        if (!split) {
            const fs::path bin = cue.parent_path() / (stem + ".bin");
            std::ofstream bf(bin, std::ios::binary);
            for (uint32_t l = 0; l < tree.SectorCount(); l++) {
                if (!tree.ReadRaw(l, sec.data())) { std::fprintf(stderr, "read failed at LBA %u\n", l); return 1; }
                bf.write((const char*)sec.data(), 2352);
            }
            cf << "FILE \"" << bin.filename().string() << "\" BINARY\n";
            for (const PS1::DiscTreeTrack& t : tree.Tracks()) {
                char m0[16], m1[16];
                cf << "  TRACK " << (t.number < 10 ? "0" : "") << t.number << (t.is_audio ? " AUDIO\n" : " MODE2/2352\n");
                if (t.pregap_lba != t.start_lba) { msf_str(t.pregap_lba, m0); cf << "    INDEX 00 " << m0 << "\n"; }
                msf_str(t.start_lba, m1);
                cf << "    INDEX 01 " << m1 << "\n";
            }
            std::printf("wrote %s (%u sectors) + %s\n", bin.string().c_str(), tree.SectorCount(), cue.string().c_str());
        } else {
            for (const PS1::DiscTreeTrack& t : tree.Tracks()) {
                char name[64];
                std::snprintf(name, sizeof name, " (Track %d).bin", t.number);
                const fs::path bin = cue.parent_path() / (stem + name);
                std::ofstream bf(bin, std::ios::binary);
                for (uint32_t l = t.pregap_lba; l < t.pregap_lba + t.sectors; l++) {
                    if (!tree.ReadRaw(l, sec.data())) { std::fprintf(stderr, "read failed at LBA %u\n", l); return 1; }
                    bf.write((const char*)sec.data(), 2352);
                }
                char m1[16];
                cf << "FILE \"" << bin.filename().string() << "\" BINARY\n";
                cf << "  TRACK " << (t.number < 10 ? "0" : "") << t.number << (t.is_audio ? " AUDIO\n" : " MODE2/2352\n");
                if (t.pregap_lba != t.start_lba) cf << "    INDEX 00 00:00:00\n";
                msf_str(t.start_lba - t.pregap_lba, m1);
                cf << "    INDEX 01 " << m1 << "\n";
                std::printf("wrote %s (%u sectors)\n", bin.string().c_str(), t.sectors);
            }
            std::printf("wrote %s\n", cue.string().c_str());
        }
        return 0;
    }
    return usage();
}
