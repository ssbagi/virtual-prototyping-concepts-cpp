// ============================================================================
// Author  : Shreyas S Bagi
// Note    : Developed with AI assistance (IBM Bob).
// Purpose : Created for learning and knowledge purposes only.
// ============================================================================
// backpressure_scenarios.cc - demo of flow control between a fast producer and a slow consumer.
// Logic in backpressure.h, reporting in logger.h (UVM-style: severities, verbosity, ids, summary).
//
// Usage:  build/backpressure_scenarios [verbosity]
//   verbosity = none | low | medium | high | full | debug      (default: high)
//   The first kTrace cycles of every scenario are narrated at that level; the rest of the run is
//   silent except for V_LOW lines, WARNINGs and ERRORs.

#include <iostream>
#include <string>

#include "backpressure.h"
#include "logger.h"

namespace {

constexpr uint64_t kTrace = 16;   // narrate the first few cycles only

int parse_verbosity(const std::string& s) {
	if (s == "none")   return rpt::V_NONE;
	if (s == "low")    return rpt::V_LOW;
	if (s == "medium") return rpt::V_MEDIUM;
	if (s == "full")   return rpt::V_FULL;
	if (s == "debug")  return rpt::V_DEBUG;
	return rpt::V_HIGH;
}

void run(const char* title, bp::Scheme s, const bp::Config& cfg, const char* explanation) {
	rpt::Logger& log = rpt::Logger::get();
	log.set_time(0);
	RPT_INFO("DEMO", rpt::V_LOW,
	         "\n==========================================================================\n"
	         " SCENARIO: " << title << "\n"
	         "==========================================================================\n" << explanation
	         << "\n  narrating the first " << kTrace << " cycles:");

	log.set_trace_window(1, kTrace);   // everything above V_LOW only for cycles 1..kTrace
	const bp::Result r = bp::run(s, cfg);
	log.clear_trace_window();

	const uint64_t slots = cfg.cycles / cfg.period;   // drain opportunities the consumer had
	log.set_time(cfg.cycles);
	RPT_INFO("DEMO", rpt::V_LOW, "RESULT after " << cfg.cycles << " cycles:\n"
	         "    items sent            : " << r.sent << "\n"
	         "    items accepted        : " << r.accepted << "\n"
	         "    items DROPPED (lost)  : " << r.dropped << (r.dropped ? "   <-- DATA LOSS" : "   ok") << "\n"
	         "    items consumed        : " << r.consumed << " of " << slots << " consumer slots ("
	         << (slots ? 100 * r.consumed / slots : 0) << "% utilisation)\n"
	         "    producer stall cycles : " << r.stall_cycles << "\n"
	         "    consumer idle slots   : " << r.consumer_idle << (r.consumer_idle ? "   <-- bubbles" : "   ok") << "\n"
	         "    peak FIFO occupancy   : " << r.max_occupancy << "/" << cfg.cap);
}

} // namespace

int main(int argc, char** argv) {
	rpt::print_banner();
	rpt::Logger& log = rpt::Logger::get();
	log.set_verbosity(argc > 1 ? parse_verbosity(argv[1]) : rpt::V_HIGH);

	const bp::Config base;   // cap 8, consumer drains 1 item / 4 cycles, 3-cycle flag latency, 200 cycles

	RPT_INFO("DEMO", rpt::V_LOW,
	         "\n##########################################################################\n"
	         "# BACKPRESSURE SCENARIOS DEMO\n"
	         "#\n"
	         "# Setup: a producer has data EVERY cycle. It feeds a FIFO of " << base.cap << " entries. The\n"
	         "# consumer drains ONE item every " << base.period << " cycles, so the producer is 4x faster and the\n"
	         "# FIFO fills up. Flow-control information needs " << base.latency << " cycles to reach the producer.\n"
	         "#\n"
	         "# BACKPRESSURE = the downstream side slowing the upstream side so nothing is lost and\n"
	         "# nothing overflows. The best possible result is 100% consumer utilisation, 0 drops.\n"
	         "##########################################################################");

	run(bp::name(bp::Scheme::None), bp::Scheme::None, base,
	    "  Rule : the producer ignores the FIFO state and sends every cycle.\n"
	    "  Risk : once the FIFO is full every further item is thrown away.\n"
	    "  Expect: most sent items are DROPPED; the consumer stays busy.");

	run(bp::name(bp::Scheme::ReadyValid), bp::Scheme::ReadyValid, base,
	    "  Rule : the producer sends only when the FIFO has space THIS cycle.\n"
	    "  Gain : lossless, the simplest form of backpressure.\n"
	    "  Cost : needs a combinational full/ready path from the consumer side to the producer,\n"
	    "         which is not always possible over a long or pipelined link.\n"
	    "  Expect: 0 drops, consumer 100% busy, the producer is stalled most cycles.");

	run(bp::name(bp::Scheme::DelayedStall), bp::Scheme::DelayedStall, base,
	    "  Rule : a stall flag is raised only when the FIFO is FULL, and it takes 3 cycles to reach\n"
	    "         the producer.\n"
	    "  Risk : during those cycles the producer keeps sending into a FIFO that has no room left.\n"
	    "         Backpressure exists, but it is too late.\n"
	    "  Expect: fewer drops than 'none', but still DATA LOSS.");

	run(bp::name(bp::Scheme::Watermark), bp::Scheme::Watermark, base,
	    "  Rule : the same 3-cycle flag, but raised EARLY at a high watermark (capacity - latency)\n"
	    "         and released at a low watermark (half of the high one).\n"
	    "  Why it works: the free entries above the high watermark absorb the items that are still\n"
	    "         in flight; the gap between the two marks avoids flag chatter (hysteresis) and keeps\n"
	    "         enough items queued for the consumer while the release travels back.\n"
	    "  Expect: 0 drops, consumer 100% busy, peak occupancy below capacity.");

	bp::Config credits_ok = base;
	credits_ok.credits    = base.cap;
	run("CREDIT-BASED (credits = capacity)", bp::Scheme::Credit, credits_ok,
	    "  Rule : the producer owns one credit per FIFO entry. Sending costs a credit, each consumer\n"
	    "         pop returns one 3 cycles later. The FIFO can never overflow.\n"
	    "  Gain : lossless by construction, works over any link latency.\n"
	    "  Expect: 0 drops, consumer 100% busy.");

	bp::Config credits_few = base;
	credits_few.credits    = 1;
	credits_few.latency    = 6;
	run("CREDIT-BASED (under-provisioned: 1 credit, 6-cycle return)", bp::Scheme::Credit, credits_few,
	    "  Rule : same scheme, but only ONE credit and a 6-cycle credit return.\n"
	    "  Risk : the producer can only have one item in flight per credit round trip, which is longer\n"
	    "         than the consumer's drain interval, so the FIFO runs dry.\n"
	    "  Expect: 0 drops (still safe) but the consumer wastes slots: THROUGHPUT LOSS, not data loss.");

	RPT_INFO("DEMO", rpt::V_LOW,
	         "\nTAKEAWAY\n"
	         "  NO BACKPRESSURE : overflow, data is lost.\n"
	         "  LATE BACKPRESSURE: a stall that is only raised when the FIFO is full still overflows;\n"
	         "                     the signal latency must be covered by headroom (watermarks) or credits.\n"
	         "  TOO MUCH CAUTION: too few credits (or a watermark that is too low) never loses data but\n"
	         "                     starves the consumer. Size the buffer for the round-trip latency:\n"
	         "                     capacity >= flow-control latency + items needed to keep the consumer busy.");

	log.report_summary();
	return 0;
}
