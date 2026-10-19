#include "fakes.hpp"

#include <algorithm>
#include <format>

#include "util/clock.hpp"

namespace nmm::test {

namespace {

double uniform(std::mt19937& rng, double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }
int randint(std::mt19937& rng, int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); }
double random01(std::mt19937& rng) { return uniform(rng, 0.0, 1.0); }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return out;
}

int64_t now_ms() { return time_ns() / 1'000'000; }

}  // namespace

// ------------------------------------------------------------------- TrueBook

TrueBook::TrueBook(uint32_t seed, double mid, int n_prices) : rng(seed) {
    for (int i = 0; i < n_prices; ++i) {
        bid_prices.push_back(std::format("{:.2f}", mid - 0.01 * (i + 1)));
        ask_prices.push_back(std::format("{:.2f}", mid + 0.01 * i));
    }
    for (auto& p : bid_prices) bids[p] = "1.0";
    for (auto& p : ask_prices) asks[p] = "1.0";
}

std::pair<StringLevels, StringLevels> TrueBook::mutate() {
    std::pair<StringLevels, StringLevels> changes;
    int n = randint(rng, 1, 4);
    for (int k = 0; k < n; ++k) {
        bool is_bid = random01(rng) < 0.5;
        auto& side = is_bid ? bids : asks;
        auto& prices = is_bid ? bid_prices : ask_prices;
        const std::string& price = prices[static_cast<size_t>(randint(rng, 0, static_cast<int>(prices.size()) - 1))];
        std::string size = random01(rng) < 0.3 ? "0" : std::format("{:.3f}", uniform(rng, 0.001, 5));
        if (size == "0") side.erase(price);
        else side[price] = size;
        (is_bid ? changes.first : changes.second).emplace_back(price, size);
    }
    return changes;
}

std::pair<StringLevels, StringLevels> TrueBook::levels() const {
    StringLevels b(bids.begin(), bids.end()), a(asks.begin(), asks.end());
    std::sort(b.begin(), b.end(), [](const auto& x, const auto& y) { return std::stod(x.first) > std::stod(y.first); });
    std::sort(a.begin(), a.end(), [](const auto& x, const auto& y) { return std::stod(x.first) < std::stod(y.first); });
    return {b, a};
}

std::tuple<std::string, std::string, std::string, std::string> TrueBook::top() const {
    auto [b, a] = levels();
    std::pair<std::string, std::string> bid = b.empty() ? std::pair<std::string, std::string> {"0", "0"} : b[0];
    std::pair<std::string, std::string> ask = a.empty() ? std::pair<std::string, std::string> {"0", "0"} : a[0];
    return {bid.first, bid.second, ask.first, ask.second};
}

std::pair<FloatLevels, FloatLevels> TrueBook::as_floats() const {
    auto [b, a] = levels();
    FloatLevels fb, fa;
    for (auto& [p, s] : b) fb.emplace_back(std::stod(p), std::stod(s));
    for (auto& [p, s] : a) fa.emplace_back(std::stod(p), std::stod(s));
    return {fb, fa};
}

Json levels_json(const StringLevels& levels, size_t limit) {
    JsonArray out;
    for (size_t i = 0; i < levels.size() && i < limit; ++i) out.push_back(JsonArray {Json(levels[i].first), Json(levels[i].second)});
    return out;
}

// --------------------------------------------------------------- StreamServer

StreamServer::StreamServer(boost::asio::io_context& ioc, StreamOptions options) : FakeServer(ioc), options(std::move(options)) {}

StreamServer::~StreamServer() { producer_.cancel(); }

void StreamServer::join(const std::shared_ptr<ServerWs>& ws) {
    ws->index = static_cast<int>(joined.size());
    if (auto it = options.delays.find(ws->index); it != options.delays.end()) ws->delay = it->second;
    joined.push_back(ws);
    if (!producer_ && static_cast<int>(joined.size()) >= options.clients) producer_ = Task::spawn(produce(), "fake producer", "test");
}

void StreamServer::broadcast(const std::string& frame, std::optional<int> index) {
    for (auto& client : joined) {
        if (client->closed()) continue;
        if (index) {
            auto drops = options.drops.find(client->index);
            if (drops != options.drops.end() && drops->second.contains(*index)) continue;
        }
        client->send(frame);
    }
}

void StreamServer::kick(int index) { joined.at(static_cast<size_t>(index))->close(); }

void StreamServer::close() {
    producer_.cancel();
    FakeServer::close();
}

double StreamServer::max_delay() const {
    double out = 0.0;
    for (auto& [index, delay] : options.delays) out = std::max(out, delay);
    return out;
}

// ---------------------------------------------------------------- FakeBinance

FakeBinance::FakeBinance(boost::asio::io_context& ioc, FakeBinanceParams params, StreamOptions options)
    : StreamServer(ioc, std::move(options)), params(std::move(params)), book(this->params.seed, this->params.mid, 30) {
    on_ws("/stream", [this](std::shared_ptr<ServerWs> ws) {
        streams = split(ws->request.query["streams"], '/');
        join(ws);  // held open until the client leaves
    });
    on_get("/api/v3/depth", [this](const HttpRequest& request) {
        ++snapshot_requests;
        auto [b, a] = book.levels();
        size_t limit = std::stoul(request.query.at("limit"));
        JsonObject body {{"lastUpdateId", update_id}, {"bids", levels_json(b, limit)}, {"asks", levels_json(a, limit)}};
        return HttpReply(200, dump_json(body));
    });
}

awaitable<void> FakeBinance::produce() {
    std::string depth_stream = streams.at(0);
    std::optional<std::string> ticker_stream;
    for (auto& s : streams)
        if (s.size() >= 11 && s.ends_with("@bookTicker")) ticker_stream = s;
    std::map<std::string, std::string> changed_bids, changed_asks;
    int64_t first_id = update_id + 1;
    auto frame = [](const std::string& stream, const Json& data) { return dump_json(JsonObject {{"stream", stream}, {"data", data}}); };
    for (int step = 0; step < params.n_steps; ++step) {
        auto before = book.top();
        ++update_id;
        auto [b, a] = book.mutate();
        for (auto& [p, s] : b) changed_bids[p] = s;
        for (auto& [p, s] : a) changed_asks[p] = s;
        auto top = book.top();
        tops[update_id] = {std::stod(std::get<0>(top)), std::stod(std::get<1>(top)), std::stod(std::get<2>(top)), std::stod(std::get<3>(top))};
        books[update_id] = book.as_floats();
        if (ticker_stream && top != before) {
            auto& [bid, bid_size, ask, ask_size] = top;
            JsonObject ticker {{"u", update_id}, {"s", "BTCUSDT"}, {"b", bid}, {"B", bid_size}, {"a", ask}, {"A", ask_size}};
            broadcast(frame(*ticker_stream, ticker));
        }
        if ((step + 1) % params.batch == 0 || step == params.n_steps - 1) {
            int index = step / params.batch;
            if (!params.drop.contains(index)) {
                StringLevels cb(changed_bids.begin(), changed_bids.end()), ca(changed_asks.begin(), changed_asks.end());
                JsonObject event {{"e", "depthUpdate"}, {"E", int64_t {1'700'000'000'000} + step}, {"s", "BTCUSDT"},
                                  {"U", first_id}, {"u", update_id}, {"b", levels_json(cb)}, {"a", levels_json(ca)}};
                broadcast(frame(depth_stream, event), index);
            }
            changed_bids.clear();
            changed_asks.clear();
            first_id = update_id + 1;
        }
        co_await sleep(params.interval);
    }
    co_await sleep(max_delay());
    done = true;
}

// --------------------------------------------------------- FakeBinanceFutures

FakeBinanceFutures::FakeBinanceFutures(boost::asio::io_context& ioc, FakeBinanceFuturesParams params, StreamOptions options)
    : StreamServer(ioc, std::move(options)), params(params), rng(params.seed) {
    on_ws("/public/stream", [this](std::shared_ptr<ServerWs> ws) {  // book streams live under /public since 2026
        streams = split(ws->request.query["streams"], '/');
        join(ws);
    });
}

awaitable<void> FakeBinanceFutures::produce() {
    double mid = params.mid;
    static const double steps[] = {-0.1, 0.0, 0.1};
    for (int i = 0; i < params.n_tickers; ++i) {
        mid += steps[randint(rng, 0, 2)];
        update_id += randint(rng, 1, 3);
        int64_t t = now_ms();  // stamped as it goes out, like the real stream
        JsonObject ticker {{"e", "bookTicker"}, {"u", update_id}, {"s", "BTCUSDT"}, {"b", std::format("{:.2f}", mid - 0.05)},
                           {"B", "1.5"}, {"a", std::format("{:.2f}", mid + 0.05)}, {"A", "2.0"}, {"T", t}, {"E", t}};
        sent.push_back(ticker);
        broadcast(dump_json(JsonObject {{"stream", streams.at(0)}, {"data", ticker}}));
        co_await sleep(params.interval);
    }
    co_await sleep(max_delay());
    done = true;
}

// ---------------------------------------------------------- FakeBinanceTrades

FakeBinanceTrades::FakeBinanceTrades(boost::asio::io_context& ioc, FakeBinanceTradesParams params, StreamOptions options)
    : StreamServer(ioc, std::move(options)), params(params), rng(params.seed) {
    on_ws("/stream", [this](std::shared_ptr<ServerWs> ws) {
        streams = split(ws->request.query["streams"], '/');
        join(ws);
    });
}

awaitable<void> FakeBinanceTrades::produce() {
    double price = params.price;
    static const double steps[] = {-0.01, 0.0, 0.01};
    for (int i = 0; i < params.n_trades; ++i) {
        price += steps[randint(rng, 0, 2)];
        trade_id += randint(rng, 1, 3);
        int64_t t = now_ms();
        JsonObject trade {{"e", "aggTrade"}, {"E", t}, {"s", "BTCUSDT"}, {"a", trade_id}, {"p", std::format("{:.2f}", price)},
                          {"q", "0.01"}, {"f", trade_id * 2}, {"l", trade_id * 2}, {"T", t}, {"m", random01(rng) < 0.5},
                          {"M", true}};
        sent.push_back(trade);
        broadcast(dump_json(JsonObject {{"stream", streams.at(0)}, {"data", trade}}));
        co_await sleep(params.interval);
    }
    co_await sleep(max_delay());
    done = true;
}

// ------------------------------------------------------------------ FakeBybit

FakeBybit::FakeBybit(boost::asio::io_context& ioc, FakeBybitParams params, StreamOptions options)
    : StreamServer(ioc, std::move(options)), params(params), rng(params.seed) {
    on_ws("/v5/public/*", [this](std::shared_ptr<ServerWs> ws) {
        paths.push_back(ws->request.path);
        join(ws);
        std::weak_ptr<ServerWs> weak = ws;
        ws->on_message = [this, weak](const std::string& text) {
            auto conn = weak.lock();
            if (!conn) return;
            Json req = parse_json(text);
            requests.push_back(req);
            auto op = jstr(req, "op");
            if (op && *op == "subscribe") {
                conn->send_now(R"({"success": true, "ret_msg": "", "conn_id": "c1", "req_id": "", "op": "subscribe"})");
            } else if (op && *op == "ping") {
                conn->send_now(R"({"success": true, "ret_msg": "pong", "conn_id": "c1", "req_id": "", "op": "ping"})");
            }
        };
    });
}

awaitable<void> FakeBybit::produce() {
    double mid = params.mid;
    static const double steps[] = {-0.1, 0.0, 0.1};
    for (int i = 0; i < params.n_books; ++i) {
        mid += steps[randint(rng, 0, 2)];
        update_id += randint(rng, 1, 3);
        int64_t t = now_ms();  // stamped as it goes out, like the real stream
        JsonObject data {{"s", "BTCUSDT"},
                         {"b", JsonArray {JsonArray {Json(std::format("{:.2f}", mid - 0.05)), Json("1.5")}}},
                         {"a", JsonArray {JsonArray {Json(std::format("{:.2f}", mid + 0.05)), Json("2.0")}}},
                         {"u", update_id},
                         {"seq", int64_t {9'000'000} + i}};
        JsonObject frame {{"topic", "orderbook.1.BTCUSDT"}, {"ts", t}, {"type", "snapshot"}, {"data", data}, {"cts", t}};
        sent.push_back(frame);
        broadcast(dump_json(frame));
        co_await sleep(params.interval);
    }
    co_await sleep(max_delay());
    done = true;
}

// ------------------------------------------------------------------ FakeArcus

FakeArcus::FakeArcus(boost::asio::io_context& ioc, FakeArcusParams params, StreamOptions options)
    : StreamServer(ioc, std::move(options)), params(std::move(params)), book(this->params.seed, this->params.mid, 10) {
    history[seq] = book.levels();
    on_ws("/v1/ws", [this](std::shared_ptr<ServerWs> ws) {
        ws->send_now(dump_json(JsonObject {{"type", "connected"}, {"connection_id", std::to_string(joined.size() + 1)}}));
        std::weak_ptr<ServerWs> weak = ws;
        ws->on_message = [this, weak](const std::string& text) {
            auto conn = weak.lock();
            if (!conn) return;
            Json payload = parse_json(text);
            received.push_back(payload);
            auto type = jstr(payload, "type");
            if (type && *type == "subscribe") {
                conn->send_now(frame("subscribed", JsonObject {}));
                join(conn);
            }
        };
    });
    on_get("/v1/l2OrderBook/*", [this](const HttpRequest& request) {
        ++snapshot_requests;
        int lag = this->params.rest_lag;
        if (!this->params.rest_lags.empty()) {
            lag = this->params.rest_lags.front();
            this->params.rest_lags.erase(this->params.rest_lags.begin());
        }
        int64_t at = std::max(history.begin()->first, seq - lag);
        auto& [b, a] = history.at(at);
        size_t n = request.query.contains("nLevels") ? std::stoul(request.query.at("nLevels")) : 20;
        JsonObject body {{"bids", levels_json(b, n)}, {"asks", levels_json(a, n)}, {"lastSequenceId", at},
                         {"globalSequenceId", at * 7}, {"timestamp", int64_t {1'780'000'000'000'000}}};
        return HttpReply(200, dump_json(body));
    });
}

std::pair<StringLevels, StringLevels> FakeArcus::step() {
    ++seq;
    auto changes = book.mutate();
    history[seq] = book.levels();
    return changes;
}

std::string FakeArcus::frame(const std::string& kind, const Json& contents) const {
    return dump_json(JsonObject {{"type", kind}, {"channel", "l2OrderbookUpdates"}, {"id", params.market}, {"contents", contents}});
}

awaitable<void> FakeArcus::produce() {
    for (int i = 0; i < params.pre_events; ++i) step();  // the market moves before we join
    for (int i = 0; i < params.n_events; ++i) {
        auto [b, a] = step();
        if (!params.drop.contains(i)) {
            JsonObject delta {{"bids", levels_json(b)}, {"asks", levels_json(a)}, {"lastSequenceId", seq}, {"globalSequenceId", seq * 7}};
            broadcast(frame("channel_data", delta), i);
        }
        co_await sleep(params.interval);
    }
    co_await sleep(max_delay());
    done = true;
}

}  // namespace nmm::test
