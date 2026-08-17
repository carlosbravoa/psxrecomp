/*
 * disc_tree.cpp — see disc_tree.h and docs/DISC_TREE.md.
 *
 * Structure:
 *   1. sector protection (EDC / ECC) and MSF helpers
 *   2. manifest model + toml loading
 *   3. host tree scan (new / missing / modified files)
 *   4. layout: fixed extents for everything that still fits, an append region
 *      after the original data area for what grew, postgap, audio tracks
 *   5. ISO9660 metadata synthesis: directory records, path tables, PVD patch
 *   6. game-side LBA table patch of the boot EXE
 *   7. sector service (ReadRaw)
 *
 * Nothing here is game specific: the only per-title knowledge is the optional
 * DiscTreeHints (boot EXE name + LBA table geometry) that game.toml provides.
 */

#include "disc_tree.h"

#include "toml.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace PS1 {

/* ───────────────────────── 1. sector protection ───────────────────────── */

namespace {

constexpr uint32_t RAW_SEC = 2352;
constexpr uint32_t USER_SEC = 2048;
constexpr uint32_t F2_SEC = 2336;   /* subheader + 2324 data + EDC */
constexpr uint8_t  SYNC[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

struct EccTables {
    uint32_t edc[256];
    uint8_t  f[256];
    uint8_t  b[256];
    EccTables() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t e = i;
            for (int k = 0; k < 8; k++) e = (e >> 1) ^ ((e & 1) ? 0xD8018001u : 0u);
            edc[i] = e;
            uint32_t j = (i << 1) ^ ((i & 0x80) ? 0x11Du : 0u);
            j &= 0xFF;
            f[i] = (uint8_t)j;
            b[i ^ j] = (uint8_t)i;
        }
    }
};

const EccTables& tables() {
    static EccTables t;
    return t;
}

void ecc_part(const uint8_t* src, uint32_t major_count, uint32_t minor_count,
              uint32_t major_mult, uint32_t minor_inc, uint8_t* dest) {
    const EccTables& t = tables();
    const uint32_t size = major_count * minor_count;
    for (uint32_t major = 0; major < major_count; major++) {
        uint32_t index = (major >> 1) * major_mult + (major & 1);
        uint8_t a = 0, b = 0;
        for (uint32_t minor = 0; minor < minor_count; minor++) {
            const uint8_t v = src[index];
            index += minor_inc;
            if (index >= size) index -= size;
            a ^= v;
            b ^= v;
            a = t.f[a];
        }
        a = t.b[t.f[a] ^ b];
        dest[major] = a;
        dest[major + major_count] = (uint8_t)(a ^ b);
    }
}

uint8_t bcd8(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

} // namespace

uint32_t cd_edc_compute(const uint8_t* data, size_t len) {
    const EccTables& t = tables();
    uint32_t e = 0;
    for (size_t i = 0; i < len; i++) e = t.edc[(e ^ data[i]) & 0xFF] ^ (e >> 8);
    return e;
}

void cd_ecc_generate_mode2_form1(uint8_t* s) {
    /* XA: parity is computed with the header (bytes 12..15) treated as zero. */
    uint8_t src[2236];
    std::memcpy(src, s + 12, 2064);      /* header + subheader + data + EDC */
    std::memset(src, 0, 4);
    ecc_part(src, 86, 24, 2, 86, s + 2076);          /* P parity  */
    std::memcpy(src + 2064, s + 2076, 172);          /* + P parity */
    ecc_part(src, 52, 43, 86, 88, s + 2248);         /* Q parity  */
}

void cd_lba_to_msf_bcd(uint32_t lba, uint8_t out[3]) {
    const uint32_t a = lba + 150;
    out[0] = bcd8(a / 4500);
    out[1] = bcd8((a / 75) % 60);
    out[2] = bcd8(a % 75);
}

/* ───────────────────────── 2. manifest model ──────────────────────────── */

namespace {

enum Form { FORM1 = 1, FORM_RAW = 2, FORM_CDDA = 3 };

struct Entry {
    bool is_dir = false;
    std::string path;              /* ISO path, '/'-separated, "" = root      */
    std::string name;              /* last component                          */
    std::string parent;            /* ISO path of the parent ("" = root)      */
    int  form = FORM1;
    int  track = 0;                /* FORM_CDDA                               */
    int  version = 1;
    uint8_t date[7] = {0};
    std::vector<uint8_t> sua;
    /* pristine */
    bool     has_orig = false;
    uint32_t orig_lba = 0;
    uint32_t orig_size = 0;        /* record size on the source disc          */
    uint32_t orig_sectors = 0;
    std::vector<std::string> order;/* dir: record order from the manifest     */
    /* host */
    fs::path host;
    uint64_t host_size = 0;
    bool     raw_has_sync = false; /* raw file stored as 2352-byte sectors    */
    bool     missing = false;
    bool     is_new = false;
    /* layout */
    uint32_t lba = 0;
    uint32_t sectors = 0;
    uint32_t rec_size = 0;
    bool     moved = false;
    std::vector<int> children;     /* dir: final record order (entry indices) */
    std::vector<uint8_t> data;     /* dir: built directory extent             */
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> patches; /* file offset -> bytes */
    /* io */
    std::ifstream stream;
    bool stream_tried = false;
};

struct RawRun {
    uint32_t lba = 0, sectors = 0;
    fs::path file;
    std::ifstream stream;
    bool stream_tried = false;
};

struct TrackSpec {
    int number = 1;
    bool audio = false;
    uint32_t sectors = 0;      /* manifest: pregap + payload */
    uint32_t pregap = 0;
    fs::path wav;
    fs::path pregap_raw;
    /* resolved audio */
    std::ifstream stream;
    bool stream_tried = false;
    uint64_t pcm_offset = 0;   /* file offset of PCM data (streamable case) */
    uint64_t pcm_bytes = 0;
    std::vector<uint8_t> pcm_mem; /* converted PCM when the WAV is not 44.1k/16/2 */
    bool from_mem = false;
    std::ifstream pregap_stream;
    bool pregap_tried = false;
    /* layout */
    uint32_t start_lba = 0, pregap_lba = 0, total = 0;
};

enum ExtKind { X_RAWFILE, X_FORM1, X_RAW2336, X_MEM, X_AUDIO_PCM, X_AUDIO_PREGAP };

struct Extent {
    uint32_t lba = 0, sectors = 0;
    ExtKind kind = X_MEM;
    int idx = -1;                       /* entry / raw run / track index */
    const std::vector<uint8_t>* mem = nullptr;
    std::vector<uint8_t> submodes;      /* X_MEM: one per sector */
};

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::vector<uint8_t> unhex(const std::string& h) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back((uint8_t)std::stoul(h.substr(i, 2), nullptr, 16));
    return out;
}

std::string hexs(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

void put_le32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 24));
}
void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
void put_both32(std::vector<uint8_t>& v, uint32_t x) { put_le32(v, x); put_be32(v, x); }
void put_both16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
void write_le32(uint8_t* p, uint32_t x) {
    p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24);
}
void write_be32(uint8_t* p, uint32_t x) {
    p[0] = (uint8_t)(x >> 24); p[1] = (uint8_t)(x >> 16); p[2] = (uint8_t)(x >> 8); p[3] = (uint8_t)x;
}
uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ECMA-119 9.3: order by file name, then extension, then version, each padded. */
struct IsoNameKey {
    std::string stem, ext;
    int version;
};
IsoNameKey iso_key(const std::string& ident) {
    IsoNameKey k;
    std::string base = ident;
    k.version = 0;
    const size_t sc = base.find(';');
    if (sc != std::string::npos) {
        k.version = std::atoi(base.c_str() + sc + 1);
        base = base.substr(0, sc);
    }
    const size_t dot = base.find('.');
    k.stem = dot == std::string::npos ? base : base.substr(0, dot);
    k.ext = dot == std::string::npos ? "" : base.substr(dot + 1);
    k.stem.resize(32, ' ');
    k.ext.resize(32, ' ');
    return k;
}
bool iso_less(const std::string& a, const std::string& b) {
    const IsoNameKey ka = iso_key(a), kb = iso_key(b);
    if (ka.stem != kb.stem) return ka.stem < kb.stem;
    if (ka.ext != kb.ext) return ka.ext < kb.ext;
    return ka.version < kb.version;
}

std::string parent_of(const std::string& path) {
    const size_t s = path.rfind('/');
    return s == std::string::npos ? std::string() : path.substr(0, s);
}
std::string name_of(const std::string& path) {
    const size_t s = path.rfind('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}
int level_of(const std::string& path) {
    if (path.empty()) return 1;
    return 2 + (int)std::count(path.begin(), path.end(), '/');
}

std::vector<uint8_t> build_record(const std::string& ident, uint32_t lba, uint32_t size,
                                  uint8_t flags, const uint8_t date[7],
                                  const std::vector<uint8_t>& sua) {
    std::vector<uint8_t> body;
    body.push_back(0);                          /* extended attribute length */
    put_both32(body, lba);
    put_both32(body, size);
    body.insert(body.end(), date, date + 7);
    body.push_back(flags);
    body.push_back(0);                          /* file unit size */
    body.push_back(0);                          /* interleave gap */
    put_both16(body, 1);                        /* volume sequence number */
    body.push_back((uint8_t)ident.size());
    body.insert(body.end(), ident.begin(), ident.end());
    if (ident.size() % 2 == 0) body.push_back(0);
    body.insert(body.end(), sua.begin(), sua.end());
    std::vector<uint8_t> rec;
    rec.push_back((uint8_t)(body.size() + 1));
    rec.insert(rec.end(), body.begin(), body.end());
    return rec;
}

/* Pack records into 2048-byte logical sectors; a record never straddles one. */
std::vector<uint8_t> pack_directory(const std::vector<std::vector<uint8_t>>& recs) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> cur;
    for (const auto& r : recs) {
        if (cur.size() + r.size() > USER_SEC) {
            cur.resize(USER_SEC, 0);
            out.insert(out.end(), cur.begin(), cur.end());
            cur.clear();
        }
        cur.insert(cur.end(), r.begin(), r.end());
    }
    cur.resize(USER_SEC, 0);
    out.insert(out.end(), cur.begin(), cur.end());
    return out;
}

bool read_file_bytes(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    out.resize((size_t)n);
    f.seekg(0);
    if (n > 0 && !f.read((char*)out.data(), n)) return false;
    return true;
}

bool file_starts_with_sync(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    uint8_t b[12];
    if (!f.read((char*)b, 12)) return false;
    return std::memcmp(b, SYNC, 12) == 0;
}

/* WAV probing: PCM chunks. Returns false when not a RIFF/WAVE file. */
struct WavInfo {
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    uint64_t data_offset = 0, data_bytes = 0;
};
bool probe_wav(const fs::path& p, WavInfo& w) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const uint64_t size = (uint64_t)f.tellg();
    f.seekg(0);
    uint8_t hdr[12];
    if (!f.read((char*)hdr, 12)) return false;
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return false;
    uint64_t pos = 12;
    bool have_fmt = false;
    while (pos + 8 <= size) {
        uint8_t ch[8];
        f.seekg((std::streamoff)pos);
        if (!f.read((char*)ch, 8)) break;
        const uint32_t clen = read_le32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (!f.read((char*)fmt, 16)) return false;
            w.format = (uint16_t)(fmt[0] | (fmt[1] << 8));
            w.channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            w.rate = read_le32(fmt + 4);
            w.bits = (uint16_t)(fmt[14] | (fmt[15] << 8));
            have_fmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            w.data_offset = pos + 8;
            w.data_bytes = std::min<uint64_t>(clen, size - (pos + 8));
            return have_fmt;
        }
        pos += 8 + clen + (clen & 1);
    }
    return false;
}

} // namespace

/* ───────────────────────── the implementation ─────────────────────────── */

struct DiscTree::Impl {
    fs::path dir;
    DiscTreeHints hints;
    std::vector<std::string>* notes = nullptr;
    /* manifest */
    std::vector<uint8_t> system_area;            /* 16 raw sectors */
    std::vector<std::vector<uint8_t>> descriptors; /* 2048 each, PVD first */
    uint32_t descriptor_lba = 16;
    int32_t  terminator_lba = 17;                /* -1 = stored raw as descriptor */
    std::vector<uint32_t> pt_l, pt_m;
    uint32_t postgap = 150;
    uint32_t src_data_sectors = 0;
    std::vector<RawRun> raw_runs;
    std::vector<TrackSpec> tracks;
    std::vector<Entry> entries;                  /* dirs + files */
    std::map<std::string, int> by_path;
    /* layout */
    std::vector<Extent> extents;                 /* sorted by lba */
    std::vector<uint8_t> desc_set;               /* descriptors + terminator, 2048 each */
    std::vector<uint8_t> pt_l_data, pt_m_data;
    uint32_t pt_sectors = 1;
    uint32_t data_sectors = 0;
    uint32_t total_sectors = 0;
    bool pristine = true;
    int exe_entry = -1;
    std::vector<DiscTreeRamPatch> ram_patches;

    void note(const std::string& s) { if (notes) notes->push_back(s); }

    bool load_manifest(std::string* err);
    bool scan_host(std::string* err);
    bool layout(std::string* err);
    void build_metadata();
    void patch_exe_tables();
    bool resolve_audio(std::string* err);
    bool read_raw(uint32_t lba, uint8_t* out);
    std::ifstream* entry_stream(Entry& e);
    std::ifstream* raw_stream(RawRun& r);
    std::ifstream* track_stream(TrackSpec& t, bool pregap);
    void synth_form1(uint8_t* out, uint32_t lba, const uint8_t* data2048, uint8_t submode);
    void synth_empty(uint8_t* out, uint32_t lba);
    void synth_raw2336(uint8_t* out, uint32_t lba, const uint8_t* body);
};

/* ───────────────────────── manifest loading ───────────────────────────── */

bool DiscTree::Impl::load_manifest(std::string* err) {
    toml::value m;
    try {
        m = toml::parse((dir / "disc.toml").string());
    } catch (const std::exception& e) {
        if (err) *err = std::string("disc.toml: ") + e.what();
        return false;
    }
    try {
        if (!m.contains("format") || toml::find<std::string>(m, "format") != "psxrecomp-disc-tree") {
            if (err) *err = "disc.toml: not a psxrecomp-disc-tree manifest";
            return false;
        }
        const auto& src = toml::find(m, "source");
        if (src.contains("data_track_sectors"))
            src_data_sectors = (uint32_t)toml::find<int64_t>(src, "data_track_sectors");
        const auto& lay = toml::find(m, "layout");
        if (!read_file_bytes(dir / toml::find<std::string>(lay, "system_area"), system_area) ||
            system_area.size() != 16 * RAW_SEC) {
            if (err) *err = "disc.toml: system_area missing or not 16 raw sectors";
            return false;
        }
        for (const std::string& d : toml::find<std::vector<std::string>>(lay, "descriptors")) {
            std::vector<uint8_t> bytes;
            if (!read_file_bytes(dir / d, bytes) || bytes.size() != USER_SEC) {
                if (err) *err = "disc.toml: descriptor " + d + " missing or not 2048 bytes";
                return false;
            }
            descriptors.push_back(std::move(bytes));
        }
        if (descriptors.empty() || descriptors[0][0] != 1 || std::memcmp(&descriptors[0][1], "CD001", 5) != 0) {
            if (err) *err = "disc.toml: first descriptor is not a primary volume descriptor";
            return false;
        }
        if (lay.contains("descriptor_lba")) descriptor_lba = (uint32_t)toml::find<int64_t>(lay, "descriptor_lba");
        if (lay.contains("terminator_lba")) terminator_lba = (int32_t)toml::find<int64_t>(lay, "terminator_lba");
        for (int64_t x : toml::find<std::vector<int64_t>>(lay, "path_table_l")) pt_l.push_back((uint32_t)x);
        for (int64_t x : toml::find<std::vector<int64_t>>(lay, "path_table_m")) pt_m.push_back((uint32_t)x);
        if (lay.contains("postgap_sectors")) postgap = (uint32_t)toml::find<int64_t>(lay, "postgap_sectors");
        if (postgap == 0) postgap = 150;

        if (m.contains("raw")) {
            for (const toml::value& v : toml::find(m, "raw").as_array()) {
                RawRun r;
                r.lba = (uint32_t)toml::find<int64_t>(v, "lba");
                r.sectors = (uint32_t)toml::find<int64_t>(v, "sectors");
                r.file = dir / toml::find<std::string>(v, "file");
                raw_runs.push_back(std::move(r));
            }
        }
        for (const toml::value& v : toml::find(m, "track").as_array()) {
            TrackSpec t;
            t.number = (int)toml::find<int64_t>(v, "number");
            t.audio = toml::find<std::string>(v, "type") == "audio";
            t.sectors = (uint32_t)toml::find<int64_t>(v, "sectors");
            if (v.contains("pregap_sectors")) t.pregap = (uint32_t)toml::find<int64_t>(v, "pregap_sectors");
            if (v.contains("file")) t.wav = dir / toml::find<std::string>(v, "file");
            if (v.contains("pregap_raw")) t.pregap_raw = dir / toml::find<std::string>(v, "pregap_raw");
            tracks.push_back(std::move(t));
        }
        if (tracks.empty() || tracks[0].audio) {
            if (err) *err = "disc.toml: track 1 must be the data track";
            return false;
        }
        if (src_data_sectors == 0) src_data_sectors = tracks[0].sectors;

        for (const toml::value& v : toml::find(m, "dir").as_array()) {
            Entry e;
            e.is_dir = true;
            e.path = toml::find<std::string>(v, "path");
            e.name = name_of(e.path);
            e.parent = parent_of(e.path);
            e.has_orig = true;
            e.orig_lba = (uint32_t)toml::find<int64_t>(v, "lba");
            e.orig_sectors = (uint32_t)toml::find<int64_t>(v, "sectors");
            e.orig_size = e.orig_sectors * USER_SEC;
            const auto date = unhex(toml::find<std::string>(v, "date"));
            if (date.size() == 7) std::memcpy(e.date, date.data(), 7);
            e.sua = unhex(toml::find<std::string>(v, "xa"));
            if (v.contains("entries"))
                e.order = toml::find<std::vector<std::string>>(v, "entries");
            by_path[e.path] = (int)entries.size();
            entries.push_back(std::move(e));
        }
        if (!by_path.count("")) {
            if (err) *err = "disc.toml: no root [[dir]]";
            return false;
        }
        for (const toml::value& v : toml::find(m, "file").as_array()) {
            Entry e;
            e.path = toml::find<std::string>(v, "path");
            e.name = name_of(e.path);
            e.parent = parent_of(e.path);
            e.has_orig = true;
            e.orig_lba = (uint32_t)toml::find<int64_t>(v, "lba");
            e.orig_size = (uint32_t)toml::find<int64_t>(v, "size");
            e.orig_sectors = (e.orig_size + USER_SEC - 1) / USER_SEC;
            const toml::value& fv = toml::find(v, "form");
            if (fv.is_string()) {
                const std::string f = fv.as_string();
                e.form = f == "cdda" ? FORM_CDDA : (f == "raw" || f == "2") ? FORM_RAW : FORM1;
            } else {
                e.form = fv.as_integer() == 2 ? FORM_RAW : FORM1;
            }
            if (v.contains("track")) e.track = (int)toml::find<int64_t>(v, "track");
            if (v.contains("version")) e.version = (int)toml::find<int64_t>(v, "version");
            const auto date = unhex(toml::find<std::string>(v, "date"));
            if (date.size() == 7) std::memcpy(e.date, date.data(), 7);
            e.sua = unhex(toml::find<std::string>(v, "xa"));
            if (!by_path.count(e.parent)) {
                if (err) *err = "disc.toml: file " + e.path + " has no [[dir]] parent";
                return false;
            }
            by_path[e.path] = (int)entries.size();
            entries.push_back(std::move(e));
        }
    } catch (const std::exception& ex) {
        if (err) *err = std::string("disc.toml: ") + ex.what();
        return false;
    }
    return true;
}

/* ───────────────────────── host scan ──────────────────────────────────── */

bool DiscTree::Impl::scan_host(std::string* err) {
    const fs::path root = dir / "cdrom";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        if (err) *err = "disc tree has no cdrom/ directory";
        return false;
    }
    /* Resolve manifest entries against the host. */
    for (Entry& e : entries) {
        if (e.form == FORM_CDDA) continue;
        fs::path h = root;
        if (!e.path.empty()) {
            std::string p = e.path;
            for (char& c : p) if (c == '/') c = (char)fs::path::preferred_separator;
            h /= fs::path(p);
        }
        e.host = h;
        if (e.is_dir) {
            if (!fs::is_directory(h, ec)) { e.missing = true; note("missing directory: " + e.path); }
            continue;
        }
        if (!fs::is_regular_file(h, ec)) {
            e.missing = true;
            note("missing file: " + e.path + " (its record is dropped; the game reads empty sectors there)");
            continue;
        }
        e.host_size = (uint64_t)fs::file_size(h, ec);
    }
    /* Discover host files/dirs the manifest does not know (additions). The
     * directory iteration order is filesystem-dependent, so sort by ISO path
     * before adopting them: the layout must not depend on the host. */
    std::vector<Entry> found;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path rel = fs::relative(it->path(), root, ec);
        std::string iso;
        for (const auto& part : rel) {
            if (!iso.empty()) iso += '/';
            iso += part.string();
        }
        if (by_path.count(iso)) continue;
        if (it->is_directory(ec)) {
            Entry d;
            d.is_dir = true; d.is_new = true;
            d.path = iso; d.name = name_of(iso); d.parent = parent_of(iso);
            d.host = it->path();
            found.push_back(std::move(d));
        } else if (it->is_regular_file(ec)) {
            Entry f;
            f.is_new = true;
            f.path = iso; f.name = name_of(iso); f.parent = parent_of(iso);
            f.host = it->path();
            f.host_size = (uint64_t)fs::file_size(it->path(), ec);
            const std::string ext = lower(fs::path(f.name).extension().string());
            const bool rawish = ext == ".str" || ext == ".xa" || ext == ".iki" || ext == ".mov";
            f.form = FORM1;
            if (rawish && (f.host_size % F2_SEC == 0 || (f.host_size % RAW_SEC == 0 && file_starts_with_sync(f.host))))
                f.form = FORM_RAW;
            found.push_back(std::move(f));
        }
    }
    std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    for (Entry& e : found) {
        note(std::string(e.is_dir ? "new directory: " : "new file: ") + e.path);
        by_path[e.path] = (int)entries.size();
        entries.push_back(std::move(e));
    }
    /* Inherit dates / XA attributes for new entries from the closest sibling
     * of the same kind, else the parent directory. Parents are guaranteed to
     * exist as entries now (recursive iteration lists them before children). */
    for (Entry& e : entries) {
        if (!e.is_new) continue;
        const Entry* model = nullptr;
        for (const Entry& s : entries) {
            if (&s == &e || s.is_new || s.parent != e.parent) continue;
            if (s.is_dir == e.is_dir && (e.is_dir || s.form == e.form)) { model = &s; break; }
        }
        if (!model) {
            for (const Entry& s : entries)
                if (!s.is_new && s.is_dir == e.is_dir && (e.is_dir || s.form == e.form)) { model = &s; break; }
        }
        const Entry& par = entries[by_path[e.parent]];
        std::memcpy(e.date, model ? model->date : par.date, 7);
        if (model) e.sua = model->sua;
        else {
            static const uint8_t f1[14]  = {0, 0, 0, 0, 0x0D, 0x55, 'X', 'A', 0, 0, 0, 0, 0, 0};
            static const uint8_t f2[14]  = {0, 0, 0, 0, 0x25, 0x55, 'X', 'A', 0, 0, 0, 0, 0, 0};
            static const uint8_t dd[14]  = {0, 0, 0, 0, 0x8D, 0x55, 'X', 'A', 0, 0, 0, 0, 0, 0};
            const uint8_t* s = e.is_dir ? dd : (e.form == FORM_RAW ? f2 : f1);
            e.sua.assign(s, s + 14);
        }
    }
    /* Raw file geometry. */
    for (Entry& e : entries) {
        if (e.is_dir || e.missing || e.form != FORM_RAW) continue;
        if (e.host_size % F2_SEC == 0) {
            e.raw_has_sync = false;
        } else if (e.host_size % RAW_SEC == 0 && file_starts_with_sync(e.host)) {
            e.raw_has_sync = true;
        } else {
            note("file " + e.path + " is a raw-sector file but its size " + std::to_string(e.host_size) +
                 " is neither a multiple of 2336 nor of 2352 with sync; serving it as plain Form 1 data");
            e.form = FORM1;
        }
    }
    return true;
}

/* ───────────────────────── layout ─────────────────────────────────────── */

bool DiscTree::Impl::layout(std::string* err) {
    /* Sizes first (directory extents depend only on names). */
    for (Entry& e : entries) {
        if (e.is_dir || e.missing) continue;
        if (e.form == FORM_CDDA) { e.sectors = 0; continue; }
        if (e.form == FORM_RAW) {
            e.sectors = (uint32_t)(e.host_size / (e.raw_has_sync ? RAW_SEC : F2_SEC));
            e.rec_size = e.sectors * USER_SEC;
        } else {
            e.sectors = (uint32_t)((e.host_size + USER_SEC - 1) / USER_SEC);
            e.rec_size = (uint32_t)e.host_size;
        }
    }
    /* Directory children (final record order) and sizes. */
    for (size_t i = 0; i < entries.size(); i++) {
        Entry& d = entries[i];
        if (!d.is_dir || d.missing) continue;
        std::vector<int> listed, extra;
        std::set<std::string> seen;
        for (const std::string& n : d.order) {
            auto it = by_path.find(d.path.empty() ? n : d.path + "/" + n);
            if (it == by_path.end() || entries[it->second].missing) continue;
            listed.push_back(it->second);
            seen.insert(n);
        }
        for (size_t j = 0; j < entries.size(); j++) {
            const Entry& c = entries[j];
            if (c.parent != d.path || j == i || c.missing || (c.path.empty())) continue;
            if (seen.count(c.name)) continue;
            extra.push_back((int)j);
        }
        std::sort(extra.begin(), extra.end(), [&](int a, int b) {
            const Entry& ea = entries[a]; const Entry& eb = entries[b];
            const std::string ia = ea.is_dir ? ea.name : ea.name + ";" + std::to_string(ea.version);
            const std::string ib = eb.is_dir ? eb.name : eb.name + ";" + std::to_string(eb.version);
            return iso_less(ia, ib);
        });
        d.children = listed;
        d.children.insert(d.children.end(), extra.begin(), extra.end());
        /* size: records with placeholder LBAs (lengths do not depend on them) */
        std::vector<std::vector<uint8_t>> recs;
        recs.push_back(build_record(std::string(1, '\0'), 0, 0, 2, d.date, d.sua));
        recs.push_back(build_record(std::string(1, '\1'), 0, 0, 2, d.date, d.sua));
        for (int c : d.children) {
            const Entry& e = entries[c];
            const std::string ident = e.is_dir ? e.name : e.name + ";" + std::to_string(e.version);
            recs.push_back(build_record(ident, 0, 0, e.is_dir ? 2 : 0, e.date, e.sua));
        }
        d.sectors = (uint32_t)(pack_directory(recs).size() / USER_SEC);
        d.rec_size = d.sectors * USER_SEC;
    }
    /* Path table size. */
    {
        std::vector<int> dirs;
        for (size_t i = 0; i < entries.size(); i++)
            if (entries[i].is_dir && !entries[i].missing) dirs.push_back((int)i);
        uint32_t bytes = 0;
        for (int i : dirs) {
            const size_t nl = entries[i].path.empty() ? 1 : entries[i].name.size();
            bytes += (uint32_t)(8 + nl + (nl & 1));
        }
        pt_sectors = (bytes + USER_SEC - 1) / USER_SEC;
    }
    const uint32_t orig_pt_size = read_le32(&descriptors[0][132]);
    const uint32_t orig_pt_sectors = std::max<uint32_t>(1, (orig_pt_size + USER_SEC - 1) / USER_SEC);

    /* Fixed placement: everything that still fits its pristine extent. */
    uint32_t append_start = 16 + (uint32_t)descriptors.size() + 1;
    auto bump = [&](uint32_t end) { if (end > append_start) append_start = end; };
    for (uint32_t x : pt_l) bump(x + orig_pt_sectors);
    for (uint32_t x : pt_m) bump(x + orig_pt_sectors);
    for (RawRun& r : raw_runs) bump(r.lba + r.sectors);
    std::vector<int> appended;
    for (size_t i = 0; i < entries.size(); i++) {
        Entry& e = entries[i];
        if (e.missing || e.form == FORM_CDDA) continue;
        if (e.has_orig && e.sectors <= e.orig_sectors) {
            e.lba = e.orig_lba;
            bump(e.lba + e.orig_sectors);
            if (!e.is_dir && e.rec_size != e.orig_size) {
                pristine = false;
                note((e.form == FORM_RAW ? "resized raw file " : "resized file ") + e.path + ": " +
                     std::to_string(e.orig_size) + " -> " + std::to_string(e.rec_size) +
                     " bytes (kept at LBA " + std::to_string(e.lba) + ")");
            }
        } else {
            appended.push_back((int)i);
        }
    }
    /* Everything else goes after the last pristine sector — the original data
     * area (minus its postgap) is never disturbed. */
    bump(src_data_sectors > postgap ? src_data_sectors - postgap : src_data_sectors);
    bool pt_relocated = false;
    uint32_t cursor = append_start;
    if (pt_sectors > orig_pt_sectors) {
        pt_relocated = true;
        pristine = false;
        std::vector<uint32_t> nl, nm;
        for (size_t k = 0; k < pt_l.size(); k++) { nl.push_back(cursor); cursor += pt_sectors; }
        for (size_t k = 0; k < pt_m.size(); k++) { nm.push_back(cursor); cursor += pt_sectors; }
        note("path tables grew to " + std::to_string(pt_sectors) + " sector(s); relocated to LBA " +
             std::to_string(nl.empty() ? nm.front() : nl.front()));
        pt_l = nl; pt_m = nm;
    }
    for (int i : appended) {
        Entry& e = entries[i];
        e.lba = cursor;
        e.moved = true;
        pristine = false;
        cursor += e.sectors;
        if (e.is_new)
            note(std::string(e.is_dir ? "new directory " : "new file ") + e.path + " placed at LBA " +
                 std::to_string(e.lba) + " (" + std::to_string(e.sectors) + " sectors)");
        else
            note((e.is_dir ? "directory " : "file ") + e.path + " grew (" + std::to_string(e.orig_size) +
                 " -> " + std::to_string(e.rec_size) + " bytes): relocated LBA " +
                 std::to_string(e.orig_lba) + " -> " + std::to_string(e.lba));
    }
    if (cursor == append_start && !pt_relocated) {
        data_sectors = src_data_sectors;
    } else {
        data_sectors = cursor + postgap;
        note("data track grew " + std::to_string(src_data_sectors) + " -> " + std::to_string(data_sectors) +
             " sectors");
    }
    /* Audio tracks follow the data track. */
    uint32_t pos = data_sectors;
    for (TrackSpec& t : tracks) {
        if (!t.audio) { t.pregap_lba = 0; t.start_lba = 0; t.total = data_sectors; continue; }
        t.pregap_lba = pos;
        t.start_lba = pos + t.pregap;
        t.total = t.sectors;
        pos += t.total;
    }
    total_sectors = pos;
    /* CD-DA aliases: record points at the track's INDEX 01, size = payload. */
    for (Entry& e : entries) {
        if (e.form != FORM_CDDA) continue;
        const TrackSpec* t = nullptr;
        for (const TrackSpec& ts : tracks) if (ts.number == e.track) t = &ts;
        if (!t) { e.missing = true; note("CD-DA alias " + e.path + " names an unknown track"); continue; }
        e.lba = t->start_lba;
        e.rec_size = (t->total - t->pregap) * USER_SEC;
        if (e.has_orig && (e.lba != e.orig_lba || e.rec_size != e.orig_size)) {
            e.moved = true;
            pristine = false;
        }
    }
    (void)err;
    return true;
}

/* ───────────────────────── metadata synthesis ─────────────────────────── */

void DiscTree::Impl::build_metadata() {
    /* Directories: real records now that every LBA is known. */
    for (Entry& d : entries) {
        if (!d.is_dir || d.missing) continue;
        const Entry& par = entries[by_path[d.parent]];
        std::vector<std::vector<uint8_t>> recs;
        recs.push_back(build_record(std::string(1, '\0'), d.lba, d.rec_size, 2, d.date, d.sua));
        recs.push_back(build_record(std::string(1, '\1'), par.lba, par.rec_size, 2, par.date, par.sua));
        for (int c : d.children) {
            const Entry& e = entries[c];
            const std::string ident = e.is_dir ? e.name : e.name + ";" + std::to_string(e.version);
            recs.push_back(build_record(ident, e.lba, e.rec_size, e.is_dir ? 2 : 0, e.date, e.sua));
        }
        d.data = pack_directory(recs);
    }
    /* Path tables: ECMA-119 6.9 — by level, then parent number, then name. */
    std::vector<int> dirs;
    for (size_t i = 0; i < entries.size(); i++)
        if (entries[i].is_dir && !entries[i].missing) dirs.push_back((int)i);
    std::map<std::string, uint16_t> number;
    std::vector<int> ordered;
    int max_level = 1;
    for (int i : dirs) max_level = std::max(max_level, level_of(entries[i].path));
    for (int lv = 1; lv <= max_level; lv++) {
        std::vector<int> same;
        for (int i : dirs) if (level_of(entries[i].path) == lv) same.push_back(i);
        std::sort(same.begin(), same.end(), [&](int a, int b) {
            const Entry& ea = entries[a]; const Entry& eb = entries[b];
            const uint16_t pa = ea.path.empty() ? 0 : number[ea.parent];
            const uint16_t pb = eb.path.empty() ? 0 : number[eb.parent];
            if (pa != pb) return pa < pb;
            return iso_less(ea.name, eb.name);
        });
        for (int i : same) {
            number[entries[i].path] = (uint16_t)(ordered.size() + 1);
            ordered.push_back(i);
        }
    }
    pt_l_data.clear();
    pt_m_data.clear();
    for (int i : ordered) {
        const Entry& d = entries[i];
        const std::string name = d.path.empty() ? std::string(1, '\0') : d.name;
        const uint16_t parent = d.path.empty() ? 1 : number[d.parent];
        for (int big = 0; big < 2; big++) {
            std::vector<uint8_t>& out = big ? pt_m_data : pt_l_data;
            out.push_back((uint8_t)name.size());
            out.push_back(0);
            if (big) put_be32(out, d.lba); else put_le32(out, d.lba);
            if (big) { out.push_back((uint8_t)(parent >> 8)); out.push_back((uint8_t)parent); }
            else     { out.push_back((uint8_t)parent); out.push_back((uint8_t)(parent >> 8)); }
            out.insert(out.end(), name.begin(), name.end());
            if (name.size() & 1) out.push_back(0);
        }
    }
    const uint32_t pt_size = (uint32_t)pt_l_data.size();
    pt_l_data.resize((size_t)pt_sectors * USER_SEC, 0);
    pt_m_data.resize((size_t)pt_sectors * USER_SEC, 0);
    /* Volume descriptor set: patched PVD + others verbatim + terminator. */
    desc_set.clear();
    for (size_t k = 0; k < descriptors.size(); k++) {
        std::vector<uint8_t> d = descriptors[k];
        if (k == 0) {
            const Entry& root = entries[by_path[""]];
            write_le32(&d[80], total_sectors); write_be32(&d[84], total_sectors);
            write_le32(&d[132], pt_size);      write_be32(&d[136], pt_size);
            write_le32(&d[140], pt_l.size() > 0 ? pt_l[0] : 0);
            write_le32(&d[144], pt_l.size() > 1 ? pt_l[1] : 0);
            write_be32(&d[148], pt_m.size() > 0 ? pt_m[0] : 0);
            write_be32(&d[152], pt_m.size() > 1 ? pt_m[1] : 0);
            const std::vector<uint8_t> rr = build_record(std::string(1, '\0'), root.lba, root.rec_size, 2, root.date, {});
            std::memcpy(&d[156], rr.data(), 34);
        }
        desc_set.insert(desc_set.end(), d.begin(), d.end());
    }
    if (terminator_lba >= 0) {
        std::vector<uint8_t> term(USER_SEC, 0);
        term[0] = 0xFF; std::memcpy(&term[1], "CD001", 5); term[6] = 1;
        desc_set.insert(desc_set.end(), term.begin(), term.end());
    }
    /* Extent map. */
    extents.clear();
    auto add = [&](Extent x) { if (x.sectors) extents.push_back(std::move(x)); };
    { Extent x; x.lba = 0; x.sectors = 16; x.kind = X_RAWFILE; x.idx = -1; add(x); }  /* system area (memory) */
    {
        Extent x; x.lba = descriptor_lba; x.sectors = (uint32_t)(desc_set.size() / USER_SEC);
        x.kind = X_MEM; x.mem = &desc_set;
        for (uint32_t s = 0; s < x.sectors; s++)
            x.submodes.push_back(s + 1 == x.sectors && terminator_lba >= 0 ? 0x89 : 0x09);
        add(x);
    }
    auto add_mem = [&](uint32_t lba, const std::vector<uint8_t>* mem) {
        Extent x; x.lba = lba; x.sectors = (uint32_t)(mem->size() / USER_SEC); x.kind = X_MEM; x.mem = mem;
        for (uint32_t s = 0; s < x.sectors; s++) x.submodes.push_back(s + 1 == x.sectors ? 0x89 : 0x08);
        add(x);
    };
    for (uint32_t l : pt_l) add_mem(l, &pt_l_data);
    for (uint32_t l : pt_m) add_mem(l, &pt_m_data);
    for (size_t i = 0; i < raw_runs.size(); i++) {
        Extent x; x.lba = raw_runs[i].lba; x.sectors = raw_runs[i].sectors; x.kind = X_RAWFILE; x.idx = (int)i; add(x);
    }
    for (size_t i = 0; i < entries.size(); i++) {
        Entry& e = entries[i];
        if (e.missing || e.form == FORM_CDDA || e.sectors == 0) continue;
        if (e.is_dir) { add_mem(e.lba, &e.data); continue; }
        Extent x; x.lba = e.lba; x.sectors = e.sectors; x.idx = (int)i;
        x.kind = e.form == FORM_RAW ? X_RAW2336 : X_FORM1;
        add(x);
    }
    for (size_t i = 0; i < tracks.size(); i++) {
        TrackSpec& t = tracks[i];
        if (!t.audio) continue;
        if (t.pregap) { Extent x; x.lba = t.pregap_lba; x.sectors = t.pregap; x.kind = X_AUDIO_PREGAP; x.idx = (int)i; add(x); }
        Extent x; x.lba = t.start_lba; x.sectors = t.total - t.pregap; x.kind = X_AUDIO_PCM; x.idx = (int)i; add(x);
    }
    std::sort(extents.begin(), extents.end(), [](const Extent& a, const Extent& b) { return a.lba < b.lba; });
}

/* ───────────────────────── boot EXE LBA tables ────────────────────────── */

void DiscTree::Impl::patch_exe_tables() {
    ram_patches.clear();
    if (hints.lba_tables.empty()) return;
    /* Locate the boot EXE entry. */
    std::string boot = hints.exe_name;
    if (boot.empty()) {
        auto it = by_path.find("SYSTEM.CNF");
        if (it != by_path.end() && !entries[it->second].missing) {
            std::vector<uint8_t> cnf;
            if (read_file_bytes(entries[it->second].host, cnf)) {
                std::string text(cnf.begin(), cnf.end());
                const size_t k = lower(text).find("cdrom:");
                if (k != std::string::npos) {
                    size_t j = k + 6;
                    while (j < text.size() && (text[j] == '\\' || text[j] == '/')) j++;
                    while (j < text.size() && text[j] != ';' && text[j] != '\r' && text[j] != '\n' && text[j] != ' ')
                        boot += text[j++];
                }
            }
        }
    }
    auto it = by_path.find(boot);
    if (boot.empty() || it == by_path.end() || entries[it->second].missing || entries[it->second].is_dir) {
        note("lba_table: boot EXE '" + boot + "' not found in the tree; tables not patched");
        return;
    }
    Entry& exe = entries[it->second];
    exe_entry = it->second;
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(exe.host, bytes) || bytes.size() < 0x800 + 4) return;
    if (std::memcmp(bytes.data(), "PS-X EXE", 8) != 0) {
        note("lba_table: " + boot + " has no PS-X EXE header; tables not patched");
        return;
    }
    const uint32_t load = read_le32(&bytes[0x18]);
    /* pristine LBA -> entry (files and CD-DA aliases) */
    std::map<uint32_t, int> by_orig;
    for (size_t i = 0; i < entries.size(); i++) {
        const Entry& e = entries[i];
        if (e.is_dir || !e.has_orig) continue;
        by_orig.emplace(e.orig_lba, (int)i);
    }
    int patched = 0;
    for (const DiscTreeLbaTable& t : hints.lba_tables) {
        for (uint32_t i = 0; i < t.count; i++) {
            const uint32_t addr = t.address + i * t.stride;
            if (addr < load) continue;
            const uint64_t off = 0x800ull + (addr - load);
            if (t.lba_offset < 0 || off + (uint64_t)t.lba_offset + 4 > bytes.size()) continue;
            const uint8_t* lp = &bytes[(size_t)off + t.lba_offset];
            uint32_t cur;
            if (t.lba_is_msf) {
                auto unbcd = [](uint8_t v) { return (uint32_t)((v >> 4) * 10 + (v & 15)); };
                cur = unbcd(lp[0]) * 4500 + unbcd(lp[1]) * 75 + unbcd(lp[2]);
                cur = cur >= 150 ? cur - 150 : 0;
            } else {
                cur = read_le32(lp);
            }
            auto f = by_orig.find(cur);
            if (f == by_orig.end()) continue;
            const Entry& e = entries[f->second];
            if (e.missing) continue;
            const bool lba_changed = e.lba != e.orig_lba;
            const bool size_changed = e.rec_size != e.orig_size;
            if (!lba_changed && !size_changed) continue;
            if (lba_changed) {
                std::vector<uint8_t> nb;
                if (t.lba_is_msf) { uint8_t m[3]; cd_lba_to_msf_bcd(e.lba, m); nb.assign(m, m + 3); }
                else { nb.resize(4); write_le32(nb.data(), e.lba); }
                exe.patches.emplace_back((uint32_t)off + t.lba_offset, nb);
                ram_patches.push_back({addr + (uint32_t)t.lba_offset, nb});
            }
            /* The size field's unit is the game's business (byte size for data
             * files, sectors x 2336 for STR/XA DMA lengths, ...): infer it from
             * the pristine value and rewrite in the same unit; leave it alone
             * when it matches none of the known conventions. */
            std::string size_note;
            if (size_changed && t.size_offset >= 0 && off + (uint64_t)t.size_offset + 4 <= bytes.size()) {
                const uint32_t cur_size = read_le32(&bytes[(size_t)off + t.size_offset]);
                const uint32_t orig_sectors = (e.orig_size + USER_SEC - 1) / USER_SEC;
                uint32_t nv = 0;
                bool known = true;
                if (cur_size == e.orig_size)                 nv = e.rec_size;
                else if (cur_size == orig_sectors * F2_SEC)  nv = e.sectors * F2_SEC;
                else if (cur_size == orig_sectors * USER_SEC) nv = e.sectors * USER_SEC;
                else if (cur_size == orig_sectors * RAW_SEC) nv = e.sectors * RAW_SEC;
                else if (cur_size == orig_sectors)           nv = e.sectors;
                else known = false;
                if (known) {
                    std::vector<uint8_t> nb(4);
                    write_le32(nb.data(), nv);
                    exe.patches.emplace_back((uint32_t)off + t.size_offset, nb);
                    ram_patches.push_back({addr + (uint32_t)t.size_offset, nb});
                    size_note = ", size " + std::to_string(cur_size) + " -> " + std::to_string(nv);
                } else {
                    size_note = ", size field " + std::to_string(cur_size) + " left as is (unknown unit)";
                }
            }
            patched++;
            note("lba_table: entry " + std::to_string(i) + " (" + e.path + ") -> LBA " +
                 std::to_string(e.lba) + size_note);
        }
    }
    if (patched) pristine = false;
}

/* ───────────────────────── audio ──────────────────────────────────────── */

bool DiscTree::Impl::resolve_audio(std::string* err) {
    for (TrackSpec& t : tracks) {
        if (!t.audio) continue;
        if (t.wav.empty()) { note("track " + std::to_string(t.number) + ": no audio file; silence"); continue; }
        WavInfo w;
        std::error_code ec;
        if (!fs::is_regular_file(t.wav, ec)) {
            note("track " + std::to_string(t.number) + ": " + t.wav.filename().string() + " missing; silence");
            t.wav.clear();
            continue;
        }
        if (!probe_wav(t.wav, w)) {
            /* Not a WAV: treat as raw PCM (s16le stereo 44.1k). */
            t.pcm_offset = 0;
            t.pcm_bytes = (uint64_t)fs::file_size(t.wav, ec);
        } else if (w.format == 1 && w.bits == 16 && w.channels == 2 && w.rate == 44100) {
            t.pcm_offset = w.data_offset;
            t.pcm_bytes = w.data_bytes;
        } else if (w.format == 1 && (w.bits == 16 || w.bits == 8) && (w.channels == 1 || w.channels == 2) && w.rate) {
            /* Convert once into memory: mono->stereo, 8->16 bit, linear resample. */
            std::vector<uint8_t> raw;
            {
                std::ifstream f(t.wav, std::ios::binary);
                f.seekg((std::streamoff)w.data_offset);
                raw.resize((size_t)w.data_bytes);
                if (!f.read((char*)raw.data(), (std::streamsize)raw.size())) raw.clear();
            }
            const uint32_t bps = w.bits / 8;
            const uint64_t frames = raw.size() / (bps * w.channels);
            auto sample = [&](uint64_t fr, int ch) -> int16_t {
                if (fr >= frames) return 0;
                const size_t o = (size_t)((fr * w.channels + (w.channels == 2 ? ch : 0)) * bps);
                if (bps == 2) return (int16_t)(raw[o] | (raw[o + 1] << 8));
                return (int16_t)(((int)raw[o] - 128) << 8);
            };
            const uint64_t out_frames = (uint64_t)((double)frames * 44100.0 / (double)w.rate);
            t.pcm_mem.resize((size_t)out_frames * 4);
            for (uint64_t i = 0; i < out_frames; i++) {
                const double src = (double)i * (double)w.rate / 44100.0;
                const uint64_t a = (uint64_t)src;
                const double fr = src - (double)a;
                for (int ch = 0; ch < 2; ch++) {
                    const double v = (1.0 - fr) * sample(a, ch) + fr * sample(a + 1, ch);
                    const int16_t s = (int16_t)std::max(-32768.0, std::min(32767.0, v));
                    t.pcm_mem[(size_t)i * 4 + ch * 2] = (uint8_t)s;
                    t.pcm_mem[(size_t)i * 4 + ch * 2 + 1] = (uint8_t)((uint16_t)s >> 8);
                }
            }
            t.from_mem = true;
            t.pcm_bytes = t.pcm_mem.size();
            note("track " + std::to_string(t.number) + ": converted " + std::to_string(w.rate) + " Hz/" +
                 std::to_string(w.bits) + "-bit/" + std::to_string(w.channels) + "ch to CD audio");
        } else {
            if (err) *err = "track " + std::to_string(t.number) + ": unsupported WAV format (need PCM 8/16-bit)";
            return false;
        }
        /* Track length follows the audio: pristine WAVs are exactly the original payload. */
        const uint32_t payload = (uint32_t)((t.pcm_bytes + RAW_SEC - 1) / RAW_SEC);
        const uint32_t want = t.pregap + std::max<uint32_t>(payload, 1);
        if (want != t.sectors) {
            note("track " + std::to_string(t.number) + ": audio is " + std::to_string(payload) +
                 " sectors, source track was " + std::to_string(t.sectors - t.pregap) + "; track resized");
            t.sectors = want;
            pristine = false;
        }
    }
    return true;
}

/* ───────────────────────── sector service ─────────────────────────────── */

std::ifstream* DiscTree::Impl::entry_stream(Entry& e) {
    if (!e.stream_tried) {
        e.stream_tried = true;
        e.stream.open(e.host, std::ios::binary);
    }
    return e.stream.is_open() ? &e.stream : nullptr;
}
std::ifstream* DiscTree::Impl::raw_stream(RawRun& r) {
    if (!r.stream_tried) { r.stream_tried = true; r.stream.open(r.file, std::ios::binary); }
    return r.stream.is_open() ? &r.stream : nullptr;
}
std::ifstream* DiscTree::Impl::track_stream(TrackSpec& t, bool pregap) {
    if (pregap) {
        if (!t.pregap_tried) { t.pregap_tried = true; if (!t.pregap_raw.empty()) t.pregap_stream.open(t.pregap_raw, std::ios::binary); }
        return t.pregap_stream.is_open() ? &t.pregap_stream : nullptr;
    }
    if (!t.stream_tried) { t.stream_tried = true; if (!t.wav.empty()) t.stream.open(t.wav, std::ios::binary); }
    return t.stream.is_open() ? &t.stream : nullptr;
}

void DiscTree::Impl::synth_form1(uint8_t* out, uint32_t lba, const uint8_t* data, uint8_t submode) {
    std::memcpy(out, SYNC, 12);
    cd_lba_to_msf_bcd(lba, out + 12);
    out[15] = 2;
    out[16] = 0; out[17] = 0; out[18] = submode; out[19] = 0;
    std::memcpy(out + 20, out + 16, 4);
    std::memcpy(out + 24, data, USER_SEC);
    write_le32(out + 2072, cd_edc_compute(out + 16, 2056));
    cd_ecc_generate_mode2_form1(out);
}
void DiscTree::Impl::synth_empty(uint8_t* out, uint32_t lba) {
    std::memset(out, 0, RAW_SEC);
    std::memcpy(out, SYNC, 12);
    cd_lba_to_msf_bcd(lba, out + 12);
    out[15] = 2;
    /* zero subheader + zero payload: EDC and ECC are zero by construction */
}
void DiscTree::Impl::synth_raw2336(uint8_t* out, uint32_t lba, const uint8_t* body) {
    std::memcpy(out, SYNC, 12);
    cd_lba_to_msf_bcd(lba, out + 12);
    out[15] = 2;
    std::memcpy(out + 16, body, F2_SEC);
}

bool DiscTree::Impl::read_raw(uint32_t lba, uint8_t* out) {
    if (lba >= total_sectors) return false;
    /* last extent whose lba <= target */
    auto it = std::upper_bound(extents.begin(), extents.end(), lba,
                               [](uint32_t l, const Extent& x) { return l < x.lba; });
    const Extent* x = nullptr;
    if (it != extents.begin()) {
        --it;
        if (lba < it->lba + it->sectors) x = &*it;
    }
    if (!x) {
        if (lba < data_sectors) { synth_empty(out, lba); return true; }
        std::memset(out, 0, RAW_SEC);      /* audio area not backed: silence */
        return true;
    }
    const uint32_t rel = lba - x->lba;
    switch (x->kind) {
    case X_RAWFILE: {
        if (x->idx < 0) { std::memcpy(out, &system_area[(size_t)rel * RAW_SEC], RAW_SEC); return true; }
        RawRun& r = raw_runs[x->idx];
        std::ifstream* f = raw_stream(r);
        if (!f) { synth_empty(out, lba); return true; }
        f->clear();
        f->seekg((std::streamoff)rel * RAW_SEC);
        if (!f->read((char*)out, RAW_SEC)) { synth_empty(out, lba); }
        return true;
    }
    case X_MEM: {
        synth_form1(out, lba, x->mem->data() + (size_t)rel * USER_SEC, x->submodes[rel]);
        return true;
    }
    case X_FORM1: {
        Entry& e = entries[x->idx];
        uint8_t data[USER_SEC];
        std::memset(data, 0, sizeof data);
        std::ifstream* f = entry_stream(e);
        if (f) {
            f->clear();
            const uint64_t off = (uint64_t)rel * USER_SEC;
            if (off < e.host_size) {
                f->seekg((std::streamoff)off);
                const uint64_t want = std::min<uint64_t>(USER_SEC, e.host_size - off);
                f->read((char*)data, (std::streamsize)want);
                f->clear();
            }
        }
        for (const auto& p : e.patches) {
            const uint64_t off = (uint64_t)rel * USER_SEC;
            for (size_t k = 0; k < p.second.size(); k++) {
                const uint64_t a = p.first + k;
                if (a >= off && a < off + USER_SEC) data[a - off] = p.second[k];
            }
        }
        synth_form1(out, lba, data, rel + 1 == x->sectors ? 0x89 : 0x08);
        return true;
    }
    case X_RAW2336: {
        Entry& e = entries[x->idx];
        uint8_t body[RAW_SEC];
        std::memset(body, 0, sizeof body);
        std::ifstream* f = entry_stream(e);
        if (f) {
            f->clear();
            if (e.raw_has_sync) {
                f->seekg((std::streamoff)rel * RAW_SEC);
                f->read((char*)body, RAW_SEC);
                f->clear();
                synth_raw2336(out, lba, body + 16);
            } else {
                f->seekg((std::streamoff)rel * F2_SEC);
                f->read((char*)body, F2_SEC);
                f->clear();
                synth_raw2336(out, lba, body);
            }
        } else {
            synth_empty(out, lba);
        }
        return true;
    }
    case X_AUDIO_PREGAP: {
        std::memset(out, 0, RAW_SEC);
        TrackSpec& t = tracks[x->idx];
        std::ifstream* f = track_stream(t, true);
        if (f) { f->clear(); f->seekg((std::streamoff)rel * RAW_SEC); f->read((char*)out, RAW_SEC); f->clear(); }
        return true;
    }
    case X_AUDIO_PCM: {
        std::memset(out, 0, RAW_SEC);
        TrackSpec& t = tracks[x->idx];
        const uint64_t off = (uint64_t)rel * RAW_SEC;
        if (off >= t.pcm_bytes) return true;
        const uint64_t want = std::min<uint64_t>(RAW_SEC, t.pcm_bytes - off);
        if (t.from_mem) {
            std::memcpy(out, t.pcm_mem.data() + off, (size_t)want);
        } else {
            std::ifstream* f = track_stream(t, false);
            if (f) { f->clear(); f->seekg((std::streamoff)(t.pcm_offset + off)); f->read((char*)out, (std::streamsize)want); f->clear(); }
        }
        return true;
    }
    }
    return false;
}

/* ───────────────────────── public API ─────────────────────────────────── */

DiscTree::DiscTree() = default;
DiscTree::~DiscTree() { Close(); }

bool DiscTree::IsTree(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec) && fs::is_regular_file(p / "disc.toml", ec);
}

bool DiscTree::Open(const fs::path& dir, const DiscTreeHints* hints, std::string* error) {
    Close();
    if (!IsTree(dir)) {
        if (error) *error = dir.string() + " is not a disc tree (no disc.toml)";
        return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->dir = dir;
    impl_->notes = &notes_;
    if (hints) impl_->hints = *hints;
    dir_ = dir;
    if (!impl_->load_manifest(error) || !impl_->scan_host(error) || !impl_->resolve_audio(error) ||
        !impl_->layout(error)) {
        impl_.reset();
        notes_.clear();
        return false;
    }
    impl_->patch_exe_tables();
    impl_->build_metadata();
    total_sectors_ = impl_->total_sectors;
    data_sectors_ = impl_->data_sectors;
    pristine_layout_ = impl_->pristine;
    ram_patches_ = impl_->ram_patches;
    tracks_.clear();
    for (const TrackSpec& t : impl_->tracks) {
        DiscTreeTrack d;
        d.number = t.number; d.is_audio = t.audio;
        d.start_lba = t.start_lba; d.pregap_lba = t.pregap_lba; d.sectors = t.total;
        tracks_.push_back(d);
    }
    {
        const std::vector<uint8_t>& pvd = impl_->descriptors[0];
        std::string vol((const char*)&pvd[40], 32);
        const size_t last = vol.find_last_not_of(" \t\0");
        volume_id_ = last == std::string::npos ? std::string() : vol.substr(0, last + 1);
    }
    open_ = true;
    return true;
}

void DiscTree::Close() {
    impl_.reset();
    tracks_.clear();
    notes_.clear();
    ram_patches_.clear();
    total_sectors_ = data_sectors_ = 0;
    open_ = false;
    pristine_layout_ = true;
}

bool DiscTree::ReadRaw(uint32_t lba, uint8_t* out) {
    if (!open_ || !out) return false;
    return impl_->read_raw(lba, out);
}

std::string DiscTree::DescribeLayout() const {
    if (!open_) return std::string();
    std::ostringstream o;
    o << "disc tree " << dir_.string() << "\n";
    o << "  data track " << data_sectors_ << " sectors, disc " << total_sectors_ << " sectors"
      << (pristine_layout_ ? " (pristine layout)" : " (modified layout)") << "\n";
    for (const DiscTreeTrack& t : tracks_)
        o << "  track " << t.number << (t.is_audio ? " audio " : " data  ") << "pregap@" << t.pregap_lba
          << " start@" << t.start_lba << " sectors " << t.sectors << "\n";
    for (const Extent& x : impl_->extents) {
        o << "  " << x.lba << " +" << x.sectors << "  ";
        switch (x.kind) {
        case X_RAWFILE: o << (x.idx < 0 ? "system area" : "raw run " + impl_->raw_runs[x.idx].file.filename().string()); break;
        case X_MEM: o << (x.mem == &impl_->desc_set ? "volume descriptors" :
                          x.mem == &impl_->pt_l_data ? "path table (L)" :
                          x.mem == &impl_->pt_m_data ? "path table (M)" : "directory");
            if (x.mem != &impl_->desc_set && x.mem != &impl_->pt_l_data && x.mem != &impl_->pt_m_data)
                for (const Entry& e : impl_->entries) if (&e.data == x.mem) o << " " << (e.path.empty() ? "/" : e.path);
            break;
        case X_FORM1: o << "form1 " << impl_->entries[x.idx].path << (impl_->entries[x.idx].moved ? " (relocated)" : ""); break;
        case X_RAW2336: o << "raw   " << impl_->entries[x.idx].path << (impl_->entries[x.idx].moved ? " (relocated)" : ""); break;
        case X_AUDIO_PREGAP: o << "pregap track " << impl_->tracks[x.idx].number; break;
        case X_AUDIO_PCM: o << "audio  track " << impl_->tracks[x.idx].number; break;
        }
        o << "\n";
    }
    for (const std::string& n : notes_) o << "  note: " << n << "\n";
    return o.str();
}

} // namespace PS1
