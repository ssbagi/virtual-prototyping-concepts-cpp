// ============================================================================
// Author  : Shreyas S Bagi
// Note    : Developed with AI assistance (IBM Bob).
// Purpose : Created for learning and knowledge purposes only.
// ============================================================================
// bounded_buffer_livelock.cc - demo of the full-buffer livelock / head-of-queue starvation.
// Logic in bounded_buffer.h, reporting in logger.h (UVM-style: severities, verbosity, ids, summary).
//
// Usage:  build/bounded_buffer_livelock [verbosity]
//   verbosity = none | low | medium | high | full | debug      (default: high)
//   The first kTrace cycles of every variant are narrated at that level; the rest of the run is
//   silent except for V_LOW lines, WARNINGs and ERRORs.

#include <iostream>
#include <string>

#include "bounded_buffer.h"
#include "logger.h"

namespace {

constexpr size_t   kCap      = 4;     // buffer capacity ("buffer size")
constexpr int      kInFlight = 8;     // requests kept in q (constant demand)
constexpr size_t   kPrefill  = 4;     // buffer starts full of the 4 youngest requests (ids 4..7)
constexpr uint64_t kCycles   = 200;
constexpr uint64_t kTrace    = 6;     // narrate the first few cycles only

int parse_verbosity(const std::string& s) {
	if (s == "none")   return rpt::V_NONE;
	if (s == "low")    return rpt::V_LOW;
	if (s == "medium") return rpt::V_MEDIUM;
	if (s == "full")   return rpt::V_FULL;
	if (s == "debug")  return rpt::V_DEBUG;
	return rpt::V_HIGH;
}

void run(bb::Variant v, const char* explanation) {
	rpt::Logger& log = rpt::Logger::get();
	log.set_time(0);
	RPT_INFO("DEMO", rpt::V_LOW,
	         "\n==========================================================================\n"
	         " VARIANT: " << bb::name(v) << "\n"
	         "==========================================================================\n" << explanation
	         << "\n  narrating the first " << kTrace << " cycles:");

	log.set_trace_window(1, kTrace);   // everything above V_LOW only for cycles 1..kTrace
	const bb::Result r = bb::run(v, kCap, kInFlight, kPrefill, kCycles);
	log.clear_trace_window();

	log.set_time(kCycles);
	RPT_INFO("DEMO", rpt::V_LOW, "RESULT after " << kCycles << " cycles:\n"
	         "    completed requests        : " << r.completed << "\n"
	         "    oldest request (id 0) : "
	         << (r.oldest_done_cycle < 0 ? std::string("NEVER completed -> in-order completion stops -> DEADLOCK")
	                                     : "completed at cycle " + std::to_string(r.oldest_done_cycle))
	         << "\n    peak buffer occupancy : " << r.max_buf << "/" << kCap);
}

} // namespace

int main(int argc, char** argv) {
	rpt::print_banner();
	rpt::Logger& log = rpt::Logger::get();
	log.set_verbosity(argc > 1 ? parse_verbosity(argv[1]) : rpt::V_HIGH);

	RPT_INFO("DEMO", rpt::V_LOW,
	         "\n##########################################################################\n"
	         "# BOUNDED BUFFER LIVELOCK DEMO  (model of the full-buffer deadlock)\n"
	         "#\n"
	         "#   q    = pending queue, oldest request first. A request leaves ONLY when it completes.\n"
	         "#   buf  = bounded FIFO of request ids, capacity " << kCap << ".\n"
	         "#   port = ONE service per cycle (service port). A service pops buf.front().\n"
	         "#\n"
	         "# Start state: requests 4..7 already fill buf, the oldest requests\n"
	         "# 0..3 are still waiting in q. Demand is constant: whenever a request completes, a new\n"
	         "# younger one enters q.\n"
	         "#\n"
	         "# Each cycle every request in q is visited oldest-first, in order:\n"
	         "#   not in buf -> try to insert; if buf is full the insert fails, but the request may\n"
	         "#                 still burn the port to pop the front ('queue-only service');\n"
	         "#   in buf     -> try the port; it completes only if buf.front() is ITS OWN entry.\n"
	         "##########################################################################");

	run(bb::Variant::Buggy,
	    "  Behaviour : the queue-only service pops the front, but the result is thrown away.\n"
	    "  Why bad   : the owner of the popped entry is not marked done and is not in buf, so it must\n"
	    "              be inserted and serviced AGAIN - and the port is burned by the oldest request every\n"
	    "              cycle before the owner gets its turn.\n"
	    "  Expect    : LIVELOCK - everyone keeps trying, the port is busy every cycle, 0 completions.");

	run(bb::Variant::ResultKept,
	    "  Behaviour : the queue-only service pops the front AND remembers the result: the owner is\n"
	    "              marked ready and completes on its next visit without needing the port.\n"
	    "  Why better: no work is wasted, so the system moves.\n"
	    "  Remaining : right after the oldest request's failed insert, younger requests refill the freed slot\n"
	    "              (they are visited later in the same pass). The oldest request never finds a free\n"
	    "              slot -> HEAD STARVATION. With in-order completion the whole system stalls again.\n"
	    "  Expect    : many completions, but request 0 NEVER completes.");

	run(bb::Variant::ReservedSlot,
	    "  Behaviour : result kept (as above) PLUS one slot is reserved for the oldest request. Every other\n"
	    "              request may fill the buffer only up to capacity-1.\n"
	    "  Why it works: the oldest request always has a free slot to enter, so it can never be starved by\n"
	    "              younger requests; once it is in buf it is serviced in FIFO order.\n"
	    "  Expect    : request 0 completes within a few cycles, then ~1 completion per cycle (1 port).");

	RPT_INFO("DEMO", rpt::V_LOW,
	         "\nTAKEAWAY\n"
	         "  LIVELOCK  : everybody is busy, nothing completes (work is done and thrown away).\n"
	         "  STARVATION: the resource works, others progress, but the oldest requester never gets in.\n"
	         "  FIX       : never waste a service (keep the result) AND guarantee the oldest requester\n"
	         "              a slot (reserve one), so progress is guaranteed from the head of the queue.");

	log.report_summary();
	return 0;
}
