// ============================================================================
// Author  : Shreyas S Bagi
// Note    : Developed with AI assistance (IBM Bob).
// Purpose : Created for learning and knowledge purposes only.
// ============================================================================
// tests.cc - self-checking tests for logger.h, arbiter.h, bounded_buffer.h and backpressure.h. No external framework.
// Exit code is 0 when every check passes, 1 otherwise (so `make test` fails on a regression).
//
// The models report through the UVM-style logger (logger.h). Before every test the logger is reset
// and redirected into g_sink, so the test output stays clean and the tests can also assert on what
// was (or was not) reported.
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "arbiter.h"
#include "backpressure.h"
#include "bounded_buffer.h"
#include "logger.h"

namespace {

int                      g_checks   = 0;
int                      g_failures = 0;
std::ostringstream       g_sink;          // captured logger output of the current test
std::vector<std::string> g_fail_msgs;     // failed CHECKs of the current test, reported by the runner

// A failed CHECK must NOT log through the shared logger while a test runs: the logger is redirected
// into g_sink and several tests assert on its counters. The runner reports g_fail_msgs afterwards.
#define CHECK(cond)                                                                        \
	do {                                                                                   \
		++g_checks;                                                                        \
		if (!(cond)) {                                                                     \
			++g_failures;                                                                  \
			std::ostringstream chk_os_;                                                    \
			chk_os_ << "CHECK FAILED " << __FILE__ << ":" << __LINE__ << " : " << #cond;   \
			g_fail_msgs.push_back(chk_os_.str());                                          \
		}                                                                                  \
	} while (0)

struct TestCase {
	const char*           name;
	std::function<void()> fn;
};

uint64_t total_served(const std::vector<arb::Requester>& r) {
	uint64_t t = 0;
	for (const auto& x : r) t += x.served;
	return t;
}

// ---------------------------------------------------------------------------
// Arbiter tests
// ---------------------------------------------------------------------------

void fixed_priority_starves_everyone_but_requester_0() {
	const auto r = arb::simulate(arb::Policy::Fixed, 4, 1000);
	CHECK(r[0].served == 1000);
	for (int i = 1; i < 4; ++i) {
		CHECK(r[i].served == 0);        // starved: never granted
		CHECK(r[i].max_wait == 1000);   // waited the whole run
	}
}

void round_robin_is_exactly_fair_when_cycles_divisible() {
	const auto r = arb::simulate(arb::Policy::RoundRobin, 4, 1000);
	for (int i = 0; i < 4; ++i) {
		CHECK(r[i].served == 250);
		CHECK(r[i].max_wait == 3);      // n-1 cycles between two grants
	}
}

void round_robin_is_fair_within_one_grant_otherwise() {
	const auto r = arb::simulate(arb::Policy::RoundRobin, 4, 1001);
	uint64_t lo = r[0].served, hi = r[0].served;
	for (const auto& x : r) { lo = std::min(lo, x.served); hi = std::max(hi, x.served); }
	CHECK(hi - lo <= 1);
}

void aging_serves_everyone_and_bounds_the_wait() {
	const uint64_t limit = 8;
	const int      n     = 4;
	const auto r = arb::simulate(arb::Policy::Aging, n, 1000, limit);
	for (int i = 0; i < n; ++i) {
		CHECK(r[i].served > 0);                  // nobody starved
		CHECK(r[i].max_wait <= limit + n);       // wait is bounded by age limit + number of requesters
	}
	CHECK(r[0].served > r[1].served);            // priority is still respected (req 0 gets the most)
}

void aging_with_huge_limit_degenerates_to_fixed() {
	const auto r = arb::simulate(arb::Policy::Aging, 4, 1000, /*age_limit=*/1000000);
	CHECK(r[0].served == 1000);
	CHECK(r[1].served == 0);
}

void every_policy_grants_exactly_one_per_cycle() {
	for (auto p : {arb::Policy::Fixed, arb::Policy::RoundRobin, arb::Policy::Aging}) {
		const auto r = arb::simulate(p, 5, 777);
		CHECK(total_served(r) == 777);
	}
}

void single_requester_is_always_served_under_every_policy() {
	for (auto p : {arb::Policy::Fixed, arb::Policy::RoundRobin, arb::Policy::Aging}) {
		const auto r = arb::simulate(p, 1, 100);
		CHECK(r[0].served == 100);
		CHECK(r[0].max_wait == 0);
	}
}

// ---------------------------------------------------------------------------
// Bounded-buffer (buffer-full) tests. Default scenario = the one used by the demo.
// ---------------------------------------------------------------------------

constexpr size_t   kCap      = 4;
constexpr int      kInFlight = 8;
constexpr size_t   kPrefill  = 4;
constexpr uint64_t kCycles   = 200;

void buggy_variant_livelocks_with_zero_completions() {
	const auto r = bb::run(bb::Variant::Buggy, kCap, kInFlight, kPrefill, kCycles);
	CHECK(r.completed == 0);
	CHECK(r.oldest_done_cycle < 0);
}

void result_kept_makes_progress_but_starves_the_oldest_request() {
	const auto r = bb::run(bb::Variant::ResultKept, kCap, kInFlight, kPrefill, kCycles);
	CHECK(r.completed > 0);              // younger requests complete ...
	CHECK(r.oldest_done_cycle < 0);      // ... but the oldest request never gets a slot
}

void reserved_slot_completes_oldest_quickly_and_everything_flows() {
	const auto r = bb::run(bb::Variant::ReservedSlot, kCap, kInFlight, kPrefill, kCycles);
	CHECK(r.oldest_done_cycle >= 1);
	CHECK(r.oldest_done_cycle <= static_cast<int64_t>(kCap) + 2);   // drains the buffer, then gets in
	CHECK(r.completed >= kCycles - 5);                              // ~1 completion per cycle (1 port)
}

void reserved_slot_fix_holds_for_other_buffer_sizes() {
	struct P { size_t cap; int in_flight; };
	const P params[] = {{2, 5}, {3, 6}, {8, 16}, {12, 24}};
	for (const auto& p : params) {
		const auto r = bb::run(bb::Variant::ReservedSlot, p.cap, p.in_flight, p.cap, 500);
		CHECK(r.oldest_done_cycle >= 1);
		CHECK(r.completed >= 400);
	}
}

void buggy_variant_livelocks_for_other_buffer_sizes() {
	struct P { size_t cap; int in_flight; };
	const P params[] = {{2, 5}, {3, 6}, {8, 16}, {12, 24}};
	for (const auto& p : params) {
		const auto r = bb::run(bb::Variant::Buggy, p.cap, p.in_flight, p.cap, 500);
		CHECK(r.completed == 0);
	}
}

void no_livelock_when_the_buffer_is_not_full() {
	// The livelock needs a FULL buffer. With an empty one every variant completes the oldest request.
	for (auto v : {bb::Variant::Buggy, bb::Variant::ResultKept, bb::Variant::ReservedSlot}) {
		const auto r = bb::run(v, kCap, kInFlight, /*prefill=*/0, kCycles);
		CHECK(r.oldest_done_cycle >= 1);
		CHECK(r.completed > 0);
	}
}

void buffer_never_exceeds_its_capacity() {
	for (auto v : {bb::Variant::Buggy, bb::Variant::ResultKept, bb::Variant::ReservedSlot}) {
		const auto r = bb::run(v, kCap, kInFlight, kPrefill, kCycles);
		CHECK(r.max_buf <= kCap);
	}
}

void completions_never_exceed_the_single_port_rate() {
	// One service per cycle -> at most one completion per cycle on average (ready owners excepted:
	// each ready owner was paid for by an earlier service), plus the initial prefill.
	for (auto v : {bb::Variant::ResultKept, bb::Variant::ReservedSlot}) {
		const auto r = bb::run(v, kCap, kInFlight, kPrefill, kCycles);
		CHECK(r.completed <= kCycles + kPrefill);
	}
}

// ---------------------------------------------------------------------------
// Logger tests
// ---------------------------------------------------------------------------

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

void logger_default_verbosity_is_medium_and_filters_higher_levels() {
	auto& L = rpt::Logger::get();
	CHECK(L.verbosity() == rpt::V_MEDIUM);
	RPT_INFO("T", rpt::V_LOW, "low-msg");
	RPT_INFO("T", rpt::V_MEDIUM, "medium-msg");
	RPT_INFO("T", rpt::V_HIGH, "high-msg");
	RPT_INFO("T", rpt::V_DEBUG, "debug-msg");
	CHECK(contains(g_sink.str(), "low-msg"));
	CHECK(contains(g_sink.str(), "medium-msg"));
	CHECK(!contains(g_sink.str(), "high-msg"));
	CHECK(!contains(g_sink.str(), "debug-msg"));
}

void logger_message_format_has_severity_time_and_id() {
	auto& L = rpt::Logger::get();
	L.set_time(42);
	RPT_INFO("ARB", rpt::V_LOW, "hello " << 7);
	CHECK(contains(g_sink.str(), "RPT_INFO @ 42 [ARB] hello 7"));
}

void logger_warning_and_error_are_never_filtered_and_are_counted() {
	auto& L = rpt::Logger::get();
	L.set_verbosity(rpt::V_NONE);
	RPT_INFO("T", rpt::V_LOW, "hidden");
	RPT_WARNING("T", "w1");
	RPT_ERROR("T", "e1");
	RPT_ERROR("T", "e2");
	CHECK(!contains(g_sink.str(), "hidden"));
	CHECK(contains(g_sink.str(), "RPT_WARNING"));
	CHECK(L.count(rpt::Severity::Info) == 0);
	CHECK(L.count(rpt::Severity::Warning) == 1);
	CHECK(L.count(rpt::Severity::Error) == 2);
	CHECK(L.count_id("T") == 3);
}

void logger_fatal_throws_after_printing_and_counting() {
	auto& L = rpt::Logger::get();
	bool thrown = false;
	try {
		RPT_FATAL("T", "boom");
	} catch (const rpt::FatalError& e) {
		thrown = contains(e.what(), "boom");
	}
	CHECK(thrown);
	CHECK(contains(g_sink.str(), "RPT_FATAL"));
	CHECK(L.count(rpt::Severity::Fatal) == 1);
}

void logger_id_verbosity_overrides_the_global_level() {
	auto& L = rpt::Logger::get();
	L.set_verbosity(rpt::V_LOW);
	L.set_id_verbosity("LOUD", rpt::V_DEBUG);
	RPT_INFO("LOUD", rpt::V_HIGH, "loud-high");
	RPT_INFO("QUIET", rpt::V_HIGH, "quiet-high");
	CHECK(contains(g_sink.str(), "loud-high"));
	CHECK(!contains(g_sink.str(), "quiet-high"));
}

void logger_trace_window_limits_messages_above_low() {
	auto& L = rpt::Logger::get();
	L.set_verbosity(rpt::V_HIGH);
	L.set_trace_window(5, 6);
	for (uint64_t t = 4; t <= 7; ++t) {
		L.set_time(t);
		RPT_INFO("T", rpt::V_HIGH, "detail@" << t);
		RPT_INFO("T", rpt::V_LOW, "summary@" << t);
	}
	CHECK(!contains(g_sink.str(), "detail@4"));
	CHECK(contains(g_sink.str(), "detail@5"));
	CHECK(contains(g_sink.str(), "detail@6"));
	CHECK(!contains(g_sink.str(), "detail@7"));
	for (uint64_t t = 4; t <= 7; ++t) CHECK(contains(g_sink.str(), "summary@" + std::to_string(t)));   // V_LOW always passes
}

void logger_filtered_message_is_not_evaluated() {
	auto& L = rpt::Logger::get();
	L.set_verbosity(rpt::V_LOW);
	int evaluated = 0;
	auto expensive = [&]() { ++evaluated; return 1; };
	RPT_INFO("T", rpt::V_HIGH, "x=" << expensive());     // filtered: argument must not be evaluated
	CHECK(evaluated == 0);
	RPT_INFO("T", rpt::V_LOW, "x=" << expensive());      // enabled: evaluated exactly once
	CHECK(evaluated == 1);
}

void logger_summary_lists_counts_by_severity_and_id() {
	RPT_WARNING("AAA", "w");
	RPT_ERROR("BBB", "e");
	std::ostringstream summary;
	rpt::Logger::get().report_summary(&summary);
	CHECK(contains(summary.str(), "RPT Report Summary"));
	CHECK(contains(summary.str(), "RPT_WARNING"));
	CHECK(contains(summary.str(), "[AAA]"));
	CHECK(contains(summary.str(), "[BBB]"));
}

// ---------------------------------------------------------------------------
// Models report what they detect (WARNING / ERROR counters are observable checks)
// ---------------------------------------------------------------------------

void arbiter_reports_an_error_per_starved_requester() {
	arb::simulate(arb::Policy::Fixed, 4, 1000);
	CHECK(rpt::Logger::get().count(rpt::Severity::Error) == 3);     // requesters 1..3
	CHECK(rpt::Logger::get().count(rpt::Severity::Warning) == 3);   // each reached the 100-cycle wait warning
}

void arbiter_reports_nothing_bad_for_fair_policies() {
	arb::simulate(arb::Policy::RoundRobin, 4, 1000);
	arb::simulate(arb::Policy::Aging, 4, 1000);
	CHECK(rpt::Logger::get().count(rpt::Severity::Error) == 0);
	CHECK(rpt::Logger::get().count(rpt::Severity::Warning) == 0);
}

void buffer_reports_livelock_warning_and_deadlock_error_when_buggy() {
	bb::run(bb::Variant::Buggy, kCap, kInFlight, kPrefill, kCycles);
	CHECK(rpt::Logger::get().count(rpt::Severity::Warning) == 1);   // livelock suspected once
	CHECK(rpt::Logger::get().count(rpt::Severity::Error) == 1);     // oldest request never completed
	CHECK(contains(g_sink.str(), "LIVELOCK suspected"));
	CHECK(contains(g_sink.str(), "DEADLOCK"));
}

void buffer_reports_no_error_when_fixed() {
	bb::run(bb::Variant::ReservedSlot, kCap, kInFlight, kPrefill, kCycles);
	CHECK(rpt::Logger::get().count(rpt::Severity::Error) == 0);
	CHECK(rpt::Logger::get().count(rpt::Severity::Warning) == 0);
}

void buffer_narration_appears_only_at_high_verbosity() {
	auto& L = rpt::Logger::get();
	L.set_verbosity(rpt::V_MEDIUM);
	bb::run(bb::Variant::ReservedSlot, kCap, kInFlight, kPrefill, 3);
	CHECK(contains(g_sink.str(), "start of cycle"));
	CHECK(!contains(g_sink.str(), "tries to insert"));
	g_sink.str("");
	L.set_verbosity(rpt::V_HIGH);
	bb::run(bb::Variant::ReservedSlot, kCap, kInFlight, kPrefill, 3);
	CHECK(contains(g_sink.str(), "tries to insert"));
	CHECK(contains(g_sink.str(), "RESERVED for the oldest request"));
}

// ---------------------------------------------------------------------------
// Backpressure tests. Default scenario = the one used by the demo (fast producer, slow consumer).
// ---------------------------------------------------------------------------

bp::Config bp_default() { return bp::Config{}; }   // cap 8, period 4, latency 3, 200 cycles

void no_backpressure_overflows_and_loses_data() {
	const auto r = bp::run(bp::Scheme::None, bp_default());
	CHECK(r.sent == 200);                         // producer never stalls
	CHECK(r.stall_cycles == 0);
	CHECK(r.dropped > 0);
	CHECK(r.accepted + r.dropped == r.sent);
	CHECK(r.max_occupancy == 8);
}

void ready_valid_is_lossless_and_keeps_the_consumer_busy() {
	const auto r = bp::run(bp::Scheme::ReadyValid, bp_default());
	CHECK(r.dropped == 0);
	CHECK(r.consumed == 50);                      // 200 cycles / period 4: every consumer slot used
	CHECK(r.consumer_idle == 0);
	CHECK(r.stall_cycles > 0);                    // the producer really was slowed down
	CHECK(r.sent + r.stall_cycles == 200);
}

void late_stall_still_overflows_but_loses_less_than_none() {
	const auto none = bp::run(bp::Scheme::None, bp_default());
	const auto late = bp::run(bp::Scheme::DelayedStall, bp_default());
	CHECK(late.dropped > 0);                      // flag is raised too late
	CHECK(late.dropped < none.dropped);
	CHECK(late.stall_cycles > 0);
}

void watermark_with_headroom_is_lossless_and_keeps_the_consumer_busy() {
	const auto r = bp::run(bp::Scheme::Watermark, bp_default());
	CHECK(r.dropped == 0);
	CHECK(r.consumer_idle == 0);
	CHECK(r.consumed == 50);
	CHECK(r.max_occupancy < 8);                   // headroom is never used up
}

void watermark_without_headroom_overflows() {
	// High watermark = capacity: the flag is raised only when full, so nothing is left to absorb
	// the items that are still in flight.
	bp::Config c = bp_default();
	c.high_watermark = c.cap;
	const auto r = bp::run(bp::Scheme::Watermark, c);
	CHECK(r.dropped > 0);
}

void credits_never_overflow_the_fifo() {
	for (size_t credits : {1u, 4u, 8u, 100u}) {   // 100 is clamped to the capacity
		bp::Config c = bp_default();
		c.credits    = credits;
		const auto r = bp::run(bp::Scheme::Credit, c);
		CHECK(r.dropped == 0);
		CHECK(r.max_occupancy <= c.cap);
	}
}

void full_credits_keep_the_consumer_busy() {
	bp::Config c = bp_default();
	c.credits    = c.cap;
	const auto r = bp::run(bp::Scheme::Credit, c);
	CHECK(r.consumed == 50);
	CHECK(r.consumer_idle == 0);
}

void too_few_credits_starve_the_consumer() {
	bp::Config c = bp_default();
	c.credits    = 1;
	c.latency    = 6;                             // credit round trip (1 + 6) is longer than the drain interval (4)
	const auto r = bp::run(bp::Scheme::Credit, c);
	CHECK(r.dropped == 0);                        // still safe ...
	CHECK(r.consumer_idle > 0);                   // ... but throughput is lost
	CHECK(r.consumed < 50);
}

void backpressure_conserves_items() {
	for (auto s : {bp::Scheme::None, bp::Scheme::ReadyValid, bp::Scheme::DelayedStall,
	               bp::Scheme::Watermark, bp::Scheme::Credit}) {
		const auto r = bp::run(s, bp_default());
		CHECK(r.sent == r.accepted + r.dropped);
		CHECK(r.accepted == r.consumed + r.final_occupancy);   // nothing vanishes inside the FIFO
		CHECK(r.sent + r.stall_cycles == 200);
	}
}

void backpressure_reports_error_only_on_data_loss() {
	bp::run(bp::Scheme::None, bp_default());
	CHECK(rpt::Logger::get().count(rpt::Severity::Error) == 1);
	CHECK(rpt::Logger::get().count(rpt::Severity::Warning) == 1);   // first overflow
	CHECK(contains(g_sink.str(), "FIFO overflow"));
	CHECK(contains(g_sink.str(), "were LOST"));

	rpt::Logger::get().reset_counts();
	bp::run(bp::Scheme::Watermark, bp_default());
	CHECK(rpt::Logger::get().count(rpt::Severity::Error) == 0);
	CHECK(rpt::Logger::get().count(rpt::Severity::Warning) == 0);
}

void backpressure_warns_about_bubbles_when_underprovisioned() {
	bp::Config c = bp_default();
	c.credits    = 1;
	c.latency    = 6;
	bp::run(bp::Scheme::Credit, c);
	CHECK(rpt::Logger::get().count(rpt::Severity::Error) == 0);
	CHECK(rpt::Logger::get().count(rpt::Severity::Warning) == 1);
	CHECK(contains(g_sink.str(), "wasted"));
}

} // namespace

int main() {
	rpt::print_banner();
	const std::vector<TestCase> tests = {
		{"arbiter: fixed priority starves all but requester 0",      fixed_priority_starves_everyone_but_requester_0},
		{"arbiter: round-robin exactly fair (divisible cycles)",     round_robin_is_exactly_fair_when_cycles_divisible},
		{"arbiter: round-robin within one grant (other cycles)",     round_robin_is_fair_within_one_grant_otherwise},
		{"arbiter: aging serves everyone, wait is bounded",          aging_serves_everyone_and_bounds_the_wait},
		{"arbiter: aging with huge limit == fixed",                  aging_with_huge_limit_degenerates_to_fixed},
		{"arbiter: exactly one grant per cycle (all policies)",      every_policy_grants_exactly_one_per_cycle},
		{"arbiter: single requester always served",                  single_requester_is_always_served_under_every_policy},
		{"buffer : BUGGY livelocks (0 completions)",                 buggy_variant_livelocks_with_zero_completions},
		{"buffer : RESULT_KEPT progresses but oldest starves",       result_kept_makes_progress_but_starves_the_oldest_request},
		{"buffer : RESERVED_SLOT fixes it",                          reserved_slot_completes_oldest_quickly_and_everything_flows},
		{"buffer : RESERVED_SLOT fix holds for other sizes",         reserved_slot_fix_holds_for_other_buffer_sizes},
		{"buffer : BUGGY livelocks for other sizes",                 buggy_variant_livelocks_for_other_buffer_sizes},
		{"buffer : no livelock when buffer is not full",             no_livelock_when_the_buffer_is_not_full},
		{"buffer : occupancy never exceeds capacity",                buffer_never_exceeds_its_capacity},
		{"buffer : completions bounded by the single port",          completions_never_exceed_the_single_port_rate},
		{"logger : default V_MEDIUM filters higher levels",          logger_default_verbosity_is_medium_and_filters_higher_levels},
		{"logger : message format severity/time/id",                 logger_message_format_has_severity_time_and_id},
		{"logger : warning/error never filtered, counted",           logger_warning_and_error_are_never_filtered_and_are_counted},
		{"logger : fatal prints, counts and throws",                 logger_fatal_throws_after_printing_and_counting},
		{"logger : per-id verbosity overrides global",               logger_id_verbosity_overrides_the_global_level},
		{"logger : trace window limits messages above V_LOW",        logger_trace_window_limits_messages_above_low},
		{"logger : filtered message is not evaluated",               logger_filtered_message_is_not_evaluated},
		{"logger : summary lists severity and id counts",            logger_summary_lists_counts_by_severity_and_id},
		{"report : arbiter flags each starved requester",            arbiter_reports_an_error_per_starved_requester},
		{"report : arbiter quiet for fair policies",                 arbiter_reports_nothing_bad_for_fair_policies},
		{"report : buggy buffer -> livelock warning + deadlock err", buffer_reports_livelock_warning_and_deadlock_error_when_buggy},
		{"report : fixed buffer -> no warning, no error",            buffer_reports_no_error_when_fixed},
		{"report : narration only at high verbosity",                buffer_narration_appears_only_at_high_verbosity},
		{"backpressure: none -> overflow, data loss",                no_backpressure_overflows_and_loses_data},
		{"backpressure: ready/valid lossless, consumer busy",        ready_valid_is_lossless_and_keeps_the_consumer_busy},
		{"backpressure: late stall still overflows",                 late_stall_still_overflows_but_loses_less_than_none},
		{"backpressure: watermark headroom is lossless",             watermark_with_headroom_is_lossless_and_keeps_the_consumer_busy},
		{"backpressure: watermark without headroom overflows",       watermark_without_headroom_overflows},
		{"backpressure: credits never overflow",                     credits_never_overflow_the_fifo},
		{"backpressure: full credits keep consumer busy",            full_credits_keep_the_consumer_busy},
		{"backpressure: too few credits starve the consumer",        too_few_credits_starve_the_consumer},
		{"backpressure: items are conserved",                        backpressure_conserves_items},
		{"report : backpressure error only on data loss",            backpressure_reports_error_only_on_data_loss},
		{"report : backpressure warns about bubbles",                backpressure_warns_about_bubbles_when_underprovisioned},
	};

	rpt::Logger& log = rpt::Logger::get();
	int failed_tests = 0;
	uint64_t index   = 0;

	for (const auto& t : tests) {
		++index;

		// Fresh logger per test: defaults, no counts, output captured in g_sink.
		g_sink.str("");
		g_sink.clear();
		g_fail_msgs.clear();
		log.reset();
		log.set_stream(&g_sink);

		const int before = g_failures;
		t.fn();
		const bool ok = (g_failures == before);
		failed_tests += ok ? 0 : 1;

		// Runner output goes through the same logger, back on the console. Time = test number.
		log.reset();
		log.set_time(index);
		log.set_verbosity(rpt::V_LOW);
		if (ok) {
			RPT_INFO("TEST", rpt::V_LOW, "[ OK ] " << t.name);
		} else {
			RPT_ERROR("TEST", "[FAIL] " << t.name);
			for (const auto& m : g_fail_msgs) RPT_ERROR("TEST", "    " << m);
		}
	}

	log.reset();
	log.set_time(index);
	log.set_verbosity(rpt::V_LOW);
	RPT_INFO("TEST", rpt::V_LOW, tests.size() << " tests, " << g_checks << " checks, " << g_failures << " failed checks");
	if (failed_tests != 0) RPT_ERROR("TEST", failed_tests << " test(s) FAILED");
	return failed_tests == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
