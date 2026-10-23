// cTrader FIX API price (QUOTE) session: one broker symbol's best bid/ask, one more
// input of the price guard (as the OKX swap's). The collector's books/feeds/ctrader.py,
// cut to what a guard needs.
//
// The broker's cTrader (IC Markets and others) serves FIX 4.4 on two sessions; this
// is the price one (SenderSubID/TargetSubID QUOTE, port 5211 over TLS, 5201 plain).
// The settings come from cTrader (Settings, FIX API) and sit in a TOML file only
// its owner can read, the collector's configs/ctrader_fix.toml format. The broker
// gives a login ONE price session: a second logon with the same login drops the
// first, so each bot (and the collector) needs a login of its own.
//
// After the logon a SecurityListRequest maps the symbol's name (US500, XAUUSD) to
// the numeric id FIX uses, then a MarketDataRequest asks for the spot price
// (MarketDepth 1): a snapshot (W) on every change; incremental updates (X) are
// applied too, should the broker send them. Every message's MsgSeqNum (34) is
// checked: a gap means a lost message, and the session is dropped and built again
// (the book invalid meanwhile), as is one silent past two heartbeats (a
// TestRequest goes out after one). The book's `seq` is the MsgSeqNum of the
// message that changed it, its exchange time the SendingTime (52, ms).
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>

#include "feeds/base.hpp"
#include "feeds/fix.hpp"

namespace nmm {

inline constexpr const char* CTRADER_CREDENTIALS_DEFAULT_PATH = "~/.config/naive_mm/ctrader_fix.toml";
inline constexpr double CTRADER_CONNECT_TIMEOUT_S = 10.0;
inline constexpr double CTRADER_LOGON_TIMEOUT_S = 15.0;

struct CTraderCredentials {
    std::string host;
    int port = 5211;
    std::string sender_comp_id;  // demo.icmarkets.1234567
    std::string password;
    std::string target_comp_id = "cServer";
    std::string sender_sub_id = "QUOTE";
    std::string target_sub_id = "QUOTE";
    std::string username;  // the trader login; by default the last part of SenderCompID
    bool ssl = true;

    std::string login() const;
    // "demo.icmarkets.1234567 at demo.cfixapi.com:5211 (TLS)": never the password.
    std::string str() const;
};

// The settings from a TOML file only its owner can read (chmod 600); throws std::invalid_argument.
CTraderCredentials load_ctrader_credentials(const std::string& path);

class CTraderSessionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CTraderFeed : public BookFeed {
public:
    CTraderFeed(OrderBook& book, std::string symbol, CTraderCredentials creds, FeedOptions options = {}, double heartbeat_s = 30.0);
    ~CTraderFeed() override;

    // One session at a time, built again after each loss (with backoff).
    awaitable<void> run() override;
    void on_message(const Json& msg, int64_t recv_ts_us, Link* link = nullptr) override {}  // no websocket here

    // ---- the session, without the socket (the tests drive these)
    void start_session();  // a new connection: the state reset, the Logon sent
    // One message from the broker; throws CTraderSessionError when the session must be built again.
    void handle(const fix::Message& msg);
    // Once a second: a Heartbeat after `heartbeat_s` of sending nothing, a TestRequest after as much
    // silence, the session dropped (the reason returned) when that goes unanswered or the logon never came.
    std::optional<std::string> tick(double now);
    void lost();  // the session ended: the book invalid

    std::string symbol;  // the broker's name, as configured
    CTraderCredentials creds;
    double heartbeat_s;
    std::function<void(std::string)> sink;  // where encoded messages go (the connection)
    std::optional<std::string> symbol_id;  // the FIX id of the symbol, once listed
    std::string symbol_name;  // the broker's name it resolved to
    std::string refused;  // why the broker refused the subscription (or the symbol is unknown)
    bool logged_on = false;
    int64_t sessions = 0;

private:
    void send(const std::string& msg_type, fix::Fields body);
    void security_list(const fix::Message& msg);
    void resolve_and_subscribe();
    void snapshot(const fix::Message& msg, int64_t seq);
    void incremental(const fix::Message& msg, int64_t seq);
    void publish(int64_t seq, const fix::Message& msg);
    awaitable<void> watch(std::shared_ptr<StreamConnection> conn);

    int64_t out_seq_ = 0;
    int64_t in_seq_ = 0;
    double started_ = 0.0;
    double last_sent_ = 0.0;
    double last_received_ = 0.0;
    std::optional<double> test_sent_;
    std::map<std::string, std::string> listed_;  // name -> FIX id, as the list's fragments come
    std::map<std::string, std::tuple<char, double, double>> entries_;  // MDEntryID -> (side '0'/'1', price, size)
    std::string dropped_;  // why the watchdog closed the connection
};

}  // namespace nmm
