#include "trading/paper.hpp"

#include "net/net.hpp"

namespace nmm {

PaperGateway::PaperGateway(MarketSpec market_, std::string ws_url_, std::function<void()> on_change_)
    : Gateway(std::move(market_), std::move(on_change_)), ws_url(std::move(ws_url_)) {
    name = "paper";
    orders_in_public_book = false;  // paper orders never reach the Arcus book
    log = &get_logger("naive_mm." + name);
}

awaitable<void> PaperGateway::run() {
    trades = std::make_unique<ArcusTradesFeed>(
        market.symbol, ws_url, [this](const Json& msg) { on_message(msg); }, [this] { trades_up(); },
        [this] { trades_down("trades feed lost"); }, trades_options);
    std::exception_ptr error;
    try {
        co_await trades->run();
    } catch (...) {
        error = std::current_exception();
    }
    trades_down("shutting down");  // the links wind down after this: nothing is left for them to cancel
    trades->on_down = nullptr;
    std::rethrow_exception(error);
}

void PaperGateway::trades_up() {
    log->info("paper trading on {} trades", market.symbol);
    ready = true;
    on_change();
}

void PaperGateway::trades_down(const std::string& reason) {
    // Without trades we cannot tell whether the orders would have filled.
    if (!ready) return;
    ready = false;
    if (reason != "shutting down") log->warning("trades feed lost: every connection is down");
    for (const auto& o : working()) finish(o, OrderState::CANCELED, reason);
    on_change();
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
    if (terminal(o->state)) placed_us_.erase(o->client_id);  // no trade can fill it any more
}

void PaperGateway::cancel_acts(const OrderPtr& o) {
    if (o->state == OrderState::PENDING_CANCEL) {  // not filled meanwhile
        finish(o, OrderState::CANCELED);
        on_change();
    }
}

void PaperGateway::on_message(const Json& msg) {
    if (jstr(msg, "channel") != "trades" || jstr(msg, "type") != "channel_data") return;
    const Json* contents = jget(msg, "contents");
    if (contents == nullptr || !contents->is_array()) return;
    for (const auto& trade : contents->get_array()) {
        for (const auto& o : working()) {  // a trade can fill only one side: below the buy or above the sell
            if (!o->resting() || printed_before(trade, *o)) continue;
            Decimal price = Decimal::parse(to_str(jget(trade, "price")));
            bool crossed = o->side == "BUY" ? price <= o->price : price >= o->price;
            if (!crossed) continue;
            Decimal size = min(Decimal::parse(to_str(jget(trade, "size"))), o->remaining());
            fill(o, o->filled + size, fmt(o->price));  // a maker fills at its own price
            if (o->remaining() <= 0) finish(o, OrderState::FILLED);
            on_change();
        }
    }
    if (on_trades && !contents->get_array().empty()) on_trades(contents->get_array());
}

bool PaperGateway::printed_before(const Json& trade, const Order& o) const {
    auto placed = placed_us_.find(o.client_id);
    const Json* ts = jget(trade, "timestamp");
    if (placed == placed_us_.end() || ts == nullptr || !is_number(*ts)) return false;
    double offset_us = clock_offset_ms.value_or(0.0) * 1000;  // host minus exchange
    return to_float(*ts) + offset_us < static_cast<double>(placed->second);
}

}  // namespace nmm
