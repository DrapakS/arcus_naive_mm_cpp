// CLI entry point: naive_mm [--config config.toml] [--ladder] [--trade MODE] ...
//
// The C++ port of naive_mm: a naive market maker for Arcus perps, priced off Binance.

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/signal_set.hpp>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "agent.hpp"
#include "config.hpp"
#include "logsetup.hpp"
#include "net/net.hpp"
#include "trading/credentials.hpp"
#include "util/aio.hpp"
#include "util/log.hpp"
#include "util/pyfmt.hpp"
#include "version.hpp"

using namespace nmm;

namespace {

const char* USAGE =
    "usage: naive_mm [-h] [--config CONFIG] [--ladder] [--log-level LOG_LEVEL] [--trade {off,quote,paper,live}]\n"
    "                [--credentials CREDENTIALS] [--orders-log ORDERS_LOG] [--color {auto,always,never}]\n"
    "                [--set SECTION.KEY=VALUE] [--check]\n";

const char* HELP =
    "\nNaive market maker for Arcus perps, priced off Binance (the C++ port of naive_mm).\n\n"
    "options:\n"
    "  -h, --help            show this help message and exit\n"
    "  --config CONFIG       TOML config (default: built-in defaults)\n"
    "  --ladder              print both top-N ladders on every book update\n"
    "  --log-level LOG_LEVEL\n"
    "                        override config log_level\n"
    "  --trade {off,quote,paper,live}\n"
    "                        override trading.mode: quote prints where orders would go, without trading\n"
    "  --credentials CREDENTIALS\n"
    "                        credentials file (default: $NAIVE_MM_CREDENTIALS, then\n"
    "                        ~/.config/naive_mm/credentials.toml); used by --trade live\n"
    "  --orders-log ORDERS_LOG\n"
    "                        also write order events (placements, fills, cancels and the exchange's answers) and\n"
    "                        the buy/sell price lines to this file, each line as it happens\n"
    "  --color {auto,always,never}\n"
    "                        colour order events in the console log (auto: when stderr is a terminal)\n"
    "  --set SECTION.KEY=VALUE\n"
    "                        override one config value after the file, e.g. --set drift.markup_factor=1\n"
    "                        (repeatable)\n"
    "  --check               load and check the config (and the --set values), then exit: 0 when it is fine\n";

[[noreturn]] void usage_error(const std::string& message) {
    std::cerr << USAGE << "naive_mm: error: " << message << "\n";
    std::exit(2);
}

struct Args {
    std::optional<std::string> config;
    bool ladder = false;
    std::optional<std::string> log_level;
    std::optional<std::string> trade;
    std::optional<std::string> credentials;
    std::optional<std::string> orders_log;
    std::string color = "auto";
    std::vector<std::string> set;
    bool check = false;
};

Args parse_args(int argc, char** argv) {
    Args args;
    auto value = [&](int& i, const std::string& name, const std::string& arg) -> std::string {
        if (auto eq = arg.find('='); eq != std::string::npos) return arg.substr(eq + 1);
        if (i + 1 >= argc) usage_error("argument " + name + ": expected one argument");
        return argv[++i];
    };
    auto choice = [&](const std::string& name, const std::string& v, std::initializer_list<const char*> choices) {
        for (const char* c : choices)
            if (v == c) return v;
        std::vector<std::string> quoted;
        for (const char* c : choices) quoted.push_back(pyrepr(std::string_view(c)));
        usage_error(std::format("argument {}: invalid choice: {} (choose from {})", name, pyrepr(std::string_view(v)), join(quoted, ", ")));
    };
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string name = arg.substr(0, arg.find('='));
        if (arg == "-h" || arg == "--help") {
            std::cout << USAGE << HELP;
            std::exit(0);
        } else if (name == "--config") {
            args.config = value(i, name, arg);
        } else if (arg == "--ladder") {
            args.ladder = true;
        } else if (name == "--log-level") {
            args.log_level = value(i, name, arg);
        } else if (name == "--trade") {
            args.trade = choice(name, value(i, name, arg), {"off", "quote", "paper", "live"});
        } else if (name == "--credentials") {
            args.credentials = value(i, name, arg);
        } else if (name == "--orders-log") {
            args.orders_log = value(i, name, arg);
        } else if (name == "--color") {
            args.color = choice(name, value(i, name, arg), {"auto", "always", "never"});
        } else if (name == "--set") {
            args.set.push_back(value(i, name, arg));
        } else if (arg == "--check") {
            args.check = true;
        } else {
            usage_error("unrecognized arguments: " + arg);
        }
    }
    return args;
}

bool use_color(const std::string& choice) {
    if (choice != "auto") return choice == "always";
    if (std::getenv("NO_COLOR") != nullptr || !isatty(STDERR_FILENO)) return false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    Config config;
    try {
        config = load_config(args.config, args.set);
    } catch (const std::exception& err) {
        usage_error(err.what());
    }
    if (args.check) return 0;
    if (args.trade) config.trading.mode = *args.trade;
    if (args.orders_log) config.trading.orders_log = *args.orders_log;
    std::optional<ArcusCredentials> credentials;
    if (config.trading.mode == "live") {
        try {
            credentials = load_credentials(resolve_credentials_path(args.credentials));
        } catch (const CredentialsError& err) {
            usage_error(err.what());
        }
    }
    int level;
    try {
        level = logging::parse_level(args.log_level.value_or(config.log_level));
    } catch (const std::invalid_argument& err) {
        usage_error(err.what());
    }
    try {
        setup_logging(level, config.trading.orders_log, use_color(args.color));
    } catch (const std::exception& err) {
        usage_error(err.what());
    }
    start_stdout();  // quote lines and ladders: written by a thread, never on the event loop
    int status = 0;
    try {
        std::optional<std::string> source = args.config;
        if (!args.set.empty()) source = std::format("{} with --set {}", source.value_or("(defaults)"), join(args.set, ", "));
        std::string version = build_version();
        log_config(config, source, version);
        Agent agent(config, args.ladder, credentials);
        get_logger(CONFIG_LOGGER).info("event loop: boost::asio, one thread");
        asio::io_context ioc(1);
        set_loop(ioc);
        init_tls();
        // A closed terminal or a dropped ssh session (SIGHUP) shuts down cleanly too: killed by it,
        // the bot would leave its order resting until the dead man's switch fires.
        asio::signal_set signals(ioc, SIGINT, SIGTERM, SIGHUP);
        signals.async_wait([&agent](const boost::system::error_code& ec, int) {
            if (ec) return;
            get_logger("naive_mm").info("shutting down");
            agent.stop();
        });
        Task repeat = Task::spawn(repeat_version(version), "version");
        std::exception_ptr failure;
        asio::co_spawn(ioc, agent.run(), [&](std::exception_ptr e) {
            failure = e;
            repeat.cancel();
            signals.cancel();
            ioc.stop();
        });
        ioc.run();
        if (failure) std::rethrow_exception(failure);
    } catch (const std::exception& err) {
        get_logger("naive_mm").error("stopped: {}", err.what());
        status = 1;
    }
    stop_stdout();
    stop_logging();
    return status;
}
