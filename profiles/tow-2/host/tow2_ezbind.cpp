#include "tow2_ezbind.hpp"

#include <cstring>
#include <fstream>
#include <limits>

namespace tow2 {
namespace {

constexpr std::uint32_t kHeaderSize = 0x10u;
constexpr std::uint32_t kEntrySize = 0x10u;
constexpr std::uint32_t kMaxEntries = 1u << 20;

std::uint32_t read_u32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) |
           (static_cast<std::uint32_t>(p[3]) << 24u);
}

bool fail(std::string *error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

} // namespace

bool EzbindArchive::load(const std::filesystem::path &path, std::string *error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail(error, "cannot open " + path.string());

    stream.seekg(0, std::ios::end);
    const std::streamoff length = stream.tellg();
    if (length <= 0) return fail(error, "empty file " + path.string());
    stream.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    stream.read(reinterpret_cast<char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!stream) return fail(error, "short read " + path.string());

    path_ = path;
    return load_from_memory(std::move(bytes), error);
}

bool EzbindArchive::load_from_memory(std::vector<std::uint8_t> bytes,
                                     std::string *error) {
    valid_ = false;
    entries_.clear();
    by_name_.clear();
    version_ = 0;

    static constexpr char kMagic[8] = {'E', 'Z', 'B', 'I', 'N', 'D', 0, 0};
    if (bytes.size() < kHeaderSize) return fail(error, "truncated header");
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
        return fail(error, "not an EZBIND archive");

    const std::uint32_t count = read_u32(bytes.data() + 0x08);
    const std::uint32_t version = read_u32(bytes.data() + 0x0C);
    if (count == 0u || count > kMaxEntries)
        return fail(error, "implausible entry count " + std::to_string(count));
    if (kHeaderSize + static_cast<std::uint64_t>(count) * kEntrySize >
        bytes.size())
        return fail(error, "entry table exceeds file");

    std::vector<EzbindEntry> parsed;
    parsed.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t *raw = bytes.data() + kHeaderSize + i * kEntrySize;
        EzbindEntry entry{};
        entry.name_offset = read_u32(raw + 0x00);
        entry.size = read_u32(raw + 0x04);
        entry.data_offset = read_u32(raw + 0x08);
        entry.hash = read_u32(raw + 0x0C);

        if (entry.name_offset >= bytes.size())
            return fail(error, "entry " + std::to_string(i) + " name out of range");
        const std::size_t name_length =
            strnlen(reinterpret_cast<const char *>(bytes.data() + entry.name_offset),
                    bytes.size() - entry.name_offset);
        if (name_length == bytes.size() - entry.name_offset)
            return fail(error, "entry " + std::to_string(i) + " name unterminated");
        entry.name.assign(reinterpret_cast<const char *>(bytes.data() + entry.name_offset),
                          name_length);

        const std::uint64_t end =
            static_cast<std::uint64_t>(entry.data_offset) + entry.size;
        if (end > bytes.size())
            return fail(error, "entry " + std::to_string(i) + " data out of range");

        parsed.push_back(std::move(entry));
    }

    entries_ = std::move(parsed);
    for (std::size_t i = 0; i < entries_.size(); ++i)
        by_name_.emplace(entries_[i].name, i);

    bytes_ = std::move(bytes);
    version_ = version;
    valid_ = true;
    return true;
}

const EzbindEntry *EzbindArchive::find(std::string_view name) const {
    const auto it = by_name_.find(std::string(name));
    if (it == by_name_.end()) return nullptr;
    return &entries_[it->second];
}

bool EzbindArchive::read(const EzbindEntry &entry,
                         std::vector<std::uint8_t> &out) const {
    if (!valid_) return false;
    const std::uint64_t end =
        static_cast<std::uint64_t>(entry.data_offset) + entry.size;
    if (end > bytes_.size()) return false;
    out.assign(bytes_.begin() + entry.data_offset,
               bytes_.begin() + entry.data_offset + entry.size);
    return true;
}

} // namespace tow2
