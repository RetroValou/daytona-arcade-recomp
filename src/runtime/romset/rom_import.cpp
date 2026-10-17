#include "runtime/romset/rom_import.h"

#include "runtime/romset/archive.h"
#include "runtime/romset/zip.h"

#include <cstdio>
#include <cstring>

namespace rt {

namespace {

std::string hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

} // namespace

std::vector<RomCheck> check_rom_set(const std::string &zip_path) {
    std::vector<RomCheck> out;
    const auto z = open_archive(zip_path);
    for (const RomLoad &l : game().roms) {
        RomCheck c;
        c.file = l.file;
        const auto it = z->entries().find(l.file);
        if (it == z->entries().end()) c.problem = "missing";
        else if (it->second.size != l.size) c.problem = "wrong size";
        else if (it->second.has_crc && it->second.crc != l.crc)
            c.problem = "wrong CRC " + hex8(it->second.crc) + " (expected " + hex8(l.crc) + ")";
        else c.ok = true;
        out.push_back(c);
    }
    return out;
}

M2Board::Images import_rom_set(const std::string &zip_path) {
    const auto z = open_archive(zip_path);
    M2Board::Images img;
    img.program.assign(0x200000, 0);
    img.main_data.assign(0x2000000, 0);
    img.copro_data.assign(0x800000, 0);
    img.polygons.assign(0x1000000, 0);
    img.textures.assign(0x1000000, 0);
    img.copro_tables.assign(0x40000, 0);
    const Game &set = game();
    if (set.sound == SoundBoardType::Model1) {
        img.sound_program.assign(0x40000, 0);
        img.pcm1.assign(0x400000, 0);
        img.pcm2.assign(0x400000, 0);
    } else if (set.sound == SoundBoardType::Scsp) { // MAME's "audiocpu" and "samples" regions
        img.sound_program.assign(0x80000, 0);
        img.samples.assign(0x800000, 0);
    } // else no sound board: the images stay empty
    for (const RomLoad &l : set.roms) {
        const std::vector<uint8_t> data = z->read(l.file); // CRC against the archive's own
        if (data.size() != l.size || crc32(data.data(), data.size()) != l.crc)
            throw ZipError(std::string(l.file) + ": not the " + set.set + " ROM this build was recompiled from");
        std::vector<uint8_t> *r = nullptr;
        switch (l.region) {
        case LoadRegion::Program: r = &img.program; break;
        case LoadRegion::MainData: r = &img.main_data; break;
        case LoadRegion::CoproData: r = &img.copro_data; break;
        case LoadRegion::Polygons: r = &img.polygons; break;
        case LoadRegion::Textures: r = &img.textures; break;
        case LoadRegion::CoproTables: r = &img.copro_tables; break;
        case LoadRegion::SoundProgram: // 68000, big-endian: the files hold byte-swapped words
            for (uint32_t i = 0; i < l.size; i += 2) {
                img.sound_program[l.offset + i] = data[i + 1];
                img.sound_program[l.offset + i + 1] = data[i];
            }
            continue;
        case LoadRegion::Samples: // 16-bit big-endian words, byte-swapped in the files
            for (uint32_t i = 0; i < l.size; i += 2) {
                img.samples[l.offset + i] = data[i + 1];
                img.samples[l.offset + i + 1] = data[i];
            }
            continue;
        case LoadRegion::Pcm1: std::copy(data.begin(), data.end(), img.pcm1.begin() + l.offset); continue;
        case LoadRegion::Pcm2: std::copy(data.begin(), data.end(), img.pcm2.begin() + l.offset); continue;
        }
        for (uint32_t w = 0; w < l.size / 2; w++) {
            (*r)[l.offset + w * 4] = data[w * 2];
            (*r)[l.offset + w * 4 + 1] = data[w * 2 + 1];
        }
    }
    for (uint32_t dst = set.mirror_from + 0x100000; set.mirror_from && dst <= 0xF00000; dst += 0x100000) // ROM_COPY mirrors
        std::copy_n(img.main_data.begin() + set.mirror_from, 0x100000, img.main_data.begin() + dst);
    return img;
}

std::vector<uint8_t> dsp_program(const M2Board::Images &img) {
    const ScspDspProgram &dsp = img.game->dsp;
    if (!dsp.steps) return {};
    const size_t bytes = size_t(dsp.steps) * 8;
    if (img.sound_program.size() < size_t(dsp.offset) + bytes ||
        crc32(img.sound_program.data() + dsp.offset, bytes) != dsp.crc)
        throw ZipError("SCSP DSP program not where expected");
    const uint8_t *p = img.sound_program.data() + dsp.offset;
    return {p, p + bytes};
}

std::vector<uint8_t> tgp_program(const M2Board::Images &img) {
    const TgpProgram &tgp = game().tgp;
    const std::vector<uint8_t> &src = tgp.in_program ? img.program : img.main_data;
    if (src.size() < size_t(tgp.offset) + tgp.words * 4) throw ZipError("TGP program not where expected");
    const uint8_t *p = src.data() + tgp.offset;
    if (crc32(p, tgp.words * 4) != tgp.crc) throw ZipError("TGP program not where expected");
    return {p, p + tgp.words * 4};
}

} // namespace rt
