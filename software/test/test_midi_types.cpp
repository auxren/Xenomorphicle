// Host-side tests for the MIDI note stack and output-port serialization.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 \
//   -o build/test_midi_types test_midi_types.cpp && ./build/test_midi_types
#include "../src/HSMIDITypes.h"
#include <cassert>
#include <cstdio>

using namespace HS;

static int checks = 0;
#define CHECK(cond) do { assert(cond); checks++; } while (0)

int main() {
  // --- NoteBuffer: insertion order, size, front/back ---
  {
    NoteBuffer b;
    CHECK(b.empty() && b.size() == 0);
    b.push(60, 100);
    b.push(64, 90);
    b.push(67, 80);
    CHECK(b.size() == 3);
    CHECK(b.front().note == 60);
    CHECK(b.back().note == 67);
  }

  // --- NoteBuffer: remove middle, remove absent, remove duplicates ---
  {
    NoteBuffer b;
    b.push(60, 100);
    b.push(64, 90);
    b.push(67, 80);
    b.remove(64);
    CHECK(b.size() == 2 && b.front().note == 60 && b.back().note == 67);
    b.remove(99); // absent: no-op
    CHECK(b.size() == 2);
    b.push(60, 50); // re-push after external remove keeps single entry
    b.remove(60);
    CHECK(b.size() == 1 && b.front().note == 67);
  }

  // --- NoteBuffer: overflow drops oldest, keeps newest ---
  {
    NoteBuffer b;
    for (uint8_t i = 0; i < NoteBuffer::kCapacity + 3; ++i)
      b.push(20 + i, 100);
    CHECK(b.size() == NoteBuffer::kCapacity);
    CHECK(b.front().note == 20 + 3);                          // oldest 3 dropped
    CHECK(b.back().note == 20 + NoteBuffer::kCapacity + 2);   // newest kept
  }

  // --- NoteBuffer: empty-access guards (no UB) ---
  {
    NoteBuffer b;
    (void)b.back();  // must not crash
    (void)b.at(5);   // out of range clamps
    b.push(72, 64);
    CHECK(b.at(0).note == 72);
    CHECK(b.at(9).note == 72); // out-of-range index clamps to 0
    b.clear();
    CHECK(b.empty());
  }

  // --- NoteBuffer: range-for iteration ---
  {
    NoteBuffer b;
    b.push(1, 10); b.push(2, 20); b.push(3, 30);
    int sum = 0;
    for (auto const &d : b) sum += d.note;
    CHECK(sum == 6);
    const NoteBuffer &cb = b;
    sum = 0;
    for (auto const &d : cb) sum += d.vel;
    CHECK(sum == 60);
  }

  // --- MIDIOutSettings: default roundtrip ---
  {
    MIDIOutSettings a, b;
    b.Unpack(a.Pack());
    CHECK(b.function == a.function && b.channel == a.channel &&
          b.data1 == a.data1 && b.data2 == a.data2 &&
          b.transpose == a.transpose && b.range_low == a.range_low &&
          b.range_high == a.range_high && b.flags == a.flags &&
          b.clkdiv == a.clkdiv);
  }

  // --- MIDIOutSettings: exhaustive per-field roundtrip ---
  {
    for (int fn = 0; fn < 16; ++fn)
    for (int ch = 0; ch < 16; ++ch) {
      MIDIOutSettings a;
      a.function = fn;
      a.channel = ch;
      MIDIOutSettings b;
      b.Unpack(a.Pack());
      CHECK(b.function == fn && b.channel == ch);
    }
    for (int v = 0; v < 128; ++v) {
      MIDIOutSettings a;
      a.data1 = v; a.data2 = 127 - v;
      a.range_low = v; a.range_high = 127; // keep low<=high (Sanitize)
      MIDIOutSettings b;
      b.Unpack(a.Pack());
      CHECK(b.data1 == v && b.data2 == 127 - v && b.range_low == v && b.range_high == 127);
    }
    for (int t = -48; t <= 48; ++t) { // negative transpose must survive
      MIDIOutSettings a;
      a.transpose = int8_t(t);
      MIDIOutSettings b;
      b.Unpack(a.Pack());
      CHECK(b.transpose == t);
    }
    for (int f = 0; f < 256; ++f) {
      MIDIOutSettings a;
      a.flags = f;
      MIDIOutSettings b;
      b.Unpack(a.Pack());
      CHECK(b.flags == f);
    }
    for (int d = 0; d < 16; ++d) {
      MIDIOutSettings a;
      a.clkdiv = d;
      MIDIOutSettings b;
      b.Unpack(a.Pack());
      CHECK(b.clkdiv == d);
    }
  }

  // --- MIDIOutSettings: Sanitize clamps garbage ---
  {
    MIDIOutSettings a;
    a.range_low = 100; a.range_high = 20;
    a.Sanitize();
    CHECK(a.range_high >= a.range_low);
  }

  // --- velocity_source accessor ---
  {
    MIDIOutSettings a;
    a.flags = uint8_t(3 << MIDIOutSettings::VEL_SOURCE_SHIFT) | MIDIOutSettings::FLAG_QUANTIZE;
    CHECK(a.velocity_source() == 3);
    CHECK(a.flags & MIDIOutSettings::FLAG_QUANTIZE);
  }

  // --- MIDIOutPort: runtime reset ---
  {
    MIDIOutPort p;
    p.gated = true; p.prev_gate = true; p.last_note = 61;
    p.last_value = 42; p.lag_count = 9; p.indicator = 5;
    p.ResetRuntime();
    CHECK(!p.gated && !p.prev_gate && p.last_note == 0 &&
          p.last_value == 0xffff && p.lag_count == 0 && p.indicator == 0);
  }

  printf("test_midi_types: all %d checks passed\n", checks);
  return 0;
}
