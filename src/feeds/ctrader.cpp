#include "feeds/ctrader.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <boost/asio/this_coro.hpp>
#include <cmath>
#include <format>
#include <limits>
#include <set>

#include "portfolio.hpp"
#include "tomlplusplus/toml.hpp"
#include "util/clock.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

namespace {

const std::set<std::string> CREDENTIAL_KEYS {"host", "port", "sender_comp_id", "password", "target_comp_id",
                                             "sender_sub_id", "target_sub_id", "username", "ssl"};

std::optional<double> number(const std::string* text) {
    if (text == nullptr || text->empty()) return std::nullopt;
    try {
        size_t used = 0;
        double x = std::stod(*text, &used);
        if (used != text->size() || !std::isfinite(x)) return std::nullopt;
        return x;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

const std::string* field(const std::map<int, std::string>& group, int tag) {
    auto it = group.find(tag);
    return it == group.end() ? nullptr : &it->second;
}

std::string describe(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown error";
    }
}

bool alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

}  // namespace

std::string CTraderCredentials::login() const {
    if (!username.empty()) return username;
    auto dot = sender_comp_id.rfind('.');
    return dot == std::string::npos ? sender_comp_id : sender_comp_id.substr(dot + 1);
}

std::string CTraderCredentials::str() const { return std::format("{} at {}:{}{}", sender_comp_id, host, port, ssl ? " (TLS)" : ""); }

CTraderCredentials load_ctrader_credentials(const std::string& raw_path) {
    std::string path = expand_path(raw_path);
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        throw std::invalid_argument(std::format("no cTrader FIX settings at {}: the collector's configs/ctrader_fix.example.toml "
                                                "with this bot's own login and its password, chmod 600", path));
    if (st.st_mode & 077) throw std::invalid_argument(std::format("{} is readable by group or others; run: chmod 600 {}", path, path));
    toml::table data;
    try {
        data = toml::parse_file(path);
    } catch (const toml::parse_error& err) {
        throw std::invalid_argument(std::format("{}: not valid TOML: {}", path, err.description()));
    }
    std::vector<std::string> unknown, missing;
    for (const auto& [key, value] : data)
        if (!CREDENTIAL_KEYS.contains(std::string(key.str()))) unknown.emplace_back(key.str());
    if (!unknown.empty())
        throw std::invalid_argument(std::format("{}: unknown keys {} (known: {})", path, join(unknown, ", "),
                                                join(std::vector<std::string>(CREDENTIAL_KEYS.begin(), CREDENTIAL_KEYS.end()), ", ")));
    CTraderCredentials c;
    auto text = [&](const char* key, std::string& out) {
        auto node = data[key];
        if (!node) return;
        if (!node.is_string()) throw std::invalid_argument(std::format("{}: {} must be a string", path, key));
        out = *node.value<std::string>();
    };
    text("host", c.host);
    text("sender_comp_id", c.sender_comp_id);
    text("password", c.password);
    text("target_comp_id", c.target_comp_id);
    text("sender_sub_id", c.sender_sub_id);
    text("target_sub_id", c.target_sub_id);
    text("username", c.username);
    if (auto node = data["port"]; node) {
        if (!node.is_integer() || *node.value<int64_t>() < 1 || *node.value<int64_t>() > 65535)
            throw std::invalid_argument(std::format("{}: port must be a port number (5211: TLS, 5201: plain)", path));
        c.port = static_cast<int>(*node.value<int64_t>());
    } else {
        missing.push_back("port");
    }
    if (auto node = data["ssl"]; node) {
        if (!node.is_boolean()) throw std::invalid_argument(std::format("{}: ssl must be true or false", path));
        c.ssl = *node.value<bool>();
    }
    for (auto [key, value] : {std::pair<const char*, std::string*> {"host", &c.host}, {"sender_comp_id", &c.sender_comp_id},
                              {"password", &c.password}})
        if (value->empty()) missing.insert(missing.begin(), key);
    if (!missing.empty()) throw std::invalid_argument(std::format("{}: missing {}", path, join(missing, ", ")));
    if (starts_with(c.password, "PASSWORD_OF_ACCOUNT")) throw std::invalid_argument(path + ": the password is still the template's placeholder");
    return c;
}

CTraderFeed::CTraderFeed(OrderBook& book, std::string symbol, CTraderCredentials creds, FeedOptions options, double heartbeat_s)
    : BookFeed("ctrader", &book, std::format("fix://{}:{}", creds.host, creds.port), std::move(options)),
      symbol(std::move(symbol)),
      creds(std::move(creds)),
      heartbeat_s(heartbeat_s) {}

CTraderFeed::~CTraderFeed() = default;

awaitable<void> CTraderFeed::run() {
    double backoff = options.reconnect_min;
    for (;;) {
        bool received = false, closed_by_server = false;
        std::exception_ptr error;
        std::optional<Task> watchdog;
        std::shared_ptr<StreamConnection> conn;
        dropped_.clear();
        try {
            conn = co_await StreamConnection::connect(creds.host, std::to_string(creds.port), creds.ssl, CTRADER_CONNECT_TIMEOUT_S);
            ++connect_count;
            log.info("connected to {}", creds.str());
            sink = [conn](std::string data) { conn->write(std::move(data)); };
            start_session();
            watchdog = Task::spawn(watch(conn), name + "-watch", "naive_mm." + name);
            fix::Parser parser;
            for (;;) {
                auto data = co_await conn->read();
                if (!data) break;
                last_received_ = monotonic();
                test_sent_.reset();
                for (const auto& msg : parser.feed(*data)) {
                    ++message_count;
                    handle(msg);
                }
                if (logged_on) received = true;
            }
            closed_by_server = true;
        } catch (...) {
            error = std::current_exception();
        }
        if (watchdog) watchdog->cancel();
        sink = nullptr;
        if (conn) conn->close();
        auto cancel_state = co_await boost::asio::this_coro::cancellation_state;
        bool cancelled = cancel_state.cancelled() != boost::asio::cancellation_type::none;
        if (!cancelled) {
            if (!dropped_.empty()) log.warning("{}: reconnecting", dropped_);
            else if (closed_by_server) log.warning("connection closed by the broker");
            else log.warning("session failed: {}", describe(error));
        }
        lost();
        if (cancelled) co_return;
        if (received) backoff = options.reconnect_min;
        log.info("reconnecting in {:.1f}s", backoff);
        co_await sleep(backoff);
        backoff = std::min(backoff * 2.0, options.reconnect_max);
    }
}

awaitable<void> CTraderFeed::watch(std::shared_ptr<StreamConnection> conn) {
    for (;;) {
        co_await sleep(1.0);
        if (auto why = tick(monotonic())) {
            dropped_ = *why;
            conn->close();  // the read fails, the session is built again
            co_return;
        }
    }
}

void CTraderFeed::start_session() {
    out_seq_ = in_seq_ = 0;
    logged_on = false;
    symbol_id.reset();
    symbol_name.clear();
    listed_.clear();
    entries_.clear();
    test_sent_.reset();
    started_ = last_sent_ = last_received_ = monotonic();
    ++sessions;
    send("A", {{98, "0"}, {108, std::format("{:g}", heartbeat_s)}, {141, "Y"}, {553, creds.login()}, {554, creds.password}});
}

std::optional<std::string> CTraderFeed::tick(double now) {
    if (!logged_on && now - started_ >= CTRADER_LOGON_TIMEOUT_S) return std::format("no logon answer in {:.0f} s", CTRADER_LOGON_TIMEOUT_S);
    if (now - last_sent_ >= heartbeat_s) send("0", {});
    double silent = now - last_received_;
    if (!test_sent_ && silent >= heartbeat_s + 5) {
        test_sent_ = now;
        send("1", {{112, std::format("t{}", static_cast<int64_t>(now))}});
    } else if (test_sent_ && now - *test_sent_ >= heartbeat_s) {
        return std::format("nothing from the broker for {:.0f} s", silent);
    }
    return std::nullopt;
}

void CTraderFeed::send(const std::string& msg_type, fix::Fields body) {
    if (!sink) return;
    ++out_seq_;
    fix::Fields header {{49, creds.sender_comp_id}, {56, creds.target_comp_id}, {34, std::to_string(out_seq_)}, {52, fix::utc_timestamp(now_us())}};
    if (!creds.sender_sub_id.empty()) header.emplace_back(50, creds.sender_sub_id);
    if (!creds.target_sub_id.empty()) header.emplace_back(57, creds.target_sub_id);
    sink(fix::encode(msg_type, header, body));
    last_sent_ = monotonic();
}

void CTraderFeed::lost() {
    logged_on = false;
    entries_.clear();
    if (book->synced) {
        book->invalidate();
        notify();
    }
}

void CTraderFeed::handle(const fix::Message& msg) {
    int64_t seq = 0;
    if (auto s = number(msg.get(34))) seq = static_cast<int64_t>(*s);
    std::string t = msg.type();
    if (t == "4") {  // SequenceReset: the next message is NewSeqNo
        auto next = number(msg.get(36));
        in_seq_ = (next ? static_cast<int64_t>(*next) : seq + 1) - 1;
        return;
    }
    if (in_seq_ && seq != in_seq_ + 1 && msg.get_or(43) != "Y")
        throw CTraderSessionError(std::format("MsgSeqNum {} after {}: a message lost", seq, in_seq_));
    in_seq_ = std::max(in_seq_, seq);
    if (t == "A") {
        logged_on = true;
        log.info("logged on as {}", creds.sender_comp_id);
        send("x", {{320, std::format("list-{}", sessions)}, {559, "0"}});  // SecurityListRequest: every symbol
    } else if (t == "5") {
        throw CTraderSessionError("logged out by the broker: " + msg.get_or(58, "(no text)"));
    } else if (t == "1") {
        send("0", {{112, msg.get_or(112)}});
    } else if (t == "3") {
        log.warning("message rejected by the broker: {} (ref tag {}, seq {})", msg.get_or(58), msg.get_or(371), msg.get_or(45));
    } else if (t == "j") {
        log.warning("business reject: {}", msg.get_or(58));
    } else if (t == "y") {
        security_list(msg);
    } else if (t == "Y") {
        refused = msg.get_or(58, msg.get_or(281, "rejected"));
        log.error("{}: market data request rejected: {}", symbol, refused);
    } else if (t == "W") {
        snapshot(msg, seq);
    } else if (t == "X") {
        incremental(msg, seq);
    }
}

void CTraderFeed::security_list(const fix::Message& msg) {
    for (const auto& g : msg.groups(146, 55)) {
        const std::string* name = field(g, 1007);
        listed_[name != nullptr && !name->empty() ? *name : g.at(55)] = g.at(55);
    }
    if (msg.get_or(893, "Y") == "Y") resolve_and_subscribe();  // LastFragment
}

void CTraderFeed::resolve_and_subscribe() {
    // the name as configured; else the one name that is it but for the case or a suffix after a
    // non-letter ("NVDA" for "NVDA.US")
    std::string want = upper(symbol);
    std::vector<std::string> exact, prefixed, alike;
    for (const auto& [name, id] : listed_) {
        std::string n = upper(name);
        if (name == symbol) exact.insert(exact.begin(), name);
        else if (n == want) exact.push_back(name);
        else if (starts_with(n, want) && !alnum(n[want.size()])) prefixed.push_back(name);
        if (n.find(want) != std::string::npos) alike.push_back(name);
    }
    const std::vector<std::string>& found = !exact.empty() ? exact : prefixed;
    log.info("symbol list: {} symbols", listed_.size());
    if (found.empty() || (exact.empty() && prefixed.size() > 1)) {
        if (alike.size() > 12) alike.resize(12);
        refused = found.empty() ? "not in the broker's symbol list" : "more than one symbol by that name";
        log.error("{}: {}{}: the cTrader guard stays idle", symbol, refused, alike.empty() ? "" : " (alike: " + join(alike, ", ") + ")");
        return;
    }
    symbol_name = found.front();
    symbol_id = listed_.at(symbol_name);
    refused.clear();
    send("V", {{262, symbol}, {263, "1"}, {264, "1"}, {265, "1"}, {267, "2"}, {269, "0"}, {269, "1"}, {146, "1"}, {55, *symbol_id}});
    log.info("{}: subscribed to {} (id {}), the spot price", symbol, symbol_name, *symbol_id);
}

void CTraderFeed::snapshot(const fix::Message& msg, int64_t seq) {
    if (!symbol_id || msg.get_or(55) != *symbol_id) return;
    entries_.clear();
    int i = 0;
    for (const auto& g : msg.groups(268, 269)) {
        const std::string* side = field(g, 269);
        auto price = number(field(g, 270));
        ++i;
        if (!price || side == nullptr || (*side != "0" && *side != "1")) continue;
        auto size = number(field(g, 271));
        const std::string* id = field(g, 278);
        entries_[id != nullptr && !id->empty() ? *id : std::format("spot-{}-{}", *side, i)] = {(*side)[0], *price,
                                                                                            size ? *size : std::numeric_limits<double>::quiet_NaN()};
    }
    publish(seq, msg);
}

void CTraderFeed::incremental(const fix::Message& msg, int64_t seq) {
    if (!symbol_id) return;
    bool touched = false;
    for (const auto& g : msg.groups(268, 279)) {
        const std::string* sym = field(g, 55);
        if ((sym != nullptr ? *sym : msg.get_or(55)) != *symbol_id) continue;
        touched = true;
        std::string id = g.contains(278) ? g.at(278) : "";
        if (g.contains(279) && g.at(279) == "2") {
            entries_.erase(id);
            continue;
        }
        const std::string* side = field(g, 269);
        auto price = number(field(g, 270));
        char s = side != nullptr && side->size() == 1 ? (*side)[0] : ' ';
        if (!price || (s != '0' && s != '1')) {
            auto it = entries_.find(id);
            if (it == entries_.end()) continue;  // a change of an entry never seen
            s = std::get<0>(it->second);
            price = std::get<1>(it->second);
        }
        auto size = number(field(g, 271));
        entries_[id] = {s, *price, size ? *size : std::numeric_limits<double>::quiet_NaN()};
    }
    if (touched) publish(seq, msg);
}

void CTraderFeed::publish(int64_t seq, const fix::Message& msg) {
    // the best bid and ask, each the sum of the quotes at its price; a size the broker leaves out
    // (the spot price has none) counts 1, the book drops a level of no size (the guard reads prices)
    std::optional<Level> bid, ask;
    for (const auto& [id, entry] : entries_) {
        auto [side, price, size] = entry;
        auto& best = side == '0' ? bid : ask;
        bool better = !best || (side == '0' ? price > best->first : price < best->first);
        double known = std::isnan(size) ? 1.0 : size;
        if (better) best = Level {price, known};
        else if (price == best->first) best->second += known;
    }
    if (!bid || !ask || bid->first >= ask->first) return;  // one side gone or crossed: the last good top stays
    book->apply_snapshot(std::vector<Level> {*bid}, std::vector<Level> {*ask}, seq, fix::parse_timestamp(msg.get_or(52)));
    notify();
}

}  // namespace nmm
