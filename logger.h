// ============================================================================
// Author  : Shreyas S Bagi
// Note    : Developed with AI assistance (IBM Bob).
// Purpose : Created for learning and knowledge purposes only.
// ============================================================================
// logger.h - tiny UVM-style reporting facility for the starvation_demo models.
//
// UVM concept              -> here
// ---------------------------------------------------------------------------------------------
// uvm_severity             -> rpt::Severity  { Info, Warning, Error, Fatal }
// uvm_verbosity            -> rpt::Verbosity { V_NONE=0, V_LOW=100, V_MEDIUM=200, V_HIGH=300,
//                                              V_FULL=400, V_DEBUG=500 }   (same numeric values as UVM)
// `uvm_info(ID,MSG,VERB)   -> RPT_INFO(ID, VERB, MSG)      printed only if VERB <= max verbosity
// `uvm_warning(ID,MSG)     -> RPT_WARNING(ID, MSG)          always printed, counted
// `uvm_error(ID,MSG)       -> RPT_ERROR(ID, MSG)            always printed, counted
// `uvm_fatal(ID,MSG)       -> RPT_FATAL(ID, MSG)            printed, counted, throws rpt::FatalError
// set_report_verbosity_level           -> Logger::set_verbosity(v)
// set_report_id_verbosity(ID, v)       -> Logger::set_id_verbosity(id, v)   (overrides the global level)
// $time in the message header          -> Logger::set_time(cycle)           (the model advances it)
// report_summarize()                   -> Logger::report_summary()
// get_report_server().get_*_count()    -> Logger::count(sev) / count_id(id)
//
// Message format:   RPT_INFO @ 7 [ARB] GRANT -> requester 0
//                   ^severity ^time ^id ^message
//
// Extra (not in UVM): a TRACE WINDOW. set_trace_window(first,last) limits every message ABOVE V_LOW
// to simulation times first..last, so a 1000-cycle run can narrate only its first few cycles while
// V_LOW results and all warnings/errors are still printed.
//
// Like the UVM macros, the message expression is only evaluated when the message is enabled, so a
// filtered `RPT_INFO(..., V_HIGH, expensive())` costs one comparison.
#pragma once

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace rpt {

// Author / provenance banner printed at the start of every run (so it lands in the log files too).
// Written straight to the stream, not through the Logger, so it never affects the report counts.
inline void print_banner(std::ostream& os = std::cout) {
	os << "// ============================================================================\n"
	      "// Author  : Shreyas S Bagi\n"
	      "// Note    : Developed with AI assistance (IBM Bob).\n"
	      "// Purpose : Created for learning and knowledge purposes only.\n"
	      "// ============================================================================\n";
}

enum Verbosity : int { V_NONE = 0, V_LOW = 100, V_MEDIUM = 200, V_HIGH = 300, V_FULL = 400, V_DEBUG = 500 };

enum class Severity { Info = 0, Warning = 1, Error = 2, Fatal = 3 };

inline const char* severity_name(Severity s) {
	switch (s) {
		case Severity::Info:    return "RPT_INFO";
		case Severity::Warning: return "RPT_WARNING";
		case Severity::Error:   return "RPT_ERROR";
		case Severity::Fatal:   return "RPT_FATAL";
	}
	return "RPT_?";
}

// Thrown by RPT_FATAL (UVM would end the simulation; an exception keeps the demo/tests testable).
class FatalError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class Logger {
public:
	static Logger& get() {
		static Logger instance;
		return instance;
	}
	Logger(const Logger&)            = delete;
	Logger& operator=(const Logger&) = delete;

	// ---- configuration ------------------------------------------------------------------------
	void set_verbosity(int v) { max_verbosity_ = v; }
	int  verbosity() const { return max_verbosity_; }

	void set_id_verbosity(const std::string& id, int v) { id_verbosity_[id] = v; }
	void clear_id_verbosity() { id_verbosity_.clear(); }

	void set_stream(std::ostream* os) { out_ = os ? os : &std::cout; }
	void set_show_location(bool on) { show_location_ = on; }

	void     set_time(uint64_t t) { time_ = t; }
	uint64_t time() const { return time_; }

	void set_trace_window(uint64_t first, uint64_t last) {
		window_on_    = true;
		window_first_ = first;
		window_last_  = last;
	}
	void clear_trace_window() { window_on_ = false; }

	// ---- filtering (used by the macros before the message is even built) -----------------------
	bool enabled(Severity s, const std::string& id, int verb) const {
		if (s != Severity::Info) return true;                  // warnings / errors / fatals are never filtered
		int limit = max_verbosity_;
		const auto it = id_verbosity_.find(id);
		if (it != id_verbosity_.end()) limit = it->second;     // per-id level overrides the global one
		if (verb > limit) return false;
		if (window_on_ && verb > V_LOW && (time_ < window_first_ || time_ > window_last_)) return false;
		return true;
	}

	// ---- output ---------------------------------------------------------------------------------
	void emit(Severity s, const std::string& id, const std::string& msg, const char* file, int line) {
		++sev_count_[static_cast<size_t>(s)];
		++id_count_[id];
		*out_ << severity_name(s) << " @ " << time_ << " [" << id << "] " << msg;
		if (show_location_) *out_ << "  (" << base_name(file) << ":" << line << ")";
		*out_ << "\n";
		if (s == Severity::Fatal) throw FatalError(id + ": " + msg);
	}

	// ---- statistics -----------------------------------------------------------------------------
	uint64_t count(Severity s) const { return sev_count_[static_cast<size_t>(s)]; }
	uint64_t count_id(const std::string& id) const {
		const auto it = id_count_.find(id);
		return it == id_count_.end() ? 0 : it->second;
	}
	void reset_counts() {
		sev_count_.fill(0);
		id_count_.clear();
	}

	// Mirrors UVM's report_summarize(): counts by severity and by id.
	void report_summary(std::ostream* os = nullptr) const {
		std::ostream& o = os ? *os : *out_;
		o << "\n--- RPT Report Summary ---\n"
		  << "** Report counts by severity\n";
		for (size_t i = 0; i < sev_count_.size(); ++i) {
			o << std::left << std::setw(13) << severity_name(static_cast<Severity>(i)) << ": " << sev_count_[i] << "\n";
		}
		o << "** Report counts by id\n";
		for (const auto& kv : id_count_) {
			o << "[" << kv.first << "]" << std::string(kv.first.size() < 10 ? 10 - kv.first.size() : 1, ' ')
			  << kv.second << "\n";
		}
		o << std::right;
	}

	// Back to defaults (stream = std::cout, verbosity = V_MEDIUM, no overrides/window, counts cleared).
	void reset() {
		out_           = &std::cout;
		max_verbosity_ = V_MEDIUM;
		show_location_ = false;
		window_on_     = false;
		time_          = 0;
		id_verbosity_.clear();
		reset_counts();
	}

private:
	Logger() = default;

	static const char* base_name(const char* path) {
		const char* b = path;
		for (const char* p = path; *p; ++p) {
			if (*p == '/' || *p == '\\') b = p + 1;
		}
		return b;
	}

	std::ostream*                   out_           = &std::cout;
	int                             max_verbosity_ = V_MEDIUM;   // UVM default
	std::map<std::string, int>      id_verbosity_;
	bool                            show_location_ = false;
	uint64_t                        time_          = 0;
	bool                            window_on_     = false;
	uint64_t                        window_first_  = 0;
	uint64_t                        window_last_   = 0;
	std::array<uint64_t, 4>         sev_count_{};
	std::map<std::string, uint64_t> id_count_;
};

} // namespace rpt

// The message argument is a stream expression:  RPT_INFO("ARB", rpt::V_HIGH, "winner=" << w);
// It is evaluated only when the message passes the verbosity filter.
#define RPT_REPORT_(SEV_, ID_, VERB_, MSG_)                                                   \
	do {                                                                                      \
		::rpt::Logger& rpt_logger_ = ::rpt::Logger::get();                                    \
		if (rpt_logger_.enabled((SEV_), (ID_), (VERB_))) {                                    \
			std::ostringstream rpt_os_;                                                       \
			rpt_os_ << MSG_;                                                                  \
			rpt_logger_.emit((SEV_), (ID_), rpt_os_.str(), __FILE__, __LINE__);              \
		}                                                                                     \
	} while (0)

#define RPT_INFO(ID_, VERB_, MSG_) RPT_REPORT_(::rpt::Severity::Info, ID_, VERB_, MSG_)
#define RPT_WARNING(ID_, MSG_)     RPT_REPORT_(::rpt::Severity::Warning, ID_, ::rpt::V_NONE, MSG_)
#define RPT_ERROR(ID_, MSG_)       RPT_REPORT_(::rpt::Severity::Error, ID_, ::rpt::V_NONE, MSG_)
#define RPT_FATAL(ID_, MSG_)       RPT_REPORT_(::rpt::Severity::Fatal, ID_, ::rpt::V_NONE, MSG_)
