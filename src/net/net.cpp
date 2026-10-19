#include "net/net.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <format>

#include "util/pyfmt.hpp"

namespace nmm {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace {

constexpr const char* USER_AGENT = "naive-mm-cpp/1.0";

std::chrono::steady_clock::duration seconds(double s) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(s));
}

ssl::context& tls_context() {
    static ssl::context ctx = [] {
        ssl::context c(ssl::context::tls_client);
        c.set_default_verify_paths();
        if (const char* file = std::getenv("SSL_CERT_FILE"); file != nullptr && *file) {
            boost::system::error_code ec;
            c.load_verify_file(file, ec);
        }
        if (const char* dir = std::getenv("SSL_CERT_DIR"); dir != nullptr && *dir) {
            boost::system::error_code ec;
            c.add_verify_path(dir, ec);
        }
        c.set_verify_mode(ssl::verify_peer);
        return c;
    }();
    return ctx;
}

std::string base64(std::string_view in) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t n = uint8_t(in[i]) << 16;
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

struct Proxy {
    Url url;
    std::string auth;  // "user:pass", or empty
};

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v ? v : nullptr;
}

bool bypass(const std::string& host) {
    if (host == "localhost" || starts_with(host, "127.") || host == "::1" || host == "[::1]") return true;
    const char* no_proxy = env("NO_PROXY");
    if (no_proxy == nullptr) no_proxy = env("no_proxy");
    if (no_proxy == nullptr) return false;
    std::string list = no_proxy;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        std::string entry(strip(std::string_view(list).substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
        start = comma == std::string::npos ? list.size() + 1 : comma + 1;
        if (entry.empty()) continue;
        if (entry == "*") return true;
        if (auto colon = entry.find(':'); colon != std::string::npos && entry.find(']') == std::string::npos) entry = entry.substr(0, colon);
        if (starts_with(entry, "*.")) entry = entry.substr(1);
        if (entry[0] == '.') entry = entry.substr(1);
        if (host == entry || ends_with(host, "." + entry)) return true;
    }
    return false;
}

std::optional<Proxy> proxy_for(const Url& target) {
    if (bypass(target.host)) return std::nullopt;
    std::vector<const char*> names;
    if (target.scheme == "wss") names = {"WSS_PROXY", "wss_proxy", "HTTPS_PROXY", "https_proxy"};
    else if (target.scheme == "ws") names = {"WS_PROXY", "ws_proxy", "HTTP_PROXY", "http_proxy"};
    else if (target.scheme == "https") names = {"HTTPS_PROXY", "https_proxy"};
    else names = {"HTTP_PROXY", "http_proxy"};
    for (const char* name : names) {
        const char* value = env(name);
        if (value == nullptr) continue;
        std::string text = value;
        if (text.find("://") == std::string::npos) text = "http://" + text;
        Proxy proxy;
        auto scheme_end = text.find("://") + 3;
        auto at = text.find('@', scheme_end);
        if (at != std::string::npos) {
            proxy.auth = text.substr(scheme_end, at - scheme_end);
            text = text.substr(0, scheme_end) + text.substr(at + 1);
        }
        proxy.url = Url::parse(text);
        return proxy;
    }
    return std::nullopt;
}

// A TCP connection to `target` (through the proxy, tunnelled by CONNECT) with its deadline set.
awaitable<beast::tcp_stream> connect_tcp(const Url& target, double timeout_s) {
    auto ex = co_await asio::this_coro::executor;
    auto proxy = proxy_for(target);
    const Url& hop = proxy ? proxy->url : target;
    tcp::resolver resolver(ex);
    auto results = co_await resolver.async_resolve(hop.host, hop.port, asio::use_awaitable);
    beast::tcp_stream stream(ex);
    stream.expires_after(seconds(timeout_s));
    co_await stream.async_connect(results, asio::use_awaitable);
    stream.socket().set_option(tcp::no_delay(true));
    if (proxy) {
        std::string authority = target.host + ":" + target.port;
        http::request<http::empty_body> req {http::verb::connect, authority, 11};
        req.set(http::field::host, authority);
        req.set(http::field::user_agent, USER_AGENT);
        if (!proxy->auth.empty()) req.set(http::field::proxy_authorization, "Basic " + base64(proxy->auth));
        co_await http::async_write(stream, req, asio::use_awaitable);
        beast::flat_buffer buffer;
        http::response_parser<http::empty_body> parser;
        parser.skip(true);  // a CONNECT reply has no body
        co_await http::async_read_header(stream, buffer, parser, asio::use_awaitable);
        int status = parser.get().result_int();
        if (status != 200) throw std::runtime_error(std::format("proxy {} refused CONNECT {}: HTTP {}", hop.host, authority, status));
    }
    co_return stream;
}

awaitable<beast::ssl_stream<beast::tcp_stream>> tls_handshake(beast::tcp_stream tcp_stream, const Url& url, double timeout_s) {
    beast::ssl_stream<beast::tcp_stream> stream(std::move(tcp_stream), tls_context());
    if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str()))
        throw std::runtime_error("TLS: cannot set the server name " + url.host);
    stream.set_verify_callback(ssl::host_name_verification(url.host));
    beast::get_lowest_layer(stream).expires_after(seconds(timeout_s));
    co_await stream.async_handshake(ssl::stream_base::client, asio::use_awaitable);
    co_return stream;
}

template <class Next>
class WsImpl : public WsConnection, public std::enable_shared_from_this<WsImpl<Next>> {
public:
    explicit WsImpl(Next&& next) : ws_(std::move(next)) {}

    awaitable<void> handshake(const Url& url, double heartbeat_s, double timeout_s) {
        beast::get_lowest_layer(ws_).expires_never();  // the websocket's own timeouts from here
        websocket::stream_base::timeout opt;
        opt.handshake_timeout = seconds(timeout_s);
        if (heartbeat_s > 0) {
            // a ping after half of it idle, the connection dropped after all of it
            opt.idle_timeout = seconds(heartbeat_s * 1.5);
            opt.keep_alive_pings = true;
        } else {
            opt.idle_timeout = websocket::stream_base::none();
            opt.keep_alive_pings = false;
        }
        ws_.set_option(opt);
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) { req.set(http::field::user_agent, USER_AGENT); }));
        ws_.read_message_max(0);
        ws_.auto_fragment(false);
        co_await ws_.async_handshake(url.host_header(), url.target, asio::use_awaitable);
        ws_.text(true);
    }

    awaitable<std::optional<std::string>> read() override {
        auto self = this->shared_from_this();
        buffer_.clear();
        auto [ec, n] = co_await ws_.async_read(buffer_, asio::as_tuple(asio::use_awaitable));
        if (ec == websocket::error::closed) {
            closed_ = true;
            co_return std::nullopt;
        }
        if (ec) {
            closed_ = true;
            if (closed_locally_ && ec == asio::error::operation_aborted) {
                co_await check_cancelled();  // a cancelled task stays cancelled
                throw std::runtime_error("connection closed");
            }
            throw boost::system::system_error(ec);
        }
        co_return beast::buffers_to_string(buffer_.data());
    }

    bool send(std::string text, std::function<void(const std::string&)> done) override {
        if (closed_) {
            if (done) done("connection closed");
            return false;
        }
        queue_.push_back({std::move(text), std::move(done)});
        if (writing_) return false;
        write_next();
        return true;
    }

    void close() override {
        if (closed_locally_) return;
        closed_locally_ = true;
        closed_ = true;
        beast::get_lowest_layer(ws_).close();
    }

    awaitable<void> close_gracefully(double timeout_s) override {
        auto self = this->shared_from_this();
        if (!closed_) {
            closed_ = true;
            websocket::stream_base::timeout opt;
            ws_.get_option(opt);
            opt.handshake_timeout = seconds(timeout_s);
            ws_.set_option(opt);
            co_await ws_.async_close(websocket::close_code::normal, asio::as_tuple(asio::use_awaitable));
        }
        close();
    }

    bool closed() const override { return closed_; }
    int close_code() const override { return static_cast<int>(ws_.reason().code); }

private:
    struct Pending {
        std::string text;
        std::function<void(const std::string&)> done;
    };

    void write_next() {
        writing_ = true;
        ws_.async_write(asio::buffer(queue_.front().text),
                        [self = this->shared_from_this()](const boost::system::error_code& ec, size_t) {
                            Pending sent = std::move(self->queue_.front());
                            self->queue_.pop_front();
                            if (ec) {
                                self->writing_ = false;
                                self->closed_ = true;
                                std::string error = ec.message();
                                if (sent.done) sent.done(error);
                                auto rest = std::move(self->queue_);
                                self->queue_.clear();
                                for (auto& p : rest)
                                    if (p.done) p.done(error);
                                return;
                            }
                            if (sent.done) sent.done("");
                            if (self->queue_.empty() || self->closed_) {
                                self->writing_ = false;
                                auto rest = std::move(self->queue_);
                                self->queue_.clear();
                                for (auto& p : rest)
                                    if (p.done) p.done("connection closed");
                            } else {
                                self->write_next();
                            }
                        });
    }

    websocket::stream<Next> ws_;
    beast::flat_buffer buffer_;
    std::deque<Pending> queue_;
    bool writing_ = false;
    bool closed_ = false;
    bool closed_locally_ = false;
};

}  // namespace

std::string Url::host_header() const {
    bool default_port = (port == "443" && tls()) || (port == "80" && !tls());
    return default_port ? host : host + ":" + port;
}

Url Url::parse(const std::string& url) {
    Url u;
    auto sep = url.find("://");
    if (sep == std::string::npos) throw std::invalid_argument("not a URL: " + url);
    u.scheme = lower(url.substr(0, sep));
    if (u.scheme != "ws" && u.scheme != "wss" && u.scheme != "http" && u.scheme != "https")
        throw std::invalid_argument("unsupported URL scheme: " + url);
    std::string rest = url.substr(sep + 3);
    auto slash = rest.find_first_of("/?");
    std::string authority = rest.substr(0, slash);
    u.target = slash == std::string::npos ? "/" : rest.substr(slash);
    if (!u.target.empty() && u.target[0] == '?') u.target = "/" + u.target;
    if (auto at = authority.find('@'); at != std::string::npos) authority = authority.substr(at + 1);
    if (!authority.empty() && authority[0] == '[') {  // [::1]:8080
        auto close = authority.find(']');
        u.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') u.port = authority.substr(close + 2);
    } else if (auto colon = authority.rfind(':'); colon != std::string::npos) {
        u.host = authority.substr(0, colon);
        u.port = authority.substr(colon + 1);
    } else {
        u.host = authority;
    }
    if (u.host.empty()) throw std::invalid_argument("no host in URL: " + url);
    if (u.port.empty()) u.port = u.tls() ? "443" : "80";
    return u;
}

std::string url_encode(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else out += std::format("%{:02X}", c);
    }
    return out;
}

std::string with_params(const std::string& url, const std::vector<std::pair<std::string, std::string>>& params) {
    if (params.empty()) return url;
    std::string out = url + (url.find('?') == std::string::npos ? "?" : "&");
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) out += "&";
        out += url_encode(params[i].first) + "=" + url_encode(params[i].second);
    }
    return out;
}

void HttpResponse::raise_for_status(const std::string& url) const {
    if (status >= 400)
        throw HttpError(status, std::format("{}, {} for url: {}", status, status >= 500 ? "Server Error" : "Client Error", url));
}

void init_tls() { (void)tls_context(); }

awaitable<HttpResponse> http_get(std::string url, std::vector<std::pair<std::string, std::string>> headers, double timeout_s) {
    Url u = Url::parse(url);
    http::request<http::empty_body> req {http::verb::get, u.target, 11};
    req.set(http::field::host, u.host_header());
    req.set(http::field::user_agent, USER_AGENT);
    req.set(http::field::accept, "*/*");
    req.set(http::field::connection, "close");
    for (auto& [name, value] : headers) req.set(name, value);
    http::response_parser<http::string_body> parser;
    parser.body_limit(64 * 1024 * 1024);
    beast::flat_buffer buffer;
    auto tcp_stream = co_await connect_tcp(u, timeout_s);
    if (u.tls()) {
        auto stream = co_await tls_handshake(std::move(tcp_stream), u, timeout_s);
        co_await http::async_write(stream, req, asio::use_awaitable);
        co_await http::async_read(stream, buffer, parser, asio::use_awaitable);
        beast::get_lowest_layer(stream).close();
    } else {
        co_await http::async_write(tcp_stream, req, asio::use_awaitable);
        co_await http::async_read(tcp_stream, buffer, parser, asio::use_awaitable);
        tcp_stream.close();
    }
    HttpResponse response;
    auto& msg = parser.get();
    response.status = msg.result_int();
    for (const auto& field : msg) response.headers[lower(field.name_string())] = std::string(field.value());
    response.body = std::move(msg.body());
    co_return response;
}

awaitable<std::shared_ptr<WsConnection>> WsConnection::connect(std::string url, double heartbeat_s, double timeout_s) {
    Url u = Url::parse(url);
    if (u.scheme != "ws" && u.scheme != "wss") throw std::invalid_argument("not a websocket URL: " + url);
    auto tcp_stream = co_await connect_tcp(u, timeout_s);
    if (u.tls()) {
        auto stream = co_await tls_handshake(std::move(tcp_stream), u, timeout_s);
        auto ws = std::make_shared<WsImpl<beast::ssl_stream<beast::tcp_stream>>>(std::move(stream));
        co_await ws->handshake(u, heartbeat_s, timeout_s);
        co_return ws;
    }
    auto ws = std::make_shared<WsImpl<beast::tcp_stream>>(std::move(tcp_stream));
    co_await ws->handshake(u, heartbeat_s, timeout_s);
    co_return ws;
}

}  // namespace nmm
