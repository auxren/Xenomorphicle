// Copyright (c) 2026, Oren Levy
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Fungen: a Hemisphere reimplementation of the multi-segment function
// generator from the Buchla 700's MIDAS VII software.
// Credit: Lynx Crowe / Don Buchla — Buchla 700 MIDAS.
// The function-point model (time/value points with per-point ACTION codes:
// NULL/SUST/ENBL/JUMP/LOOP/KYUP/KYDN) is reimplemented here from the MIDAS
// documentation; no original MIDAS code is used.
//
// Note: everything except the per-tick hot path of Controller() is defined
// out-of-line below the class with FLASHMEM, to keep ITCM (RAM1) usage
// minimal. (FLASHMEM on in-class definitions gets dropped by LTO.)

#ifndef _HEM_FUNGEN_H_
#define _HEM_FUNGEN_H_

// Exponential time lookup: index 0..127 -> segment duration in ticks
// (16.667kHz), ~5ms .. ~10s. t(i) = 83 * (166667/83)^(i/127)
// PROGMEM: only read at segment boundaries & in the UI, so keep it in flash.
static constexpr uint32_t FUNGEN_TICKS[128] PROGMEM = {
      83,     88,     94,     99,    105,    112,    119,    126,
     134,    142,    151,    160,    170,    181,    192,    204,
     216,    230,    244,    259,    275,    292,    310,    329,
     349,    371,    394,    418,    444,    471,    500,    531,
     564,    599,    636,    675,    717,    761,    808,    858,
     911,    967,   1026,   1090,   1157,   1228,   1304,   1385,
    1470,   1561,   1657,   1759,   1868,   1983,   2106,   2236,
    2374,   2520,   2676,   2841,   3016,   3202,   3400,   3610,
    3832,   4069,   4320,   4587,   4870,   5170,   5489,   5828,
    6188,   6569,   6975,   7405,   7862,   8347,   8862,   9409,
    9990,  10606,  11261,  11956,  12694,  13477,  14309,  15192,
   16129,  17125,  18181,  19303,  20494,  21759,  23102,  24528,
   26041,  27648,  29354,  31166,  33089,  35131,  37299,  39601,
   42045,  44639,  47394,  50319,  53424,  56721,  60221,  63937,
   67883,  72072,  76519,  81241,  86255,  91578,  97229, 103229,
  109599, 116363, 123543, 131167, 139262, 147855, 156980, 166667,
};

// Q12 chromatic ratios for 1V/oct time-scale modulation
static constexpr uint16_t FUNGEN_SEMI_Q12[12] = {
  4096, 4340, 4598, 4871, 5161, 5468, 5793, 6137, 6502, 6889, 7298, 7732
};

class Fungen : public HemisphereApplet {
public:
    static constexpr int MAX_PTS = 8;
    static constexpr uint32_t PHASE_ONE = 0x80000000u; // end-of-segment phase

    // MIDAS VII per-point action codes
    enum FnAction : uint8_t {
        AC_NULL, // proceed to next point
        AC_SUST, // hold here while gate ("key") is high
        AC_ENBL, // hold here while gate is low
        AC_JUMP, // unconditional jump to point par1
        AC_LOOP, // jump to par1, par2 times
        AC_KYUP, // jump to par1 if gate is low
        AC_KYDN, // jump to par1 if gate is high

        AC_LAST = AC_KYDN
    };

    enum FnCursor {
        PT_SEL, PT_TIME, PT_VALUE, PT_ACTION, PT_PAR1, PT_PAR2, NUM_PTS,

        MAX_CURSOR = NUM_PTS
    };

    enum FnState : uint8_t {
        FN_IDLE, // never triggered; output at 0
        FN_RAMP, // ramping toward point 'cur'
        FN_SUST, // arrived; holding while gate high (AC_SUST)
        FN_ENBL, // arrived; holding while gate low (AC_ENBL)
        FN_DONE  // ran off the end; holding last value
    };

    const char* applet_name() { // Maximum 9 characters
        return "700 FnGen";
    }
    const uint8_t* applet_icon() { return PhzIcons::fungen; }

    void Start();
    void Reset();

    void Controller() {
        ForEachChannel(ch) {
            FnChannel &r = run[ch];
            const bool gate = Gate(ch);

            if (Clock(ch)) Retrigger(ch); // gate rising edge: (re)start

            switch (r.state) {
            case FN_RAMP: {
                // CV scales overall time 1V/oct style: +1V = twice as fast
                int cv = DetentedIn(ch);
                CONSTRAIN(cv, -4 * ONE_OCTAVE, 4 * ONE_OCTAVE);
                const uint32_t idx = uint32_t(cv + 4 * ONE_OCTAVE); // 0..12288
                const uint32_t oct = idx / ONE_OCTAVE;              // 0..8
                const uint32_t semi = (idx % ONE_OCTAVE) >> 7;      // 0..11
                uint64_t step = (uint64_t(r.inc) * FUNGEN_SEMI_Q12[semi]) >> (12 + 4);
                step <<= oct; // net: inc * 2^(cv/1V), cv in [-4V..4V]
                if (step == 0) step = 1;
                if (step > PHASE_ONE) step = PHASE_ONE;

                r.phase += uint32_t(step);
                if (r.phase >= PHASE_ONE) { // reached the point: run its action
                    r.value = r.target;
                    Arrive(ch, gate);
                } else { // linear interpolation toward the point value
                    r.value = r.from
                      + int32_t((int64_t(r.target - r.from) * (r.phase >> 15)) >> 16);
                }
                break;
            }
            case FN_SUST: if (!gate) AdvancePoint(ch); break;
            case FN_ENBL: if (gate)  AdvancePoint(ch); break;
            default: break; // FN_IDLE / FN_DONE hold value
            }

            Out(ch, r.value);
        }
    }

    void View();
    void AuxButton();
    void OnEncoderMove(int direction);

    /* Preset packing
     *
     * Main 64-bit word (EEPROM):
     *   bits 0- 2: npts[0] - 1
     *   bits 3- 5: npts[1] - 1
     *
     * Point tables are stored as 8 additional 64-bit words via the applet
     * data blob store (SetData/GetData), key = ch * 4 + w (w = 0..3);
     * each word holds points 2w (bits 0-31) and 2w+1 (bits 32-63).
     *
     * Packed point (32 bits, same format used in RAM):
     *   bits 0- 6: time index (0..127, exponential ~5ms..10s)
     *   bits 8-15: target value (int8, -127..127 => -MAX_CV..+MAX_CV)
     *   bits 16-18: action (0..6, FnAction)
     *   bits 19-21: jump target point (0..7)
     *   bits 22-25: loop count (1..15)
     */
    uint64_t OnDataRequest();
    void OnDataReceive(uint64_t data);

protected:
    void SetHelp();

private:
    // Per-channel runtime state (increments precomputed at segment entry)
    struct FnChannel {
        uint32_t phase;   // 0..PHASE_ONE within the current segment
        uint32_t inc;     // nominal phase increment per tick
        int16_t from;     // segment start CV
        int16_t target;   // segment end CV (the point's value)
        int16_t value;    // current output CV
        uint8_t cur;      // point currently being approached / held
        uint8_t state;    // FnState
        uint8_t loop_ct[MAX_PTS]; // per-point AC_LOOP counters
    };

    // Settings: packed points (see OnDataRequest comment) + counts
    uint32_t pt[2][MAX_PTS];
    int8_t npts[2];

    FnChannel run[2];

    // UI state
    int8_t cursor;
    int8_t edit_ch;
    int8_t sel_pt;

    static constexpr uint32_t PackPoint(uint8_t time, int8_t val, uint8_t act,
                                        uint8_t tgt, uint8_t count) {
        return uint32_t(time & 0x7f)
             | (uint32_t(uint8_t(val)) << 8)
             | (uint32_t(act & 0x7) << 16)
             | (uint32_t(tgt & 0x7) << 19)
             | (uint32_t(count & 0xf) << 22);
    }
    uint8_t PtTime(int ch, int i) const { return pt[ch][i] & 0x7f; }
    int8_t  PtVal(int ch, int i) const { return int8_t((pt[ch][i] >> 8) & 0xff); }
    uint8_t PtAct(int ch, int i) const { return (pt[ch][i] >> 16) & 0x7; }
    uint8_t PtTgt(int ch, int i) const { return (pt[ch][i] >> 19) & 0x7; }
    uint8_t PtCnt(int ch, int i) const { return (pt[ch][i] >> 22) & 0xf; }

    // Segment-boundary code below runs only a few times per envelope pass,
    // so it lives in flash (defined out-of-line, FLASHMEM) to spare ITCM.
    int16_t ValueCV(int v) const;
    void ResetChannel(int ch);
    void Retrigger(int ch);
    void GotoPoint(int ch, int j);
    void AdvancePoint(int ch);
    void Arrive(int ch, bool gate);

    uint8_t PtX(uint8_t ch, uint8_t i) const;
    uint8_t ValY(int v) const;
    void DrawInterface();
    void DrawFunction(uint8_t ch);
};

static_assert(sizeof(Fungen) <= 200, "Keep Fungen per-instance state small");

// ---------------------------------------------------------------------------
// Out-of-line definitions: FLASHMEM only takes effect on non-inline
// definitions when building with LTO, so everything but Controller() lives
// down here.
// ---------------------------------------------------------------------------

FLASHMEM void Fungen::Start() {
    ForEachChannel(ch) {
        // Default: attack / decay-to-sustain (SUST) / release
        npts[ch] = 3;
        pt[ch][0] = PackPoint(30, 127, AC_NULL, 0, 1); //  ~30ms to full
        pt[ch][1] = PackPoint(62,  64, AC_SUST, 0, 1); // ~200ms to 50%, sustain
        pt[ch][2] = PackPoint(73,   0, AC_NULL, 0, 1); // ~400ms to 0
        for (int i = 3; i < MAX_PTS; ++i) pt[ch][i] = PackPoint(64, 0, AC_NULL, 0, 1);
        ResetChannel(ch);
    }
    cursor = 0;
    edit_ch = 0;
    sel_pt = 0;
}

FLASHMEM void Fungen::Reset() {
    ForEachChannel(ch) ResetChannel(ch);
}

// point value (int8) -> DAC CV
FLASHMEM int16_t Fungen::ValueCV(int v) const {
    return int16_t(constrain(v * HEMISPHERE_MAX_CV / 127,
                             HEMISPHERE_MIN_CV, HEMISPHERE_MAX_CV));
}

FLASHMEM void Fungen::ResetChannel(int ch) {
    FnChannel &r = run[ch];
    r.phase = 0;
    r.inc = 0;
    r.from = r.target = r.value = 0;
    r.cur = 0;
    r.state = FN_IDLE;
    for (int i = 0; i < MAX_PTS; ++i) r.loop_ct[i] = 0;
}

// (Re)start the function from point 0 on a rising gate edge
FLASHMEM void Fungen::Retrigger(int ch) {
    for (int i = 0; i < MAX_PTS; ++i) run[ch].loop_ct[i] = 0;
    GotoPoint(ch, 0);
}

// Begin ramping from the current value toward point j
FLASHMEM void Fungen::GotoPoint(int ch, int j) {
    FnChannel &r = run[ch];
    if (j >= npts[ch]) j = npts[ch] - 1;
    r.cur = j;
    r.from = r.value;
    r.target = ValueCV(PtVal(ch, j));
    r.inc = PHASE_ONE / FUNGEN_TICKS[PtTime(ch, j)];
    r.phase = 0;
    r.state = FN_RAMP;
}

FLASHMEM void Fungen::AdvancePoint(int ch) {
    FnChannel &r = run[ch];
    if (r.cur + 1 >= npts[ch]) r.state = FN_DONE; // hold final value
    else GotoPoint(ch, r.cur + 1);
}

// A point has been reached: execute its MIDAS action
FLASHMEM void Fungen::Arrive(int ch, bool gate) {
    FnChannel &r = run[ch];
    const uint8_t i = r.cur;
    switch (PtAct(ch, i)) {
    default:
    case AC_NULL: AdvancePoint(ch); break;
    case AC_SUST:
        if (gate) r.state = FN_SUST;
        else AdvancePoint(ch);
        break;
    case AC_ENBL:
        if (!gate) r.state = FN_ENBL;
        else AdvancePoint(ch);
        break;
    case AC_JUMP: GotoPoint(ch, PtTgt(ch, i)); break;
    case AC_LOOP:
        if (r.loop_ct[i] < PtCnt(ch, i)) {
            ++r.loop_ct[i];
            GotoPoint(ch, PtTgt(ch, i));
        } else {
            r.loop_ct[i] = 0;
            AdvancePoint(ch);
        }
        break;
    case AC_KYUP:
        if (!gate) GotoPoint(ch, PtTgt(ch, i));
        else AdvancePoint(ch);
        break;
    case AC_KYDN:
        if (gate) GotoPoint(ch, PtTgt(ch, i));
        else AdvancePoint(ch);
        break;
    }
}

FLASHMEM void Fungen::View() {
    DrawInterface();
}

FLASHMEM void Fungen::AuxButton() { // switch edited channel
    edit_ch = 1 - edit_ch;
    if (sel_pt >= npts[edit_ch]) sel_pt = npts[edit_ch] - 1;
    CancelEdit();
}

FLASHMEM void Fungen::OnEncoderMove(int direction) {
    if (!EditMode()) {
        MoveCursor(cursor, direction, MAX_CURSOR);
        // skip jump params that don't apply to the selected action
        const uint8_t act = PtAct(edit_ch, sel_pt);
        const bool has_p1 = (act >= AC_JUMP);
        const bool has_p2 = (act == AC_LOOP);
        while ((cursor == PT_PAR1 && !has_p1) || (cursor == PT_PAR2 && !has_p2))
            cursor += (direction > 0) ? 1 : -1;
        return;
    }

    const uint8_t ch = edit_ch;
    uint32_t p = pt[ch][sel_pt];
    switch (cursor) {
    case PT_SEL:
        sel_pt = constrain(sel_pt + direction, 0, npts[ch] - 1);
        return;
    case NUM_PTS:
        npts[ch] = constrain(npts[ch] + direction, 1, MAX_PTS);
        if (sel_pt >= npts[ch]) sel_pt = npts[ch] - 1;
        return;
    case PT_TIME: {
        int t = int(p & 0x7f) + direction;
        p = (p & ~uint32_t(0x7f)) | uint32_t(constrain(t, 0, 127));
        break;
    }
    case PT_VALUE: {
        int v = int8_t((p >> 8) & 0xff) + direction;
        CONSTRAIN(v, -127, 127);
        p = (p & ~uint32_t(0xff << 8)) | (uint32_t(uint8_t(int8_t(v))) << 8);
        break;
    }
    case PT_ACTION: {
        int a = int((p >> 16) & 0x7) + direction;
        p = (p & ~uint32_t(0x7 << 16)) | (uint32_t(constrain(a, 0, int(AC_LAST))) << 16);
        break;
    }
    case PT_PAR1: {
        int t = int((p >> 19) & 0x7) + direction;
        p = (p & ~uint32_t(0x7 << 19)) | (uint32_t(constrain(t, 0, npts[ch] - 1)) << 19);
        break;
    }
    case PT_PAR2: {
        int c = int((p >> 22) & 0xf) + direction;
        p = (p & ~uint32_t(0xf << 22)) | (uint32_t(constrain(c, 1, 15)) << 22);
        break;
    }
    }
    pt[ch][sel_pt] = p;

    // If the live segment targets the edited point, refresh its
    // precomputed increment/target so edits are heard immediately.
    FnChannel &r = run[ch];
    if (r.state == FN_RAMP && r.cur == sel_pt) {
        r.inc = PHASE_ONE / FUNGEN_TICKS[PtTime(ch, sel_pt)];
        r.target = ValueCV(PtVal(ch, sel_pt));
    }
}

FLASHMEM uint64_t Fungen::OnDataRequest() {
    ForEachChannel(ch) {
        for (int w = 0; w < MAX_PTS / 2; ++w) {
            uint64_t blob = uint64_t(pt[ch][w * 2])
                          | (uint64_t(pt[ch][w * 2 + 1]) << 32);
            SetData(ch * 4 + w, blob);
        }
    }
    uint64_t data = 0;
    Pack(data, PackLocation {0, 3}, npts[0] - 1);
    Pack(data, PackLocation {3, 3}, npts[1] - 1);
    return data;
}

FLASHMEM void Fungen::OnDataReceive(uint64_t data) {
    npts[0] = Unpack(data, PackLocation {0, 3}) + 1;
    npts[1] = Unpack(data, PackLocation {3, 3}) + 1;
    ForEachChannel(ch) {
        for (int w = 0; w < MAX_PTS / 2; ++w) {
            uint64_t blob;
            if (GetData(ch * 4 + w, blob)) {
                pt[ch][w * 2]     = uint32_t(blob & 0xffffffff);
                pt[ch][w * 2 + 1] = uint32_t(blob >> 32);
            }
        }
        ResetChannel(ch);
    }
    if (sel_pt >= npts[edit_ch]) sel_pt = npts[edit_ch] - 1;
}

FLASHMEM void Fungen::SetHelp() {
    //                    "-------" <-- Label size guide
    help[HELP_DIGITAL1] = "Gate 1";
    help[HELP_DIGITAL2] = "Gate 2";
    help[HELP_CV1]      = "Rate 1";
    help[HELP_CV2]      = "Rate 2";
    help[HELP_OUT1]     = "Fn A";
    help[HELP_OUT2]     = "Fn B";
    help[HELP_EXTRA1] = "AuxBtn: edit channel";
    help[HELP_EXTRA2] = "Buchla 700 MIDAS fn";
    //                  "---------------------" <-- Extra text size guide
}

FLASHMEM void Fungen::DrawInterface() {
    const uint8_t ch = edit_ch;
    const uint8_t act = PtAct(ch, sel_pt);
    static constexpr const char* const act_names[AC_LAST + 1] = {
        "----", "SUST", "ENBL", "JUMP", "LOOP", "KYUP", "KYDN"
    };

    // Row 1: channel, point x/n, action
    gfxPrint(1, 15, OutputLabel(ch));
    gfxInvert(0, 14, 8, 9);

    gfxPrint(12, 15, "P");
    gfxPrint(sel_pt + 1);
    gfxPrint("/");
    gfxPrint(npts[ch]);
    gfxPrint(40, 15, act_names[act]);

    if (cursor == PT_SEL)    gfxSpicyCursor(12, 23, 12);
    if (cursor == NUM_PTS)   gfxSpicyCursor(24, 23, 12);
    if (cursor == PT_ACTION) gfxSpicyCursor(40, 23, 24);

    // Row 2: time & value of selected point, or jump parameters
    if (cursor == PT_PAR1 || cursor == PT_PAR2) {
        gfxIcon(1, 25, ROTATE_L_ICON);
        gfxPrint(10, 25, "P");
        gfxPrint(PtTgt(ch, sel_pt) + 1);
        if (cursor == PT_PAR1) gfxSpicyCursor(10, 33, 12);
        if (act == AC_LOOP) {
            gfxPrint(34, 25, "x");
            gfxPrint(PtCnt(ch, sel_pt));
            if (cursor == PT_PAR2) gfxSpicyCursor(34, 33, 18);
        }
    } else {
        const uint32_t ms = FUNGEN_TICKS[PtTime(ch, sel_pt)] / HEMISPHERE_CLOCK_TICKS;
        if (ms >= 1000) {
            gfxPrint(1, 25, int(ms / 1000));
            gfxPrint(".");
            gfxPrint(int((ms % 1000) / 100));
            gfxPrint("s");
        } else {
            gfxPrint(1, 25, int(ms));
            gfxPrint("ms");
        }
        const int pct = PtVal(ch, sel_pt) * 100 / 127;
        gfxPos(37 + (pct >= 0 ? 6 : 0), 25);
        gfxPrint(pct);
        if (cursor == PT_TIME)  gfxSpicyCursor(1, 33, 30);
        if (cursor == PT_VALUE) gfxSpicyCursor(37, 33, 25);
    }

    DrawFunction(ch);
}

// point i -> graph x (points spaced evenly; function starts at x=0)
FLASHMEM uint8_t Fungen::PtX(uint8_t ch, uint8_t i) const {
    return ((i + 1) * 62) / npts[ch];
}
// value -> graph y (bipolar, zero line at y=47)
FLASHMEM uint8_t Fungen::ValY(int v) const {
    return 47 - (v * 16) / 127;
}

FLASHMEM void Fungen::DrawFunction(uint8_t ch) {
    // zero line
    gfxDottedLine(0, 47, 63, 47, 8);

    uint8_t px = 0, py = 47; // functions ramp from 0V at trigger
    for (uint8_t i = 0; i < npts[ch]; ++i) {
        const uint8_t x = PtX(ch, i);
        const uint8_t y = ValY(PtVal(ch, i));
        gfxLine(px, py, x, y);
        gfxRect(x - 1, y - 1, 3, 3); // point marker
        px = x; py = y;
    }

    // selected point blinker
    if (EditMode() || CursorBlink()) {
        const uint8_t x = PtX(ch, sel_pt);
        const uint8_t y = ValY(PtVal(ch, sel_pt));
        gfxFrame(x - 3, y - 3, 7, 7);
    }

    // jump arc for the selected point
    const uint8_t act = PtAct(ch, sel_pt);
    if (act >= AC_JUMP) {
        const uint8_t x1 = PtX(ch, sel_pt);
        const uint8_t x2 = PtX(ch, PtTgt(ch, sel_pt));
        gfxDottedLine(x2, 30, x1, 30, 2);
        gfxPixel(x2, 31);
        gfxPixel(x2, 32);
    }

    // playhead
    const FnChannel &r = run[ch];
    if (r.state != FN_IDLE) {
        uint8_t x;
        if (r.state == FN_RAMP) {
            const uint8_t x0 = r.cur ? PtX(ch, r.cur - 1) : 0;
            const uint8_t x1 = PtX(ch, r.cur);
            x = x0 + uint8_t((uint32_t(x1 - x0) * (r.phase >> 16)) >> 15);
        } else {
            x = PtX(ch, r.cur < npts[ch] ? r.cur : npts[ch] - 1);
        }
        gfxDottedLine(x, 30, x, 63, 2);
    }
}
#endif // _HEM_FUNGEN_H_
