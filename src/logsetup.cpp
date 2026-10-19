#include "logsetup.hpp"

#include <cstdio>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

#include "util/clock.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

namespace {

// ANSI colours of order events on the console, by how their message starts.
const std::vector<std::pair<std::string, std::string>> COLORS {
    {">>> PLACE", "1;36"},  // bold cyan
    {">>> CANCEL", "1;33"},  // bold yellow
    {"<<< FILL", "1;32"},  // bold green: FILL and FILLED
    {"<<< CANCELED", "33"},  // yellow
    {"<<< cancel", "33"},
    {"<<< REJECTED", "1;31"},  // bold red
    {"!!! DROP", "1;35"},  // bold magenta: the price guard
    {"!!! RISE", "1;35"},
    {"!!!", "1;31"},
    {"=== ROUND TRIP", "1;34"},  // bold blue: a round trip's result
};

const std::vector<std::string_view> FILE_LOGGERS {ORDERS_LOGGER, QUOTES_LOGGER, CONFIG_LOGGER};

std::vector<std::shared_ptr<logging::ThreadedHandler>> writers;
std::shared_ptr<logging::ThreadedHandler> console;
std::shared_ptr<logging::ThreadedHandler> orders_file;
std::FILE* orders_fh = nullptr;

}  // namespace

std::string console_line(const logging::Record& record, bool color) {
    std::string text = logging::format_record(record);
    if (!color || record.name != ORDERS_LOGGER) return text;
    for (const auto& [prefix, code] : COLORS)
        if (starts_with(record.message, prefix)) return std::format("\x1b[{}m{}\x1b[0m\n", code, text.substr(0, text.size() - 1));
    return text;
}

std::shared_ptr<logging::ThreadedHandler> setup_logging(int level, const std::string& orders_log, bool color) {
    stop_logging();
    console = std::make_shared<logging::ThreadedHandler>(
        [](const std::string& line) {
            std::fwrite(line.data(), 1, line.size(), stderr);
            std::fflush(stderr);
        },
        [color](const logging::Record& r) { return console_line(r, color); });
    writers.push_back(console);
    logging::Logger& root = logging::root();
    root.add_handler(console);
    root.set_level(level);
    if (orders_log.empty()) return nullptr;
    orders_fh = std::fopen(orders_log.c_str(), "a");
    if (orders_fh == nullptr) throw std::runtime_error("cannot open the orders log " + orders_log);
    std::FILE* fh = orders_fh;
    orders_file = std::make_shared<logging::ThreadedHandler>([fh](const std::string& line) {
        std::fwrite(line.data(), 1, line.size(), fh);
        std::fflush(fh);
    });
    orders_file->level = logging::INFO;  // order events whatever the console level
    writers.push_back(orders_file);
    for (auto name : FILE_LOGGERS) {
        Logger& logger = get_logger(name);
        logger.add_handler(orders_file);
        logger.set_level(logging::INFO);
    }
    get_logger(QUOTES_LOGGER).set_propagate(false);
    return orders_file;
}

void flush_logging(double timeout_s) {
    for (auto& w : writers) w->flush(timeout_s);
}

void stop_logging() {
    for (auto& w : writers) w->stop();
    if (console) logging::root().remove_handler(console);
    if (orders_file) {
        for (auto name : FILE_LOGGERS) {
            Logger& logger = get_logger(name);
            logger.remove_handler(orders_file);
            logger.set_level(logging::NOTSET);
        }
        get_logger(QUOTES_LOGGER).set_propagate(true);
    }
    // a writer stuck on the file still holds it: writing to or closing it under that thread would
    // hang or pull it away mid-write, so it stays open (the process is on its way out)
    if (orders_file && orders_fh != nullptr && !orders_file->left_behind()) {
        if (size_t dropped = orders_file->dropped()) {
            logging::Record r {logging::WARNING, std::string(ORDERS_LOGGER),
                               std::format("!!! orders log: {} lines dropped (the writer was far behind)", dropped), now_us()};
            std::string line = logging::format_record(r);
            std::fwrite(line.data(), 1, line.size(), orders_fh);
        }
        std::fclose(orders_fh);
    }
    orders_fh = nullptr;
    writers.clear();
    console.reset();
    orders_file.reset();
}

void log_config(const Config& config, const std::optional<std::string>& source, const std::string& version) {
    Logger& log = get_logger(CONFIG_LOGGER);
    log.info("naive_mm {}, config {}", version, source.value_or("(defaults only)"));
    for (const auto& line : describe(config)) log.info("{}", line);
}

awaitable<void> repeat_version(std::string version, double every_s) {
    Logger& log = get_logger(CONFIG_LOGGER);
    double started = monotonic();
    for (;;) {
        co_await sleep(every_s);
        log.info("naive_mm {}, up {:.1f} h", version, (monotonic() - started) / 3600);
    }
}

}  // namespace nmm
