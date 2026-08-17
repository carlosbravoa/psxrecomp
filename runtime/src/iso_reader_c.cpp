/*
 * iso_reader_c.cpp — C wrapper for the C++ ISOReader class
 *
 * Provides iso_open() / iso_read_sector() / iso_close() for use by cdrom.c
 */

#include "iso_reader.h"
#include "mod_runtime.h"
#include <cstdio>
#include <string>
#include <vector>

extern "C" {

void* iso_open(const char* path) {
    auto* reader = new PS1::ISOReader();
    if (!reader->Open(path)) {
        delete reader;
        return nullptr;
    }
    return reader;
}

int iso_read_sector(void* handle, uint32_t lba, uint8_t* buffer, int size) {
    if (!handle) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    (void)size; /* ReadSector always reads 2048 bytes */
    if (!reader->ReadSector(lba, buffer)) return 0;
    mod_runtime_patch_disc_sector(lba, 0, buffer, 2048);
    return 1;
}

int iso_read_raw_sector(void* handle, uint32_t lba, uint8_t* buffer, int size) {
    if (!handle || size < 2352) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    if (!reader->ReadRawSector(lba, buffer)) return 0;
    mod_runtime_patch_disc_sector(lba, 1, buffer, 2352);
    return 1;
}

uint32_t iso_sector_count(void* handle) {
    if (!handle) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    return reader->GetSectorCount();
}

/* CD-track TOC accessors (multi-track / CD-DA support). track is 1-based. */
int iso_track_count(void* handle) {
    if (!handle) return 1;
    return static_cast<PS1::ISOReader*>(handle)->TrackCount();
}

uint32_t iso_track_start_lba(void* handle, int track) {
    if (!handle) return 0;
    return static_cast<PS1::ISOReader*>(handle)->TrackStartLBA(track);
}

uint32_t iso_track_pregap_lba(void* handle, int track) {
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    return reader ? reader->TrackPregapLBA(track) : 0;
}

int iso_track_is_audio(void* handle, int track) {
    if (!handle) return 0;
    return static_cast<PS1::ISOReader*>(handle)->TrackIsAudio(track) ? 1 : 0;
}

/* Path of the file that contains disc sector `lba` ("DIR/NAME.EXT", no
 * version suffix), or 0 when no file covers it. The whole directory tree is
 * walked once per reader and cached (a few hundred entries on a PSX disc);
 * used by the FMV pack to name the movie the CD is streaming. */
namespace {
struct IsoLbaEntry { uint32_t lba, sectors; std::string path; };
struct IsoLbaTable { const void* owner = nullptr; std::vector<IsoLbaEntry> entries; };
IsoLbaTable g_lba_table;
void iso_walk(PS1::ISOReader* r, uint32_t dir_lba, uint32_t dir_size, const std::string& prefix,
              std::vector<IsoLbaEntry>& out, int depth) {
    if (depth > 8) return;
    for (const auto& e : r->ListFilesByLBA(dir_lba, dir_size)) {
        std::string name = e.name;
        const size_t sc = name.find(';');
        if (sc != std::string::npos) name.erase(sc);
        if (e.is_directory) iso_walk(r, e.lba, e.size, prefix + name + "/", out, depth + 1);
        else out.push_back({ e.lba, (e.size + 2047u) / 2048u, prefix + name });
    }
}
}

int iso_path_for_lba(void* handle, uint32_t lba, char* out, int cap) {
    if (!handle || !out || cap <= 0) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    if (g_lba_table.owner != handle) {
        g_lba_table.entries.clear();
        PS1::RootDirectoryInfo root = reader->GetRootDirectory();
        iso_walk(reader, root.lba, root.size, "", g_lba_table.entries, 0);
        g_lba_table.owner = handle;
    }
    for (const auto& e : g_lba_table.entries) {
        if (lba >= e.lba && lba < e.lba + (e.sectors ? e.sectors : 1u)) {
            std::snprintf(out, (size_t)cap, "%s", e.path.c_str());
            return 1;
        }
    }
    return 0;
}

void iso_close(void* handle) {
    if (g_lba_table.owner == handle) { g_lba_table.owner = nullptr; g_lba_table.entries.clear(); }
    if (!handle) return;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    reader->Close();
    delete reader;
}

} /* extern "C" */
