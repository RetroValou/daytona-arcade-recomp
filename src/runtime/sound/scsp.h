// license:BSD-3-Clause
// copyright-holders:ElSemi, R. Belmont
//
// The Sega/Yamaha YMF292-F SCSP of the Model 2A/2B/2C sound board, and its
// DSP: MAME's scsp_device and SCSPDSP (src/devices/sound/scsp.cpp,
// scspdsp.cpp; BSD-3-Clause, notice kept), transplanted (THIRD_PARTY.md).
// The sound computation is MAME's, line for line, except where noted
// (DecodeSlot / UpdateSlotPlain: the same results, faster). What MAME's device
// framework did is the host's (ScspHost): the 68000 interrupt lines, the
// three timers (one-shot, in seconds of board time), the MIDI output, and
// memory (the 512 KiB sound RAM it shares with the 68000, big-endian). The
// host renders samples up to the current time before every register access,
// as MAME's stream update does.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace snd {

class ScspHost {
public:
    virtual ~ScspHost() = default;
    // MAME m_irq_cb(level, state): the 68000 input line `level` (1-7; 0: none).
    virtual void scsp_irq(int level, bool assert) = 0;
    // (Re)start timer n (0 A, 1 B, 2 C): it expires `seconds` from now.
    virtual void scsp_timer(int n, double seconds) = 0;
    // A byte the SCSP sends on its MIDI output (Model 2: to the i960's UART).
    virtual void scsp_midi_out(uint8_t byte) = 0;
    // At a slot's key-on: whether it plays a sound effect rather than the
    // music (the driver's own bookkeeping; false when unknown).
    virtual bool scsp_slot_effect(int) { return false; }
};

// MAME scspdsp.cpp's helpers, shared by the DSP interpreter (scsp.cpp) and the
// recompiled DSP programs (tools/m2dsprecomp).
namespace dsp {
inline int32_t sext(int32_t v, int bits) { return int32_t(uint32_t(v) << (32 - bits)) >> (32 - bits); }
inline uint16_t PACK(int32_t val) {
    int const sign = (val >> 23) & 1;
    uint32_t const temp = uint32_t(val ^ (val << 1)) & 0xFFFFFF;
    // MAME counts temp's leading zeros (24 bits) one by one, up to 12; one CLZ
    // here (temp == 0: 32, so 12 as well). Same results for every val.
    int const exponent = std::min(12, std::countl_zero(temp << 8));
    if (exponent < 12) val = (val << exponent) & 0x3FFFFF;
    else val <<= 11;
    val >>= 11;
    val &= 0x7FF;
    val |= sign << 15;
    val |= exponent << 11;
    return uint16_t(val);
}

inline int32_t UNPACK(uint16_t val) {
    int const sign = (val >> 15) & 1;
    int exponent = (val >> 11) & 0xF;
    int const mantissa = val & 0x7FF;
    int32_t uval = mantissa << 11;
    if (exponent > 11) {
        exponent = 11;
        uval |= sign << 22;
    } else {
        uval |= (sign ^ 1) << 22;
    }
    uval |= sign << 23;
    uval <<= 8;
    uval >>= 8;
    uval >>= exponent;
    return uval;
}
} // namespace dsp

struct ScspDsp;
// A recompiled DSP program (tools/m2dsprecomp's dsp_gen.cpp sets scsp_dsp_native at
// start-up): ScspDsp::Step runs `step` instead of the interpreter while MPRO's first
// steps x 4 words are `mpro` and LastStep is `steps`. Null: the interpreter only.
struct ScspDspNative {
    const uint16_t *mpro;
    int steps;
    void (*step)(ScspDsp &);
};
extern const ScspDspNative *scsp_dsp_native;

// MAME SCSPDSP.
struct ScspDsp {
    uint8_t *ram = nullptr; // the sound RAM (big-endian 16-bit words), 0x80000 bytes
    uint32_t RBP; // Ring buf pointer
    uint32_t RBL; // Delay ram (Ring buffer) size in words

    int16_t COEF[64];    // 16 bit signed
    uint16_t MADRS[32];  // offsets (in words), 16 bit
    uint16_t MPRO[128 * 4]; // 128 steps 64 bit
    int32_t TEMP[128];   // TEMP regs, 24 bit signed
    int32_t MEMS[32];    // MEMS regs, 24 bit signed
    uint32_t DEC;

    int32_t MIXS[16];    // MIXS, 24 bit signed
    int16_t EXTS[2];     // External inputs (CDDA) 16 bit signed

    int16_t EFREG[16];   // EFREG, 16 bit signed

    bool Stopped;
    int LastStep;
    // The recompiled program in use (scsp_dsp_native), once checked against MPRO.
    bool native_checked;
    void (*native)(ScspDsp &);

    void Init();
    void SetSample(int32_t sample, int32_t SEL, int32_t MXL);
    void Step();
    void Start();

    // The sound RAM, as the DSP sees it (big-endian words; the recompiled programs too).
    uint16_t read_word(uint32_t a) const { a &= ~1u; return a < 0x80000 ? uint16_t(ram[a] << 8 | ram[a + 1]) : 0; }
    void write_word(uint32_t a, uint16_t v) {
        a &= ~1u;
        if (a < 0x80000) {
            ram[a] = uint8_t(v >> 8);
            ram[a + 1] = uint8_t(v);
        }
    }
};

class Scsp {
public:
    static constexpr uint32_t kClock = 22579200; // 45.1584 MHz / 2 on the Model 2A-CRX video board
    static constexpr uint32_t kRate = kClock / 512; // 44100 Hz

    Scsp(ScspHost &host, uint8_t *ram); // ram: 0x80000 bytes, the 68000's view (big-endian words)

    // The 68000's register accesses (MAME read/write: offset in words).
    uint16_t read(uint32_t offset);
    void write(uint32_t offset, uint16_t data, uint16_t mem_mask);
    // A byte received on the MIDI input (MAME rcv_complete).
    void midi_in(uint8_t byte);
    // The music and effects volumes (0..1; both 1, the default: MAME's output,
    // untouched). Each slot's output, direct and to the DSP, is scaled by its
    // owner's volume (ScspHost::scsp_slot_effect at its key-on).
    void set_volumes(float music, float effects) {
        music_ = music, effects_ = effects;
        unity_ = music == 1.0f && effects == 1.0f;
    }
    // Timer n expired (MAME timerA_cb ...).
    void timer_expired(int n);
    // Render n samples (MAME sound_stream_update): interleaved stereo, the
    // stream's values as MAME puts them (-1..1), times the master volume
    // (MAME's output gain MVOL / 15).
    void render(float *stereo, int n);

private:
    enum SCSP_STATE { SCSP_ATTACK, SCSP_DECAY1, SCSP_DECAY2, SCSP_RELEASE };

    struct SCSP_EG_t {
        int volume;
        SCSP_STATE state;
        int step;
        int AR;  // Attack
        int D1R; // Decay1
        int D2R; // Decay2
        int RR;  // Release
        int DL;  // Decay level
        uint8_t EGHOLD;
        uint8_t LPLINK;
    };

    struct SCSP_LFO_t {
        uint16_t phase;
        uint32_t phase_step;
        int *table;
        int *scale;
    };

    struct SCSP_SLOT {
        union {
            uint16_t data[0x10]; // only 0x1a bytes used
            uint8_t datab[0x20];
        } udata;
        uint8_t Backwards; // the wave is playing backwards
        uint8_t active;    // this slot is currently playing
        uint32_t cur_addr; // current play address (24.8)
        uint32_t nxt_addr; // next play address
        uint32_t step;     // pitch step (24.8)
        SCSP_EG_t EG;      // Envelope
        SCSP_LFO_t PLFO;   // Phase LFO
        SCSP_LFO_t ALFO;   // Amplitude LFO
        int slot;
        int16_t Prev; // Previous sample (for interpolation)
        bool effect;  // keyed for a sound effect (set_volumes), else the music
        // Not MAME's: the slot registers render() reads on every sample, decoded
        // when they change (w16 sets `decoded` false), and the gains they select
        // from the pan tables. `plain`: the common case UpdateSlotPlain renders
        // (sound RAM data, no LFO, no modulation); otherwise MAME's UpdateSlot.
        struct Decoded {
            uint32_t sa;
            uint16_t lsa, lea;
            uint8_t lpctl, sbctl, isel, imxl;
            bool pcm8, sdir, stwinh, lpslnk, plain;
            int dsp_gain, left_gain, right_gain, ring_gain; // m_LPANTABLE / m_RPANTABLE entries
        } dec;
        bool decoded; // zeroed with the slots (Scsp's constructor): decoded at first use
    };

    ScspHost &host_;
    uint8_t *ram_;
    float music_ = 1.0f, effects_ = 1.0f;
    bool unity_ = true; // both volumes 1: the samples are not touched
    uint32_t rand_ = 0x12345678; // machine().rand() stand-in (noise; LFO noise tables)
    uint32_t rand();
    int8_t read_byte(uint32_t a) const { return a < 0x80000 ? int8_t(ram_[a]) : 0; }
    int16_t read_word(uint32_t a) const { a &= ~1u; return a < 0x80000 ? int16_t(ram_[a] << 8 | ram_[a + 1]) : 0; }
    void write_word(uint32_t a, uint16_t v) { a &= ~1u; if (a < 0x80000) { ram_[a] = uint8_t(v >> 8); ram_[a + 1] = uint8_t(v); } }

    union {
        uint16_t data[0x30 / 2];
        uint8_t datab[0x30];
    } m_udata;

    SCSP_SLOT m_Slots[32];
    int16_t m_RINGBUF[128];
    uint8_t m_BUFPTR = 0;

    uint32_t m_IrqTimA = 0;
    uint32_t m_IrqTimBC = 0;
    uint32_t m_IrqMidi = 0;
    uint32_t m_IrqCPU = 0;
    uint32_t m_IrqDMA = 0;

    uint8_t m_latched_MSLC = 0;
    uint16_t m_latched_MSLC_data = 0;
    uint8_t m_MidiOutStack[32];
    uint8_t m_MidiOutW = 0, m_MidiOutR = 0;
    uint8_t m_MidiStack[32];
    uint8_t m_MidiW = 0, m_MidiR = 0;

    int32_t m_EG_TABLE[0x400];
    int m_LPANTABLE[0x10000];
    int m_RPANTABLE[0x10000];

    int m_TimPris[3];
    int m_TimCnt[3];

    struct {
        uint32_t dmea;
        uint16_t drga;
        uint16_t dtlg;
        uint8_t dgate;
        uint8_t ddir;
    } m_dma{};

    uint16_t m_mcieb = 0;
    uint16_t m_mcipd = 0;

    int m_ARTABLE[64], m_DRTABLE[64];

    ScspDsp m_DSP;

    int16_t *m_RBUFDST = nullptr; // this points to where the sample will be stored in the RingBuf

    int m_PLFO_TRI[256], m_PLFO_SQR[256], m_PLFO_SAW[256], m_PLFO_NOI[256];
    int m_ALFO_TRI[256], m_ALFO_SQR[256], m_ALFO_SAW[256], m_ALFO_NOI[256];
    int m_PSCALES[8][256];
    int m_ASCALES[8][256];

    void exec_dma();
    uint8_t DecodeSCI(uint8_t irq);
    void CheckPendingIRQ();
    void MainCheckPendingIRQ(uint16_t irq_type);
    void ResetInterrupts();
    int Get_AR(int base, int R);
    int Get_DR(int base, int R);
    void Compute_EG(SCSP_SLOT *slot);
    // Inlined into render()'s per-sample path (UpdateSlotPlain): a call per slot and sample.
    [[gnu::always_inline]] inline int EG_Update(SCSP_SLOT *slot);
    uint32_t Step(SCSP_SLOT *slot);
    void Compute_LFO(SCSP_SLOT *slot);
    void StartSlot(SCSP_SLOT *slot);
    void StopSlot(SCSP_SLOT *slot, int keyoff);
    void init();
    void UpdateSlotReg(int s, int r);
    void UpdateReg(int reg);
    void UpdateRegR(int reg);
    void w16(uint32_t addr, uint16_t val);
    uint16_t r16(uint32_t addr);
    int32_t UpdateSlot(SCSP_SLOT *slot);
    [[gnu::always_inline]] inline int32_t UpdateSlotPlain(SCSP_SLOT *slot);
    void DecodeSlot(SCSP_SLOT *slot);
    void start_timer(int n, int reg);

    void LFO_Init();
    int32_t PLFO_Step(SCSP_LFO_t *LFO);
    int32_t ALFO_Step(SCSP_LFO_t *LFO);
    void LFO_ComputeStep(SCSP_LFO_t *LFO, uint32_t LFOF, uint32_t LFOWS, uint32_t LFOS, int ALFO);
};

} // namespace snd
