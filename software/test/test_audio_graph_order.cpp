// Host tests for the audio graph's update-order analysis
// (src/AudioGraphOrder.h): which patch cables cross the update list
// backwards, and therefore cost a whole audio block of latency.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_audio_graph_order test_audio_graph_order.cpp && ./build/...
#include <cstdio>
#include <vector>

#include "../src/AudioGraphOrder.h"

using OC::AudioGraph::Edge;
using OC::AudioGraph::Stats;

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static Edge mk(uint16_t src, uint16_t dst) { return Edge{src, dst, 0, 0, false}; }

// The audio ISR walks one list in construction order. A cable from a node
// EARLIER in that list to one LATER is free: the destination runs after the
// source and consumes the block just produced.
static void test_a_forward_cable_is_not_late() {
  CHECK(!OC::AudioGraph::EdgeIsLate(mk(3, 7)));
  CHECK(!OC::AudioGraph::EdgeIsFeedback(mk(3, 7)));
}

// A cable pointing backwards is the defect: the destination already ran this
// pass, so it consumed what the source produced on the PREVIOUS pass.
static void test_a_backward_cable_is_late() {
  CHECK(OC::AudioGraph::EdgeIsLate(mk(7, 3)));
}

// A node wired into itself is feedback, which is inherently a one-block
// delay -- that is the point of it, not an ordering mistake. It must not be
// reported as a defect or every delay/reverb applet would show up as broken.
static void test_self_connection_is_feedback_not_lateness() {
  CHECK(!OC::AudioGraph::EdgeIsLate(mk(4, 4)));
  CHECK(OC::AudioGraph::EdgeIsFeedback(mk(4, 4)));
}

static void test_summary_counts_each_kind() {
  std::vector<Edge> e{mk(0, 1), mk(1, 2), mk(5, 2), mk(3, 3), mk(9, 4)};
  Stats s = OC::AudioGraph::Summarize(e.data(), (uint16_t)e.size(), 10);
  CHECK(s.nodes == 10);
  CHECK(s.edges == 5);
  CHECK(s.late == 2);       // 5->2 and 9->4; 3->3 is feedback, not late
  CHECK(s.feedback == 1);
}

static void test_empty_graph_is_clean() {
  Stats s = OC::AudioGraph::Summarize(nullptr, 0, 0);
  CHECK(s.nodes == 0 && s.edges == 0 && s.late == 0 && s.feedback == 0);
  CHECK(s.worst_late_in == 0);
}

static void test_late_inbound_counts_only_that_node() {
  // dst 3 so that a forward inbound cable is even possible: nothing can
  // arrive at node 0 forwards, every inbound cable there is late by
  // definition.
  std::vector<Edge> e{mk(5, 3), mk(6, 3), mk(1, 3), mk(7, 2)};
  CHECK(OC::AudioGraph::LateInboundForNode(e.data(), 4, 3) == 2);
  CHECK(OC::AudioGraph::LateInboundForNode(e.data(), 4, 2) == 1);
  CHECK(OC::AudioGraph::LateInboundForNode(e.data(), 4, 1) == 0);
}

// The shape this instrument was written to find: output_route is a
// namespace-scope static, so it is constructed -- and therefore updated --
// before every applet that feeds it.
static void test_static_sink_fed_by_runtime_sources_is_all_late() {
  std::vector<Edge> e{mk(5, 0), mk(6, 0), mk(7, 0)};
  Stats s = OC::AudioGraph::Summarize(e.data(), 3, 8);
  CHECK(s.late == 3);
  CHECK(s.worst_node == 0);
  CHECK(s.worst_late_in == 3);
}

// The other shape: an F32 applet's edge adapters are base-class members, so
// both are constructed before the derived DSP. input_adapter -> dsp is fine;
// dsp -> output_adapter points backwards.
static void test_f32_applet_edge_adapters() {
  const uint16_t in_adapter = 0, out_adapter = 1, dsp = 2;
  CHECK(!OC::AudioGraph::EdgeIsLate(mk(in_adapter, dsp)));
  CHECK(OC::AudioGraph::EdgeIsLate(mk(dsp, out_adapter)));
}

int main() {
  test_a_forward_cable_is_not_late();
  test_a_backward_cable_is_late();
  test_self_connection_is_feedback_not_lateness();
  test_summary_counts_each_kind();
  test_empty_graph_is_clean();
  test_late_inbound_counts_only_that_node();
  test_static_sink_fed_by_runtime_sources_is_all_late();
  test_f32_applet_edge_adapters();
  printf("test_audio_graph_order: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
