// ============================================================================
// Author  : Shreyas S Bagi
// Note    : Developed with AI assistance (IBM Bob).
// Purpose : Created for learning and knowledge purposes only.
// ============================================================================
// arbiter.h - one-grant-per-cycle arbiter model used by the demo and the tests.
//
// N requesters ALL bid every cycle; the policy picks exactly one winner per cycle.
//   Fixed      : lowest index always wins.
//   RoundRobin : a pointer rotates over the requesters.
//   Aging      : fixed priority, but a requester that has waited >= age_limit cycles wins
//                (longest waiter first).
//
// Reporting (see logger.h), id "ARB":
//   V_MEDIUM : one line per cycle with the grant
//   V_HIGH   : waits before the grant and the reason the arbiter chose that winner
//   WARNING  : a requester has waited kStarveWarnWait cycles (starvation suspected)
//   ERROR    : a requester was never granted during the whole run (starved)
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "logger.h"

namespace arb {

constexpr uint64_t kDefaultAgeLimit = 8;
constexpr uint64_t kStarveWarnWait  = 100;   // WARNING when a requester's wait reaches this many cycles

enum class Policy { Fixed, RoundRobin, Aging };

inline const char* name(Policy p) {
	switch (p) {
		case Policy::Fixed:      return "FIXED priority (req 0 always wins)";
		case Policy::RoundRobin: return "ROUND-ROBIN";
		case Policy::Aging:      return "FIXED + AGING";
	}
	return "?";
}

struct Requester {
	uint64_t served   = 0;
	uint64_t wait     = 0;   // cycles since last served (or since start)
	uint64_t max_wait = 0;   // worst wait observed
};

inline std::string waits_str(const std::vector<Requester>& r) {
	std::string s = "[";
	for (size_t i = 0; i < r.size(); ++i) s += (i ? "," : "") + std::to_string(r[i].wait);
	return s + "]";
}

// Index of the winner. All requesters are always requesting.
inline int pick(Policy p, const std::vector<Requester>& r, int& rr_ptr, uint64_t age_limit) {
	const int n = static_cast<int>(r.size());
	switch (p) {
		case Policy::Fixed:
			RPT_INFO("ARB", rpt::V_HIGH, "  reason: lowest index always wins, nobody else is even considered");
			return 0;
		case Policy::RoundRobin: {
			const int w = rr_ptr;
			rr_ptr = (rr_ptr + 1) % n;
			RPT_INFO("ARB", rpt::V_HIGH, "  reason: round-robin pointer was at " << w << ", it moves on to " << rr_ptr);
			return w;
		}
		case Policy::Aging: {
			int oldest = -1;
			for (int i = 0; i < n; ++i) {
				if (r[i].wait >= age_limit && (oldest < 0 || r[i].wait > r[oldest].wait)) oldest = i;
			}
			if (oldest >= 0) {
				RPT_INFO("ARB", rpt::V_HIGH, "  reason: requester " << oldest << " has waited " << r[oldest].wait
				         << " >= age limit " << age_limit << " -> AGED OUT, promoted over the fixed order");
			} else {
				RPT_INFO("ARB", rpt::V_HIGH, "  reason: nobody has waited >= " << age_limit
				         << " cycles -> plain fixed priority");
			}
			return oldest >= 0 ? oldest : 0;   // nobody aged out -> plain fixed priority
		}
	}
	return 0;
}

// Runs `cycles` cycles with `n` (>= 1) always-requesting requesters, one grant per cycle.
inline std::vector<Requester> simulate(Policy p, int n, uint64_t cycles,
                                       uint64_t age_limit = kDefaultAgeLimit) {
	std::vector<Requester> r(n);
	int rr_ptr = 0;
	rpt::Logger& log = rpt::Logger::get();

	for (uint64_t c = 1; c <= cycles; ++c) {
		log.set_time(c);
		RPT_INFO("ARB", rpt::V_HIGH, "waits before = " << waits_str(r));
		const int w = pick(p, r, rr_ptr, age_limit);
		for (int i = 0; i < n; ++i) {
			if (i == w) {
				r[i].served++;
				r[i].wait = 0;
			} else {
				r[i].wait++;
				r[i].max_wait = std::max(r[i].max_wait, r[i].wait);
				if (r[i].wait == kStarveWarnWait) {
					RPT_WARNING("ARB", "requester " << i << " has now waited " << kStarveWarnWait
					            << " cycles without a grant: STARVATION suspected");
				}
			}
		}
		RPT_INFO("ARB", rpt::V_MEDIUM, "GRANT -> requester " << w << "   waits after = " << waits_str(r));
	}

	for (int i = 0; i < n; ++i) {
		if (r[i].served == 0) {
			RPT_ERROR("ARB", "requester " << i << " was NEVER granted in " << cycles << " cycles: STARVED");
		}
	}
	return r;
}

} // namespace arb
