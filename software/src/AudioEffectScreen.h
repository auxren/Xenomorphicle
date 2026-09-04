#pragma once
// ---------------------------------------------------------------------------
// AudioEffectScreen -- the SCREEN half of the standalone full-screen effect
// apps, as AudioAppletHost.h is the lifecycle half.
//
// WHY THIS EXISTS, since it was not in the original plan. The host was written
// to be reusable and it is: it holds no parameter and draws nothing, so
// hosting a second effect needed no change to it at all. But writing the
// second app made the actual duplication obvious, and it was not lifecycle --
// it was the grammar. Two apps that share a header, a row pitch, a bar, a
// cursor band, a CV page and a refusal box, each with its own private copy of
// all six, are two apps that will drift apart the first time one of them is
// touched alone. On a 1-bit panel that drift is the whole product: the family
// argument in Audio-Apps-Screens.md is that you open Tweighty, open Delay,
// open Reverb and the same things are in the same places.
//
// So the geometry and every shared mark live here, once. What stays in an app
// is only what is genuinely its own: which parameters exist, what their bars
// and values mean, and what its buttons do.
//
// THE LAYOUT IS TWEIGHTY'S SCR_EDIT (TweightyApp.h:616-647): label at x=4, bar
// at x=34 w=54 h=8, value right-aligned to x=127. The one change is the row
// pitch, 9px -> 10px, which buys a fourth row that clears the footer -- at
// Tweighty's 9px the last cursor band spans y=47..55 and forces gfxFooter() to
// be drawn FIRST or the band erases it (its own comment at :598-601). At 10px
// the last band ends at y=50 and draw order is free.
//
// INVERSION. Exactly one function here inverts anything: CursorBand(). Every
// other state this family shows -- effect live or bypassed, gate high or low,
// out of memory -- is a DRAWN mark: a filled or hollow box, or a frame with
// words in it. That is finding L-06 held to (invert exactly what the right
// encoder will change, and nothing else), and it is enforced by there being
// exactly one invertRect in this file.
// ---------------------------------------------------------------------------

#ifdef ARDUINO_TEENSY41

#include "CVInputMap.h"
#include "HSUtils.h"
#include "OC_ui.h"
#include "util/util_math.h"

namespace AudioEffectScreen {

// Four rows, never five. The body is y=12..53 = 42px; 4 x 10px = 40, so the
// last cursor band ends at y=50 and clears the footer rule at y=54 with 3px to
// spare. An 8px pitch would fit five rows with zero leading and cursor bands
// touching their neighbours' ink on a 1-bit panel.
static constexpr int kMaxRows = 4;
static constexpr int kRowY[kMaxRows] = { 12, 22, 32, 42 };
static constexpr int kRowH = 10;
static constexpr int kLabelX = 4;
static constexpr int kBarX = 34;
static constexpr int kBarW = 54;
static constexpr int kBarH = 8;
// CV page columns: source name, then a narrower live-input bar.
static constexpr int kCvSrcX = 40;
static constexpr int kCvBarX = 64;
static constexpr int kCvBarW = 24;
// The presence box, top right. Clears the longest header this family has
// ("F R E E V E R B", 15 chars, ending x=90) by 28px.
static constexpr int kPresenceX = 118;

// Track + fill, generalising Tweighty's DrawBar (TweightyApp.h:528-534), which
// hard-codes h=8. frac is clamped here so no caller needs its own bounds check.
inline void Bar(int x, int y, int w, int h, float frac) {
  graphics.drawFrame(x, y, w, h);
  if (frac < 0.0f) frac = 0.0f;
  else if (frac > 1.0f) frac = 1.0f;
  const int fillw = (int)((w - 2) * frac + 0.5f);
  if (fillw > 0) graphics.drawRect(x + 1, y + 1, fillw, h - 2);
}

// Signed parameters: same frame, plus a zero datum down the centre column, and
// the fill grows left or right from it. Without the datum a bipolar bar at 0%
// and a unipolar bar at 0% are the same picture.
inline void BipolarBar(int x, int y, int w, int h, float frac) {
  graphics.drawFrame(x, y, w, h);
  if (frac < -1.0f) frac = -1.0f;
  else if (frac > 1.0f) frac = 1.0f;
  const int mid = x + w / 2;
  graphics.drawVLine(mid, y + 1, h - 2);
  const int half = (w - 2) / 2;
  const int fillw = (int)(half * (frac < 0 ? -frac : frac) + 0.5f);
  if (fillw > 0) {
    if (frac >= 0.0f) graphics.drawRect(mid + 1, y + 1, fillw, h - 2);
    else graphics.drawRect(mid - fillw, y + 1, fillw, h - 2);
  }
}

inline void Label(int row, const char *text) {
  graphics.setPrintPos(kLabelX, kRowY[row]);
  graphics.print(text);
}

inline void Value(int row, const char *text) {
  graphics.setPrintPos(127, kRowY[row]);
  graphics.print_right(text);
}

// The ordinary parameter row: name on the left, how much on a bar, how much in
// words on the right. Three answers to the same question at three levels of
// precision, which is what lets one screen serve a glance and an edit.
inline void ParamRow(int row, const char *label, float frac, const char *value) {
  Label(row, label);
  Bar(kBarX, kRowY[row], kBarW, kBarH, frac);
  Value(row, value);
}

inline void BipolarParamRow(int row, const char *label, float frac,
                            const char *value) {
  Label(row, label);
  BipolarBar(kBarX, kRowY[row], kBarW, kBarH, frac);
  Value(row, value);
}

// A row with a value but no bar -- an enumerated setting, where a bar would
// imply a quantity that is not there.
inline void EnumRow(int row, const char *label, const char *value) {
  Label(row, label);
  Value(row, value);
}

// One CV destination: where it comes from, how much is arriving right now, and
// how much of it is being applied.
//
// This replaces BOTH the inline CV columns in these applets' View() methods and
// the gfxDisplayInputMapEditor() overlay -- worth stating, because that overlay
// ends in gfxInvert(0,0,63,11), an inverted banner and an L-06 violation in its
// own right (HemisphereApplet.h:344-370). Not used by anything here.
//
// The live bar is InRescaled(12) into 24px of WIDTH, not the shipped
// gfxPrint(CVInputMap&) (HemisphereApplet.cpp:239-246), whose line grows up to
// 24 pixels tall UPWARD from the row baseline -- at this 10px row pitch that
// would bleed through the two rows above it.
inline void CvRow(int row, const char *label, CVInputMap &map) {
  Label(row, label);

  graphics.setPrintPos(kCvSrcX, kRowY[row]);
  graphics.print(map.InputName());

  BipolarBar(kCvBarX, kRowY[row], kCvBarW, kBarH,
             (float)map.InRescaled(12) / 12.0f);

  // Atten() returns tenths of a percent (util_math.h:55). Whole percent is what
  // fits, and a tenth of a percent of attenuversion is not a thing anybody sets
  // by ear.
  char buf[12];
  snprintf(buf, sizeof(buf), "%d%%", Atten(map.attenuversion) / 10);
  Value(row, buf);
}

// Title plus the presence box. Filled = the effect is reaching the output bus,
// hollow = bypassed. Same x, same 8x8 geometry and the same filled-means-live
// semantics as Tweighty's transport box (TweightyApp.h:556-557), and the same
// banana-jack presence idiom the rig's design system uses everywhere.
inline void Header(const char *title, bool live) {
  gfxHeader(title);
  if (live) graphics.drawRect(kPresenceX, 1, 8, 8);
  else graphics.drawFrame(kPresenceX, 1, 8, 8);
}

// THE ONE INVERSION IN THIS FAMILY. encL turn moves the cursor and encR turn
// changes that row's value, with no edit mode, so the row the cursor is on and
// the value the right encoder will change are the same object -- which makes
// this literally `invert exactly what the right encoder will change`, the rule
// stated at PresetBusUI.cpp:433 and adopted as finding L-06.
inline void CursorBand(int row) {
  graphics.invertRect(0, kRowY[row] - 1, 128, kRowH);
}

// A refusal is a DRAWN box, never an inverted band -- L-06 is explicit that
// banners get a frame. The frame's interior is x=11..116, so `text` has 17
// characters to work with; at 16 (the longest this family uses) x=19 puts it at
// x=19..114, inside on both sides.
inline void RefusalBox(const char *text) {
  graphics.drawFrame(10, 22, 108, 20);
  graphics.setPrintPos(19, 26);
  graphics.print(text);
}

// X held is the fine-adjust modifier across the whole family, read live off the
// pin the way Tweighty reads its own held modifier (TweightyApp.h:513).
inline bool Fine() {
  return OC::ui.read_immediate(OC::CONTROL_BUTTON_X);
}

// Coarse steps are sized so a full sweep is a wrist's worth of turning rather
// than a minute of it; X held gives the finest step the parameter actually HAS.
// Deliberately not a blanket "divide by ten": on a row whose value is a whole
// percent a tenth of a step does not exist, and a modifier that silently does
// nothing is exactly the defect this family exists to stop repeating.
inline int Step(int coarse, int fine) {
  return Fine() ? fine : coarse;
}

}  // namespace AudioEffectScreen

#endif  // ARDUINO_TEENSY41
