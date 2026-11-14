// ============================================================================
// Author  : Shreyas S Bagi
// Note    : Developed with AI assistance (IBM Bob).
// Purpose : Created for learning and knowledge purposes only.
// ============================================================================
// backpressure.h - producer -> bounded FIFO -> slow consumer, with different flow-control schemes.
//
// A producer has data EVERY cycle. A consumer drains the FIFO only once every `period` cycles, so
// the producer is faster than the consumer and the FIFO fills up. BACKPRESSURE is how the consumer
// side tells the producer to slow down. Schemes:
//
//   None         : no flow control. The producer always sends; a full FIFO drops the item (data LOSS).
//   ReadyValid   : the producer sees "FIFO has space" in the same cycle. Lossless.
//   DelayedStall : a "FIFO is full" stall flag reaches the producer `latency` cycles late. By then the
//                  producer has sent more items than the FIFO can hold -> overflow (data LOSS).
//   Watermark    : same delayed stall flag, but it is raised EARLY (high watermark = cap - latency) and
//                  released late (low watermark = high / 2, hysteresis). The headroom absorbs the items
//                  sent while the flag is in flight. Lossless.
//   Credit       : the producer holds `credits` (<= cap). Each send costs a credit, each consumer pop
//                  returns one `latency` cycles later. Lossless by construction; too few credits
//                  starve the consumer (bubbles), because the credit round trip is longer than the drain.
//
// Per cycle: credits return -> consumer slot (every `period` cycles) -> producer decision -> flag update.
//
// Reporting (see logger.h), id "BP":
//   V_MEDIUM : one line per cycle (occupancy, flow-control state)
//   V_HIGH   : every decision (send / stall / drop / consume / bubble)
//   WARNING  : first item dropped (FIFO overflow); consumer slots wasted after warm-up (bubbles)
//   ERROR    : at least one item was lost during the run
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include "logger.h"

namespace bp {

enum class Scheme { None, ReadyValid, DelayedStall, Watermark, Credit };

inline const char* name(Scheme s) {
	switch (s) {
		case Scheme::None:         return "NO BACKPRESSURE (producer ignores a full FIFO)";
		case Scheme::ReadyValid:   return "READY/VALID (immediate full indication)";
		case Scheme::DelayedStall: return "DELAYED STALL (flag raised only when full)";
		case Scheme::Watermark:    return "WATERMARK (early stall with headroom + hysteresis)";
		case Scheme::Credit:       return "CREDIT-BASED";
	}
	return "?";
}

struct Config {
	size_t   cap     = 8;     // FIFO capacity, must be >= 2
	uint64_t period  = 4;     // consumer drains one item every `period` cycles, must be >= 1
	uint64_t latency = 3;     // cycles for the stall flag / a returned credit to reach the producer, must be >= 1
	size_t   credits = 8;     // initial credits (Credit scheme only); clamped to [1, cap]
	size_t   high_watermark = 0;   // Watermark scheme: 0 = automatic (cap - latency), else the stall threshold
	uint64_t cycles  = 200;
};

struct Result {
	uint64_t sent           = 0;   // items the producer put on the link
	uint64_t accepted       = 0;   // items that entered the FIFO
	uint64_t dropped        = 0;   // items lost because the FIFO was full
	uint64_t consumed       = 0;   // items drained by the consumer
	uint64_t stall_cycles   = 0;   // cycles the producer wanted to send but was held back
	uint64_t consumer_idle  = 0;   // consumer slots wasted on an empty FIFO after the first item (bubbles)
	size_t   max_occupancy  = 0;   // highest FIFO occupancy observed
	size_t   final_occupancy = 0;  // items still in the FIFO when the run ended
};

class Sim {
public:
	Sim(Scheme scheme, const Config& config) : s_(scheme), cfg_(config) {
		cfg_.cap     = std::max<size_t>(cfg_.cap, 2);
		cfg_.period  = std::max<uint64_t>(cfg_.period, 1);
		cfg_.latency = std::max<uint64_t>(cfg_.latency, 1);
		cfg_.credits = std::min(std::max<size_t>(cfg_.credits, 1), cfg_.cap);
		credits_     = cfg_.credits;
		// Headroom for the items that are still sent while the stall flag is in flight.
		high_ = cfg_.cap > cfg_.latency ? cfg_.cap - static_cast<size_t>(cfg_.latency) : 1;
		if (cfg_.high_watermark > 0) high_ = std::min(cfg_.high_watermark, cfg_.cap);
		low_  = high_ / 2;
		stall_pipe_.assign(static_cast<size_t>(cfg_.latency), false);
	}

	void cycle(uint64_t c) {
		rpt::Logger::get().set_time(c);

		// Credits that were returned `latency` cycles ago arrive now.
		while (!credit_due_.empty() && credit_due_.front() <= c) {
			++credits_;
			credit_due_.pop_front();
		}

		consumer_step(c);
		producer_step();
		update_stall_flag();

		res_.max_occupancy = std::max(res_.max_occupancy, occ_);
		RPT_INFO("BP", rpt::V_MEDIUM, "cycle end: occupancy " << occ_ << "/" << cfg_.cap
		         << (occ_ >= cfg_.cap ? "  <-- FULL" : "") << "   " << flow_state());
	}

	Result finish() {
		res_.final_occupancy = occ_;
		if (res_.dropped > 0) {
			RPT_ERROR("BP", res_.dropped << " of " << res_.sent << " items were LOST (FIFO overflow)  ["
			          << name(s_) << "]");
		}
		if (res_.consumer_idle > 0) {
			RPT_WARNING("BP", "consumer wasted " << res_.consumer_idle
			            << " slot(s) on an empty FIFO: the flow-control loop is too slow or under-provisioned");
		}
		return res_;
	}

private:
	void consumer_step(uint64_t c) {
		if (c % cfg_.period != 0) return;
		if (occ_ > 0) {
			--occ_;
			++res_.consumed;
			warmed_up_ = true;
			RPT_INFO("BP", rpt::V_HIGH, "  consumer: pops one item, occupancy now " << occ_ << "/" << cfg_.cap);
			if (s_ == Scheme::Credit) {
				credit_due_.push_back(c + cfg_.latency);
				RPT_INFO("BP", rpt::V_HIGH, "  consumer: returns a credit, it reaches the producer at cycle "
				         << (c + cfg_.latency));
			}
		} else if (warmed_up_) {
			++res_.consumer_idle;
			RPT_INFO("BP", rpt::V_HIGH, "  consumer: slot WASTED, FIFO is empty (bubble)");
		}
	}

	void producer_step() {
		bool allowed = true;
		switch (s_) {
			case Scheme::None:
				break;
			case Scheme::ReadyValid:
				allowed = occ_ < cfg_.cap;
				break;
			case Scheme::DelayedStall:
			case Scheme::Watermark:
				allowed = !stall_pipe_.front();   // the flag as it was `latency` cycles ago
				break;
			case Scheme::Credit:
				allowed = credits_ > 0;
				break;
		}

		if (!allowed) {
			++res_.stall_cycles;
			RPT_INFO("BP", rpt::V_HIGH, "  producer: STALLED by backpressure (" << flow_state() << ")");
			return;
		}

		++res_.sent;
		if (s_ == Scheme::Credit) --credits_;
		if (occ_ < cfg_.cap) {
			++occ_;
			++res_.accepted;
			RPT_INFO("BP", rpt::V_HIGH, "  producer: sends, accepted, occupancy now " << occ_ << "/" << cfg_.cap);
		} else {
			++res_.dropped;
			RPT_INFO("BP", rpt::V_HIGH, "  producer: sends, FIFO FULL -> item DROPPED (total dropped " << res_.dropped << ")");
			if (res_.dropped == 1) {
				RPT_WARNING("BP", "FIFO overflow: first item lost (occupancy " << occ_ << "/" << cfg_.cap << ")");
			}
		}
	}

	// Computes the stall flag from the occupancy at the end of the cycle; the producer sees it
	// `latency` cycles later (it travels through stall_pipe_).
	void update_stall_flag() {
		if (s_ != Scheme::DelayedStall && s_ != Scheme::Watermark) return;

		if (s_ == Scheme::DelayedStall) {
			raw_stall_ = occ_ >= cfg_.cap;
		} else if (!raw_stall_ && occ_ >= high_) {
			raw_stall_ = true;
			RPT_INFO("BP", rpt::V_HIGH, "  flag: occupancy " << occ_ << " >= high watermark " << high_ << " -> stall RAISED");
		} else if (raw_stall_ && occ_ <= low_) {
			raw_stall_ = false;
			RPT_INFO("BP", rpt::V_HIGH, "  flag: occupancy " << occ_ << " <= low watermark " << low_ << " -> stall RELEASED");
		}
		stall_pipe_.push_back(raw_stall_);
		stall_pipe_.pop_front();
	}

	std::string flow_state() const {
		switch (s_) {
			case Scheme::None:         return "no flow control";
			case Scheme::ReadyValid:   return std::string("ready=") + (occ_ < cfg_.cap ? "1" : "0");
			case Scheme::DelayedStall:
			case Scheme::Watermark:    return std::string("stall(seen by producer)=") + (stall_pipe_.front() ? "1" : "0")
			                                  + " stall(raw)=" + (raw_stall_ ? "1" : "0");
			case Scheme::Credit:       return "credits=" + std::to_string(credits_);
		}
		return "?";
	}

	Scheme              s_;
	Config              cfg_;
	size_t              occ_        = 0;
	size_t              credits_    = 0;
	size_t              high_       = 1;
	size_t              low_        = 0;
	bool                raw_stall_  = false;
	bool                warmed_up_  = false;
	std::deque<bool>    stall_pipe_;
	std::deque<uint64_t> credit_due_;
	Result              res_;
};

inline Result run(Scheme s, const Config& cfg) {
	Sim sim(s, cfg);
	for (uint64_t c = 1; c <= cfg.cycles; ++c) sim.cycle(c);
	return sim.finish();
}

} // namespace bp
