// Minimal ZIP reader for the user's ROM set: the central directory, stored
// and deflated members, CRC-32. No dependencies (inflate is RFC 1951 written
// here), so the game can import its ROMs with nothing but itself.
#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace rt {

struct ZipError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc = 0);

class Zip {
public:
    // Reads the central directory; members are read from the file when asked
    // for (the archive is not held in memory: Dead or Alive's is 32 MB of the
    // Vita's heap).
    explicit Zip(const std::string &path); // throws ZipError
    struct Entry {
        uint32_t crc = 0, csize = 0, usize = 0, local = 0;
        uint16_t method = 0;
    };
    const std::map<std::string, Entry> &entries() const { return entries_; }
    bool has(const std::string &name) const { return entries_.count(name) != 0; }
    std::vector<uint8_t> read(const std::string &name) const; // decompressed; CRC checked against the directory

private:
    // n bytes at offset `at` of the file; ZipError (what) if past its end
    std::vector<uint8_t> bytes(uint64_t at, size_t n, const std::string &what) const;

    mutable std::ifstream file_;
    uint64_t size_ = 0;
    std::map<std::string, Entry> entries_;
};

// RFC 1951 raw deflate stream -> out (out.size() = expected length).
void inflate(const uint8_t *in, size_t in_len, std::vector<uint8_t> &out);

} // namespace rt
