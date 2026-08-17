#pragma once
/*
 * disc_tree.h — mount an *extracted disc tree* as a virtual raw CD image.
 *
 * A disc tree (docs/DISC_TREE.md, produced by tools/disc_tree.py) is a plain
 * directory: `disc.toml` (layout manifest), `cdrom/` (the ISO9660 files),
 * `audio/trackNN.wav` (CD-DA) and `meta/` (licence area, PVD, raw runs).
 * DiscTree re-synthesises the disc sector by sector — ISO9660 descriptors,
 * path tables and directory records are rebuilt from the manifest, Mode 2
 * Form 1 sectors get their subheader/EDC/ECC computed, raw (Form 2 / XA / STR)
 * files and CD-DA tracks are served verbatim — so an unmodified tree is
 * byte-identical to the dump it came from, while an edited, grown or added
 * file is served in place (relocated after the original data area when it no
 * longer fits its extent).
 *
 * PS1::ISOReader::Open() accepts a tree directory and routes through this
 * class, so every consumer of the reader (the CD-ROM controller, disc
 * identity / TOC fingerprint, the text-image guard) sees the same disc.
 */

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace PS1 {

/* A game-side table of hardcoded {LBA, size} pairs (Capcom-style titles keep
 * one instead of using filenames). When the tree relocates or resizes a file,
 * the entry that named its ORIGINAL LBA is rewritten in the boot EXE as it is
 * served, and reported through RamPatches() so the text-image guard can bless
 * the bytes. Offsets are byte offsets within one entry; -1 = absent. */
struct DiscTreeLbaTable {
    uint32_t address = 0;      /* RAM address of entry 0 (e.g. 0x80136F7C)   */
    uint32_t count = 0;        /* number of entries                          */
    uint32_t stride = 8;       /* bytes per entry                            */
    int32_t  lba_offset = 0;   /* u32 LE sector number within the entry      */
    int32_t  size_offset = 4;  /* u32 LE byte size within the entry          */
    bool     lba_is_msf = false; /* entry stores BCD MM:SS:FF instead of LBA */
};

struct DiscTreeHints {
    std::string exe_name;                    /* boot EXE name on disc; "" = SYSTEM.CNF */
    std::vector<DiscTreeLbaTable> lba_tables;
};

struct DiscTreeTrack {
    int      number = 1;
    bool     is_audio = false;
    uint32_t start_lba = 0;    /* INDEX 01 */
    uint32_t pregap_lba = 0;   /* INDEX 00 (== start when no pregap) */
    uint32_t sectors = 0;      /* pregap + payload */
};

struct DiscTreeRamPatch {
    uint32_t address = 0;
    std::vector<uint8_t> bytes;
};

class DiscTree {
public:
    DiscTree();
    ~DiscTree();

    /* True when `p` is a directory holding a disc.toml manifest. */
    static bool IsTree(const std::filesystem::path& p);

    bool Open(const std::filesystem::path& dir, const DiscTreeHints* hints, std::string* error);
    void Close();
    bool IsOpen() const { return open_; }

    uint32_t SectorCount() const { return total_sectors_; }
    uint32_t DataTrackSectors() const { return data_sectors_; }
    const std::vector<DiscTreeTrack>& Tracks() const { return tracks_; }
    const std::string& VolumeId() const { return volume_id_; }
    const std::filesystem::path& Dir() const { return dir_; }

    /* Full 2352-byte raw sector (sync + header + subheader + data + EDC/ECC,
     * or 2352 bytes of little-endian PCM inside an audio track). */
    bool ReadRaw(uint32_t lba, uint8_t* out);

    /* Layout notes for the log: relocated / added / missing files, table
     * patches, track moves. Empty when the tree is pristine. */
    const std::vector<std::string>& Notes() const { return notes_; }
    /* True when the served disc is exactly the source dump layout. */
    bool IsPristineLayout() const { return pristine_layout_; }
    /* Bytes rewritten in the boot EXE (RAM addresses) — see DiscTreeLbaTable. */
    const std::vector<DiscTreeRamPatch>& RamPatches() const { return ram_patches_; }

    /* Human-readable layout dump (one line per object). */
    std::string DescribeLayout() const;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    std::filesystem::path dir_;
    std::string volume_id_;
    std::vector<DiscTreeTrack> tracks_;
    std::vector<std::string> notes_;
    std::vector<DiscTreeRamPatch> ram_patches_;
    uint32_t total_sectors_ = 0;
    uint32_t data_sectors_ = 0;
    bool open_ = false;
    bool pristine_layout_ = true;
};

/* CD-ROM sector protection helpers (ECMA-130 / Yellow Book), exposed for
 * tests and tools. `edc` covers [16, 2072) for Mode 2 Form 1 and [16, 2348)
 * for Form 2; `ecc` fills the P/Q parity of a Mode 2 Form 1 sector (the header
 * is treated as zero, as XA requires). */
uint32_t cd_edc_compute(const uint8_t* data, size_t len);
void     cd_ecc_generate_mode2_form1(uint8_t* sector /* 2352 */);
void     cd_lba_to_msf_bcd(uint32_t lba, uint8_t out[3]);

} // namespace PS1
