// Local fake exchanges that keep a "true" book and stream changes to it.
//
// Tests point the real feeds at these servers and compare the client's book with
// the server's true book, including runs where the server drops messages.
#pragma once

#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "fake_server.hpp"
#include "orderbook.hpp"
#include "util/aio.hpp"
#include "util/json.hpp"

namespace nmm::test {

using StringLevels = std::vector<std::pair<std::string, std::string>>;
using FloatLevels = std::vector<Level>;

class TrueBook {
public:
    TrueBook(uint32_t seed, double mid, int n_prices);
    // Change a few random levels; return the changes as absolute sizes (bids, asks).
    std::pair<StringLevels, StringLevels> mutate();
    std::pair<StringLevels, StringLevels> levels() const;
    // Best bid price/size and best ask price/size; "0" for an empty side.
    std::tuple<std::string, std::string, std::string, std::string> top() const;
    std::pair<FloatLevels, FloatLevels> as_floats() const;

    std::mt19937 rng;
    std::vector<std::string> bid_prices, ask_prices;
    std::map<std::string, std::string> bids, asks;
};

// [[price, size], ...] as JSON.
Json levels_json(const StringLevels& levels, size_t limit = SIZE_MAX);

struct StreamOptions {
    int clients = 1;  // the producer starts once this many have joined
    std::map<int, double> delays;  // a client (by join order) with a slower route
    std::map<int, std::set<int>> drops;  // frames (by index) a client never gets
};

// One stream produced once and sent to every connected client.
//
// The producer starts when `clients` clients have joined, so they all see the
// stream from its start; later clients join mid-stream. `delays` and `drops` give
// a client (by join order) a slower route or frames it never gets; kick closes a
// client's connection.
class StreamServer : public FakeServer {
public:
    StreamServer(boost::asio::io_context& ioc, StreamOptions options);
    ~StreamServer() override;
    void kick(int index);
    void close() override;

    StreamOptions options;
    std::vector<std::shared_ptr<ServerWs>> joined;
    bool done = false;

protected:
    void join(const std::shared_ptr<ServerWs>& ws);
    // Send `frame` to every live client that does not drop frame `index`.
    void broadcast(const std::string& frame, std::optional<int> index = std::nullopt);
    double max_delay() const;
    virtual awaitable<void> produce() = 0;

private:
    Task producer_;
};

// Combined /stream (diff depth + bookTicker) plus /api/v3/depth.
//
// Each step changes a few levels under one new update id. A bookTicker frame goes
// out at once whenever the top of book changes; a diff depth frame aggregates
// `batch` steps with final absolute sizes, like @depth@100ms. `drop` lists depth
// frame indexes the server applies but does not send (StreamOptions::drops does
// the same for one client only). `tops` and `books` map every update id to the
// true best bid/ask and the true full book (as floats) after it.
struct FakeBinanceParams {
    int n_steps = 400;
    int batch = 5;
    std::set<int> drop;
    uint32_t seed = 1;
    double mid = 100.0;
    double interval = 0.002;
};

class FakeBinance : public StreamServer {
public:
    FakeBinance(boost::asio::io_context& ioc, FakeBinanceParams params = {}, StreamOptions options = {});
    std::string ws_url() const { return "ws://" + base(); }
    std::string rest_url() const { return "http://" + base(); }

    FakeBinanceParams params;
    TrueBook book;
    int64_t update_id = 1'000;
    int snapshot_requests = 0;
    std::vector<std::string> streams;
    std::map<int64_t, std::tuple<double, double, double, double>> tops;
    std::map<int64_t, std::pair<FloatLevels, FloatLevels>> books;

protected:
    awaitable<void> produce() override;
};

// Combined /public/stream of <symbol>@bookTicker like Binance USDⓈ-M futures:
// `n_tickers` tickers a few ticks apart, each with a larger update id `u` and an
// event time `E`; `sent` keeps them in order.
struct FakeBinanceFuturesParams {
    int n_tickers = 200;
    double mid = 100'000.0;
    uint32_t seed = 3;
    double interval = 0.002;
};

class FakeBinanceFutures : public StreamServer {
public:
    FakeBinanceFutures(boost::asio::io_context& ioc, FakeBinanceFuturesParams params = {}, StreamOptions options = {});
    std::string ws_url() const { return "ws://" + base(); }

    FakeBinanceFuturesParams params;
    std::mt19937 rng;
    int64_t update_id = 5'000'000;
    std::vector<Json> sent;
    std::vector<std::string> streams;

protected:
    awaitable<void> produce() override;
};

// Combined /stream of <symbol>@aggTrade like Binance spot: `n_trades` trades a
// few ticks apart, each with a larger aggregate trade id `a` and a trade time
// `T`; `sent` keeps them in order.
struct FakeBinanceTradesParams {
    int n_trades = 200;
    double price = 100'000.0;
    uint32_t seed = 5;
    double interval = 0.002;
};

class FakeBinanceTrades : public StreamServer {
public:
    FakeBinanceTrades(boost::asio::io_context& ioc, FakeBinanceTradesParams params = {}, StreamOptions options = {});
    std::string ws_url() const { return "ws://" + base(); }

    FakeBinanceTradesParams params;
    std::mt19937 rng;
    int64_t trade_id = 9'000'000;
    std::vector<Json> sent;
    std::vector<std::string> streams;

protected:
    awaitable<void> produce() override;
};

// Bybit V5 public /v5/public/<category>: acks subscribe and ping like Bybit, then
// pushes `n_books` orderbook.1 snapshots a few ticks apart, each with a larger
// update id `u` and engine time `cts`. `requests` keeps what the clients sent,
// `sent` the snapshots in order.
struct FakeBybitParams {
    int n_books = 200;
    double mid = 100'000.0;
    uint32_t seed = 4;
    double interval = 0.002;
};

class FakeBybit : public StreamServer {
public:
    FakeBybit(boost::asio::io_context& ioc, FakeBybitParams params = {}, StreamOptions options = {});
    std::string ws_url() const { return "ws://" + base(); }

    FakeBybitParams params;
    std::mt19937 rng;
    int64_t update_id = 7'000'000;
    std::vector<Json> sent;
    std::vector<Json> requests;
    std::vector<std::string> paths;

protected:
    awaitable<void> produce() override;
};

// l2OrderbookUpdates over websockets plus GET /v1/l2OrderBook.
//
// Like the live server: {"type": "connected"} first, deltas only for changes made
// after the subscription (`pre_events` changes happen before it and are never
// sent), deltas at any depth, no timestamp on deltas. The REST snapshot is the
// true book `rest_lag` sequences behind the head, labeled with that sequence;
// `rest_lags` overrides the lag per request. `drop` lists delta indexes the server
// applies but does not send (StreamOptions::drops does the same for one client only).
struct FakeArcusParams {
    int n_events = 300;
    int pre_events = 20;
    std::set<int> drop;
    uint32_t seed = 2;
    double mid = 100.05;
    int rest_lag = 3;
    std::vector<int> rest_lags;
    double interval = 0.002;
    std::string market = "BTC-USD";
};

class FakeArcus : public StreamServer {
public:
    FakeArcus(boost::asio::io_context& ioc, FakeArcusParams params = {}, StreamOptions options = {});
    std::string ws_url() const { return "ws://" + base() + "/v1/ws"; }
    std::string rest_url() const { return "http://" + base(); }

    FakeArcusParams params;
    TrueBook book;
    int64_t seq = 5'000;
    std::map<int64_t, std::pair<StringLevels, StringLevels>> history;
    std::vector<Json> received;
    int snapshot_requests = 0;

protected:
    awaitable<void> produce() override;

private:
    std::pair<StringLevels, StringLevels> step();
    std::string frame(const std::string& kind, const Json& contents) const;
};

}  // namespace nmm::test
