// Ported from tests/test_main.py: the logging of the command line (the console and the orders
// log, each written by a thread of its own) and the settings and version lines at the top.
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <chrono>
#include <future>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include "agent_helpers.hpp"
#include "logsetup.hpp"
#include "trading/quotes.hpp"
#include "version.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

double elapsed_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// stderr (fd 2) into a file while it lives (pytest's capsys); the console thread writes there.
struct StderrCapture {
    TempFile file {"stderr.txt"};
    int saved = -1;
    StderrCapture() {
        std::fflush(stderr);
        saved = dup(2);
        int fd = open(file.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 2);
        close(fd);
    }
    ~StderrCapture() { restore(); }
    void restore() {
        if (saved < 0) return;
        std::fflush(stderr);
        dup2(saved, 2);
        close(saved);
        saved = -1;
    }
    std::string text() {
        restore();
        return slurp(file.path);
    }
};

// The root logger's level as it was, back when the test ends (setup_logging sets it).
struct RootLevel {
    int level = logging::root().level();
    ~RootLevel() { logging::root().set_level(level); }
};

// The orders log at a named pipe, so the test decides when its writer can write: the pipe
// holds a page until drain() starts reading it (the disk that holds a write up).
struct OrdersPipe {
    TempFile dir {"orders.log"};
    int fd = -1;
    std::thread reader;
    std::mutex mutex;
    std::string got;

    OrdersPipe() {
        mkfifo(dir.path.c_str(), 0600);
        fd = open(dir.path.c_str(), O_RDONLY | O_NONBLOCK);  // a reader, so that the writer's open does not wait
        fcntl(fd, F_SETPIPE_SZ, 4096);
    }
    ~OrdersPipe() {
        if (reader.joinable()) reader.join();
        close(fd);
    }
    std::string path() const { return dir.path.string(); }
    void drain() {
        fcntl(fd, F_SETPIPE_SZ, 1 << 20);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        reader = std::thread([this] {
            char buf[1 << 16];
            for (;;) {
                ssize_t n = ::read(fd, buf, sizeof buf);
                if (n > 0) {
                    std::lock_guard lock(mutex);
                    got.append(buf, static_cast<size_t>(n));
                } else if (n == 0 || errno != EINTR) {
                    return;  // the writer closed the log
                }
            }
        });
    }
    bool wait_for(const std::string& part, double timeout_s = 2.0) {
        auto started = std::chrono::steady_clock::now();
        while (elapsed_since(started) < timeout_s) {
            {
                std::lock_guard lock(mutex);
                if (got.find(part) != std::string::npos) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
    // Everything written, once the log is closed.
    std::string text() {
        if (reader.joinable()) reader.join();
        return got;
    }
};

class OrdersLog : public ::testing::Test {
protected:
    RootLevel root_level;
    StderrCapture console;  // the console only shows warnings, but the order events reach it too
    TempFile file {"orders.log"};
    void SetUp() override { setup_logging(logging::WARNING, file.path.string()); }
    void TearDown() override { stop_logging(); }
    // The orders log once its writer thread has written what it holds.
    std::string read() {
        flush_logging();
        return slurp(file.path);
    }
};

logging::Record record(std::string name, std::string message) { return {logging::INFO, std::move(name), std::move(message), now_us()}; }

}  // namespace

TEST_F(OrdersLog, GetsEachEventAsItHappens) {
    get_logger(ORDERS_LOGGER).info("fill BUY 0.001 @ 83244 (order 0.001/0.001) -> position 0.001");
    get_logger("naive_mm.agent").info("binance BTCUSDT OK ...");  // not an order event
    std::string text = read();  // still open: the line must be on disk as soon as the thread writes it
    EXPECT_NE(text.find("naive_mm.orders: fill BUY 0.001 @ 83244"), std::string::npos) << text;
    EXPECT_EQ(text.find("binance BTCUSDT"), std::string::npos) << text;
}

TEST_F(OrdersLog, GetsThePriceLinesTooButTheConsoleDoesNot) {
    auto console_records = std::make_shared<logging::CaptureHandler>();  // what the root logger, i.e. stderr, would print
    std::string out;
    logging::root().add_handler(console_records);
    set_stdout_capture([&](const std::string& text) { out += text; });
    {
        QuoteStrategy strategy(nullptr, btc_spec(), opts(0.001, 0.0002));
        QuoteWatch w(strategy);
        MarketView view;
        auto venue = std::make_shared<BookTop>();
        venue->venue = "arcus";
        venue->symbol = "BTC-USD";
        venue->bids = {{99'999.0, 1.0}};
        venue->asks = {{99'999.1, 1.0}};
        venue->seq = 1;
        view.venue = venue;
        view.fair_bid = 100'000.0;
        w.update(view);
    }
    set_stdout_capture(nullptr);
    logging::root().remove_handler(console_records);
    std::string text = read();
    EXPECT_NE(text.find("naive_mm.quotes: buy 99900  sell 100100.2  fair bid 100000.00"), std::string::npos) << text;
    EXPECT_NE(out.find("buy 99900  sell 100100.2"), std::string::npos) << out;  // stdout has them
    std::lock_guard lock(console_records->mutex);
    for (const auto& r : console_records->records) EXPECT_NE(r.name, QUOTES_LOGGER) << r.message;  // so stderr does not
}

TEST(Console, ColoursOrderEventsOnly) {
    auto place = record("naive_mm.orders", ">>> PLACE BUY 0.001 @ 83000");
    std::string coloured = console_line(place, true);
    EXPECT_TRUE(coloured.starts_with("\x1b[1;36m") && ends_with(coloured, "\x1b[0m\n")) << coloured;  // the line, then its newline
    EXPECT_TRUE(console_line(record("naive_mm.orders", "<<< FILLED BUY 0.001 @ 83000"), true).starts_with("\x1b[1;32m"));
    EXPECT_TRUE(console_line(record("naive_mm.orders", "!!! DROP Binance bid ..."), true).starts_with("\x1b[1;35m"));
    EXPECT_EQ(console_line(place, false).find("\x1b["), std::string::npos);
    EXPECT_EQ(console_line(record("naive_mm.agent", ">>> PLACE looks alike"), true).find("\x1b["), std::string::npos);
}

TEST_F(OrdersLog, TheSettingsInEffectOpenIt) {
    Config config;
    config.trading.markup = 0.000005;
    config.guard.window_ms = 1500.0;
    std::string version = build_version();
    log_config(config, "my.toml", version);
    auto lines = split_lines(read());
    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(ends_with(lines[0], "naive_mm.config: naive_mm " + version + ", config my.toml")) << lines[0];
    EXPECT_TRUE(std::regex_search(version, std::regex("code [0-9a-f]{10}$"))) << version;  // the code's own fingerprint, git or not
    EXPECT_TRUE(std::any_of(lines.begin(), lines.end(), [](const std::string& line) {
        return ends_with(line, "naive_mm.config: [guard] window_ms=1500.0 drawdown=0.0001 pause_ms=100.0");
    }));
    auto trading = std::find_if(lines.begin(), lines.end(), [](const std::string& line) { return line.find("[trading]") != std::string::npos; });
    ASSERT_NE(trading, lines.end());
    EXPECT_NE(trading->find("markup=5e-06"), std::string::npos) << *trading;
    EXPECT_NE(trading->find("never_lead=False"), std::string::npos) << *trading;
    // the version line may hold a "[" of its own (a commit subject naming a section)
    std::set<std::string> sections;
    const std::string mark = "naive_mm.config: [";
    for (const auto& line : lines)
        if (auto at = line.find(mark); at != std::string::npos) {
            std::string rest = line.substr(at + mark.size());
            sections.insert(rest.substr(0, rest.find(']')));
        }
    // every section, so a log shows all it ran with
    EXPECT_EQ(sections, (std::set<std::string> {"binance", "arcus", "pricing", "ml", "trading", "guard", "futures", "trades", "bybit",
                                                "okx", "ctrader", "arcus_guard", "takes", "trend", "add_guard", "open_guard", "open_halt", "portfolio",
                                                "vol_markup", "drift", "lag", "imbalance", "agent"}));
}

TEST_F(OrdersLog, TheVersionLineComesAgainWhileRunning) {
    {
        Loop loop;
        boost::asio::co_spawn(loop.ioc, repeat_version("commit abc (branch x), code 0123456789", 0.01), boost::asio::detached);
        loop.run_for(0.035);
    }  // the loop and the task with it
    std::vector<std::string> lines;
    for (const auto& line : split_lines(read()))
        if (line.find("naive_mm commit abc") != std::string::npos) lines.push_back(line);
    ASSERT_GE(lines.size(), 2u);
    EXPECT_TRUE(ends_with(lines[0], "naive_mm.config: naive_mm commit abc (branch x), code 0123456789, up 0.0 h")) << lines[0];
}

// Review: writing to a paused terminal or a stalled pipe froze the event loop. The console is
// now written by a thread; a line must still come out formatted once.
TEST(Console, IsWrittenByAThreadAndEachLineFormattedOnce) {
    RootLevel root_level;
    StderrCapture capture;
    setup_logging(logging::INFO);
    get_logger("naive_mm.arcus").info("book synced at seq {}", 7);
    stop_logging();  // writes out what the thread holds
    std::string err = capture.text();
    EXPECT_TRUE(std::regex_search(err, std::regex(R"(\d\d:\d\d:\d\d\.\d{3} INFO    naive_mm\.arcus: book synced at seq 7\n)"))) << err;
    EXPECT_EQ(err.find("INFO:naive_mm"), std::string::npos) << err;
}

// Live ETH 18-19 Oct: the event loop stood 100-228 ms twenty times with nothing to do; the orders
// log was written and flushed line by line on it. A write held up (a slow disk; here a full pipe)
// now holds up only the writer thread: logging returns at once, and the line is in the log when
// the thread gets to it.
TEST(OrdersLogWriter, IsAThreadNotTheCaller) {
    RootLevel root_level;
    StderrCapture console;
    OrdersPipe pipe;
    setup_logging(logging::WARNING, pipe.path());
    Logger& orders = get_logger(ORDERS_LOGGER);
    auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) orders.info("<<< FILL BUY 0.001 @ 83000 (filler {})", i);  // more than the pipe holds
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the writer is stuck on the full pipe now
    orders.info(">>> PLACE BUY 0.001 @ 83000");
    EXPECT_LT(elapsed_since(started), 0.1);  // the caller did not wait for the disk
    pipe.drain();
    flush_logging();
    EXPECT_TRUE(pipe.wait_for(">>> PLACE BUY 0.001 @ 83000"));
    stop_logging();
    EXPECT_NE(pipe.text().find(">>> PLACE BUY 0.001 @ 83000"), std::string::npos);
}

// The Python test sets the count by hand; here the writer is held up on a full pipe until its
// queue is full and lines are really dropped.
TEST(OrdersLogWriter, LinesItDroppedAreCountedAtTheEnd) {
    RootLevel root_level;
    StderrCapture console;
    OrdersPipe pipe;
    auto handler = setup_logging(logging::WARNING, pipe.path());
    ASSERT_NE(handler, nullptr);
    Logger& orders = get_logger(ORDERS_LOGGER);
    orders.info("<<< FILL BUY 0.001 @ 83000");
    for (size_t i = 0; i < logging::QUEUE_SIZE + 500; ++i) orders.info("filler {}", i);
    size_t dropped = handler->dropped();
    EXPECT_GT(dropped, 0u);
    pipe.drain();
    stop_logging();
    std::string text = pipe.text();
    EXPECT_NE(text.find("<<< FILL BUY 0.001 @ 83000"), std::string::npos);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    EXPECT_TRUE(ends_with(text, std::format("!!! orders log: {} lines dropped (the writer was far behind)", dropped)))
        << text.substr(text.size() > 200 ? text.size() - 200 : 0);
}

// test_review_19oct.test_7: a writer still stuck on its file after stop_logging's timeouts keeps
// the file: no line is written beside its own, and the file is not closed under it.
TEST(OrdersLogWriter, AWriterStillStuckAfterItsTimeoutKeepsItsFile) {
    RootLevel root_level;
    StderrCapture console;
    OrdersPipe pipe;  // a page: the writer is stuck once it is full, until the test reads
    auto handler = setup_logging(logging::WARNING, pipe.path());
    ASSERT_NE(handler, nullptr);
    Logger& orders = get_logger(ORDERS_LOGGER);
    const size_t n = logging::QUEUE_SIZE + 500;
    for (size_t i = 0; i < n; ++i) orders.info("filler {}", i);
    size_t dropped = handler->dropped();
    ASSERT_GT(dropped, 0u);  // a "lines dropped" line is due, if the file could take one
    auto started = std::chrono::steady_clock::now();
    stop_logging();  // two seconds to write out what is held, two more to end: then it is left behind
    EXPECT_LT(elapsed_since(started), 6.0);
    EXPECT_TRUE(handler->left_behind());
    // once the file takes lines again the writer writes all it held: whole, in order, nothing beside them
    fcntl(pipe.fd, F_SETPIPE_SZ, 1 << 20);
    const std::string last = std::format("filler {}\n", n - dropped - 1);
    std::string got;
    bool eof = false;
    std::vector<char> buf(1 << 16);
    auto reading = std::chrono::steady_clock::now();
    while (elapsed_since(reading) < 20.0 && !ends_with(got, last)) {
        ssize_t r = ::read(pipe.fd, buf.data(), buf.size());
        if (r > 0) {
            got.append(buf.data(), static_cast<size_t>(r));
        } else if (r == 0) {
            eof = true;  // the file closed under the writer
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    EXPECT_FALSE(eof);
    EXPECT_TRUE(ends_with(got, last)) << got.substr(got.size() > 200 ? got.size() - 200 : 0);
    EXPECT_EQ(got.find("lines dropped"), std::string::npos);
    EXPECT_EQ(static_cast<size_t>(std::count(got.begin(), got.end(), '\n')), n - dropped);
}

TEST(OrdersLogWriter, StopSaysWhetherTheThreadIsDone) {
    std::vector<std::string> out;
    logging::BackgroundQueue<std::string> records([&out](std::string& s) { out.push_back(s); });
    records.push("a");
    EXPECT_TRUE(records.stop());
    EXPECT_TRUE(records.stop());
    EXPECT_EQ(out, std::vector<std::string> {"a"});
}

TEST(OrdersLogWriter, AWriterStuckOnItsDiskDoesNotHangTheShutdown) {
    std::promise<void> release;
    std::shared_future<void> stuck = release.get_future().share();
    std::atomic<int> written {0};
    {
        logging::BackgroundQueue<std::string> records(
            [stuck, &written](std::string&) {
                stuck.wait_for(std::chrono::seconds(10));
                ++written;
            },
            1);
        records.push("a");  // the thread takes it and is stuck writing it
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        records.push("b");  // the queue is full now
        auto started = std::chrono::steady_clock::now();
        EXPECT_FALSE(records.stop(0.1));  // left behind: what it writes to must stay open
        EXPECT_LT(elapsed_since(started), 1.0);
        EXPECT_FALSE(records.stop(0.1));
    }  // and gone, while its thread is still stuck
    release.set_value();
    for (int i = 0; i < 1000 && written < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(written, 2);  // the thread woke to a queue gone and wrote what it held
}
