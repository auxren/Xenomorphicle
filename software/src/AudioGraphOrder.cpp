// The live half of src/AudioGraphOrder.h: walk the real audio graph on the
// module and report every patch cable that runs backwards through the
// update list. See that header for why such a cable costs an audio block.
#include "AudioGraphOrder.h"

#if defined(XENO_CODEC_AUDIO) && defined(AUDIO_DEBUG_CLASS)

#include <Arduino.h>
#include <AudioStream.h>

#include "AudioIO.h"
#include "extern/f32/AudioStream_F32.h"

// Write access to AudioStream's private update-list links.
//
// `first_update` and `next_update` are private, and the core's AudioDebug
// friend (AudioStream.h:205-230) offers getters only, so reordering the list
// cannot be done through the sanctioned interface. The two alternatives were
// patching the vendored header -- which lives outside this repo, so CI would
// build something different from the bench -- or this: the explicit
// instantiation access rule.
//
// It is not a hack in the "undefined behaviour" sense. [temp.spec] says
// access checking is not performed on the names used in an explicit
// instantiation, which makes `&AudioStream::next_update` legal there even
// though it is private. The idiom is well known and standard-conforming; it
// is simply unusual, which is why Reorder() cross-checks the pointers it
// gets here against AudioDebug's getters before trusting them. If a library
// update ever moves these, the check fails and the reorder is refused rather
// than silently corrupting the list the audio ISR walks.
namespace privacy {
template <typename Tag, typename Tag::type M>
struct Rob { friend typename Tag::type get(Tag) { return M; } };

struct NextUpd {
  using type = AudioStream* AudioStream::*;
  friend type get(NextUpd);
};
template struct Rob<NextUpd, &AudioStream::next_update>;

struct FirstUpd {
  using type = AudioStream**;
  friend type get(FirstUpd);
};
template struct Rob<FirstUpd, &AudioStream::first_update>;
}  // namespace privacy

// Provided by the linker script (slot*.ld): the first address the heap can
// hand out. Must sit at file scope -- inside a namespace, even declared
// extern "C", GCC mangles it into that namespace and the link fails.
extern "C" unsigned long _heap_start;

namespace OC {
namespace AudioGraph {

// AudioIO.cpp defines the strong version so its own globals get names; any
// other build gets the weak one and prints bare addresses, which is still a
// usable report.
__attribute__((weak)) const char* NodeName(const void*) { return nullptr; }

namespace {

// Bounded, and honest about it: a truncated walk reports that it truncated
// rather than quietly analysing part of the graph. DMAMEM keeps ~5 KB out of
// DTCM, which is this firmware's binding constraint (see docs/Timing-Budget).
constexpr uint16_t kMaxNodes = 256;
constexpr uint16_t kMaxEdges = 512;

DMAMEM AudioStream* g_nodes[kMaxNodes];
DMAMEM Edge         g_edges[kMaxEdges];
DMAMEM uint16_t     g_order[kMaxNodes];
DMAMEM uint16_t     g_scratch[kMaxNodes];
DMAMEM uint8_t      g_seen[kMaxNodes];

uint16_t g_node_count = 0;
uint16_t g_edge_count = 0;
bool     g_nodes_truncated = false;
bool     g_edges_truncated = false;

// Everything below _heap_start was constructed before main() (a static or a
// DMAMEM global); everything at or above it came from calloc/new at runtime.
// That single line explains almost every late cable in this graph: the audio
// infrastructure is static, the applets are built on demand by
// Registry::get(), so an applet can only ever be constructed -- and updated
// -- after the infrastructure it feeds.
bool IsRuntime(const void* p) {
  return (const void*)p >= (const void*)&_heap_start;
}

uint16_t IndexOf(const AudioStream* s) {
  for (uint16_t i = 0; i < g_node_count; ++i) {
    if (g_nodes[i] == s) return i;
  }
  return 0xFFFF;
}

void AddEdge(const AudioStream* src, const AudioStream* dst,
             uint8_t sch, uint8_t dch, bool f32) {
  const uint16_t si = IndexOf(src), di = IndexOf(dst);
  // A cable whose endpoint is not in the update list cannot happen: every
  // AudioStream links itself in at construction. Drop it rather than record
  // a bogus index, and let the edge count show the discrepancy.
  if (si == 0xFFFF || di == 0xFFFF) return;
  if (g_edge_count >= kMaxEdges) { g_edges_truncated = true; return; }
  g_edges[g_edge_count++] = Edge{si, di, sch, dch, f32};
}

// Collect the whole graph with the audio ISR held off, so the lists cannot
// be re-patched underneath the walk. No printing happens in here: Serial is
// far too slow to sit inside an audio mute.
void Collect() {
  AudioDebug dbg;
  AudioDebug_F32 dbg32;

  g_node_count = 0;
  g_edge_count = 0;
  g_nodes_truncated = false;
  g_edges_truncated = false;

  AudioNoInterrupts();

  for (AudioStream* p = dbg.firstUpdate(OC::AudioIO::OutputStream()); p;
       p = dbg.nextUpdate(*p)) {
    if (g_node_count >= kMaxNodes) { g_nodes_truncated = true; break; }
    g_nodes[g_node_count++] = p;
  }

  // int16 cables: every node carries them, F32 streams included -- an
  // AudioStream_F32 inherits the base destination_list for its i16 outputs.
  for (uint16_t i = 0; i < g_node_count; ++i) {
    for (AudioConnection* c = dbg.dstList(*g_nodes[i]); c; c = dbg.getNext(*c)) {
      if (!dbg.isConnected(*c)) continue;
      AddEdge(dbg.getSrc(*c), dbg.getDst(*c), dbg.getSrcN(*c), dbg.getDstN(*c), false);
    }
  }

  // F32 cables, reached through our own registry because -fno-rtti leaves no
  // way to spot an AudioStream_F32 inside the shared update list.
  for (AudioStream_F32* p = dbg32.firstF32(); p; p = dbg32.nextF32(*p)) {
    for (AudioConnection_F32* c = dbg32.dstList(*p); c; c = dbg32.getNext(*c)) {
      if (!dbg32.isConnected(*c)) continue;
      AddEdge(dbg32.getSrc(*c), dbg32.getDst(*c), dbg32.getSrcN(*c), dbg32.getDstN(*c), true);
    }
  }

  AudioInterrupts();
}

void PrintNode(Print& out, uint16_t idx) {
  if (idx >= g_node_count) { out.printf("#%u ??", idx); return; }
  const AudioStream* n = g_nodes[idx];
  const char* name = NodeName(n);
  const char* origin = IsRuntime(n) ? "runtime" : "static ";
  if (name) out.printf("#%-3u %s %s", idx, origin, name);
  else      out.printf("#%-3u %s %p", idx, origin, (const void*)n);
}

}  // namespace

void Report(Print& out) {
  Collect();

  const Stats s = Summarize(g_edges, g_edge_count, g_node_count);

  uint16_t f32_edges = 0;
  for (uint16_t i = 0; i < g_edge_count; ++i) if (g_edges[i].f32) ++f32_edges;

  out.printf("audio graph: %u nodes, %u cables (%u i16, %u f32)\n",
             s.nodes, s.edges, (unsigned)(s.edges - f32_edges), f32_edges);
  if (g_nodes_truncated) out.printf("  WARNING: more than %u nodes; walk truncated\n", kMaxNodes);
  if (g_edges_truncated) out.printf("  WARNING: more than %u cables; walk truncated\n", kMaxEdges);

  // One audio block at the engine's real rate. AUDIO_SAMPLE_RATE_EXACT is
  // 48000 here, not the library's default 44100, so 128 samples is 2.667 ms.
  // Printed from integer microseconds on purpose: TEENSY_OPT_SMALLEST_CODE
  // links a newlib whose printf has no float support, so a %f here silently
  // prints nothing at all -- which is exactly what the first bench run did.
  const uint32_t block_us =
      (uint32_t)((1000000ull * AUDIO_BLOCK_SAMPLES) / (uint32_t)AUDIO_SAMPLE_RATE_EXACT);

  // Split the late cables by cause: the three classes need different fixes,
  // and lumping them together hides the one that is cheap to fix.
  uint16_t ss = 0, rs = 0, rr = 0;
  for (uint16_t i = 0; i < g_edge_count; ++i) {
    if (!EdgeIsLate(g_edges[i])) continue;
    const bool src_rt = IsRuntime(g_nodes[g_edges[i].src]);
    const bool dst_rt = IsRuntime(g_nodes[g_edges[i].dst]);
    if (!src_rt && !dst_rt) ++ss;
    else if (src_rt && !dst_rt) ++rs;
    else ++rr;
  }

  out.printf("  feedback: %u self-connections (inherent, not a defect)\n", s.feedback);
  out.printf("  late: %u cables, %lu.%03lu ms each\n", s.late,
             (unsigned long)(block_us / 1000), (unsigned long)(block_us % 1000));
  out.printf("    %u static  -> static   (declaration/link order; fixable here)\n", ss);
  out.printf("    %u runtime -> static   (applet feeds fixed infrastructure)\n", rs);
  out.printf("    %u runtime -> runtime  (ordering inside one applet)\n", rr);

  if (s.late == 0) {
    out.println("  every cable runs forwards: no avoidable block latency");
    return;
  }

  out.print("  worst sink: ");
  PrintNode(out, s.worst_node);
  out.printf("  <- %u late cables\n", s.worst_late_in);
  out.println("  --- late cables (source updates AFTER its destination) ---");
  for (uint16_t i = 0; i < g_edge_count; ++i) {
    if (!EdgeIsLate(g_edges[i])) continue;
    out.print("    ");
    PrintNode(out, g_edges[i].src);
    out.printf(":%u -> ", g_edges[i].src_ch);
    PrintNode(out, g_edges[i].dst);
    out.printf(":%u  %s\n", g_edges[i].dst_ch, g_edges[i].f32 ? "f32" : "i16");
  }
}

bool Reorder(Print* out) {
  Collect();

  if (g_nodes_truncated || g_edges_truncated) {
    if (out) out->println("reorder refused: the walk truncated, so the graph is only partly known");
    return false;
  }
  if (g_node_count == 0) return false;

  // Cross-check the private links against the library's own getters before
  // writing through them. Reading agreement on the head and on every node's
  // successor proves the member pointers alias the same storage AudioDebug
  // reports, which is the thing that would break if the library changed.
  AudioDebug dbg;
  auto next_of = get(privacy::NextUpd{});
  auto first_pp = get(privacy::FirstUpd{});
  if (*first_pp != dbg.firstUpdate(*g_nodes[0])) {
    if (out) out->println("reorder refused: private head link disagrees with AudioDebug");
    return false;
  }
  for (uint16_t i = 0; i < g_node_count; ++i) {
    if (g_nodes[i]->*next_of != dbg.nextUpdate(*g_nodes[i])) {
      if (out) out->printf("reorder refused: private next link disagrees at node %u\n", i);
      return false;
    }
  }

  uint16_t cycles = 0;
  const uint16_t produced =
      TopoSort(g_edges, g_edge_count, g_node_count, g_order, g_scratch, &cycles);

  // The sort must return every node exactly once. Anything else would hand
  // the ISR a list that skips a live object or loops forever, so this is
  // checked rather than assumed.
  if (produced != g_node_count) {
    if (out) out->printf("reorder refused: sort produced %u of %u nodes\n", produced, g_node_count);
    return false;
  }
  for (uint16_t i = 0; i < g_node_count; ++i) g_seen[i] = 0;
  for (uint16_t i = 0; i < g_node_count; ++i) {
    const uint16_t n = g_order[i];
    if (n >= g_node_count || g_seen[n]) {
      if (out) out->println("reorder refused: sort result is not a permutation");
      return false;
    }
    g_seen[n] = 1;
  }

  // Count what this is about to buy, before the list changes.
  const Stats before = Summarize(g_edges, g_edge_count, g_node_count);

  {
    // The audio software ISR preempts thread mode, so if this code is
    // running then no update_all() is part-way through the list; pausing
    // here means none can start either. Restores rather than enables, for
    // the same reasons as everywhere else (see AudioIO.h).
    AudioIsrPause pause_isr;
    for (uint16_t i = 0; i + 1 < g_node_count; ++i) {
      g_nodes[g_order[i]]->*next_of = g_nodes[g_order[i + 1]];
    }
    g_nodes[g_order[g_node_count - 1]]->*next_of = nullptr;
    *first_pp = g_nodes[g_order[0]];
  }

  // Walk the list the ISR will actually walk and confirm it still holds
  // every node exactly once.
  uint16_t walked = 0;
  for (AudioStream* p = *first_pp; p && walked <= g_node_count; p = p->*next_of) ++walked;
  if (out) {
    if (walked != g_node_count) {
      out->printf("REORDER BROKE THE LIST: walked %u, expected %u\n", walked, g_node_count);
    } else {
      out->printf("reordered %u nodes; %u late cables before\n", g_node_count, before.late);
      if (cycles) {
        out->printf("  %u nodes sit in feedback loops and cannot be ordered (this is normal)\n",
                    cycles);
      }
      out->println("  run 'O' to see what is left");
    }
  }
  return walked == g_node_count;
}

}  // namespace AudioGraph
}  // namespace OC

#endif  // XENO_CODEC_AUDIO && AUDIO_DEBUG_CLASS
