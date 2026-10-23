#include "trading/paper.hpp"

#include <algorithm>
#include <format>

#include "net/net.hpp"

namespace nmm {

PaperGateway::PaperGateway(MarketSpec market_, std::string ws_url_, std::function<void()> on_change_, int connections,
                           double slow_link_ms_)
    : Gateway(std::move(market_), std::move(on_change_)), ws_url(std::move(ws_url_)), slow_link_ms(slow_link_ms_) {
    name = "paper";
    orders_in_public_book = false;  // paper orders never reach the Arcus book
    log = &get_logger("naive_mm." + name);
    links.resize(static_cast<size_t>(std::max(connections, 1)));
    for (size_t i = 0; i < links.size(); ++i) links[i].index = static_cast<int>(i);
}

awaitable<void> PaperGateway::run() {
    std::vector<Task> tasks;
    for (auto& link : links) tasks.push_back(Task::spawn(run_link(link), "paper-trades", log->name()));
    if (links.size() > 1 && slow_link_ms) tasks.push_back(Task::spawn(watch_links(), "paper-slow-watch", log->name()));
    try {
        Event forever;
        co_await forever.wait();
    } catch (...) {
        for (auto& task : tasks) task.cancel();
        trades_lost("shutting down");
        throw;
    }
}

awaitable<void> PaperGateway::run_link(Link& link) {
    std::string tag = links.size() > 1 ? std::format(" [link {}/{}]", link.index + 1, links.size()) : "";
    double backoff = 0.5;
    for (;;) {
        std::exception_ptr error;
        bool closed = false;
        int close_code = 0;
        try {
            auto ws = co_await WsConnection::connect(ws_url, 20.0);
            link.ws = ws;
            link.lag.connected();
            ws->send(dump_json(JsonObject {{"type", "subscribe"}, {"channel", "trades"}, {"id", market.symbol}}));
            log->info("paper trading on {} trades{}", market.symbol, tag);
            ready = true;
            on_change();
            FrameParser parser;
            for (;;) {
                auto text = co_await ws->read();
                if (!text) break;
                on_message(parser.parse(*text), &link);
            }
            closed = true;
            close_code = ws->close_code();
        } catch (...) {
            error = std::current_exception();
        }
        bool cancelled = false;
        if (error) cancelled = co_await is_cancelled();
        if (closed) {
            if (link.slow_drop) log->info("closed a slow trades connection{}", tag);
            else log->warning("trades connection closed by server (code={}){}", close_code ? std::to_string(close_code) : "None", tag);
            backoff = 0.5;
        } else if (!cancelled) {
            log->warning("trades connection error{}: {}", tag, what(error));  // a missed heartbeat pong among them
        }
        if (link.ws) link.ws->close();
        link.ws.reset();
        link.slow_drop = false;
        if (cancelled) std::rethrow_exception(error);  // run() cancels the orders
        if (std::none_of(links.begin(), links.end(), [](const Link& other) { return other.up(); })) trades_lost("trades feed lost");
        co_await sleep(backoff);
        backoff = std::min(backoff * 2, 30.0);
    }
}

void PaperGateway::trades_lost(const std::string& reason) {
    ready = false;
    for (const auto& o : working()) finish(o, OrderState::CANCELED, reason);
    on_change();
}

Link* PaperGateway::check_links(std::optional<double> now) {
    auto slow = slow_link(links, slow_link_ms, now);
    if (!slow) return nullptr;
    auto [link, median, n] = *slow;
    events->warning("!!! trades link {}/{} slow: its copies {:.0f} ms behind the first at the median over the last {:.0f} s (n {}, over {:g} ms): reconnecting",
                    link->index + 1, links.size(), median, SLOW_LINK_WINDOW_S, n, slow_link_ms);
    link->lag.dropped_for_slowness(now);
    link->slow_drop = true;
    link->ws->close();
    return link;
}

awaitable<void> PaperGateway::watch_links() {
    for (;;) {
        co_await sleep(SLOW_LINK_CHECK_S);
        check_links();
    }
}

std::string PaperGateway::status() const {
    std::string text = Gateway::status();
    if (links.size() > 1) {
        int up = 0;
        int64_t drops = 0;
        std::vector<const LinkLag*> lags;
        for (const auto& link : links) {
            up += link.up();
            drops += link.lag.slow_drops;
            lags.push_back(&link.lag);
        }
        text += std::format("; trades links up {}/{} {}", up, links.size(), lag_summary(lags));
        if (drops) text += std::format(" slow-reconnects {}", drops);
    }
    return text;
}

void PaperGateway::send_place(const OrderPtr& o) {
    o->order_id = "paper-" + o->client_id;
    placed_us_[o->client_id] = now_us();
    set_state(*o, OrderState::OPEN);
    call_soon([this] { on_change(); });
}

void PaperGateway::send_cancel(const OrderPtr& o, const std::string& why) {
    events->info(">>> CANCEL {}{}", o->str(), why.empty() ? "" : ": " + why);
    call_later(CANCEL_LATENCY_S, [this, o] { cancel_acts(o); });
}

void PaperGateway::finish(const OrderPtr& order, OrderState state, std::optional<std::string> reason, bool fresh) {
    const OrderPtr o = order;  // a copy: finishing may destroy what `order` refers to
    Gateway::finish(o, state, std::move(reason), fresh);
    if (terminal(o->state)) {  // no trade can fill it any more
        placed_us_.erase(o->client_id);
        ahead_.erase(o->client_id);
    }
}

void PaperGateway::cancel_acts(const OrderPtr& o) {
    if (o->state == OrderState::PENDING_CANCEL) {  // not filled meanwhile
        finish(o, OrderState::CANCELED);
        on_change();
    }
}

void PaperGateway::on_message(const Json& msg, Link* link) {
    if (jstr(msg, "channel") != "trades" || jstr(msg, "type") != "channel_data") {
        if (jstr(msg, "type") == "error") log->warning("trades channel error: {}", dump_json(msg));
        return;
    }
    const Json* contents = jget(msg, "contents");
    if (contents == nullptr || !contents->is_array()) return;
    if (link != nullptr && links.size() > 1 && !contents->get_array().empty()) {
        auto [first, behind] = copies_.arrived(trade_frame_key(contents->get_array()), now_us());
        link->lag.add(behind);
        if (!first) return;  // another connection delivered it first
    }
    for (const auto& trade : contents->get_array()) {
        for (const auto& o : working()) {  // a trade can fill only one side: below the buy or above the sell
            if (!o->resting() || printed_before(trade, *o)) continue;
            Decimal price = Decimal::parse(to_str(jget(trade, "price")));
            bool crossed = o->side == "BUY" ? price <= o->price : price >= o->price;
            if (!crossed) continue;
            Decimal traded = Decimal::parse(to_str(jget(trade, "size")));
            if (book != nullptr && price == o->price) traded = after_queue(*o, traded);
            if (traded <= 0) continue;
            Decimal size = min(traded, o->remaining());
            fill(o, o->filled + size, fmt(o->price));  // a maker fills at its own price
            if (o->remaining() <= 0) finish(o, OrderState::FILLED);
            on_change();
        }
    }
    if (on_trades && !contents->get_array().empty()) on_trades(contents->get_array());
}

Decimal PaperGateway::after_queue(const Order& o, const Decimal& size) {
    double p = o.price.to_double();
    auto [it, fresh] = ahead_.try_emplace(o.client_id, o.book_base.value_or(0.0));  // what stood there as it went out
    double& ahead = it->second;
    const std::vector<Level> levels = o.side == "BUY" ? book->bids(50) : book->asks(50);
    auto shown = std::find_if(levels.begin(), levels.end(), [&](const Level& l) { return l.first == p; });
    bool covered = !levels.empty() && (o.side == "BUY" ? levels.back().first <= p : levels.back().first >= p);
    if (shown != levels.end()) ahead = std::min(ahead, shown->second);  // those ahead that cancelled
    else if (covered) ahead = 0.0;  // the level is gone: nobody left ahead
    double s = size.to_double();
    double left = s - ahead;
    ahead = std::max(ahead - s, 0.0);
    return left > 0 ? Decimal::from_double(left) : Decimal(0);
}

bool PaperGateway::printed_before(const Json& trade, const Order& o) const {
    auto placed = placed_us_.find(o.client_id);
    const Json* ts = jget(trade, "timestamp");
    if (placed == placed_us_.end() || ts == nullptr || !is_number(*ts)) return false;
    double offset_us = clock_offset_ms.value_or(0.0) * 1000;  // host minus exchange
    return to_float(*ts) + offset_us < static_cast<double>(placed->second);
}

}  // namespace nmm
