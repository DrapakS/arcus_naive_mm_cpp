// [ctrader]: the broker's spot price over the cTrader FIX API (QUOTE session), one more
// input of the price guard (the collector's tests/test_books_ctrader.py, cut to a guard).
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <cmath>

#include "agent_helpers.hpp"
#include "fakes.hpp"
#include "feed_helpers.hpp"
#include "feeds/ctrader.hpp"
#include "feeds/fix.hpp"
#include "test_util.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
using namespace nmm::test;

namespace {

CTraderCredentials creds() {
    CTraderCredentials c;
    c.host = "demo.cfixapi.com";
    c.sender_comp_id = "demo.icmarkets.10128514";
    c.password = "secret";
    return c;
}

// A message from the broker, as the parser hands it over.
fix::Message msg(const std::string& type, int64_t seq, fix::Fields body = {}) {
    fix::Fields head {{35, type}, {49, "cServer"}, {56, "demo.icmarkets.10128514"}, {34, std::to_string(seq)},
                      {52, "20261023-09:30:00.250"}};
    head.insert(head.end(), body.begin(), body.end());
    return fix::Message {head};
}

struct Session {
    OrderBook book {"ctrader", "US500"};
    std::vector<fix::Message> sent;
    int updates = 0;
    CTraderFeed feed;

    explicit Session(const std::string& symbol = "US500")
        : feed(book, symbol, creds(), [this] {
              FeedOptions o;
              o.on_update = [this](OrderBook&) { ++updates; };
              return o;
          }()) {
        feed.sink = [this](std::string data) {
            fix::Parser parser;
            for (auto& m : parser.feed(data)) sent.push_back(std::move(m));
        };
    }

    // Logged on and the symbol list in: US500 is id 1, XAUUSD 41.
    void listed(fix::Fields symbols = {{146, "2"}, {55, "1"}, {1007, "US500"}, {1008, "1"}, {55, "41"}, {1007, "XAUUSD"}}) {
        feed.start_session();
        feed.handle(msg("A", 1, {{98, "0"}, {108, "30"}}));
        symbols.emplace_back(893, "Y");
        feed.handle(msg("y", 2, symbols));
    }
};

fix::Fields spot(const std::string& bid, const std::string& ask, const std::string& id = "1") {
    return {{55, id}, {268, "2"}, {269, "0"}, {270, bid}, {269, "1"}, {270, ask}};
}

}  // namespace

TEST(Fix, EncodeFramesTheMessageWithItsLengthAndChecksum) {
    std::string data = fix::encode("0", {{49, "a"}, {56, "b"}}, {{112, "t"}});
    std::string body = std::string("35=0\x01") + "49=a\x01" + "56=b\x01" + "112=t\x01";
    EXPECT_EQ(body.size(), 21u);
    std::string head = "8=FIX.4.4\x01" "9=21\x01" + body;
    EXPECT_EQ(data, head + "10=" + fix::checksum(head) + "\x01");
    fix::Parser parser;
    auto out = parser.feed(data);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].type(), "0");
    EXPECT_EQ(out[0].get_or(112), "t");
    EXPECT_EQ(out[0].get(999), nullptr);
}

TEST(Fix, TheParserJoinsChunksSkipsGarbageAndRefusesABadChecksum) {
    std::string a = fix::encode("W", {{34, "7"}}, spot("6500.1", "6500.3"));
    std::string b = fix::encode("0", {{34, "8"}}, {});
    std::string stream = "junk" + a + b;
    fix::Parser parser;
    std::vector<fix::Message> got;
    for (size_t i = 0; i < stream.size(); i += 7)  // seven bytes at a time
        for (auto& m : parser.feed(std::string_view(stream).substr(i, 7))) got.push_back(std::move(m));
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].groups(268, 269).size(), 2u);
    EXPECT_EQ(got[0].groups(268, 269)[1].at(270), "6500.3");
    EXPECT_EQ(got[1].get_or(34), "8");
    std::string bad = a;
    bad[bad.size() - 2] = bad[bad.size() - 2] == '0' ? '1' : '0';
    fix::Parser p2;
    EXPECT_THROW(p2.feed(bad), fix::FixError);
    EXPECT_EQ(p2.feed(b).size(), 1u);  // the next message still comes through
}

TEST(Fix, Timestamps) {
    EXPECT_EQ(fix::parse_timestamp("20261023-09:30:00.250"), 1792747800'250'000);
    EXPECT_EQ(fix::parse_timestamp("20261023-09:30:00"), 1792747800'000'000);
    EXPECT_EQ(fix::parse_timestamp("20261023-09:30:00.123456"), 1792747800'123'456);
    EXPECT_EQ(fix::parse_timestamp("2026-10-23 09:30"), std::nullopt);
    EXPECT_EQ(fix::utc_timestamp(1792747800'250'999), "20261023-09:30:00.250");
}

TEST(CTraderFeed, TheLogonCarriesTheLoginAndThePasswordAndTheListIsAskedAfterIt) {
    Session s;
    s.feed.start_session();
    ASSERT_EQ(s.sent.size(), 1u);
    const fix::Message& logon = s.sent[0];
    EXPECT_EQ(logon.type(), "A");
    for (auto [tag, value] : std::initializer_list<std::pair<int, std::string>> {
             {49, "demo.icmarkets.10128514"}, {56, "cServer"}, {34, "1"}, {50, "QUOTE"}, {57, "QUOTE"}, {98, "0"}, {108, "30"},
             {141, "Y"}, {553, "10128514"}, {554, "secret"}})
        EXPECT_EQ(logon.get_or(tag), value) << tag;
    EXPECT_EQ(logon.str().find("secret"), std::string::npos);  // never in a log line
    s.feed.handle(msg("A", 1));
    EXPECT_TRUE(s.feed.logged_on);
    ASSERT_EQ(s.sent.size(), 2u);
    EXPECT_EQ(s.sent[1].type(), "x");
    EXPECT_EQ(s.sent[1].get_or(559), "0");
    EXPECT_EQ(s.sent[1].get_or(34), "2");
}

TEST(CTraderFeed, TheSymbolIsSubscribedByItsIdAndEachSpotPriceIsTheTop) {
    Session s;
    s.feed.start_session();
    s.feed.handle(msg("A", 1));
    s.feed.handle(msg("y", 2, {{146, "1"}, {55, "41"}, {1007, "XAUUSD"}, {893, "N"}}));  // the list comes in two parts
    EXPECT_FALSE(s.feed.symbol_id);
    s.feed.handle(msg("y", 3, {{146, "1"}, {55, "1"}, {1007, "US500"}, {893, "Y"}}));
    ASSERT_EQ(s.feed.symbol_id, "1");
    const fix::Message& req = s.sent.back();
    EXPECT_EQ(req.type(), "V");
    EXPECT_EQ(req.get_or(55), "1");
    EXPECT_EQ(req.get_or(264), "1");  // the spot price
    EXPECT_EQ(req.groups(267, 269).size(), 2u);
    s.feed.handle(msg("W", 4, spot("6500.1", "6500.3")));
    EXPECT_TRUE(s.book.is_ready());
    EXPECT_EQ(s.book.best_bid()->first, 6500.1);
    EXPECT_EQ(s.book.best_bid()->second, 1.0);  // no sizes from the broker: 1
    EXPECT_EQ(s.book.best_ask()->first, 6500.3);
    EXPECT_EQ(s.book.seq, 4);
    EXPECT_EQ(s.book.exchange_ts_us, 1792747800'250'000);
    s.feed.handle(msg("W", 5, spot("1", "2", "41")));  // another symbol
    EXPECT_EQ(s.book.best_bid()->first, 6500.1);
    s.feed.handle(msg("W", 6, spot("6500.4", "6500.2")));  // crossed: the last good top stays
    EXPECT_EQ(s.book.best_bid()->first, 6500.1);
    EXPECT_EQ(s.updates, 1);
    // incremental updates by MDEntryID, should the broker send them
    s.feed.handle(msg("W", 7, {{55, "1"}, {268, "3"}, {269, "0"}, {270, "6500.0"}, {271, "5"}, {278, "a"}, {269, "0"}, {270, "6500.0"},
                               {271, "2"}, {278, "b"}, {269, "1"}, {270, "6500.2"}, {271, "4"}, {278, "c"}}));
    EXPECT_EQ(s.book.best_bid(), Level(6500.0, 7.0));  // two quotes at one price: their sizes summed
    s.feed.handle(msg("X", 8, {{268, "2"}, {279, "2"}, {278, "a"}, {55, "1"}, {279, "0"}, {269, "1"}, {270, "6500.1"}, {271, "1"},
                               {278, "d"}, {55, "1"}}));
    EXPECT_EQ(s.book.best_bid(), Level(6500.0, 2.0));
    EXPECT_EQ(s.book.best_ask(), Level(6500.1, 1.0));
    EXPECT_EQ(s.book.seq, 8);
    s.feed.lost();
    EXPECT_FALSE(s.book.is_ready());
}

TEST(CTraderFeed, ALostMessageALogoutOrSilenceDropsTheSession) {
    {
        Session s;
        s.listed();
        s.feed.handle(msg("4", 9, {{36, "20"}}));  // SequenceReset: the next is 20
        s.feed.handle(msg("0", 20));
        EXPECT_THROW(s.feed.handle(msg("0", 22)), CTraderSessionError);
    }
    {
        Session s;
        s.listed();
        s.feed.handle(msg("0", 3));
        s.feed.handle(msg("0", 2, {{43, "Y"}}));  // a possible duplicate is let through
        try {
            s.feed.handle(msg("5", 4, {{58, "Another session with the same login"}}));
            ADD_FAILURE() << "no throw";
        } catch (const CTraderSessionError& e) {
            EXPECT_EQ(std::string(e.what()), "logged out by the broker: Another session with the same login");
        }
    }
    {
        Session s;
        s.listed();
        size_t before = s.sent.size();
        s.feed.handle(msg("1", 3, {{112, "ping7"}}));  // a TestRequest is answered
        ASSERT_EQ(s.sent.size(), before + 1);
        EXPECT_EQ(s.sent.back().type(), "0");
        EXPECT_EQ(s.sent.back().get_or(112), "ping7");
        double now = monotonic();
        EXPECT_EQ(s.feed.tick(now + 10), std::nullopt);
        EXPECT_EQ(s.sent.size(), before + 1);
        EXPECT_EQ(s.feed.tick(now + 31), std::nullopt);  // 30 s of sending nothing: a Heartbeat
        EXPECT_EQ(s.sent.back().type(), "0");
        EXPECT_EQ(s.feed.tick(now + 36), std::nullopt);  // 35 s of silence: a TestRequest
        EXPECT_EQ(s.sent.back().type(), "1");
        auto why = s.feed.tick(now + 67);
        ASSERT_TRUE(why);
        EXPECT_TRUE(starts_with(*why, "nothing from the broker for 6")) << *why;
    }
    {
        Session s;
        s.feed.start_session();  // no answer to the logon
        EXPECT_EQ(s.feed.tick(monotonic() + 16), "no logon answer in 15 s");
    }
}

TEST(CTraderFeed, ANameIsFoundButForItsCaseOrASuffixAndAnUnknownOneLeavesTheGuardIdle) {
    {
        Session s("NVDA");
        s.listed({{146, "3"}, {55, "7"}, {1007, "NVDA.US"}, {55, "8"}, {1007, "NVDAX"}, {55, "1"}, {1007, "US500"}});
        EXPECT_EQ(s.feed.symbol_name, "NVDA.US");
        EXPECT_EQ(s.feed.symbol_id, "7");
    }
    {
        Session s("us500");
        s.listed();
        EXPECT_EQ(s.feed.symbol_id, "1");
    }
    {
        LogCapture logs("naive_mm.ctrader", logging::ERROR);
        Session s("SPY");
        s.listed({{146, "2"}, {55, "1"}, {1007, "US500"}, {55, "9"}, {1007, "SPY.US"}});
        Session t("NVD");
        t.listed({{146, "1"}, {55, "1"}, {1007, "US500"}});
        EXPECT_EQ(s.feed.symbol_id, "9");
        EXPECT_FALSE(t.feed.symbol_id);
        EXPECT_EQ(t.sent.back().type(), "x");  // nothing subscribed
        EXPECT_EQ(logs.messages(), (std::vector<std::string> {"NVD: not in the broker's symbol list: the cTrader guard stays idle"}));
    }
    {
        LogCapture logs("naive_mm.ctrader", logging::ERROR);
        Session s;
        s.listed();
        s.feed.handle(msg("Y", 3, {{262, "US500"}, {58, "Symbol is not available"}}));
        EXPECT_EQ(s.feed.refused, "Symbol is not available");
        EXPECT_EQ(logs.messages(), (std::vector<std::string> {"US500: market data request rejected: Symbol is not available"}));
    }
}

TEST(CTraderFeed, CredentialsComeFromAFileOnlyItsOwnerReads) {
    TempFile f("ctrader_fix.toml");
    const std::string& path = f.write(R"(host = "demo.cfixapi.com"
port = 5211
sender_comp_id = "demo.icmarkets.10128514"
password = "secret"
)");
    ::chmod(path.c_str(), 0644);
    auto error = [&] {
        try {
            load_ctrader_credentials(path);
        } catch (const std::invalid_argument& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    EXPECT_NE(error().find("readable by group or others"), std::string::npos) << error();
    ::chmod(path.c_str(), 0600);
    CTraderCredentials c = load_ctrader_credentials(path);
    EXPECT_EQ(c.host, "demo.cfixapi.com");
    EXPECT_EQ(c.port, 5211);
    EXPECT_EQ(c.login(), "10128514");
    EXPECT_EQ(c.target_comp_id, "cServer");
    EXPECT_EQ(c.sender_sub_id, "QUOTE");
    EXPECT_TRUE(c.ssl);
    EXPECT_EQ(c.str(), "demo.icmarkets.10128514 at demo.cfixapi.com:5211 (TLS)");
    for (auto [text, wants] : std::initializer_list<std::pair<std::string, std::string>> {
             {"host = \"h\"\nport = 5211\nsender_comp_id = \"a.b.1\"\npassword = \"PASSWORD_OF_ACCOUNT_1\"\n", "placeholder"},
             {"host = \"h\"\nport = 5211\nsender_comp_id = \"a.b.1\"\npassword = \"p\"\nlogin = \"1\"\n", "unknown keys login"},
             {"host = \"h\"\nsender_comp_id = \"a.b.1\"\n", "missing password, port"},
             {"host = \"h\"\nport = \"5211\"\nsender_comp_id = \"a.b.1\"\npassword = \"p\"\n", "port must be a port number"}}) {
        f.write(text);
        ::chmod(path.c_str(), 0600);
        EXPECT_NE(error().find(wants), std::string::npos) << error();
    }
    f.write("host = \"h\"\nport = 5201\nssl = false\nsender_comp_id = \"a.b.1\"\npassword = \"p\"\nusername = \"77\"\n");
    ::chmod(path.c_str(), 0600);
    c = load_ctrader_credentials(path);
    EXPECT_FALSE(c.ssl);
    EXPECT_EQ(c.login(), "77");
    std::remove(path.c_str());
    EXPECT_NE(error().find("no cTrader FIX settings at"), std::string::npos);
}

namespace {

using boost::asio::ip::tcp;

struct FakeBroker {
    int sessions = 0;
    std::vector<std::string> received;
};

awaitable<void> broker_reply(tcp::socket& socket, int& out, const std::string& type, fix::Fields body) {
    std::string data = fix::encode(type, {{49, "cServer"}, {56, "demo.icmarkets.1"}, {34, std::to_string(++out)}, {52, "20261023-09:30:00.250"}},
                                   body);
    co_await boost::asio::async_write(socket, boost::asio::buffer(data), boost::asio::use_awaitable);
}

// The logon, the symbol list, a price, then (first session) the broker hangs up.
awaitable<void> broker_session(FakeBroker& broker, tcp::socket socket) {
    int n = ++broker.sessions;
    fix::Parser parser;
    int out = 0;
    char buf[4096];
    // GCC 13 breaks on braced lists inside a co_await: each reply is built first
    fix::Fields logon {{98, "0"}, {108, "30"}};
    fix::Fields list {{146, "1"}, {55, "1"}, {1007, "US500"}};
    fix::Fields price = n == 1 ? spot("6500.1", "6500.3") : spot("6501.1", "6501.3");
    for (;;) {
        boost::system::error_code ec;
        size_t len = co_await socket.async_read_some(boost::asio::buffer(buf), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) co_return;
        std::vector<std::string> types;
        for (auto& m : parser.feed(std::string_view(buf, len))) types.push_back(m.type());
        for (const std::string& type : types) {
            broker.received.push_back(type);
            if (type == "A") co_await broker_reply(socket, out, "A", logon);
            if (type == "x") co_await broker_reply(socket, out, "y", list);
            if (type == "V") co_await broker_reply(socket, out, "W", price);
        }
        if (n == 1 && broker.received.size() >= 3) {
            socket.close();
            co_return;
        }
    }
}

awaitable<void> broker_accept(FakeBroker& broker, tcp::acceptor& acceptor) {
    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(boost::asio::use_awaitable);
        boost::asio::co_spawn(acceptor.get_executor(), broker_session(broker, std::move(socket)), boost::asio::detached);
    }
}

}  // namespace

// A broker on 127.0.0.1 (plain FIX): the session goes through, and is built again after the
// broker hangs up.
TEST(CTraderFeed, ASessionAgainstAFakeBroker) {
    boost::asio::io_context ioc;
    set_loop(ioc);
    tcp::acceptor acceptor(ioc, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    FakeBroker broker;
    boost::asio::co_spawn(ioc, broker_accept(broker, acceptor), boost::asio::detached);
    CTraderCredentials c = creds();
    c.host = "127.0.0.1";
    c.port = acceptor.local_endpoint().port();
    c.ssl = false;
    c.sender_comp_id = "demo.icmarkets.1";
    OrderBook book("ctrader", "US500");
    FeedOptions options;
    options.reconnect_min = 0.05;
    double bid = 0.0;
    options.on_update = [&](OrderBook& b) {
        if (b.is_ready()) bid = b.best_bid()->first;
    };
    CTraderFeed feed(book, "US500", c, options);
    Task task = Task::spawn(feed.run());
    EXPECT_TRUE(run_until(ioc, [&] { return bid == 6500.1; }));
    EXPECT_TRUE(run_until(ioc, [&] { return bid == 6501.1; }));  // built again after the broker hung up
    EXPECT_EQ(feed.connect_count, 2);
    ASSERT_GE(broker.received.size(), 3u);
    EXPECT_EQ(std::vector<std::string>(broker.received.begin(), broker.received.begin() + 3), (std::vector<std::string> {"A", "x", "V"}));
    task.cancel();
    run_for(ioc, 0.02);
    acceptor.close();
    run_for(ioc, 0.02);
}

namespace {

// The cTrader top is now `bid` / `bid` + 0.2 (mid `bid` + 0.1), stamped 40 ms before it was read.
void ctrader(Agent& agent, const std::string& bid) {
    OrderBook& book = *agent.ctrader_book;
    book.apply_snapshot(lv(bid), lv(std::format("{:.1f}", std::stod(bid) + 0.2)), book.seq + 1, 5'000'000);
    book.exchange_ts_us = book.local_ts_us - 40'000;
    agent.on_book_update(book);
}

Bot ctrader_agent(const std::string& order_side) {
    Config c;
    c.pricing.window = c.pricing.min_samples = 1;
    c.ctrader.enabled = true;
    c.ctrader.symbol = "US500";
    c.ctrader.window_ms = 10;
    c.ctrader.drawdown = 0.0001;
    c.ctrader.pause_ms = 50;
    c.guard.drawdown = c.futures.drawdown = c.arcus_guard.drawdown = 0;
    Bot b;
    b.agent = std::make_unique<Agent>(c);
    for (Pauses* g : b->guards) g->clock = b.clock.fn();
    b.gw = install(*b.agent, opts(0.001, 0.01, b->guards));
    b->reference_book.apply_snapshot(lv("100000"), lv("100000.1"), 1);
    b->venue_book.apply_snapshot(lv("99990"), lv("99990.1"), 1);
    frame(*b.agent, b->venue_book);
    b->reference_book.apply_update(lv("100000", "2"), JsonArray {}, 2);
    frame(*b.agent, b->reference_book);
    if (order_side == "SELL") b.gw->add_pos(Decimal("0.001"));
    evaluate(*b.agent);
    b.gw->ack();
    EXPECT_EQ(b.gw->sides(), (Sides {{"place", order_side}}));
    return b;
}

}  // namespace

TEST(CTraderFeed, ABrokerFallPullsABuyAndARisePullsASell) {
    Loop loop;
    LogCapture log(ORDERS_LOGGER);
    {
        Bot b = ctrader_agent("BUY");
        Agent& a = *b.agent;
        ctrader(a, "6500.0");
        b.clock.advance(0.01);
        ctrader(a, "6500.0");
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place"}));  // cTrader frames alone change nothing
        ctrader(a, "6499.0");  // the mid 1.5 bp under its mean
        EXPECT_EQ(b.gw->kinds(), (Kinds {"place", "cancel"}));
        auto messages = log.messages();
        EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const std::string& m) {
            return starts_with(m, "!!! DROP cTrader US500 mid 6499.10 is 1.54 bp under its mean 6500.10 over the last") &&
                   m.find("(read 40 ms after the exchange's event)") != std::string::npos;
        })) << join(messages, "\n");
        EXPECT_TRUE(a.strategy->paused("BUY"));
        EXPECT_FALSE(a.strategy->paused("SELL"));
    }
    {
        Bot b = ctrader_agent("SELL");
        Agent& a = *b.agent;
        ctrader(a, "6500.0");
        b.clock.advance(0.01);
        ctrader(a, "6501.0");
        EXPECT_EQ(b.gw->sides(), (Sides {{"place", "SELL"}, {"cancel", "SELL"}}));
    }
}

TEST(CTraderFeed, Settings) {
    Loop loop;
    Config config;
    EXPECT_FALSE(config.ctrader.enabled);
    {
        Agent off(config);
        EXPECT_FALSE(off.ctrader_book);
        EXPECT_FALSE(off.ctrader_guard);
    }
    config.ctrader.enabled = true;
    config.ctrader.symbol = "XAUUSD";
    {
        Agent on(config);
        ASSERT_TRUE(on.ctrader_guard);
        EXPECT_EQ(on.ctrader_book->symbol, "XAUUSD");
        EXPECT_DOUBLE_EQ(on.ctrader_guard->window_s, 0.3);
        EXPECT_DOUBLE_EQ(on.ctrader_guard->drawdown, 0.00005);
        EXPECT_DOUBLE_EQ(on.ctrader_guard->pause_s, 0.2);
    }
    config.ctrader.drawdown = 0;  // the feed without the guard
    {
        Agent quiet(config);
        EXPECT_TRUE(quiet.ctrader_book);
        EXPECT_FALSE(quiet.ctrader_guard);
    }
    TempFile f;
    for (auto [text, wants] : std::initializer_list<std::pair<std::string, std::string>> {
             {"[ctrader]\nenabled = true\n", "ctrader.symbol is empty"},
             {"[ctrader]\nenabled = true\nsymbol = \"US500\"\ndrawdown = 0.5\n", "ctrader.drawdown = 0.5 is 5000 bp"},
             {"[ctrader]\nheartbeat_s = 1\n", "ctrader.heartbeat_s must be within 5..120"},
             {"[ctrader]\nwindow_ms = 0\n", "ctrader needs 0 < window_ms"},
             {"[ctrader]\nsymbl = \"US500\"\n", "ctrader.symbl"}}) {
        SCOPED_TRACE(text);
        EXPECT_LOAD_ERROR(f.write(text), {}, wants);
    }
    Config loaded = load_config(f.write("[ctrader]\nenabled = true\nsymbol = \"USTEC\"\ncredentials = \"../creds/ctrader_qqq.toml\"\n"), {});
    EXPECT_TRUE(loaded.ctrader.enabled);
    EXPECT_EQ(loaded.ctrader.symbol, "USTEC");
    EXPECT_EQ(loaded.ctrader.credentials, "../creds/ctrader_qqq.toml");
}

// The stock bots carry the section, each with its broker symbol and a login file of its own (a bot
// with it on and no file does not start): on for QQQ and GLD from 23 Oct.
TEST(CTraderFeed, TheStockConfigsNameTheirBrokerSymbols) {
    for (auto [name, symbol, drawdown] : std::initializer_list<std::tuple<std::string, std::string, double>> {
             {"spy", "US500", 0.00005}, {"qqq", "USTEC", 0.00005}, {"gld", "XAUUSD", 0.000025}, {"nvda", "NVDA", 0.00005}}) {
        Config c = load_config(repo("config." + name + "-both.toml"), {});
        EXPECT_EQ(c.ctrader.enabled, name == "qqq" || name == "gld") << name;  // on from 23 Oct (SPY by --set, NVDA off)
        EXPECT_EQ(c.ctrader.symbol, symbol) << name;
        EXPECT_EQ(c.ctrader.credentials, "../creds/ctrader_" + name + ".toml") << name;
        EXPECT_EQ(c.ctrader.drawdown, drawdown) << name;  // as its [futures] guard
        EXPECT_EQ(c.ctrader.drawdown, c.futures.drawdown) << name;
        EXPECT_EQ(c.ctrader.window_ms, 300.0) << name;
        EXPECT_EQ(c.ctrader.pause_ms, 200.0) << name;
    }
}
