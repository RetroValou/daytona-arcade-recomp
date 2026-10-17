#pragma once

// The package's files: the set's data folder on ux0:, the ROM set and the saved
// settings in it, files written whole through a temporary (a power cut keeps the
// old or the new file, never half of one), and the game's non-volatile memories.

#include <cstddef>
#include <cstdint>
#include <string>

#ifndef M2_ROMSET
#define M2_ROMSET "daytona93"
#endif

namespace vita {

constexpr const char *kDirectory = "ux0:data/" M2_ROMSET;
constexpr const char *kRom = "ux0:data/" M2_ROMSET "/" M2_ROMSET ".zip";
constexpr const char *kConfig = "ux0:data/" M2_ROMSET "/vita.cfg";

bool app_file(const std::string &path); // the file exists and can be read
// Exactly `size` bytes, or false (missing file, other size).
bool load_bytes(const std::string &path, uint8_t *data, size_t size);
// Written to path.tmp, then renamed over path.
bool save_bytes(const std::string &path, const uint8_t *data, size_t size);

// A board memory saved in kDirectory (ioboard_eeprom.bin, backup_ram.bin): any
// container with data() and size(). A missing or wrong-sized file leaves it as is.
template<class C> void load_nv(const char *name, C &data) {
    load_bytes(std::string(kDirectory) + "/" + name, data.data(), data.size());
}
template<class C> bool save_nv(const char *name, const C &data) {
    return save_bytes(std::string(kDirectory) + "/" + name, data.data(), data.size());
}

} // namespace vita
