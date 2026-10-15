// license:BSD-3-Clause
// copyright-holders:ElSemi, R. Belmont
// thanks-to: kingshriek
//
// Sega/Yamaha YMF292-F (SCSP = Saturn Custom Sound Processor) emulation:
// MAME's scsp.cpp and scspdsp.cpp (by ElSemi, R. Belmont, kingshriek and
// others; see MAME's ChangeLog in scsp.cpp), transplanted from the MAME tree
// pinned in THIRD_PARTY.md. Changes: no device framework (ScspHost, see
// scsp.h); MAME's stream update is render(); timers report their period in
// seconds; MIDI output goes out a byte at a time at once; the random number
// generator is a fixed LCG; save states, logging and popmessages removed.
// The sound computation is MAME's.
#include "runtime/scsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace snd {

namespace {

constexpr int SHIFT = 12;
constexpr int LFO_SHIFT = 8;
uint32_t FIX(float v) { return uint32_t(float(1 << SHIFT) * v); }
constexpr int EG_SHIFT = 16;

int32_t sext(int32_t v, int bits) { return int32_t(uint32_t(v) << (32 - bits)) >> (32 - bits); }

// Envelope times in ms
const double ARTimes[64] = {100000 /*infinity*/, 100000 /*infinity*/, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0, 1700.0, 1500.0,
                            1200.0, 1000.0, 860.0, 760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0, 190.0, 150.0, 130.0, 110.0, 95.0,
                            76.0, 63.0, 55.0, 47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0, 12.0, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4,
                            2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0.0, 0.0};
const double DRTimes[64] = {100000 /*infinity*/, 100000 /*infinity*/, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0, 29600.0, 25300.0, 22200.0, 17700.0,
                            14800.0, 12700.0, 11100.0, 8900.0, 7400.0, 6300.0, 5500.0, 4400.0, 3700.0, 3200.0, 2800.0, 2200.0, 1800.0, 1600.0, 1400.0, 1100.0,
                            920.0, 790.0, 690.0, 550.0, 460.0, 390.0, 340.0, 270.0, 230.0, 200.0, 170.0, 140.0, 110.0, 98.0, 85.0, 68.0, 57.0, 49.0, 43.0, 34.0,
                            28.0, 25.0, 22.0, 18.0, 14.0, 12.0, 11.0, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1};

const float SDLT[8] = {-1000000.0f, -36.0f, -30.0f, -24.0f, -18.0f, -12.0f, -6.0f, 0.0f};

const float LFOFreq[32] = {0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f, 0.55f, 0.68f, 0.78f, 0.92f, 1.10f, 1.39f, 1.60f, 1.87f, 2.27f,
                           2.87f, 3.31f, 3.92f, 4.79f, 6.15f, 7.18f, 8.60f, 10.8f, 14.4f, 17.2f, 21.5f, 28.7f, 43.1f, 57.4f, 86.1f, 172.3f};
const float ASCALE[8] = {0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f};
const float PSCALE[8] = {0.0f, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f};

uint32_t LFIX(float v) { return uint32_t(float(1 << LFO_SHIFT) * v); }
// Convert DB to multiply amplitude
uint32_t DB(float v) { return LFIX(powf(10.0f, v / 20.0f)); }
// Convert cents to step increment
uint32_t CENTS(float v) { return LFIX(powf(2.0f, v / 1200.0f)); }

} // namespace

// SLOT PARAMETERS
#define KEYONEX(slot) ((slot->udata.data[0x0] >> 0x0) & 0x1000)
#define KEYONB(slot) ((slot->udata.data[0x0] >> 0x0) & 0x0800)
#define SBCTL(slot) ((slot->udata.data[0x0] >> 0x9) & 0x0003)
#define SSCTL(slot) ((slot->udata.data[0x0] >> 0x7) & 0x0003)
#define LPCTL(slot) ((slot->udata.data[0x0] >> 0x5) & 0x0003)
#define PCM8B(slot) ((slot->udata.data[0x0] >> 0x0) & 0x0010)
#define SA(slot) (((slot->udata.data[0x0] & 0xF) << 16) | (slot->udata.data[0x1]))
#define LSA(slot) (slot->udata.data[0x2])
#define LEA(slot) (slot->udata.data[0x3])
#define D2R(slot) ((slot->udata.data[0x4] >> 0xB) & 0x001F)
#define D1R(slot) ((slot->udata.data[0x4] >> 0x6) & 0x001F)
#define EGHOLD(slot) ((slot->udata.data[0x4] >> 0x0) & 0x0020)
#define AR(slot) ((slot->udata.data[0x4] >> 0x0) & 0x001F)
#define LPSLNK(slot) ((slot->udata.data[0x5] >> 0x0) & 0x4000)
#define KRS(slot) ((slot->udata.data[0x5] >> 0xA) & 0x000F)
#define DL(slot) ((slot->udata.data[0x5] >> 0x5) & 0x001F)
#define RR(slot) ((slot->udata.data[0x5] >> 0x0) & 0x001F)
#define STWINH(slot) ((slot->udata.data[0x6] >> 0x0) & 0x0200)
#define SDIR(slot) ((slot->udata.data[0x6] >> 0x0) & 0x0100)
#define TL(slot) ((slot->udata.data[0x6] >> 0x0) & 0x00FF)
#define MDL(slot) ((slot->udata.data[0x7] >> 0xC) & 0x000F)
#define MDXSL(slot) ((slot->udata.data[0x7] >> 0x6) & 0x003F)
#define MDYSL(slot) ((slot->udata.data[0x7] >> 0x0) & 0x003F)
#define OCT(slot) ((slot->udata.data[0x8] >> 0xB) & 0x000F)
#define FNS(slot) ((slot->udata.data[0x8] >> 0x0) & 0x03FF)
#define LFORE(slot) ((slot->udata.data[0x9] >> 0x0) & 0x8000)
#define LFOF(slot) ((slot->udata.data[0x9] >> 0xA) & 0x001F)
#define PLFOWS(slot) ((slot->udata.data[0x9] >> 0x8) & 0x0003)
#define PLFOS(slot) ((slot->udata.data[0x9] >> 0x5) & 0x0007)
#define ALFOWS(slot) ((slot->udata.data[0x9] >> 0x3) & 0x0003)
#define ALFOS(slot) ((slot->udata.data[0x9] >> 0x0) & 0x0007)
#define ISEL(slot) ((slot->udata.data[0xA] >> 0x3) & 0x000F)
#define IMXL(slot) ((slot->udata.data[0xA] >> 0x0) & 0x0007)
#define DISDL(slot) ((slot->udata.data[0xB] >> 0xD) & 0x0007)
#define DIPAN(slot) ((slot->udata.data[0xB] >> 0x8) & 0x001F)
#define EFSDL(slot) ((slot->udata.data[0xB] >> 0x5) & 0x0007)
#define EFPAN(slot) ((slot->udata.data[0xB] >> 0x0) & 0x001F)

#define MEM4B() ((m_udata.data[0] >> 0x0) & 0x0200)
#define DAC18B() ((m_udata.data[0] >> 0x0) & 0x0100)
#define MVOL() ((m_udata.data[0] >> 0x0) & 0x000F)
#define RBL() ((m_udata.data[1] >> 0x7) & 0x0003)
#define RBP() ((m_udata.data[1] >> 0x0) & 0x003F)

#define SCILV0() ((m_udata.data[0x24 / 2] >> 0x0) & 0xff)
#define SCILV1() ((m_udata.data[0x26 / 2] >> 0x0) & 0xff)
#define SCILV2() ((m_udata.data[0x28 / 2] >> 0x0) & 0xff)

#define SCIEX0 0
#define SCIEX1 1
#define SCIEX2 2
#define SCIMID 3
#define SCIDMA 4
#define SCIIRQ 5
#define SCITMA 6
#define SCITMB 7

Scsp::Scsp(ScspHost &host, uint8_t *ram) : host_(host), ram_(ram) {
    std::fill(std::begin(m_RINGBUF), std::end(m_RINGBUF), 0);
    std::fill(std::begin(m_MidiStack), std::end(m_MidiStack), 0);
    std::fill(std::begin(m_MidiOutStack), std::end(m_MidiOutStack), 0);
    std::fill(std::begin(m_TimPris), std::end(m_TimPris), 0);
    std::memset(m_Slots, 0, sizeof(m_Slots));
    std::memset(&m_udata.data, 0, sizeof(m_udata.data));
    m_TimCnt[0] = m_TimCnt[1] = m_TimCnt[2] = 0;
    init();
}

uint32_t Scsp::rand() { // a fixed linear congruential generator in place of machine().rand()
    rand_ = rand_ * 1103515245u + 12345u;
    return rand_ >> 8;
}

void Scsp::render(float *stereo, int n) {
    // MAME DoMasterSamples
    const float gain = float(MVOL()) / 15.0f; // set_output_gain(MVOL() / 15.0)
    for (int s = 0; s < n; ++s) {
        int32_t smpl = 0, smpr = 0;

        for (int sl = 0; sl < 32; ++sl) {
            m_RBUFDST = m_RINGBUF + m_BUFPTR;
            if (m_Slots[sl].active) {
                SCSP_SLOT *slot = m_Slots + sl;
                uint16_t Enc;

                int32_t sample = UpdateSlot(slot);
                if (!unity_) sample = int32_t(float(sample) * (slot->effect ? effects_ : music_));

                // SDIR bypasses the EG and the TL attenuator in both the DSP
                // input feed and the direct-output mix (MAME's comment).
                uint16_t eff_tl = SDIR(slot) ? 0 : TL(slot);
                Enc = uint16_t((eff_tl << 0x0) | (IMXL(slot) << 0xd));
                m_DSP.SetSample((sample * m_LPANTABLE[Enc]) >> (SHIFT - 2), ISEL(slot), IMXL(slot));
                uint16_t dir_tl = SDIR(slot) ? 0 : TL(slot);
                Enc = uint16_t((dir_tl << 0x0) | (DIPAN(slot) << 0x8) | (DISDL(slot) << 0xd));
                smpl += (sample * m_LPANTABLE[Enc]) >> SHIFT;
                smpr += (sample * m_RPANTABLE[Enc]) >> SHIFT;
            }
            ++m_BUFPTR;
            m_BUFPTR &= 63;
        }

        m_DSP.Step();

        for (int i = 0; i < 16; ++i) {
            SCSP_SLOT *slot = m_Slots + i;
            if (EFSDL(slot)) {
                uint16_t Enc = uint16_t((EFPAN(slot) << 0x8) | (EFSDL(slot) << 0xd));
                smpl += (m_DSP.EFREG[i] * m_LPANTABLE[Enc]) >> SHIFT;
                smpr += (m_DSP.EFREG[i] * m_RPANTABLE[Enc]) >> SHIFT;
            }
        }

        for (int i = 0; i < 2; ++i) {
            SCSP_SLOT *slot = m_Slots + i + 16; // EFSDL, EFPAN for EXTS0/1
            uint16_t Enc = EFSDL(slot) ? uint16_t((EFPAN(slot) << 0x8) | (EFSDL(slot) << 0xd))
                                       : uint16_t((DIPAN(slot) << 0x8) | (DISDL(slot) << 0xd));
            m_DSP.EXTS[i] = 0; // Model 2: nothing on the external inputs
            smpl += (m_DSP.EXTS[i] * m_LPANTABLE[Enc]) >> SHIFT;
            smpr += (m_DSP.EXTS[i] * m_RPANTABLE[Enc]) >> SHIFT;
        }

        // put_int_clamp: the value clamped to the range, as a fraction of it
        if (DAC18B()) {
            stereo[2 * s] = float(std::clamp(smpl, -131072, 131071)) / 131072.0f * gain;
            stereo[2 * s + 1] = float(std::clamp(smpr, -131072, 131071)) / 131072.0f * gain;
        } else {
            stereo[2 * s] = float(std::clamp(smpl >> 2, -32768, 32767)) / 32768.0f * gain;
            stereo[2 * s + 1] = float(std::clamp(smpr >> 2, -32768, 32767)) / 32768.0f * gain;
        }

        // latch the new MSLC, updates every 44.1 kHz
        uint8_t MSLC = m_latched_MSLC;
        SCSP_SLOT *slot = m_Slots + MSLC;
        uint32_t SGC = (slot->EG.state) & 3;
        uint32_t CA = (slot->cur_addr >> (SHIFT + 12)) & 0xf;
        uint32_t EG = (0x1f - (slot->EG.volume >> (EG_SHIFT + 5))) & 0x1f;
        m_latched_MSLC_data = uint16_t((CA << 7) | (SGC << 5) | EG);
    }
}

uint8_t Scsp::DecodeSCI(uint8_t irq) {
    uint8_t SCI = 0;
    uint8_t v;
    v = (SCILV0() & (1 << irq)) ? 1 : 0;
    SCI |= v;
    v = (SCILV1() & (1 << irq)) ? 1 : 0;
    SCI |= v << 1;
    v = (SCILV2() & (1 << irq)) ? 1 : 0;
    SCI |= v << 2;
    return SCI;
}

void Scsp::CheckPendingIRQ() {
    uint32_t pend = m_udata.data[0x20 / 2];
    uint32_t en = m_udata.data[0x1e / 2];
    if (m_MidiW != m_MidiR) {
        m_udata.data[0x20 / 2] |= 8;
        pend |= 8;
    }
    if (!pend) return;
    if (pend & en & 0x20) {
        host_.scsp_irq(int(m_IrqCPU), true);
        return;
    }
    if (pend & 0x40)
        if (en & 0x40) {
            host_.scsp_irq(int(m_IrqTimA), true);
            return;
        }
    if (pend & 0x80)
        if (en & 0x80) {
            host_.scsp_irq(int(m_IrqTimBC), true);
            return;
        }
    if (pend & 0x100)
        if (en & 0x100) {
            host_.scsp_irq(int(m_IrqTimBC), true);
            return;
        }
    if (pend & 8)
        if (en & 8) {
            host_.scsp_irq(int(m_IrqMidi), true);
            return;
        }

    host_.scsp_irq(0, false);
}

void Scsp::MainCheckPendingIRQ(uint16_t irq_type) {
    m_mcipd |= irq_type; // main CPU interrupts: not connected on Model 2
}

void Scsp::ResetInterrupts() {
    uint32_t reset = m_udata.data[0x22 / 2];

    if (reset & 0x40) host_.scsp_irq(int(m_IrqTimA), false);
    if (reset & 0x180) host_.scsp_irq(int(m_IrqTimBC), false);
    if (reset & 0x20) {
        m_udata.data[0x20 / 2] &= ~0x20;
        host_.scsp_irq(int(m_IrqCPU), false);
    }
    if (reset & 0x8) host_.scsp_irq(int(m_IrqMidi), false);

    CheckPendingIRQ();
}

void Scsp::timer_expired(int n) {
    m_TimCnt[n] = 0xFFFF;
    m_udata.data[0x20 / 2] |= uint16_t(0x40 << n);
    m_udata.data[(0x18 + 2 * n) / 2] &= 0xff00;
    m_udata.data[(0x18 + 2 * n) / 2] |= uint16_t(m_TimCnt[n] >> 8);

    CheckPendingIRQ();
    if (n == 0) MainCheckPendingIRQ(0x40);
}

int Scsp::Get_AR(int base, int R) {
    int Rate = base + (R << 1);
    return m_ARTABLE[std::clamp(Rate, 0, 63)];
}

int Scsp::Get_DR(int base, int R) {
    int Rate = base + (R << 1);
    return m_DRTABLE[std::clamp(Rate, 0, 63)];
}

void Scsp::Compute_EG(SCSP_SLOT *slot) {
    int octave = (OCT(slot) ^ 8) - 8;
    int rate;
    if (KRS(slot) != 0xf) rate = octave + 2 * KRS(slot) + ((FNS(slot) >> 9) & 1);
    else rate = 0;

    slot->EG.volume = 0x17F << EG_SHIFT;
    slot->EG.AR = Get_AR(rate, AR(slot));
    slot->EG.D1R = Get_DR(rate, D1R(slot));
    slot->EG.D2R = Get_DR(rate, D2R(slot));
    slot->EG.RR = Get_DR(rate, RR(slot));
    slot->EG.DL = 0x1f - DL(slot);
    slot->EG.EGHOLD = uint8_t(EGHOLD(slot));
}

int Scsp::EG_Update(SCSP_SLOT *slot) {
    switch (slot->EG.state) {
    case SCSP_ATTACK:
        slot->EG.volume += slot->EG.AR;
        if (slot->EG.volume >= (0x3ff << EG_SHIFT)) {
            if (!LPSLNK(slot)) {
                slot->EG.state = SCSP_DECAY1;
                if (slot->EG.D1R >= (1024 << EG_SHIFT)) // Skip SCSP_DECAY1, go directly to SCSP_DECAY2
                    slot->EG.state = SCSP_DECAY2;
            }
            slot->EG.volume = 0x3ff << EG_SHIFT;
        }
        if (slot->EG.EGHOLD) return 0x3ff << (SHIFT - 10);
        break;
    case SCSP_DECAY1:
        slot->EG.volume -= slot->EG.D1R;
        if (slot->EG.volume <= 0) slot->EG.volume = 0;
        if (slot->EG.volume >> (EG_SHIFT + 5) <= slot->EG.DL) slot->EG.state = SCSP_DECAY2;
        break;
    case SCSP_DECAY2:
        if (D2R(slot) == 0) return (slot->EG.volume >> EG_SHIFT) << (SHIFT - 10);
        slot->EG.volume -= slot->EG.D2R;
        if (slot->EG.volume <= 0) slot->EG.volume = 0;
        break;
    case SCSP_RELEASE:
        slot->EG.volume -= slot->EG.RR;
        if (slot->EG.volume <= 0) {
            slot->EG.volume = 0;
            StopSlot(slot, 0);
        }
        break;
    default: return 1 << SHIFT;
    }
    return (slot->EG.volume >> EG_SHIFT) << (SHIFT - 10);
}

uint32_t Scsp::Step(SCSP_SLOT *slot) {
    int octave = (OCT(slot) ^ 8) - 8 + SHIFT - 10;
    uint32_t Fn = FNS(slot) + (1 << 10);
    if (octave >= 0) Fn <<= octave;
    else Fn >>= -octave;
    return Fn;
}

void Scsp::Compute_LFO(SCSP_SLOT *slot) {
    if (PLFOS(slot) != 0) LFO_ComputeStep(&(slot->PLFO), LFOF(slot), PLFOWS(slot), PLFOS(slot), 0);
    if (ALFOS(slot) != 0) LFO_ComputeStep(&(slot->ALFO), LFOF(slot), ALFOWS(slot), ALFOS(slot), 1);
}

void Scsp::StartSlot(SCSP_SLOT *slot) {
    slot->effect = host_.scsp_slot_effect(int(slot - m_Slots));
    slot->active = 1;
    slot->cur_addr = 0;
    slot->nxt_addr = 1 << SHIFT;
    slot->step = Step(slot);
    Compute_EG(slot);
    slot->EG.state = SCSP_ATTACK;
    slot->EG.volume = 0x17F << EG_SHIFT;
    slot->Prev = 0;
    slot->Backwards = 0;

    Compute_LFO(slot);
}

void Scsp::StopSlot(SCSP_SLOT *slot, int keyoff) {
    if (keyoff) slot->EG.state = SCSP_RELEASE;
    else slot->active = 0;
    slot->udata.data[0] &= ~0x800;
}

void Scsp::init() {
    int i;

    m_DSP.Init();
    m_DSP.ram = ram_;

    m_IrqTimA = m_IrqTimBC = m_IrqMidi = m_IrqCPU = m_IrqDMA = 0;
    m_MidiR = m_MidiW = 0;
    m_MidiOutR = m_MidiOutW = 0;

    for (i = 0; i < 0x400; ++i) {
        float envDB = (float(3 * (i - 0x3ff))) / 32.0f;
        float scale = float(1 << SHIFT);
        m_EG_TABLE[i] = int32_t(powf(10.0f, envDB / 20.0f) * scale);
    }

    for (i = 0; i < 0x10000; ++i) {
        int iTL = (i >> 0x0) & 0xff;
        int iPAN = (i >> 0x8) & 0x1f;
        int iSDL = (i >> 0xD) & 0x07;
        float TL;
        float SegaDB = 0.0f;
        float fSDL;
        float PAN;
        float LPAN, RPAN;

        if (iTL & 0x01) SegaDB -= 0.4f;
        if (iTL & 0x02) SegaDB -= 0.8f;
        if (iTL & 0x04) SegaDB -= 1.5f;
        if (iTL & 0x08) SegaDB -= 3.0f;
        if (iTL & 0x10) SegaDB -= 6.0f;
        if (iTL & 0x20) SegaDB -= 12.0f;
        if (iTL & 0x40) SegaDB -= 24.0f;
        if (iTL & 0x80) SegaDB -= 48.0f;

        TL = powf(10.0f, SegaDB / 20.0f);

        SegaDB = 0;
        if (iPAN & 0x1) SegaDB -= 3.0f;
        if (iPAN & 0x2) SegaDB -= 6.0f;
        if (iPAN & 0x4) SegaDB -= 12.0f;
        if (iPAN & 0x8) SegaDB -= 24.0f;

        if ((iPAN & 0xf) == 0xf) PAN = 0.0;
        else PAN = powf(10.0f, SegaDB / 20.0f);

        if (iPAN < 0x10) {
            LPAN = PAN;
            RPAN = 1.0;
        } else {
            RPAN = PAN;
            LPAN = 1.0;
        }

        if (iSDL) fSDL = powf(10.0f, (SDLT[iSDL]) / 20.0f);
        else fSDL = 0.0;

        m_LPANTABLE[i] = int(FIX((4.0f * LPAN * TL * fSDL)));
        m_RPANTABLE[i] = int(FIX((4.0f * RPAN * TL * fSDL)));
    }

    m_ARTABLE[0] = m_DRTABLE[0] = 0; // Infinite time
    m_ARTABLE[1] = m_DRTABLE[1] = 0; // Infinite time
    for (i = 2; i < 64; ++i) {
        double step, scale;
        double t = ARTimes[i]; // In ms
        if (t != 0.0) {
            step = (1023 * 1000.0) / (44100.0 * t);
            scale = double(1 << EG_SHIFT);
            m_ARTABLE[i] = int(step * scale);
        } else
            m_ARTABLE[i] = 1024 << EG_SHIFT;

        t = DRTimes[i]; // In ms
        step = (1023 * 1000.0) / (44100.0 * t);
        scale = double(1 << EG_SHIFT);
        m_DRTABLE[i] = int(step * scale);
    }

    // make sure all the slots are off
    for (i = 0; i < 32; ++i) {
        m_Slots[i].slot = i;
        m_Slots[i].active = 0;
        m_Slots[i].EG.state = SCSP_RELEASE;
    }

    LFO_Init();
    // no "pend"
    m_udata.data[0x20 / 2] = 0;
    m_TimCnt[0] = 0xffff;
    m_TimCnt[1] = 0xffff;
    m_TimCnt[2] = 0xffff;
}

void Scsp::UpdateSlotReg(int s, int r) {
    SCSP_SLOT *slot = m_Slots + s;
    switch (r & 0x3f) {
    case 0:
    case 1:
        if (KEYONEX(slot)) {
            for (int sl = 0; sl < 32; ++sl) {
                SCSP_SLOT *s2 = m_Slots + sl;
                if (KEYONB(s2) && s2->EG.state == SCSP_RELEASE) StartSlot(s2);
                if (!KEYONB(s2)) StopSlot(s2, 1);
            }
            slot->udata.data[0] &= ~0x1000;
        }
        break;
    case 0x10:
    case 0x11: slot->step = Step(slot); break;
    case 0xA:
    case 0xB:
        slot->EG.RR = Get_DR(0, RR(slot));
        slot->EG.DL = 0x1f - DL(slot);
        break;
    case 0x12:
    case 0x13: Compute_LFO(slot); break;
    }
}

// MAME: m_timerX->adjust(attotime::from_ticks(512, time)), time = (clock /
// TimPris) / (255 - count): a one-shot after 512 / time seconds.
void Scsp::start_timer(int n, int reg) {
    m_TimPris[n] = 1 << ((m_udata.data[reg / 2] >> 8) & 0x7);
    m_TimCnt[n] = (m_udata.data[reg / 2] & 0xff) << 8;

    if ((m_udata.data[reg / 2] & 0xff) != 255) {
        uint32_t time = (kClock / uint32_t(m_TimPris[n])) / (255 - (m_udata.data[reg / 2] & 0xff));
        if (time) host_.scsp_timer(n, 512.0 / double(time));
    }
}

void Scsp::UpdateReg(int reg) {
    switch (reg & 0x3f) {
    case 0x0:
        break; // MVOL: render() applies it
    case 0x2:
    case 0x3:
        m_DSP.RBL = (8 * 1024) << RBL(); // 8 / 16 / 32 / 64 kwords
        m_DSP.RBP = RBP();
        break;
    case 0x6:
    case 0x7: {
        uint8_t data = m_udata.data[0x6 / 2] & 0xff;
        m_MidiOutStack[m_MidiOutW++] = data;
        m_MidiOutW &= 31;
        host_.scsp_midi_out(data); // sent at once (MAME: one serial frame later)
        m_MidiOutR = m_MidiOutW;
    } break;
    case 8:
    case 9:
        // Only MSLC could be written.
        m_latched_MSLC = uint8_t((m_udata.data[0x8 / 2] & 0xf800) >> 11);
        break;
    case 0x12:
    case 0x13: m_dma.dmea = (m_udata.data[0x12 / 2] & 0xfffe) | (m_dma.dmea & 0xf0000); break;
    case 0x14:
    case 0x15:
        m_dma.dmea = ((m_udata.data[0x14 / 2] & 0xf000) << 4) | (m_dma.dmea & 0xfffe);
        m_dma.drga = (m_udata.data[0x14 / 2] & 0x0ffe);
        break;
    case 0x16:
    case 0x17:
        m_dma.dtlg = (m_udata.data[0x16 / 2] & 0x0ffe);
        m_dma.ddir = (m_udata.data[0x16 / 2] & 0x2000) >> 13;
        m_dma.dgate = (m_udata.data[0x16 / 2] & 0x4000) >> 14;
        if (m_udata.data[0x16 / 2] & 0x1000) // dexe
            exec_dma();
        break;
    case 0x18:
    case 0x19: start_timer(0, 0x18); break;
    case 0x1a:
    case 0x1b: start_timer(1, 0x1a); break;
    case 0x1c:
    case 0x1d: start_timer(2, 0x1c); break;
    case 0x1e: // SCIEB
    case 0x1f: CheckPendingIRQ(); break;
    case 0x20: // SCIPD
    case 0x21:
        if (m_udata.data[0x1e / 2] & m_udata.data[0x20 / 2] & 0x20) CheckPendingIRQ();
        break;
    case 0x22: // SCIRE
    case 0x23:
        m_udata.data[0x20 / 2] &= ~m_udata.data[0x22 / 2];
        ResetInterrupts();

        // behavior from real hardware: if you SCIRE a timer that's expired,
        // it'll immediately pop up again in SCIPD.
        if (m_TimCnt[0] == 0xffff) m_udata.data[0x20 / 2] |= 0x40;
        if (m_TimCnt[1] == 0xffff) m_udata.data[0x20 / 2] |= 0x80;
        if (m_TimCnt[2] == 0xffff) m_udata.data[0x20 / 2] |= 0x100;
        break;
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x29:
        m_IrqTimA = DecodeSCI(SCITMA);
        m_IrqTimBC = DecodeSCI(SCITMB);
        m_IrqMidi = DecodeSCI(SCIMID);
        m_IrqCPU = DecodeSCI(SCIIRQ);
        m_IrqDMA = DecodeSCI(SCIDMA);
        break;
    case 0x2a:
    case 0x2b:
        m_mcieb = m_udata.data[0x2a / 2];
        MainCheckPendingIRQ(0);
        break;
    case 0x2c:
    case 0x2d:
        if (m_udata.data[0x2c / 2] & 0x20) MainCheckPendingIRQ(0x20);
        break;
    case 0x2e:
    case 0x2f:
        m_mcipd &= ~m_udata.data[0x2e / 2];
        MainCheckPendingIRQ(0);
        break;
    }
}

void Scsp::UpdateRegR(int reg) {
    switch (reg & 0x3f) {
    case 4:
    case 5: {
        uint16_t v = m_udata.data[0x4 / 2];
        v &= 0xff00;
        v |= m_MidiStack[m_MidiR];
        if (m_MidiR != m_MidiW) {
            ++m_MidiR;
            m_MidiR &= 31;
        }
        if (m_MidiR == m_MidiW) { // if the input FIFO is empty, clear the IRQ
            host_.scsp_irq(int(m_IrqMidi), false);
            m_udata.data[0x20 / 2] &= ~8;
        }
        m_udata.data[0x4 / 2] = v;
    } break;
    case 8:
    case 9: m_udata.data[0x8 / 2] = m_latched_MSLC_data; break;
    case 0x2a:
    case 0x2b: m_udata.data[0x2a / 2] = m_mcieb; break;
    case 0x2c:
    case 0x2d: m_udata.data[0x2c / 2] = m_mcipd; break;
    }
}

void Scsp::w16(uint32_t addr, uint16_t val) {
    addr &= 0xffff;
    if (addr < 0x400) {
        int slot = int(addr / 0x20);
        addr &= 0x1f;
        m_Slots[slot].udata.data[addr / 2] = val;
        UpdateSlotReg(slot, int(addr & 0x1f));
    } else if (addr < 0x600) {
        if (addr < 0x430) {
            // SCIPD and MCIPD are r/o except for bit 5 CPU irqs
            if (addr == 0x420 || addr == 0x42e) m_udata.data[(addr & 0x3f) / 2] |= val & 0x20;
            else m_udata.data[(addr & 0x3f) / 2] = val;
            UpdateReg(int(addr & 0x3f));
        }
    } else if (addr < 0x700)
        m_RINGBUF[(addr - 0x600) / 2] = int16_t(val);
    else {
        // DSP
        if (addr < 0x780) // COEF
            m_DSP.COEF[(addr - 0x700) / 2] = int16_t(val);
        else if (addr < 0x7c0)
            m_DSP.MADRS[(addr - 0x780) / 2] = val;
        else if (addr < 0x800) // MADRS is mirrored twice
            m_DSP.MADRS[(addr - 0x7c0) / 2] = val;
        else if (addr < 0xC00) {
            m_DSP.MPRO[(addr - 0x800) / 2] = val;
            if (addr == 0xBF0) m_DSP.Start();
        }
    }
}

uint16_t Scsp::r16(uint32_t addr) {
    uint16_t v = 0;
    addr &= 0xffff;
    if (addr < 0x400) {
        int slot = int(addr / 0x20);
        addr &= 0x1f;
        v = m_Slots[slot].udata.data[addr / 2];
    } else if (addr < 0x600) {
        if (addr < 0x430) {
            UpdateRegR(int(addr & 0x3f));
            v = m_udata.data[(addr & 0x3f) / 2];
        }
    } else if (addr < 0x700)
        v = uint16_t(m_RINGBUF[(addr - 0x600) / 2]);
    else {
        // DSP
        if (addr < 0x780) // COEF
            v = uint16_t(m_DSP.COEF[(addr - 0x700) / 2]);
        else if (addr < 0x7c0)
            v = m_DSP.MADRS[(addr - 0x780) / 2];
        else if (addr < 0x800)
            v = m_DSP.MADRS[(addr - 0x7c0) / 2];
        else if (addr < 0xC00)
            v = m_DSP.MPRO[(addr - 0x800) / 2];
        else if (addr < 0xE00) {
            if (addr & 2) v = uint16_t(m_DSP.TEMP[(addr >> 2) & 0x7f] & 0xffff);
            else v = uint16_t(m_DSP.TEMP[(addr >> 2) & 0x7f] >> 16);
        } else if (addr < 0xE80) {
            if (addr & 2) v = uint16_t(m_DSP.MEMS[(addr >> 2) & 0x1f] & 0xffff);
            else v = uint16_t(m_DSP.MEMS[(addr >> 2) & 0x1f] >> 16);
        } else if (addr < 0xEC0) {
            if (addr & 2) v = uint16_t(m_DSP.MIXS[(addr >> 2) & 0xf] & 0xffff);
            else v = uint16_t(m_DSP.MIXS[(addr >> 2) & 0xf] >> 16);
        } else if (addr < 0xEE0)
            v = uint16_t(m_DSP.EFREG[(addr - 0xec0) / 2]);
        else if (addr < 0xEE4)
            v = uint16_t(m_DSP.EXTS[(addr - 0xee0) / 2]);
    }
    return v;
}

int32_t Scsp::UpdateSlot(SCSP_SLOT *slot) {
    if (SSCTL(slot) == 3) return 0; // manual says cannot be used

    int32_t sample = 0;
    int step = int(slot->step);
    uint32_t addr1, addr2, addr_select;                        // current and next sample addresses
    uint32_t *addr[2] = {&addr1, &addr2};                      // used for linear interpolation
    uint32_t *slot_addr[2] = {&(slot->cur_addr), &(slot->nxt_addr)};

    if (PLFOS(slot) != 0) {
        step = step * PLFO_Step(&(slot->PLFO));
        step >>= SHIFT;
    }

    if (PCM8B(slot)) {
        addr1 = slot->cur_addr >> SHIFT;
        addr2 = slot->nxt_addr >> SHIFT;
    } else {
        addr1 = (slot->cur_addr >> (SHIFT - 1)) & ~1;
        addr2 = (slot->nxt_addr >> (SHIFT - 1)) & ~1;
    }

    if (MDL(slot) != 0 || MDXSL(slot) != 0 || MDYSL(slot) != 0) {
        int32_t smp = (m_RINGBUF[(m_BUFPTR + MDXSL(slot)) & 63] + m_RINGBUF[(m_BUFPTR + MDYSL(slot)) & 63]) / 2;

        smp <<= 0xA; // associate cycle with 1024
        smp >>= 0x1A - MDL(slot);
        if (!PCM8B(slot)) smp <<= 1;

        addr1 += uint32_t(smp);
        addr2 += uint32_t(smp);
    }

    if (SSCTL(slot) == 0) { // External DRAM data
        if (PCM8B(slot)) { // 8 bit signed
            int8_t p1 = read_byte(uint32_t(SA(slot)) + addr1);
            int8_t p2 = read_byte(uint32_t(SA(slot)) + addr2);
            int32_t s;
            int32_t fpart = int32_t(slot->cur_addr & ((1 << SHIFT) - 1));
            s = int(p1 << 8) * ((1 << SHIFT) - fpart) + int(p2 << 8) * fpart;
            sample = (s >> SHIFT);
        } else { // 16 bit signed
            int16_t p1 = read_word(uint32_t(SA(slot)) + addr1);
            int16_t p2 = read_word(uint32_t(SA(slot)) + addr2);
            int32_t s;
            int32_t fpart = int32_t(slot->cur_addr & ((1 << SHIFT) - 1));
            s = int(p1) * ((1 << SHIFT) - fpart) + int(p2) * fpart;
            sample = (s >> SHIFT);
        }
    } else if (SSCTL(slot) == 1) // Internally generated data (Noise)
        sample = int16_t(rand() & 0xffff); // Unknown algorithm
    else if (SSCTL(slot) >= 2)          // Internally generated data (All 0)
        sample = 0;

    if (SBCTL(slot) & 0x1) sample ^= 0x7FFF;
    if (SBCTL(slot) & 0x2) sample = int16_t(sample ^ 0x8000);

    if (slot->Backwards) slot->cur_addr -= uint32_t(step);
    else slot->cur_addr += uint32_t(step);
    slot->nxt_addr = slot->cur_addr + (1 << SHIFT);

    addr1 = slot->cur_addr >> SHIFT;
    addr2 = slot->nxt_addr >> SHIFT;

    if (addr1 >= LSA(slot) && !(slot->Backwards)) {
        if (LPSLNK(slot) && slot->EG.state == SCSP_ATTACK) slot->EG.state = SCSP_DECAY1;
    }

    for (addr_select = 0; addr_select < 2; addr_select++) {
        int32_t rem_addr;
        switch (LPCTL(slot)) {
        case 0: // no loop
            if (*addr[addr_select] >= LSA(slot) && *addr[addr_select] >= LEA(slot)) StopSlot(slot, 0);
            break;
        case 1: // normal loop
            if (*addr[addr_select] >= LEA(slot)) {
                rem_addr = int32_t(*slot_addr[addr_select] - (uint32_t(LEA(slot)) << SHIFT));
                *slot_addr[addr_select] = (uint32_t(LSA(slot)) << SHIFT) + uint32_t(rem_addr);
            }
            break;
        case 2: // reverse loop
            if ((*addr[addr_select] >= LSA(slot)) && !(slot->Backwards)) {
                rem_addr = int32_t(*slot_addr[addr_select] - (uint32_t(LSA(slot)) << SHIFT));
                *slot_addr[addr_select] = (uint32_t(LEA(slot)) << SHIFT) - uint32_t(rem_addr);
                slot->Backwards = 1;
            } else if ((*addr[addr_select] < LSA(slot) || (*slot_addr[addr_select] & 0x80000000)) && slot->Backwards) {
                rem_addr = int32_t((uint32_t(LSA(slot)) << SHIFT) - *slot_addr[addr_select]);
                *slot_addr[addr_select] = (uint32_t(LEA(slot)) << SHIFT) - uint32_t(rem_addr);
            }
            break;
        case 3: // ping-pong
            if (*addr[addr_select] >= LEA(slot)) { // reached end, reverse till start
                rem_addr = int32_t(*slot_addr[addr_select] - (uint32_t(LEA(slot)) << SHIFT));
                *slot_addr[addr_select] = (uint32_t(LEA(slot)) << SHIFT) - uint32_t(rem_addr);
                slot->Backwards = 1;
            } else if ((*addr[addr_select] < LSA(slot) || (*slot_addr[addr_select] & 0x80000000)) && slot->Backwards) { // reached start or negative
                rem_addr = int32_t((uint32_t(LSA(slot)) << SHIFT) - *slot_addr[addr_select]);
                *slot_addr[addr_select] = (uint32_t(LSA(slot)) << SHIFT) + uint32_t(rem_addr);
                slot->Backwards = 0;
            }
            break;
        }
    }

    if (!SDIR(slot)) {
        if (ALFOS(slot) != 0) {
            sample = sample * ALFO_Step(&(slot->ALFO));
            sample >>= SHIFT;
        }

        if (slot->EG.state == SCSP_ATTACK) sample = (sample * EG_Update(slot)) >> SHIFT;
        else sample = (sample * m_EG_TABLE[EG_Update(slot) >> (SHIFT - 10)]) >> SHIFT;
    }

    if (!STWINH(slot)) {
        if (!SDIR(slot)) {
            uint16_t Enc = uint16_t((TL(slot) << 0x0) | (0x7 << 0xd));
            *m_RBUFDST = int16_t((sample * m_LPANTABLE[Enc]) >> (SHIFT + 1));
        } else {
            uint16_t Enc = uint16_t((0 << 0x0) | (0x7 << 0xd));
            *m_RBUFDST = int16_t((sample * m_LPANTABLE[Enc]) >> (SHIFT + 1));
        }
    }

    return sample;
}

void Scsp::exec_dma() {
    static uint16_t tmp_dma[3];
    int i;

    // Copy the dma values in a temp storage for resuming later
    // (DMA *can't* overwrite its parameters).
    if (!(m_dma.ddir))
        for (i = 0; i < 3; i++) tmp_dma[i] = m_udata.data[(0x12 + (i * 2)) / 2];

    if (m_dma.ddir) {
        if (m_dma.dgate) {
            for (i = 0; i < m_dma.dtlg; i += 2) {
                write_word(m_dma.dmea, 0);
                m_dma.dmea += 2;
            }
        } else {
            for (i = 0; i < m_dma.dtlg; i += 2) {
                uint16_t tmp = r16(m_dma.drga);
                write_word(m_dma.dmea, tmp);
                m_dma.dmea += 2;
                m_dma.drga += 2;
            }
        }
    } else {
        if (m_dma.dgate) {
            for (i = 0; i < m_dma.dtlg; i += 2) {
                w16(m_dma.drga, 0);
                m_dma.drga += 2;
            }
        } else {
            for (i = 0; i < m_dma.dtlg; i += 2) {
                uint16_t tmp = uint16_t(read_word(m_dma.dmea));
                w16(m_dma.drga, tmp);
                m_dma.dmea += 2;
                m_dma.drga += 2;
            }
        }
    }

    // Resume the values
    if (!(m_dma.ddir))
        for (i = 0; i < 3; i++) m_udata.data[(0x12 + (i * 2)) / 2] = tmp_dma[i];

    // Job done
    m_udata.data[0x16 / 2] &= ~0x1000;
    // request a dma end irq (MAME: HOLD_LINE)
    if (m_udata.data[0x1e / 2] & 0x10) host_.scsp_irq(int(m_IrqDMA), true);
}

uint16_t Scsp::read(uint32_t offset) { return r16(offset * 2); }

void Scsp::write(uint32_t offset, uint16_t data, uint16_t mem_mask) {
    uint16_t tmp = r16(offset * 2);
    tmp = uint16_t((tmp & ~mem_mask) | (data & mem_mask)); // COMBINE_DATA
    w16(offset * 2, tmp);
}

void Scsp::midi_in(uint8_t byte) { // MAME rcv_complete
    m_MidiStack[m_MidiW++] = byte;
    m_MidiW &= 31;
    CheckPendingIRQ();
}

// LFO handling

void Scsp::LFO_Init() {
    for (int i = 0; i < 256; ++i) {
        int a, p;
        // Saw
        a = 255 - i;
        if (i < 128) p = i;
        else p = i - 256;
        m_ALFO_SAW[i] = a;
        m_PLFO_SAW[i] = p;

        // Square
        if (i < 128) {
            a = 255;
            p = 127;
        } else {
            a = 0;
            p = -128;
        }
        m_ALFO_SQR[i] = a;
        m_PLFO_SQR[i] = p;

        // Tri
        if (i < 128) a = 255 - (i * 2);
        else a = (i * 2) - 256;
        if (i < 64) p = i * 2;
        else if (i < 128) p = 255 - i * 2;
        else if (i < 192) p = 256 - i * 2;
        else p = i * 2 - 511;
        m_ALFO_TRI[i] = a;
        m_PLFO_TRI[i] = p;

        // noise
        a = int(rand() & 0xff);
        p = 128 - a;
        m_ALFO_NOI[i] = a;
        m_PLFO_NOI[i] = p;
    }

    for (int s = 0; s < 8; ++s) {
        float limit = PSCALE[s];
        for (int i = -128; i < 128; ++i) m_PSCALES[s][i + 128] = int(CENTS(((limit * float(i)) / 128.0f)));
        limit = -ASCALE[s];
        for (int i = 0; i < 256; ++i) m_ASCALES[s][i] = int(DB(((limit * float(i)) / 256.0f)));
    }
}

int32_t Scsp::PLFO_Step(SCSP_LFO_t *LFO) {
    int p;
    LFO->phase = uint16_t(LFO->phase + LFO->phase_step);
    p = LFO->table[LFO->phase >> LFO_SHIFT];
    p = LFO->scale[p + 128];
    return p << (SHIFT - LFO_SHIFT);
}

int32_t Scsp::ALFO_Step(SCSP_LFO_t *LFO) {
    int p;
    LFO->phase = uint16_t(LFO->phase + LFO->phase_step);
    p = LFO->table[LFO->phase >> LFO_SHIFT];
    p = LFO->scale[p];
    return p << (SHIFT - LFO_SHIFT);
}

void Scsp::LFO_ComputeStep(SCSP_LFO_t *LFO, uint32_t LFOF, uint32_t LFOWS, uint32_t LFOS, int ALFO) {
    float step = float(LFOFreq[LFOF]) * 256.0f / 44100.0f;
    LFO->phase_step = uint32_t(float(1 << LFO_SHIFT) * step);
    if (ALFO) {
        switch (LFOWS) {
        case 0: LFO->table = m_ALFO_SAW; break;
        case 1: LFO->table = m_ALFO_SQR; break;
        case 2: LFO->table = m_ALFO_TRI; break;
        case 3: LFO->table = m_ALFO_NOI; break;
        }
        LFO->scale = m_ASCALES[LFOS];
    } else {
        switch (LFOWS) {
        case 0: LFO->table = m_PLFO_SAW; break;
        case 1: LFO->table = m_PLFO_SQR; break;
        case 2: LFO->table = m_PLFO_TRI; break;
        case 3: LFO->table = m_PLFO_NOI; break;
        }
        LFO->scale = m_PSCALES[LFOS];
    }
}

// --- DSP (MAME scspdsp.cpp) --------------------------------------------------

namespace {

uint16_t PACK(int32_t val) {
    int const sign = (val >> 23) & 1;
    uint32_t temp = uint32_t(val ^ (val << 1)) & 0xFFFFFF;
    int exponent = 0;
    for (int k = 0; k < 12; k++) {
        if (temp & 0x800000) break;
        temp <<= 1;
        exponent += 1;
    }
    if (exponent < 12) val = (val << exponent) & 0x3FFFFF;
    else val <<= 11;
    val >>= 11;
    val &= 0x7FF;
    val |= sign << 15;
    val |= exponent << 11;
    return uint16_t(val);
}

int32_t UNPACK(uint16_t val) {
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

} // namespace

uint16_t ScspDsp::read_word(uint32_t a) const { a &= ~1u; return a < 0x80000 ? uint16_t(ram[a] << 8 | ram[a + 1]) : 0; }
void ScspDsp::write_word(uint32_t a, uint16_t v) {
    a &= ~1u;
    if (a < 0x80000) {
        ram[a] = uint8_t(v >> 8);
        ram[a + 1] = uint8_t(v);
    }
}

void ScspDsp::Init() {
    uint8_t *const keep = ram;
    std::memset(this, 0, sizeof(*this));
    ram = keep;
    RBL = (8 * 1024); // Initial RBL is 0
    Stopped = true;
}

void ScspDsp::Step() {
    if (Stopped) return;

    std::fill(std::begin(EFREG), std::end(EFREG), int16_t(0));

    int32_t ACC = 0; // 26 bit
    int32_t MEMVAL = 0;
    int32_t FRC_REG = 0; // 13 bit
    int32_t Y_REG = 0;   // 24 bit
    uint32_t ADRS_REG = 0; // 13 bit

    for (int step = 0; step < LastStep; ++step) {
        uint16_t *const IPtr = MPRO + (step * 4);

        uint32_t const TRA = (IPtr[0] >> 8) & 0x7f;
        uint32_t const TWT = (IPtr[0] >> 7) & 0x01;
        uint32_t const TWA = (IPtr[0] >> 0) & 0x7f;

        uint32_t const XSEL = (IPtr[1] >> 15) & 0x01;
        uint32_t const YSEL = (IPtr[1] >> 13) & 0x03;
        uint32_t const IRA = (IPtr[1] >> 6) & 0x3f;
        uint32_t const IWT = (IPtr[1] >> 5) & 0x01;
        uint32_t const IWA = (IPtr[1] >> 0) & 0x1f;

        uint32_t const TABLE = (IPtr[2] >> 15) & 0x01;
        uint32_t const MWT = (IPtr[2] >> 14) & 0x01;
        uint32_t const MRD = (IPtr[2] >> 13) & 0x01;
        uint32_t const EWT = (IPtr[2] >> 12) & 0x01;
        uint32_t const EWA = (IPtr[2] >> 8) & 0x0f;
        uint32_t const ADRL = (IPtr[2] >> 7) & 0x01;
        uint32_t const FRCL = (IPtr[2] >> 6) & 0x01;
        uint32_t const SHIFT = (IPtr[2] >> 4) & 0x03;
        uint32_t const YRL = (IPtr[2] >> 3) & 0x01;
        uint32_t const NEGB = (IPtr[2] >> 2) & 0x01;
        uint32_t const ZERO = (IPtr[2] >> 1) & 0x01;
        uint32_t const BSEL = (IPtr[2] >> 0) & 0x01;

        uint32_t const NOFL = (IPtr[3] >> 15) & 0x01;
        uint32_t const COEF = (IPtr[3] >> 9) & 0x3f;

        uint32_t const MASA = (IPtr[3] >> 2) & 0x1f;
        uint32_t const ADREB = (IPtr[3] >> 1) & 0x01;
        uint32_t const NXADR = (IPtr[3] >> 0) & 0x01;

        // operations are done at 24 bit precision

        // INPUTS RW
        int32_t INPUTS; // 24-bit
        if (IRA <= 0x1f) INPUTS = MEMS[IRA];
        else if (IRA <= 0x2F) INPUTS = MIXS[IRA - 0x20] << 4; // MIXS is 20 bit
        else if (IRA <= 0x31) INPUTS = EXTS[IRA - 0x30] << 8; // EXTS is 16 bit
        else return;

        INPUTS = sext(INPUTS, 24);

        if (IWT) {
            MEMS[IWA] = MEMVAL; // MEMVAL was selected in previous MRD
            if (IRA == IWA) INPUTS = MEMVAL;
        }

        // Operand sel
        int32_t B; // 26-bit
        if (!ZERO) {
            if (BSEL) B = ACC;
            else B = sext(TEMP[(TRA + DEC) & 0x7f], 24);
            if (NEGB) B = 0 - B;
        } else
            B = 0;

        int32_t X; // 24-bit
        if (XSEL) X = INPUTS;
        else X = sext(TEMP[(TRA + DEC) & 0x7f], 24);

        int32_t Y = 0; // 13 bit
        if (YSEL == 0) Y = FRC_REG;
        else if (YSEL == 1) Y = this->COEF[COEF] >> 3; // COEF is 16 bits
        else if (YSEL == 2) Y = (Y_REG >> 11) & 0x1fff;
        else if (YSEL == 3) Y = (Y_REG >> 4) & 0x0fff;

        if (YRL) Y_REG = INPUTS;

        // Shifter
        int32_t SHIFTED = 0; // 24 bit
        if (SHIFT == 0) SHIFTED = std::clamp<int32_t>(ACC, -0x00800000, 0x007fffff);
        else if (SHIFT == 1) SHIFTED = std::clamp<int32_t>(ACC * 2, -0x00800000, 0x007fffff);
        else if (SHIFT == 2) SHIFTED = sext(ACC * 2, 24);
        else if (SHIFT == 3) SHIFTED = sext(ACC, 24);

        // ACCUM
        Y = sext(Y, 13);

        int64_t const v = (int64_t(X) * int64_t(Y)) >> 12;
        ACC = int(v + B);

        if (TWT) TEMP[(TWA + DEC) & 0x7f] = SHIFTED;

        if (FRCL) {
            if (SHIFT == 3) FRC_REG = SHIFTED & 0x0fff;
            else FRC_REG = (SHIFTED >> 11) & 0x1fff;
        }

        if (MRD || MWT) {
            uint32_t ADDR = MADRS[MASA];
            if (!TABLE) ADDR += DEC;
            if (ADREB) ADDR += ADRS_REG & 0x0FFF;
            if (NXADR) ADDR++;
            if (!TABLE) ADDR &= RBL - 1;
            else ADDR &= 0xffff;
            ADDR += RBP << 12;
            ADDR <<= 1;
            if (MRD && (step & 1)) { // memory only allowed on odd? DoA inserts NOPs on even
                if (NOFL) MEMVAL = read_word(ADDR) << 8;
                else MEMVAL = UNPACK(read_word(ADDR));
            }
            if (MWT && (step & 1)) {
                if (NOFL) write_word(ADDR, uint16_t(SHIFTED >> 8));
                else write_word(ADDR, PACK(SHIFTED));
            }
        }

        if (ADRL) {
            if (SHIFT == 3) ADRS_REG = uint32_t(SHIFTED >> 12) & 0xfff;
            else ADRS_REG = uint32_t(INPUTS >> 16);
        }

        if (EWT) EFREG[EWA] = int16_t(EFREG[EWA] + (SHIFTED >> 8));
    }
    --DEC;
    std::fill(std::begin(MIXS), std::end(MIXS), 0);
}

void ScspDsp::SetSample(int32_t sample, int32_t SEL, int32_t) { MIXS[SEL] += sample; }

void ScspDsp::Start() {
    Stopped = false;
    int i;
    for (i = 127; i >= 0; --i) {
        uint16_t const *const IPtr = MPRO + (i * 4);
        if (IPtr[0] || IPtr[1] || IPtr[2] || IPtr[3]) break;
    }
    LastStep = i + 1;
}

} // namespace snd
