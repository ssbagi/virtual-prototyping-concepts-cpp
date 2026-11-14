// ============================================================================
// Author  : Shreyas S Bagi
// Note    : Developed with AI assistance (IBM Bob).
// Purpose : Created for learning and knowledge purposes only.
// ============================================================================
// arbitration_starvation.cc - demo: which arbitration policy starves whom.
// Logic in arbiter.h, reporting in logger.h (UVM-style: severities, verbosity, ids, summary).
//
// Usage:  build/arbitration_starvation [verbosity]
//   verbosity = none | low | medium | high | full | debug      (default: high)
//   The first kTrace cycles of every policy are narrated at that level; the rest of the run is
//   silent except for V_LOW lines, WARNINGs and ERRORs.

#include <iostream>
#include <string>

#include "arbiter.h"
#include "logger.h"

namespace {

constexpr int      kRequesters = 4;
constexpr uint64_t kCycles     = 1000;
constexpr uint64_t kTrace      = 12;

int parse_verbosity(const std::string& s) {
	if (s == "none")   return rpt::V_NONE;
	if (s == "low")    return rpt::V_LOW;
	if (s == "medium") return rpt::V_MEDIUM;
	if (s == "full")   return rpt::V_FULL;
	if (s == "debug")  return rpt::V_DEBUG;
	return rpt::V_HIGH;
}

void banner(const char* title, const char* explanation) {
	RPT_INFO("DEMO", rpt::V_LOW, "\n==========================================================================\n"
	         << " POLICY: " << title << "\n"
	         << "==========================================================================\n" << explanation);
}

void run(arb::Policy p, const char* explanation) {
	rpt::Logger& log = rpt::Logger::get();
	log.set_time(0);
	banner(arb::name(p), explanation);
	RPT_INFO("DEMO", rpt::V_LOW, "narrating the first " << kTrace << " cycles (waits = cycles since last grant)");

	log.set_trace_window(1, kTrace);   // everything above V_LOW only for cycles 1..kTrace
	const auto r = arb::simulate(p, kRequesters, kCycles);
	log.clear_trace_window();

	log.set_time(kCycles);
	RPT_INFO("DEMO", rpt::V_LOW, "RESULT after " << kCycles << " cycles (" << kRequesters << " requesters, 1 grant/cycle):");
	for (int i = 0; i < kRequesters; ++i) {
		RPT_INFO("DEMO", rpt::V_LOW, "  requester " << i << ": served=" << r[i].served << " max_wait=" << r[i].max_wait
		         << (r[i].served == 0 ? "   <-- STARVED (never granted)" : "   ok"));
	}
}

} // namespace

int main(int argc, char** argv) {
	rpt::print_banner();
	rpt::Logger& log = rpt::Logger::get();
	log.set_verbosity(argc > 1 ? parse_verbosity(argv[1]) : rpt::V_HIGH);

	RPT_INFO("DEMO", rpt::V_LOW,
	         "\n##########################################################################\n"
	         "# ARBITRATION STARVATION DEMO\n"
	         "#\n"
	         "# Setup: " << kRequesters << " requesters ALL ask for ONE shared resource EVERY cycle.\n"
	         "# The resource has a single port, so exactly one request is granted per cycle.\n"
	         "#\n"
	         "# STARVATION = the resource works and others keep getting it, but one requester\n"
	         "# never does. 'max_wait' == total cycles means 'never granted at all'.\n"
	         "##########################################################################");

	run(arb::Policy::Fixed,
	    "  Rule : the lowest index always wins.\n"
	    "  Risk : requester 0 asks every cycle, so requesters 1..3 are never even looked at.\n"
	    "         This is what 'a high-priority requester always wins' does to everyone else when\n"
	    "         that requester bids every cycle.");

	run(arb::Policy::RoundRobin,
	    "  Rule : a pointer rotates over the requesters, each gets its turn.\n"
	    "  Gain : perfectly fair, the wait is bounded by (requesters - 1) cycles.\n"
	    "  Cost : no priority at all - an urgent requester (e.g. the oldest request) waits like the rest.");

	run(arb::Policy::Aging,
	    "  Rule : fixed priority, BUT a requester that has waited >= the age limit (8 cycles) is\n"
	    "         promoted ahead of the fixed order (longest waiter first).\n"
	    "  Gain : priority is kept (requester 0 still gets the most grants) and nobody starves:\n"
	    "         the wait is bounded by roughly (age limit + requesters).\n"
	    "  Watch: from cycle 9 on, requesters 1..3 age out one after another and get their grants.");

	RPT_INFO("DEMO", rpt::V_LOW,
	         "\nTAKEAWAY: starvation is a property of the SELECTION RULE, not of the resource.\n"
	         "A rule that can pick the same winner forever (fixed priority) starves the rest;\n"
	         "round-robin or aging puts an upper bound on how long anyone waits.");

	log.report_summary();
	return 0;
}
