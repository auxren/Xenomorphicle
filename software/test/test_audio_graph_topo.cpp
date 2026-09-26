// Host tests for the audio graph's topological sort (src/AudioGraphOrder.h).
//
// The sort's job: produce an update order in which every source is visited
// before the destinations it feeds, so no block is ever consumed a cycle
// late. Two properties matter more than the ordering itself, because
// getting either wrong corrupts the list the audio ISR walks:
//   1. the result is a PERMUTATION of the input -- every node exactly once;
//   2. feedback loops, which are normal in delays and reverbs, must not
//      cause a node to be dropped or duplicated.
#include <cstdio>
#include <cstdint>
#include <vector>
#include <set>

#include "../src/AudioGraphOrder.h"

using OC::AudioGraph::Edge;

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static Edge mk(uint16_t s, uint16_t d) { return Edge{s, d, 0, 0, false}; }

struct Result {
  std::vector<uint16_t> order;
  uint16_t cycles = 0;
  uint16_t count = 0;
};

static Result run(const std::vector<Edge>& e, uint16_t nodes) {
  Result r;
  r.order.assign(nodes ? nodes : 1, 0xFFFF);
  std::vector<uint16_t> scratch(nodes ? nodes : 1, 0);
  r.count = OC::AudioGraph::TopoSort(e.data(), (uint16_t)e.size(), nodes,
                                     r.order.data(), scratch.data(), &r.cycles);
  return r;
}

// Every node exactly once, whatever the graph looks like.
static void expect_permutation(const Result& r, uint16_t nodes) {
  CHECK(r.count == nodes);
  std::set<uint16_t> seen(r.order.begin(), r.order.begin() + r.count);
  CHECK(seen.size() == nodes);
  for (uint16_t i = 0; i < nodes; ++i) CHECK(seen.count(i) == 1);
}

static int pos_of(const Result& r, uint16_t node) {
  for (uint16_t i = 0; i < r.count; ++i) if (r.order[i] == node) return (int)i;
  return -1;
}

static void test_already_correct_order_is_preserved() {
  Result r = run({mk(0, 1), mk(1, 2)}, 3);
  expect_permutation(r, 3);
  CHECK(pos_of(r, 0) < pos_of(r, 1));
  CHECK(pos_of(r, 1) < pos_of(r, 2));
  CHECK(r.cycles == 0);
}

// The real shape on the module: a sink constructed early, fed by sources
// constructed late. After the sort the sources must all precede it.
static void test_backwards_chain_is_repaired() {
  Result r = run({mk(2, 0), mk(1, 0)}, 3);
  expect_permutation(r, 3);
  CHECK(pos_of(r, 2) < pos_of(r, 0));
  CHECK(pos_of(r, 1) < pos_of(r, 0));
  CHECK(r.cycles == 0);
}

// A self-connection is feedback, not a dependency. If it were treated as
// one the node could never reach in-degree zero and would be reported as a
// cycle -- which would flag every delay and reverb applet.
static void test_self_edge_is_not_a_dependency() {
  Result r = run({mk(0, 1), mk(1, 1), mk(1, 2)}, 3);
  expect_permutation(r, 3);
  CHECK(r.cycles == 0);
  CHECK(pos_of(r, 0) < pos_of(r, 1));
  CHECK(pos_of(r, 1) < pos_of(r, 2));
}

// A genuine two-node feedback loop cannot be ordered correctly -- that is
// what feedback means. It must still come back as a permutation, and be
// counted so the caller can report it.
static void test_two_node_cycle_still_returns_every_node() {
  Result r = run({mk(0, 1), mk(1, 0)}, 2);
  expect_permutation(r, 2);
  CHECK(r.cycles == 2);
}

// A feedback loop hanging off an otherwise orderable graph must not stop
// the rest from being ordered.
static void test_cycle_does_not_poison_the_acyclic_part() {
  // 0 -> 1, and 2 <-> 3 is a loop
  Result r = run({mk(0, 1), mk(2, 3), mk(3, 2)}, 4);
  expect_permutation(r, 4);
  CHECK(pos_of(r, 0) < pos_of(r, 1));
  CHECK(r.cycles == 2);
}

static void test_diamond() {
  // 0 -> 1, 0 -> 2, 1 -> 3, 2 -> 3
  Result r = run({mk(0, 1), mk(0, 2), mk(1, 3), mk(2, 3)}, 4);
  expect_permutation(r, 4);
  CHECK(pos_of(r, 0) < pos_of(r, 1));
  CHECK(pos_of(r, 0) < pos_of(r, 2));
  CHECK(pos_of(r, 1) < pos_of(r, 3));
  CHECK(pos_of(r, 2) < pos_of(r, 3));
  CHECK(r.cycles == 0);
}

// Nodes nothing connects to must survive. An audio graph is full of them:
// an applet that is loaded but not patched in still has to be updated.
static void test_isolated_nodes_survive() {
  Result r = run({mk(0, 1)}, 5);
  expect_permutation(r, 5);
  CHECK(r.cycles == 0);
}

static void test_empty_and_single() {
  Result r0 = run({}, 0);
  CHECK(r0.count == 0);
  Result r1 = run({}, 1);
  expect_permutation(r1, 1);
}

// After sorting, re-running the lateness analysis over the same edges
// expressed in the NEW positions must report no late cables at all, except
// those belonging to genuine feedback loops. This is the end-to-end claim.
static void test_sorted_graph_has_no_late_cables() {
  std::vector<Edge> e{mk(5, 0), mk(6, 0), mk(7, 0), mk(0, 1), mk(4, 2)};
  Result r = run(e, 8);
  expect_permutation(r, 8);
  CHECK(r.cycles == 0);
  uint16_t late = 0;
  for (const Edge& x : e) {
    Edge moved{(uint16_t)pos_of(r, x.src), (uint16_t)pos_of(r, x.dst), 0, 0, false};
    if (OC::AudioGraph::EdgeIsLate(moved)) ++late;
  }
  CHECK(late == 0);
}

// A long backwards chain: 9->8->7->...->0. Every cable points the wrong
// way; the sort must reverse the whole thing.
static void test_fully_reversed_chain() {
  std::vector<Edge> e;
  for (uint16_t i = 9; i > 0; --i) e.push_back(mk(i, (uint16_t)(i - 1)));
  Result r = run(e, 10);
  expect_permutation(r, 10);
  CHECK(r.cycles == 0);
  uint16_t late = 0;
  for (const Edge& x : e) {
    Edge moved{(uint16_t)pos_of(r, x.src), (uint16_t)pos_of(r, x.dst), 0, 0, false};
    if (OC::AudioGraph::EdgeIsLate(moved)) ++late;
  }
  CHECK(late == 0);
}

int main() {
  test_already_correct_order_is_preserved();
  test_backwards_chain_is_repaired();
  test_self_edge_is_not_a_dependency();
  test_two_node_cycle_still_returns_every_node();
  test_cycle_does_not_poison_the_acyclic_part();
  test_diamond();
  test_isolated_nodes_survive();
  test_empty_and_single();
  test_sorted_graph_has_no_late_cables();
  test_fully_reversed_chain();
  printf("test_audio_graph_topo: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
