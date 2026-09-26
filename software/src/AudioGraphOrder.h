#pragma once
// Which patch cables in the live audio graph run backwards through the
// update list -- and therefore cost a whole audio block of latency.
//
// The Teensy Audio library has one singly-linked list of every AudioStream
// ever constructed (AudioStream::first_update, built by the AudioStream
// constructor at AudioStream.h:149-156). The audio ISR walks that list in
// order and calls update() on each node. There is no destructor, nothing
// unlinks, and nothing reorders: **update order is construction order, for
// the life of the process.** The library's own comment there calls this a
// TODO awaiting "a proper data flow analysis".
//
// That makes a patch cable's cost depend entirely on where its two ends sit
// in that list:
//
//   source earlier than destination -> the destination runs second and
//     consumes the block the source just produced. Zero added latency.
//   source later than destination   -> the destination already ran. It
//     consumed whatever the source left from the PREVIOUS pass: one whole
//     audio block of extra latency (2.667 ms at 48 kHz / 128 samples),
//     permanently, and invisibly -- the audio is correct, just late.
//
// This is a silent failure mode. Nothing faults, no counter moves, the
// signal is present and sounds right. It is only findable by reading
// declaration order across many files by hand, which is how the monitor-mix
// and InputApplet srcmix cases were found. This header exists so the module
// can be asked the question directly instead.
//
// The pure analysis below is split out so it is host-testable
// (test/test_audio_graph_order.cpp); the live walk needs the hardware.

#include <stdint.h>

namespace OC {
namespace AudioGraph {

// One patch cable, with both endpoints given as their position in the
// update list rather than as pointers, so the analysis is pure.
struct Edge {
  uint16_t src;     // index of the source in the update list
  uint16_t dst;     // index of the destination
  uint8_t  src_ch;
  uint8_t  dst_ch;
  bool     f32;     // an AudioConnection_F32 rather than an AudioConnection
};

// A cable pointing backwards through the update list.
//
// Strictly greater, not >=: src == dst is a node wired into itself, which is
// feedback. A one-block delay is the whole point of a feedback path, so
// counting it as a defect would flag every delay and reverb applet in the
// tree and bury the real findings.
inline bool EdgeIsLate(const Edge& e) { return e.src > e.dst; }
inline bool EdgeIsFeedback(const Edge& e) { return e.src == e.dst; }

struct Stats {
  uint16_t nodes = 0;
  uint16_t edges = 0;
  uint16_t late = 0;          // cables costing one extra block
  uint16_t feedback = 0;      // self-connections, reported but not faulted
  uint16_t worst_node = 0;    // the destination fed by the most late cables
  uint16_t worst_late_in = 0; // how many that is
};

// How many of `node`'s inbound cables are late. This is the number that
// answers "is this sink constructed before the things that feed it?" -- a
// sink whose every inbound cable is late is a sink that was constructed too
// early, which is exactly what a namespace-scope static sink looks like.
inline uint16_t LateInboundForNode(const Edge* edges, uint16_t n, uint16_t node) {
  uint16_t count = 0;
  for (uint16_t i = 0; i < n; ++i) {
    if (edges[i].dst == node && EdgeIsLate(edges[i])) ++count;
  }
  return count;
}

inline Stats Summarize(const Edge* edges, uint16_t n, uint16_t nodes) {
  Stats s;
  s.nodes = nodes;
  s.edges = n;
  for (uint16_t i = 0; i < n; ++i) {
    if (EdgeIsFeedback(edges[i])) { ++s.feedback; continue; }
    if (EdgeIsLate(edges[i])) ++s.late;
  }
  // Second pass rather than a running tally: worst_node is a property of a
  // destination, and a destination's edges are not contiguous in the list.
  for (uint16_t i = 0; i < n; ++i) {
    if (!EdgeIsLate(edges[i])) continue;
    uint16_t c = LateInboundForNode(edges, n, edges[i].dst);
    if (c > s.worst_late_in) { s.worst_late_in = c; s.worst_node = edges[i].dst; }
  }
  return s;
}

}  // namespace AudioGraph
}  // namespace OC

// The live walk. XENO_CODEC_AUDIO, not AUDIO_INTERFACE and not
// ARDUINO_TEENSY41 -- see platformio.ini's [env] comment and
// tools/ci/fork-gates.sh gate 6. It additionally needs AUDIO_DEBUG_CLASS,
// which unlocks the core's AudioDebug accessor (AudioStream.h:205-230); that
// flag adds only friend declarations and a header-only class, so it changes
// no layout and no behaviour.
#if defined(XENO_CODEC_AUDIO) && defined(AUDIO_DEBUG_CLASS)
class Print;
namespace OC {
namespace AudioGraph {
// Walks the live update list and both connection lists and prints a report.
// Loop context only: it is O(nodes * edges) and allocates nothing.
void Report(Print& out);
}  // namespace AudioGraph
}  // namespace OC
#endif
