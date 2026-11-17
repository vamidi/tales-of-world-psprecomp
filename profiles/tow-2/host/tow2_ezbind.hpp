#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

namespace tow2 {

// One file record in an EZBIND archive.
//
// On-disk layout (all little-endian, observed v4 archives):
//   0x00  char   magic[8]   "EZBIND\0\0"
//   0x08  u32    count
//   0x0C  u32    version    (4 for every tow-2 archive inspected)
//   0x10  entry  entries[count]   (16 bytes each)
//   0x..  char   name_table[]     (entry.name_offset points here, absolute)
//   ..    u8     data[]           (entry.data_offset, 4-byte aligned)
//
//   struct entry {
//       u32 name_offset;  // absolute offset into the archive file
//       u32 size;         // uncompressed byte size
//       u32 data_offset;  // absolute offset into the archive file
//       u32 hash;         // engine hash (not CRC32 of data or of the name)
//   };
//
// EZBIND is a plain file container: nothing in the format depends on PSP
// hardware, so the recompiled guest parser is valid as-is. This reader exists
// so the host can inspect archives and, where a native path is preferable,
// serve entries without going through guest code.
struct EzbindEntry {
    std::string name;
    std::uint32_t name_offset = 0;
    std::uint32_t size = 0;
    std::uint32_t data_offset = 0;
    std::uint32_t hash = 0;
};

class EzbindArchive {
public:
    bool load(const std::filesystem::path &path, std::string *error = nullptr);
    bool load_from_memory(std::vector<std::uint8_t> bytes, std::string *error = nullptr);

    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] std::uint32_t version() const { return version_; }
    [[nodiscard]] const std::filesystem::path &path() const { return path_; }
    [[nodiscard]] const std::vector<EzbindEntry> &entries() const { return entries_; }

    [[nodiscard]] const EzbindEntry *find(std::string_view name) const;
    bool read(const EzbindEntry &entry, std::vector<std::uint8_t> &out) const;

private:
    std::filesystem::path path_;
    std::vector<std::uint8_t> bytes_;
    std::vector<EzbindEntry> entries_;
    std::unordered_map<std::string, std::size_t> by_name_;
    std::uint32_t version_ = 0;
    bool valid_ = false;
};

} // namespace tow2
