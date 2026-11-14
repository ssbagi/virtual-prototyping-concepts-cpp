// ============================================================================
// Author  : Shreyas S Bagi
// Note    : Developed with AI assistance (IBM Bob).
// Purpose : Created for learning and knowledge purposes only.
// ============================================================================
// bounded_buffer.h - model of the full-buffer livelock, used by the demo and the tests.
//
// Pieces:
//   q    : pending queue, oldest request first. A request leaves q only when it completes.
//   buf  : bounded FIFO of request ids, capacity `cap`.
//   port : ONE service per cycle (a shared resource). Servicing pops buf.front().
// Each cycle every request in q is visited oldest-first (in order):
//   1. not in buf -> push into buf (fails if full; then it may still burn the port to pop the front).
//   2. in buf     -> try to use the port; if buf.front() is itself, it completes.
//
// Variants:
//   Buggy        : a queue-only service pops the front but forgets the result -> the owner needs the
//                  port again. Livelock: busy every cycle, zero completions.
//   ResultKept   : the popped owner is marked 'ready' and completes without the port. Others
//                  progress, but younger requests refill the buffer first, so the OLDEST request starves.
//   ReservedSlot : ResultKept + one slot reserved for the oldest request (others fill <= cap-1).
//
// Reporting (see logger.h), id "BUF":
//   V_MEDIUM : start-of-cycle state (q, buf, occupancy) and end-of-cycle completions
//   V_HIGH   : every decision of every request (insert / fail / service / stall / complete)
//   WARNING  : no request completed for kLivelockWarn consecutive cycles (livelock suspected)
//   ERROR    : the oldest request (id 0) never completed during the run (deadlock)
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "logger.h"

namespace bb {

constexpr uint64_t kLivelockWarn = 50;   // WARNING after this many consecutive cycles without a completion

enum class Variant { Buggy, ResultKept, ReservedSlot };

inline const char* name(Variant v) {
	switch (v) {
		case Variant::Buggy:        return "BUGGY (result dropped on queue-only service)";
		case Variant::ResultKept:   return "RESULT_KEPT (no reserved slot)";
		case Variant::ReservedSlot: return "RESERVED_SLOT (fix)";
	}
	return "?";
}

struct Result {
	uint64_t completed         = 0;
	int64_t  oldest_done_cycle = -1;   // cycle in which request 0 completed (-1: never)
	size_t   max_buf           = 0;    // highest buffer occupancy observed
};

template <typename Container>
inline std::string fmt(const Container& c) {
	std::string s = "[";
	bool first = true;
	for (const auto& x : c) {
		s += (first ? "" : ",") + std::to_string(x);
		first = false;
	}
	return s + "]";
}

struct Sim {
	Variant         v;
	size_t          cap;                // buffer capacity ("buffer size"), must be >= 1
	std::deque<int> q;                  // oldest first
	std::deque<int> buf;                // FIFO
	std::set<int>   ready;              // owners whose result is already known
	int             next_id     = 0;
	uint64_t        idle_streak = 0;    // consecutive cycles without a completion
	Result          res;

	// in_flight : number of requests kept in q (constant demand), must be >= 2.
	// prefill   : the YOUNGEST `prefill` requests start inside the buffer (models a full-buffer start:
	//             younger requests already fill the buffer while the oldest ones are still in q).
	//             Clamped to [0, min(cap, in_flight - 1)] so request 0 always starts outside the buffer.
	Sim(Variant var, size_t capacity, int in_flight, size_t prefill) : v(var), cap(capacity) {
		for (int i = 0; i < in_flight; ++i) q.push_back(next_id++);
		prefill = std::min(prefill, std::min(cap, static_cast<size_t>(in_flight - 1)));
		for (int i = in_flight - static_cast<int>(prefill); i < in_flight; ++i) buf.push_back(i);
		res.max_buf = buf.size();
	}

	bool in_buf(int id) const { return std::find(buf.begin(), buf.end(), id) != buf.end(); }

	void push(int id) {
		buf.push_back(id);
		res.max_buf = std::max(res.max_buf, buf.size());
	}

	// Pops the buffer front. Whether the result is remembered depends on the variant.
	int service_front() {
		const int owner = buf.front();
		buf.pop_front();
		if (v != Variant::Buggy) ready.insert(owner);   // Buggy forgets the result
		return owner;
	}

	void cycle(uint64_t c) {
		rpt::Logger::get().set_time(c);
		bool port_used = false;
		std::vector<int> done;
		const std::vector<int> snapshot(q.begin(), q.end());   // visit oldest-first, in order
		const int oldest = snapshot.front();

		RPT_INFO("BUF", rpt::V_MEDIUM, "---- start of cycle: q (oldest first) = " << fmt(q)
		         << "   buf (front..back) = " << fmt(buf) << "   occupancy " << buf.size() << "/" << cap
		         << (buf.size() >= cap ? "  <-- FULL" : ""));
		RPT_INFO("BUF", rpt::V_HIGH, "port = FREE (one service allowed this cycle), oldest request = " << oldest);

		for (const int id : snapshot) {
			if (v != Variant::Buggy && ready.count(id)) {      // result already known: no port needed
				done.push_back(id);
				RPT_INFO("BUF", rpt::V_HIGH, "  request " << id << ": result was kept from an earlier service -> completes now, needs no port");
				continue;
			}

			if (!in_buf(id)) {
				// ReservedSlot: everybody except the oldest request must leave one slot free.
				const size_t limit = (v == Variant::ReservedSlot && id != oldest) ? cap - 1 : cap;
				RPT_INFO("BUF", rpt::V_HIGH, "  request " << id << ": not in buf, tries to insert (allowed fill for this request: "
				         << limit << "/" << cap
				         << (limit < cap ? ", last slot is RESERVED for the oldest request" : "") << ")");
				if (buf.size() < limit) {
					push(id);
					RPT_INFO("BUF", rpt::V_HIGH, "    -> inserted, buf = " << fmt(buf));
				} else {
					RPT_INFO("BUF", rpt::V_HIGH, "    -> insert FAILED (buf " << buf.size() << " >= " << limit
					         << "): request stays in q and rebids next cycle");
					// Cannot insert; still burn the port to drain the front (queue-only service).
					if (!port_used) {
						port_used = true;
						const int owner = service_front();
						RPT_INFO("BUF", rpt::V_HIGH, "    -> port is free, so a queue-only service pops buf front = request "
						         << owner << ", buf = " << fmt(buf));
						if (v == Variant::Buggy) {
							RPT_INFO("BUF", rpt::V_HIGH, "       BUG: that result is DROPPED. request " << owner
							         << " is no longer in buf but not done either: it must be inserted AND serviced again");
						} else {
							RPT_INFO("BUF", rpt::V_HIGH, "       result KEPT: request " << owner
							         << " is marked ready and completes on its next visit without the port");
						}
					} else {
						RPT_INFO("BUF", rpt::V_HIGH, "    -> port already used this cycle, nothing more for this request");
					}
					continue;                                   // stays in q, rebids next cycle
				}
			} else {
				RPT_INFO("BUF", rpt::V_HIGH, "  request " << id << ": already in buf (no duplicate push)");
			}

			// In the buffer now.
			if (port_used) {
				RPT_INFO("BUF", rpt::V_HIGH, "    -> port busy this cycle: BUF-STALL, request " << id << " stays in q");
				continue;
			}
			port_used = true;
			const int owner = service_front();
			RPT_INFO("BUF", rpt::V_HIGH, "    -> port free: service buf front = request " << owner << ", buf = " << fmt(buf));
			if (owner == id) {
				done.push_back(id);                             // its own entry was serviced -> completes
				RPT_INFO("BUF", rpt::V_HIGH, "       front was MY entry -> request " << id << " completes and leaves q");
			} else {
				RPT_INFO("BUF", rpt::V_HIGH, "       front belongs to request " << owner << ", not me -> request " << id << " stays in q"
				         << (v == Variant::Buggy ? " (and the owner's result is lost)" : ""));
			}
		}

		for (const int id : done) {
			q.erase(std::find(q.begin(), q.end(), id));
			ready.erase(id);
			++res.completed;
			if (id == 0) res.oldest_done_cycle = static_cast<int64_t>(c);
			q.push_back(next_id++);                             // constant demand
		}

		RPT_INFO("BUF", rpt::V_MEDIUM, "---- end of cycle: completed this cycle = " << fmt(done)
		         << "  (total " << res.completed << ")   buf = " << fmt(buf));

		idle_streak = done.empty() ? idle_streak + 1 : 0;
		if (idle_streak == kLivelockWarn) {
			RPT_WARNING("BUF", "LIVELOCK suspected: no request completed for " << kLivelockWarn
			            << " consecutive cycles (buf occupancy " << buf.size() << "/" << cap << ")");
		}
	}
};

inline Result run(Variant v, size_t cap, int in_flight, size_t prefill, uint64_t cycles) {
	Sim s(v, cap, in_flight, prefill);
	for (uint64_t c = 1; c <= cycles; ++c) s.cycle(c);
	if (s.res.oldest_done_cycle < 0) {
		RPT_ERROR("BUF", "oldest request (id 0) never completed in " << cycles
		          << " cycles -> oldest request blocked -> DEADLOCK  [" << name(v) << "]");
	}
	return s.res;
}

} // namespace bb
