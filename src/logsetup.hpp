// The logging of the command line: the console (stderr) and the orders log, each
// written by a thread of its own, and the settings in effect at the top of both.
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "config.hpp"
#include "util/log.hpp"

namespace nmm {

// The console line of a record: order events in colour when `color` is set.
std::string console_line(const logging::Record& record, bool color);

// Everything to stderr; order events also to `orders_log` when given, together with the buy/sell
// price lines (those are on stdout already, so the file is the only place they are logged to).
// Both are written by threads: a paused terminal or a stalled pipe must not freeze trading, nor a
// disk that holds a write for a while (live ETH, 18-19 Oct: the event loop stood 100-228 ms twenty
// times while the orders log was written and flushed line by line on it). Records a thread falls far
// behind on are dropped. Returns the handler that feeds the orders log (none without one).
std::shared_ptr<logging::ThreadedHandler> setup_logging(int level, const std::string& orders_log = "", bool color = false);

// Wait (up to `timeout_s`) until the writer threads have written what they hold.
void flush_logging(double timeout_s = 2.0);

// Write out what the writer threads still hold and stop them; a line in the orders log says how many
// lines were dropped, if any. The loggers are left as they were before setup_logging.
void stop_logging();

// The version and every setting in effect, at the top of the console log and of the orders log, so a
// log shows what produced it.
void log_config(const Config& config, const std::optional<std::string>& source, const std::string& version);

}  // namespace nmm
